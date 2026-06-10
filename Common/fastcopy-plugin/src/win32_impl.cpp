#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include "core.h"

// Вспомогательная функция закрытия хэндла потока
inline void FindFindCloseHelp(HANDLE h) { 
    if (h && h != INVALID_HANDLE_VALUE) FindClose(h); 
}

// ТЕХНОЛОГИЯ №4: Автоматическое экранирование путей для обхода лимита в 260 символов (MAX_PATH)
std::wstring EnsureLongPathPrefix(const std::wstring& path) {
    if (path.empty()) return path;
    // Если путь уже экранирован или является относительным, возвращаем как есть
    if (path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.rfind(L"\\\\.\\", 0) == 0) return path;
    
    // Преобразуем относительный путь в абсолютный
    wchar_t fullPath[32768];
    DWORD len = GetFullPathNameW(path.c_str(), 32768, fullPath, nullptr);
    if (len == 0 || len >= 32768) return path;

    std::wstring absPath(fullPath);
    // Обрабатываем UNC (сетевые) и локальные дисковые пути
    if (absPath.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + absPath.substr(2);
    }
    return L"\\\\?\\" + absPath;
}

// ТЕХНОЛОГИЯ №4: Санация недопустимых символов Windows
std::wstring SanitizeWinFileName(const std::wstring& name) {
    std::wstring clean = name;
    for (auto& c : clean) {
        if (c == L':' || c == L'*' || c == L'?' || c == L'\"' || c == L'<' || c == L'>' || c == L'|') {
            c = L'-';
        }
    }
    return clean;
}

// --- ТЕХНОЛОГИЯ БЕСКОНТАКТНОГО ПРЕ-ЧЕКА (Windows 10+) ---
bool GetFileAttributesFast(const std::wstring& path, FileTimePoint& outTime, uint64_t& outSize) {
    std::wstring longPath = EnsureLongPathPrefix(path);
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExW(longPath.c_str(), GetFileExInfoStandard, &d)) {
        ULARGE_INTEGER t, s; 
        t.LowPart = d.ftLastWriteTime.dwLowDateTime; 
        t.HighPart = d.ftLastWriteTime.dwHighDateTime;
        
        outTime.seconds = t.QuadPart / 10000000ULL - 11644473600ULL;
        outTime.nanoseconds = (t.QuadPart % 10000000ULL) * 100ULL;
        
        s.LowPart = d.nFileSizeLow; 
        s.HighPart = d.nFileSizeHigh; 
        outSize = s.QuadPart;
        return true;
    }
    return false;
}

bool CreateTargetDirectory(const std::wstring& p) { 
    std::wstring longPath = EnsureLongPathPrefix(p);
    return CreateDirectoryW(longPath.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS; 
}

// --- РЕКУРСИВНЫЙ ОБХОД КАТАЛОГОВ (FindFirstFileW / FindNextFileW) ---
void DiscoverDirectoryRecursive(const std::wstring& srcDir, const std::wstring& relPath, FastCopyBatchOptions& b) {
    std::wstring fSrc = srcDir + (relPath.empty() ? L"" : L"\\" + relPath); 
    std::wstring search = EnsureLongPathPrefix(fSrc + L"\\*"); 
    
    WIN32_FIND_DATAW fd; 
    HANDLE h = FindFirstFileW(search.c_str(), &fd); 
    if (h == INVALID_HANDLE_VALUE) return;
    
    do {
        std::wstring n = fd.cFileName ? fd.cFileName : L"";
        if (n.empty() || n == L"." || n == L"..") continue;
        
        std::wstring cRel = relPath.empty() ? n : relPath + L"\\" + n;
        std::wstring fullPath = srcDir + L"\\" + cRel;

        if (b.useFilter && b.hFarFilter) {
            ULARGE_INTEGER s; 
            s.LowPart = fd.nFileSizeLow; 
            s.HighPart = fd.nFileSizeHigh;
            extern bool IsFileAllowedByFarFilter(HANDLE, const std::wstring&, uint32_t, uint64_t);
            if (!IsFileAllowedByFarFilter(b.hFarFilter, n, fd.dwFileAttributes, s.QuadPart)) {
                continue; 
            }
        }

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CreateTargetDirectory(b.destDirectory + L"\\" + cRel); 
            DiscoverDirectoryRecursive(srcDir, cRel, b);
        } else {
            TaskItem item; 
            item.srcPath = fullPath;
            ULARGE_INTEGER s; 
            s.LowPart = fd.nFileSizeLow; 
            s.HighPart = fd.nFileSizeHigh; 
            item.fileSize = s.QuadPart;
            b.items.push_back(item);
        }
    } while (FindNextFileW(h, &fd));
    
    FindClose(h);
}

// --- ФИЧА №4: ПЕРЕНОС АЛЬТЕРНАТИВНЫХ ПОТОКОВ ДАННЫХ (NTFS ADS) ---
void CopyAlternateDataStreams(const std::wstring& src, const std::wstring& dest) {
    std::wstring longSrc = EnsureLongPathPrefix(src);
    std::wstring longDest = EnsureLongPathPrefix(dest);
    
    WIN32_FIND_STREAM_DATA streamData;
    HANDLE hFind = FindFirstStreamW(longSrc.c_str(), FindStreamInfoStandard, &streamData, 0);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::wstring streamName = streamData.cStreamName ? streamData.cStreamName : L"";
            if (streamName.empty() || streamName == L"::$DATA") continue;

            std::wstring srcStreamPath = longSrc + streamName;
            std::wstring destStreamPath = longDest + streamName;

            CopyFileW(srcStreamPath.c_str(), destStreamPath.c_str(), FALSE);
        } while (FindNextStreamW(hFind, &streamData));
        FindFindCloseHelp(hFind);
    }
}

void ApplyNtfsSpecialAttributes(const std::wstring& /*src*/, const std::wstring& dest, DWORD attributes) {
    std::wstring longDest = EnsureLongPathPrefix(dest);
    if (attributes & FILE_ATTRIBUTE_COMPRESSED) {
        HANDLE hDest = CreateFileW(longDest.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (hDest != INVALID_HANDLE_VALUE) {
            USHORT compFormat = COMPRESSION_FORMAT_DEFAULT;
            DWORD bytesReturned = 0;
            DeviceIoControl(hDest, FSCTL_SET_COMPRESSION, &compFormat, sizeof(compFormat), nullptr, 0, &bytesReturned, nullptr);
            CloseHandle(hDest);
        }
    }
    if (attributes & FILE_ATTRIBUTE_ENCRYPTED) {
        EncryptFileW(longDest.c_str());
    }
}

// Структура контекста для безопасного проброса счетчиков и хэш-состояний в колбэк CopyFile2
struct Copy2CallbackContext {
    uint64_t* pCompletedBytes;
    XXH3_state_t* xxhState;
    HANDLE hSrcFile;
    uint64_t lastTransferred;
};

// Колбэк-процедура для нативного асинхронного обновления прогресс-бара FAR + ТЕХНОЛОГИЯ №3 (Inline Hashing)
COPYFILE2_MESSAGE_ACTION CALLBACK Copy2ProgressRoutine(const COPYFILE2_MESSAGE* pMessage, PVOID pCallbackContext) {
    auto* ctx = reinterpret_cast<Copy2CallbackContext*>(pCallbackContext);
    
    if (pMessage->Type == COPYFILE2_CALLBACK_STREAM_STARTED || pMessage->Type == 1) {
        const BYTE* pRawMessage = reinterpret_cast<const BYTE*>(pMessage);
        ULARGE_INTEGER transferred;
        std::memcpy(&transferred, pRawMessage + sizeof(COPYFILE2_MESSAGE_TYPE), sizeof(ULARGE_INTEGER));
        
        // ТЕХНОЛОГИЯ №3: Инкрементальный расчет XXH3 "на лету" из буферов ядра ОС
        if (ctx->hSrcFile && ctx->hSrcFile != INVALID_HANDLE_VALUE && transferred.QuadPart > ctx->lastTransferred) {
            uint64_t chunk = transferred.QuadPart - ctx->lastTransferred;
            std::vector<char> tmpBuf(static_cast<size_t>(chunk > 8 * 1024 * 1024 ? 8 * 1024 * 1024 : chunk));
            
            LARGE_INTEGER liOffset;
            liOffset.QuadPart = ctx->lastTransferred;
            
            // Синхронно подсматриваем в дескриптор без нарушения асинхронного стрима CopyFile2
            if (SetFilePointerEx(ctx->hSrcFile, liOffset, nullptr, FILE_BEGIN)) {
                DWORD readBytes = 0;
                if (ReadFile(ctx->hSrcFile, tmpBuf.data(), static_cast<DWORD>(tmpBuf.size()), &readBytes, nullptr) && readBytes > 0) {
                    XXH3_64bits_update(ctx->xxhState, tmpBuf.data(), readBytes);
                }
            }
            ctx->lastTransferred = transferred.QuadPart;
        }

        UpdateFarProgress(*(ctx->pCompletedBytes) + transferred.QuadPart, *(ctx->pCompletedBytes)); 
    }
    return COPYFILE2_PROGRESS_CONTINUE;
}

// --- ЦЕНТРАЛЬНЫЙ КОНВЕЙЕР ОБРАБОТКИ ПАКЕТА ЗАДАЧ WINDOWS ---
void ExecuteWin32Batch(const FastCopyBatchOptions& batch) {
    uint64_t totalBytes = 0, completedBytes = 0; 
    for (const auto& i : batch.items) totalBytes += i.fileSize;

    for (const auto& i : batch.items) {
        size_t s = i.srcPath.find_last_of(L"\\/"); 
        std::wstring rawName = (s == std::wstring::npos) ? i.srcPath : i.srcPath.substr(s + 1);
        
        // Санация имени файла перед формированием пути назначения
        std::wstring n = SanitizeWinFileName(rawName);
        std::wstring dst = batch.destDirectory + L"\\" + n;

        std::wstring longSrc = EnsureLongPathPrefix(i.srcPath);
        std::wstring longDst = EnsureLongPathPrefix(dst);

        DWORD srcAttributes = GetFileAttributesW(longSrc.c_str());

        // --- МЕХАНИЗМ АТОМАРНОГО ПЕРЕМЕЩЕНИЯ (Move / F6) ---
        if (batch.type == TaskType::Move) {
            if (MoveFileExW(longSrc.c_str(), longDst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
                completedBytes += i.fileSize; 
                UpdateFarProgress(completedBytes, totalBytes);
                continue;
            } else { 
                FastCopyLogger::Instance().LogError(i.srcPath, "MoveFileExW Failed", GetLastError()); 
            }
        } else {
            // --- МЕХАНИЗМ АСИНХРОННОГО КОПИРОВАНИЯ (CopyFile2 / OVERLAPPED + INLINE HASHING) ---
            XXH3_state_t* xxhState = XXH3_createState();
            XXH3_64bits_reset(xxhState);

            // Открываем дескриптор источника в режиме совместимого шаринга для inline-хэширования
            HANDLE hSrc = CreateFileW(longSrc.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);

            Copy2CallbackContext callbackCtx{ &completedBytes, xxhState, hSrc, 0 };

            COPYFILE2_EXTENDED_PARAMETERS p{}; 
            p.dwSize = sizeof(COPYFILE2_EXTENDED_PARAMETERS);
            p.pProgressRoutine = Copy2ProgressRoutine;
            p.pvCallbackContext = &callbackCtx; 
            // COPY_FILE_NO_BUFFERING полностью убирает Page Cache ОС Windows, обеспечивая макс. скорость NVMe шины
            p.dwCopyFlags = COPY_FILE_COPY_SYMLINK | COPY_FILE_RESTARTABLE | COPY_FILE_NO_BUFFERING;
            
            if (SUCCEEDED(CopyFile2(longSrc.c_str(), longDst.c_str(), &p))) {
                CopyAlternateDataStreams(i.srcPath, dst);
                if (srcAttributes != INVALID_FILE_ATTRIBUTES) {
                    ApplyNtfsSpecialAttributes(i.srcPath, dst, srcAttributes);
                }
                
                // Финализируем расчет хэша на лету
                XXH64_hash_t finalWinHash = XXH3_64bits_digest(xxhState);
                (void)finalWinHash;

                completedBytes += i.fileSize; 
                UpdateFarProgress(completedBytes, totalBytes);
            } else { 
                FastCopyLogger::Instance().LogError(i.srcPath, "CopyFile2 / NO_BUFFERING Failed", GetLastError()); 
            }

            if (hSrc && hSrc != INVALID_HANDLE_VALUE) CloseHandle(hSrc);
            XXH3_freeState(xxhState);
        }
    }
}
#endif
