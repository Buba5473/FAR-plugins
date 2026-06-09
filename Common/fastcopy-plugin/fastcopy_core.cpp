#include "bridge.h"
#include <fstream>
#include <shlobj.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <filesystem>
#include <sstream>
#include <atomic>
#include <vector>
#include <cstring>

#if PLATFORM_LINUX
    #include <sys/ioctl.h>
    #include <linux/fs.h>
    #include <fcntl.h>
    #include <sys/stat.h>
#else
    #include <winioctl.h>
    #include <ntddstor.h>
#endif

namespace fs = std::filesystem;

// ============================================================================
// КОНСТАНТЫ FAR MANAGER 3 API И ИДЕНТИФИКАТОРЫ
// ============================================================================
#define ACTL_KEY 15
#define FMSG_WARNING          0x00000001
#define FMSG_LEFTALIGN        0x00000004
#define ACTL_SETPROGRESSVALUE 28

struct ProgressValue {
    unsigned __int64 Completed;
    unsigned __int64 Total;
};

enum FarDialogItemTypes {
    DI_DOUBLEBOX,
    DI_TEXT,
    DI_BUTTON
};

struct FarDialogItem {
    int Type;
    int X1, Y1, X2, Y2;
    int Focus;
    intptr_t Selected;
    unsigned __int64 Flags;
    const wchar_t* Data;
};

enum PluginCommands {
    CMD_IMMEDIATE_COPY = 1,
    CMD_IMMEDIATE_MOVE = 2,
    CMD_PAUSE_TOGGLE   = 3,
    CMD_QUEUE_COPY     = 4,
    CMD_QUEUE_MOVE     = 5
};

enum class ConflictResult { 
    Overwrite, 
    Skip, 
    Append 
};

struct CopyTask {
    fs::path source;
    fs::path destination;
    bool deleteSourceAfterCopy;
    ConflictResult conflictAction;
};

struct OptimizedCopyContext {
    fs::path srcPath;
    fs::path dstPath;
    std::atomic<unsigned __int64> copiedBytes{0};
    std::atomic<unsigned __int64> totalBytes{0};
    std::atomic<bool> isCancelled{false};
    std::atomic<bool> isFinished{false};
    ConflictResult action{ConflictResult::Overwrite};
};

static std::atomic<bool> g_isQueuePaused{false};
static size_t g_globalTotalTasks = 0;

// Таблица функций Far Manager 3 SDK
typedef intptr_t(WINAPI *FARAPIMESSAGE)(const GUID* PluginId, const GUID* Id, struct STARTUPINFOFLAGS Flags, const wchar_t* HelpTopic, const wchar_t* const *Items, size_t ItemsNumber, intptr_t ButtonsNumber);
typedef intptr_t(WINAPI *FARAPICONTROL)(HANDLE hPlugin, intptr_t Command, intptr_t Param1, void* Param2);
typedef intptr_t(WINAPI *FARAPIDIALOGINIT)(const GUID* PluginId, const GUID* Id, int X1, int Y1, int X2, int Y2, const wchar_t* HelpTopic, const struct FarDialogItem* Item, size_t ItemsNumber, intptr_t Reserved, unsigned __int64 Flags, void* DlgProc, void* Param);
typedef intptr_t(WINAPI *FARAPIDIALOGRUN)(HANDLE hDlg);
typedef void(WINAPI *FARAPIDIALOGFREE)(HANDLE hDlg);

struct PluginStartupInfo {
    size_t StructSize;
    const wchar_t* ModuleName;
    FARAPIMESSAGE Message;
    FARAPICONTROL Control;
    FARAPIDIALOGINIT DialogInit;
    FARAPIDIALOGRUN DialogRun;
    FARAPIDIALOGFREE DialogFree;
};

static PluginStartupInfo GInfo;
static bool g_FarInfoInitialized = false;
static const GUID MainGuid = { 0xB5473F4A, 0xE27F, 0x4B9A, { 0x99, 0xA0, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB } };

// ============================================================================
// 1. МИНИМАЛЬНАЯ РЕАЛИЗАЦИЯ XXHASH (ДЛЯ ИНКРЕМЕНТАЛЬНОЙ СИНХРОНИЗАЦИИ)
// ============================================================================
using XXH64_hash_t = unsigned long long;
constexpr XXH64_hash_t PRIME64_1 = 11400714785074694791ULL;
constexpr XXH64_hash_t PRIME64_2 = 14029467366897019727ULL;

inline XXH64_hash_t XXH64_round(XXH64_hash_t acc, XXH64_hash_t input) {
    acc += input * PRIME64_2;
    acc = (acc << 31) | (acc >> (64 - 31));
    acc *= PRIME64_1;
    return acc;
}

XXH64_hash_t CalculateFastHash(const fs::path& filePath) {
    std::ifstream f(filePath, std::ios::binary);
    if (!f.is_open()) return 0;
    
    char buffer[4096];
    XXH64_hash_t acc = PRIME64_1 + PRIME64_2;
    
    f.read(buffer, sizeof(buffer));
    std::streamsize bytes = f.gcount();
    
    for (std::streamsize i = 0; i < bytes / 8; ++i) {
        XXH64_hash_t input;
        std::memcpy(&input, buffer + (i * 8), 8);
        acc = XXH64_round(acc, input);
    }
    return acc;
}

// ============================================================================
// 2. ДИНАМИЧЕСКОЕ ОПРЕДЕЛЕНИЕ ТИПА НАКОПИТЕЛЯ (HDD VS SSD)
// ============================================================================
bool IsTargetDriveSSD(const fs::path& p) {
#if PLATFORM_WINDOWS
    std::wstring root = p.root_path().wstring();
    std::wstring volPath = L"\\\\.\\" + root.substr(0, 2);
    HANDLE hDevice = ::CreateFileW(volPath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDevice == INVALID_HANDLE_VALUE) return true; // Фоллбэк на SSD стратегии

    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    
    DEVICE_SEEK_PENALTY_DESCRIPTOR result = {};
    DWORD bytesReturned = 0;
    bool isSSD = true;
    
    if (::DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &result, sizeof(result), &bytesReturned, nullptr)) {
        isSSD = !result.IncursSeekPenalty;
    }
    ::CloseHandle(hDevice);
    return isSSD;
#else
    std::string base_dev = p.root_path().string();
    std::ifstream rotational("/sys/block/" + base_dev + "/queue/rotational");
    if (rotational.is_open()) {
        int val = 1;
        rotational >> val;
        return val == 0;
    }
    return true; 
#endif
}

// ============================================================================
// 3. ИНТЕГРАЦИЯ С ПАНЕЛЬЮ ЗАДАЧ WINDOWS (TASKBAR PROGRESS) И ИНТЕРФЕЙСОМ
// ============================================================================
void UpdateFarTaskbarProgress(unsigned __int64 completed, unsigned __int64 total) {
    if (g_FarInfoInitialized && GInfo.Control) {
        ProgressValue pv = { completed, total };
        GInfo.Control(INVALID_HANDLE_VALUE, (static_cast<intptr_t>(ACTL_SETPROGRESSVALUE)), 0, &pv);
    }
}

void ShowQueueStatusToast(size_t currentQueueSize, const std::wstring& lastAddedFile) {
    if (!g_FarInfoInitialized) return;
    std::wstring title = g_isQueuePaused ? L" FastCopy Queue Engine [PAUSED] " : L" FastCopy Queue Engine ";
    std::wstring line1 = L"Задача добавлена в фоновую очередь.";
    std::wstring line2 = L"Всего задач в пуле: " + std::to_wstring(currentQueueSize);
    std::wstring line3 = L"Элемент: " + lastAddedFile;
    const wchar_t* msgItems[] = { title.c_str(), line1.c_str(), line2.c_str(), line3.c_str() };
    GInfo.Message(&MainGuid, nullptr, {(STARTUPINFOFLAGS)(FMSG_WARNING | FMSG_LEFTALIGN)}, nullptr, msgItems, 4, 0);
}

// ============================================================================
// 4. РЕАЛИЗАЦИЯ ДИАЛОГА РАЗРЕШЕНИЯ КОНФЛИКТОВ (FAR DIALOG API)
// ============================================================================
ConflictResult ShowFarConflictDialog(const std::wstring& filename) {
    if (!g_FarInfoInitialized || !GInfo.DialogInit) return ConflictResult::Skip;

    std::wstring promptStr = L"Файл '" + filename + L"' уже существует и имеет такой же размер!";
    
    FarDialogItem items[] = {
        { DI_DOUBLEBOX, 3,  1, 72, 8, 0, 0, 0, L"Конфликт имен файлов" },
        { DI_TEXT,      5,  3, 0,  0, 0, 0, 0, promptStr.c_str() },
        { DI_BUTTON,    5,  6, 0,  0, 1, 0, 0, L"[ Перезаписать ]" },
        { DI_BUTTON,    25, 6, 0,  0, 0, 0, 0, L"[ Пропустить ]" },
        { DI_BUTTON,    45, 6, 0,  0, 0, 0, 0, L"[ Дописать (Resume) ]" }
    };

    HANDLE hDlg = (HANDLE)GInfo.DialogInit(&MainGuid, nullptr, -1, -1, 76, 10, nullptr, items, 5, 0, 0, nullptr, nullptr);
    if (hDlg == INVALID_HANDLE_VALUE) return ConflictResult::Skip;

    intptr_t exitCode = GInfo.DialogRun(hDlg);
    GInfo.DialogFree(hDlg);

    if (exitCode == 2) return ConflictResult::Overwrite;
    if (exitCode == 4) return ConflictResult::Append;
    return ConflictResult::Skip;
}

// ============================================================================
// 5. НИЗКОУРОВНЕВЫЕ СИСТЕМНЫЕ ФУНКЦИИ И ПЕРЕНОС МЕТАДАННЫХ NTFS
// ============================================================================
#if PLATFORM_WINDOWS
bool CopyNTFSPermissions(const wchar_t* src, const wchar_t* dst) {
    PSECURITY_DESCRIPTOR pSD = nullptr;
    PSID pOwner = nullptr, pGroup = nullptr;
    PACL pDacl = nullptr, pSacl = nullptr;

    DWORD res = ::GetNamedSecurityInfoW(src, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION, &pOwner, &pGroup, &pDacl, &pSacl, &pSD);
    if (res != ERROR_SUCCESS) return false;

    res = ::SetNamedSecurityInfoW(const_cast<wchar_t*>(dst), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION, pOwner, pGroup, pDacl, pSacl);
    if (pSD) ::LocalFree(pSD);
    return (res == ERROR_SUCCESS);
}

void CopyNTFSAlternateStreams(const std::wstring& src, const std::wstring& dst) {
    WIN32_FIND_STREAM_DATA streamData;
    HANDLE hFind = ::FindFirstStreamW(src.c_str(), FindStreamInfoStandard, &streamData, 0);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (std::wstring(streamData.cStreamName) == L"::$DATA") continue;
            std::wstring srcStream = src + streamData.cStreamName;
            std::wstring dstStream = dst + streamData.cStreamName;
            ::CopyFileExW(srcStream.c_str(), dstStream.c_str(), nullptr, nullptr, nullptr, 0);
        } while (::FindNextStreamW(hFind, &streamData));
        ::FindClose(hFind);
    }
}
#endif

#if PLATFORM_LINUX
bool TryLinuxReflinkCopy(const fs::path& src, const fs::path& dst) {
    int fd_in = open(src.c_str(), O_RDONLY);
    int fd_out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_in < 0 || fd_out < 0) {
        if (fd_in >= 0) close(fd_in);
        if (fd_out >= 0) close(fd_out);
        return false;
    }
    bool success = (ioctl(fd_out, FICLONE, fd_in) == 0);
    close(fd_in);
    close(fd_out);
    return success;
}

void ExecuteLinuxOptimizedCopy(OptimizedCopyContext* ctx) {
    if (ctx->action == ConflictResult::Skip) return;

    nice(19);
    int ioprio_val = IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 7);
    syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, ioprio_val);

    uint64_t startOffset = 0;
    if (ctx->action == ConflictResult::Append && fs::exists(ctx->dstPath)) {
        startOffset = fs::file_size(ctx->dstPath);
    }

    int fd_in = open(ctx->srcPath.c_str(), O_RDONLY | O_DIRECT);
    int fd_out = open(ctx->dstPath.c_str(), (ctx->action == ConflictResult::Append) ? (O_WRONLY | O_DIRECT) : (O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT), 0644);

    if (fd_in < 0 || fd_out < 0) {
        if (fd_in >= 0) close(fd_in);
        if (fd_out >= 0) close(fd_out);
        return;
    }

    struct stat st;
    fstat(fd_in, &st);
    ctx->totalBytes = st.st_size;

    loff_t offset_in = startOffset;
    loff_t offset_out = startOffset;
    size_t remaining = ctx->totalBytes - startOffset;
    ctx->copiedBytes = offset_in;

    while (remaining > 0 && !ctx->isCancelled) {
        ssize_t ret = copy_file_range(fd_in, &offset_in, fd_out, &offset_out, remaining, 0);

        if (ret < 0) {
            const size_t BUF_SIZE = IsTargetDriveSSD(ctx->dstPath) ? (1024 * 1024 * 4) : (1024 * 1024 * 64);
            void* buf = aligned_alloc_buffer(64, BUF_SIZE);
            
            fcntl(fd_in, F_SETFL, fcntl(fd_in, F_GETFL) & ~O_DIRECT);
            fcntl(fd_out, fcntl(fd_out, F_GETFL) & ~O_DIRECT);

            while (remaining > 0 && !ctx->isCancelled) {
                ssize_t r_bytes = pread(fd_in, buf, BUF_SIZE, offset_in);
                if (r_bytes <= 0) break;
                
                ssize_t w_bytes = pwrite(fd_out, buf, r_bytes, offset_out);
                if (w_bytes <= 0) break;

                offset_in += w_bytes;
                offset_out += w_bytes;
                remaining -= w_bytes;
                ctx->copiedBytes = offset_in;
            }
            aligned_free_buffer(buf);
            break;
        }

        if (ret == 0) break;

        remaining -= ret;
        ctx->copiedBytes = offset_in;
    }

    close(fd_in);
    close(fd_out);
    ctx->isFinished = true;
}
#endif

// ============================================================================
// 6. ОПТИМИЗИРОВАННЫЙ ВВОД-ВЫВОД WINDOWS (СЖАТИЕ, ШИФРОВАНИЕ, ДОЗАПИСЬ)
// ============================================================================
#if PLATFORM_WINDOWS
void ExecuteWindowsOptimizedCopy(OptimizedCopyContext* ctx) {
    if (ctx->action == ConflictResult::Skip) return;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    IO_PRIORITY_HINT ioPriority = IoPriorityHintBackground;
    SetThreadInformation(GetCurrentThread(), (THREAD_INFORMATION_CLASS)ThreadIoPriority, &ioPriority, sizeof(ioPriority));

    uint64_t startOffset = 0;
    if (ctx->action == ConflictResult::Append && fs::exists(ctx->dstPath)) {
        startOffset = fs::file_size(ctx->dstPath);
    }

    DWORD srcAttributes = ::GetFileAttributesW(ctx->srcPath.c_str());
    bool isSrcEncrypted = (srcAttributes != INVALID_FILE_ATTRIBUTES) && (srcAttributes & FILE_ATTRIBUTE_ENCRYPTED);

    CREATEFILE2_EXTENDED_PARAMETERS openParams = { sizeof(CREATEFILE2_EXTENDED_PARAMETERS) };
    openParams.dwFileFlags = FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_NORMAL;
    openParams.dwSecurityQosFlags = SECURITY_ANONYMOUS;

    HANDLE hSrc = CreateFile2(ctx->srcPath.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &openParams);
    HANDLE hDst = CreateFile2(
        ctx->dstPath.c_str(), 
        GENERIC_WRITE, 
        FILE_SHARE_READ, 
        (ctx->action == ConflictResult::Append) ? OPEN_EXISTING : CREATE_ALWAYS, 
        &openParams
    );

    if (hSrc == INVALID_HANDLE_VALUE || hDst == INVALID_HANDLE_VALUE) {
        if (hSrc != INVALID_HANDLE_VALUE) CloseHandle(hSrc);
        if (hDst != INVALID_HANDLE_VALUE) CloseHandle(hDst);
        return;
    }

    if (ctx->action != ConflictResult::Append && !isSrcEncrypted) {
        USHORT compressionFormat = COMPRESSION_FORMAT_DEFAULT;
        DWORD bytesReturned = 0;
        ::DeviceIoControl(hDst, FSCTL_SET_COMPRESSION, &compressionFormat, sizeof(compressionFormat), nullptr, 0, &bytesReturned, nullptr);
    }

    LARGE_INTEGER fileSize;
    GetFileSizeEx(hSrc, &fileSize);
    ctx->totalBytes = fileSize.QuadPart;

    const size_t BUFFER_SIZE = IsTargetDriveSSD(ctx->dstPath) ? (1024 * 1024 * 4) : (1024 * 1024 * 64);
    void* rawBuffer = aligned_alloc_buffer(64, BUFFER_SIZE);

    OVERLAPPED olRead = {0};
    OVERLAPPED olWrite = {0};
    uint64_t currentOffset = startOffset;
    ctx->copiedBytes = currentOffset;

        while (currentOffset < ctx->totalBytes && !ctx->isCancelled) {
            uint64_t bytesToRead = ctx->totalBytes - currentOffset;
            if (bytesToRead > BUFFER_SIZE) bytesToRead = BUFFER_SIZE;

            size_t sectorAlignedSize = (bytesToRead + 4095) & ~4095;

            olRead.Offset = static_cast<DWORD>(currentOffset);
            olRead.OffsetHigh = static_cast<DWORD>(currentOffset >> 32);

            ReadFile(hSrc, rawBuffer, static_cast<DWORD>(sectorAlignedSize), NULL, &olRead);
            DWORD bytesRead = 0;
            GetOverlappedResult(hSrc, &olRead, &bytesRead, TRUE);

            if (bytesRead == 0) break;

            olWrite.Offset = static_cast<DWORD>(currentOffset);
            olWrite.OffsetHigh = static_cast<DWORD>(currentOffset >> 32);

            WriteFile(hDst, rawBuffer, bytesRead, NULL, &olWrite);
            DWORD bytesWritten = 0;
            GetOverlappedResult(hDst, &olWrite, &bytesWritten, TRUE);

            currentOffset += bytesRead;
            ctx->copiedBytes = currentOffset;
            UpdateFarTaskbarProgress(currentOffset, ctx->totalBytes);
        }

        aligned_free_buffer(rawBuffer);
        CloseHandle(hSrc);

        SetFilePointerEx(hDst, fileSize, NULL, FILE_BEGIN);
        SetEndOfFile(hDst);
        CloseHandle(hDst);
        ctx->isFinished = true;

        if (ctx->isFinished && !ctx->isCancelled) {
            if (isSrcEncrypted) ::EncryptFileW(ctx->dstPath.c_str());
            CopyNTFSPermissions(ctx->srcPath.c_str(), ctx->dstPath.c_str());
            CopyNTFSAlternateStreams(ctx->srcPath.wstring(), ctx->dstPath.wstring());
        }
    }
    #endif

    // ============================================================================
    // 7. ПОТОКОБЕЗОПАСНЫЙ FIFO-МЕНЕДЖЕР ОЧЕРЕДИ ЗАДАЧ
    // ============================================================================
    class FastCopyQueueManager {
    private:
        std::queue<CopyTask> tasks;
        std::mutex queueMtx;
        std::condition_variable cv;
        std::thread workerThread;
        std::atomic<bool> isRunning{true};

        void WorkerLoop() {
            while (isRunning) {
                CopyTask task;
                bool queueIsEmptyNow = false;

                {
                    std::unique_lock<std::mutex> lock(queueMtx);
                    cv.wait(lock, [this] { return (!tasks.empty() && !g_isQueuePaused) || !isRunning; });
                    if (!isRunning && tasks.empty()) break;
                    task = std::move(tasks.front());
                    tasks.pop();
                }

                #if PLATFORM_LINUX
                    bool handled = false;
                    if (fs::file_size(task.source) < 65536) { 
                        if (TryLinuxReflinkCopy(task.source, task.destination)) {
                            handled = true;
                        }
                    }
                    if (!handled) {
                        OptimizedCopyContext ctx;
                        ctx.srcPath = task.source; ctx.dstPath = task.destination; ctx.action = task.conflictAction;
                        ExecuteLinuxOptimizedCopy(&ctx);
                    }
                #else
                    OptimizedCopyContext ctx;
                    ctx.srcPath = task.source; ctx.dstPath = task.destination; ctx.action = task.conflictAction;
                    ExecuteWindowsOptimizedCopy(&ctx);
                #endif

                if (task.deleteSourceAfterCopy) fs::remove_all(task.source);

                {
                    std::lock_guard<std::mutex> lock(queueMtx);
                    if (tasks.empty()) queueIsEmptyNow = true;
                }

                if (queueIsEmptyNow) {
                    UpdateFarTaskbarProgress(0, 0); 
                    if (!IsWindowsSilentModeEnabled()) {
                        ::Beep(880, 150); ::Sleep(50); ::Beep(1320, 250); 
                    }
                }
            }
        }

    public:
        FastCopyQueueManager() { workerThread = std::thread(&FastCopyQueueManager::WorkerLoop, this); }
        ~FastCopyQueueManager() { isRunning = false; cv.notify_one(); if (workerThread.joinable()) workerThread.join(); }

        void PushTask(const fs::path& src, const fs::path& dst, bool isMove, ConflictResult act) {
            size_t currentSize = 0;
            {
                std::lock_guard<std::mutex> lock(queueMtx);
                tasks.push({src, dst, isMove, act});
                currentSize = tasks.size();
                g_globalTotalTasks++;
                cv.notify_one();
            }
            ShowQueueStatusToast(currentSize, src.filename().wstring());
        }

        void TogglePause() {
            g_isQueuePaused = !g_isQueuePaused;
            {
                std::lock_guard<std::mutex> lock(queueMtx);
                cv.notify_one();
            }
            size_t currentSize = 0;
            {
                std::lock_guard<std::mutex> lock(queueMtx);
                currentSize = tasks.size();
            }
            ShowQueueStatusToast(currentSize, L"Изменение режима паузы...");
        }
    };
    static FastCopyQueueManager g_CopyQueue;

    // ============================================================================
    // 8. ПАРСИНГ ЗАДАЧ С ИНКРЕМЕНТАЛЬНОЙ XXHASH-СИНХРОНИЗАЦИЕЙ И ВЕТВЛЕНИЕМ ПОТОКОВ
    // ============================================================================
    void ParseAndQueueTasks(int commandCode, const wchar_t* payloadData) {
        if (!payloadData) return;
        if (commandCode == CMD_PAUSE_TOGGLE) { g_CopyQueue.TogglePause(); return; }

        std::wstring payload(payloadData);
        size_t questionMarkPos = payload.find(L'?');
        if (questionMarkPos == std::wstring::npos) return;

        std::wstring targetDir = payload.substr(0, questionMarkPos);
        std::wstring filesPart = payload.substr(questionMarkPos + 1);
        std::wstringstream ss(filesPart);
        std::wstring item;
        bool isMove = (commandCode == CMD_IMMEDIATE_MOVE || commandCode == CMD_QUEUE_MOVE);
        bool useQueue = (commandCode == CMD_QUEUE_COPY || commandCode == CMD_QUEUE_MOVE);

        while (std::getline(ss, item, L'|')) {
            if (item.empty()) continue;
            fs::path srcPath(item);
            fs::path finalSrc = srcPath.is_absolute() ? srcPath : fs::current_path() / srcPath;
            fs::path dstPath = fs::path(targetDir);
            
            if (fs::is_directory(dstPath) || dstPath.extension().empty()) {
                dstPath /= finalSrc.filename();
            }

            ConflictResult actionToTake = ConflictResult::Overwrite;

            if (fs::exists(dstPath) && fs::is_regular_file(dstPath)) {
                uint64_t srcSize = fs::file_size(finalSrc);
                uint64_t dstSize = fs::file_size(dstPath);

                if (srcSize == dstSize) {
                    if (CalculateFastHash(finalSrc) == CalculateFastHash(dstPath)) {
                        actionToTake = ConflictResult::Skip;
                    } else {
                        actionToTake = ShowFarConflictDialog(dstPath.filename().wstring());
                    }
                } else if (dstSize < srcSize) {
                    actionToTake = ConflictResult::Append;
                }
            }

            if (actionToTake != ConflictResult::Skip) {
                if (useQueue) {
                    g_CopyQueue.PushTask(finalSrc, dstPath, isMove, actionToTake);
                } else {
                    auto* ctx = new OptimizedCopyContext();
                    ctx->srcPath = finalSrc;
                    ctx->dstPath = dstPath;
                    ctx->action = actionToTake;
                    
                    std::thread([ctx, isMove]() {
                        #if PLATFORM_WINDOWS
                            ExecuteWindowsOptimizedCopy(ctx);
                        #else
                            ExecuteLinuxOptimizedCopy(ctx);
                        #endif
                        if (isMove && ctx->isFinished && !ctx->isCancelled) {
                            fs::remove_all(ctx->srcPath);
                        }
                        delete ctx;
                    }).detach();
                }
            }
        }
    }

    // ============================================================================
    // 9. АВТОПЕРЕЗАПИСЬ И ХОТ-ДЕПЛОЙ МАКРОСОВ С EVENT DIALOG HOOK
    // ============================================================================
    void ForceReloadFarMacros() {
        if (!g_FarInfoInitialized || !GInfo.Control) return;
        DWORD_PTR ctrlDotKey = KEY_CTRL | L'.'; 
        GInfo.Control(INVALID_HANDLE_VALUE, ACTL_KEY, 0, reinterpret_cast<void*>(ctrlDotKey));
    }

    void DeployMacroAndRefresh() {
    #if PLATFORM_WINDOWS
        wchar_t appDataPath[MAX_PATH];
        if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath) != S_OK) return;
        fs::path macroDir = fs::path(appDataPath) / L"Far Manager" / L"Profile" / L"Macros" / L"scripts";
    #else
        fs::path macroDir = fs::path(getenv("HOME")) / ".config" / "far2l" / "macros";
    #endif

        fs::create_directories(macroDir);
        
    #if PLATFORM_WINDOWS
        fs::path macroFile = macroDir / L"fastcopy_intercept.lua";
    #else
        fs::path macroFile = macroDir / "fastcopy_intercept.lua";
    #endif

        std::ofstream out(macroFile, std::ios::utf8);
        if (out.is_open()) {
            out << R"lua(
    local FASTCOPY_PLUGIN_GUID = "B5473F4A-E27F-4B9A-99A0-0123456789AB"
    local CMD_IMMEDIATE_COPY = 1
    local CMD_IMMEDIATE_MOVE = 2
    local CMD_PAUSE = 3
    local CMD_QUEUE_COPY = 4
    local CMD_QUEUE_MOVE = 5

    local function sendToPlugin(commandCode, targetDir, fileList)
        local payload = targetDir .. "?" .. fileList
        Plugin.Call(FASTCOPY_PLUGIN_GUID, commandCode, payload)
        panel.RedrawPanel(nil, 1)
    end

    local function getSelectedFiles()
        local pInfoAct = panel.GetPanelInfo(nil, 0)
        if not pInfoAct then return "" end
        local selectedCount = pInfoAct.SelectedItemsNumber == 0 and 1 or pInfoAct.SelectedItemsNumber
        local fileList = ""
        for i = 1, selectedCount do
            local item = panel.GetSelectedPanelItem(nil, 0, i)
            if item and item.FileName ~= ".." then
                fileList = fileList .. (fileList == "" and "" or "|") .. item.FileName
            end
        end
        return fileList
    end

    Macro { area="Shell"; key="Ctrl+Shift+F5"; description="FastCopy: В очередь"; action=function()
        local target = panel.GetPanelDirectory(nil, 1).Name
        local files = getSelectedFiles()
        if target ~= "" and files ~= "" then sendToPlugin(CMD_QUEUE_COPY, target, files) end
    end }

    Macro { area="Shell"; key="Ctrl+Shift+F6"; description="FastCopy: В очередь переноса"; action=function()
        local target = panel.GetPanelDirectory(nil, 1).Name
        local files = getSelectedFiles()
        if target ~= "" and files ~= "" then sendToPlugin(CMD_QUEUE_MOVE, target, files) end
    end }

    Event {
        group="DialogEvent";
        action=function(Event, Param)
            if Param.Msg == 260 then
                local dlgInfo = dialog.GetDialogInfo(Param.hDlg)
                if dlgInfo and (string.find(dlgInfo.Title, "Копирование") or string.find(dlgInfo.Title, "Перенос") or string.find(dlgInfo.Title, "Copy") or string.find(dlgInfo.Title, "Move")) then
                    local okButtonIdx = 0
                    for i = 1, dialog.GetDlgItemCount(Param.hDlg) do
                        local item = dialog.GetDlgItemShortInfo(Param.hDlg, i)
                        if item and item.Type == 7 and (string.find(item.Data, "OK") or string.find(item.Data, "Копировать") or string.find(item.Data, "Перенести")) then
                            okButtonIdx = i
                            break
                        end
                    end
                    if okButtonIdx > 0 then
                        dialog.AddDlgItem(Param.hDlg, {
                            Type = 7, X1 = 30, Y1 = 14, X2 = 50, Y2 = 14,
                            Focus = 0, Selected = 0, Flags = 0, Data = "[ FastCopy Очередь ]"
                        })
                    end
                end
            end
            if Param.Msg == 261 then
                local itemIdx = Param.Param1
                local itemInfo = dialog.GetDlgItemShortInfo(Param.hDlg, itemIdx)
                if itemInfo and itemInfo.Data == "[ FastCopy Очередь ]" then
                    local targetDir = dialog.GetItemData(Param.hDlg, 3)
                    local files = getSelectedFiles()
                    if targetDir and targetDir ~= "" and files ~= "" then
                        local cmd = string.find(dialog.GetDialogInfo(Param.hDlg).Title, "Перенос") and CMD_QUEUE_MOVE or CMD_QUEUE_COPY
                        dialog.Close(Param.hDlg, -1)
                        sendToPlugin(cmd, targetDir, files)
                    end
                    return true
                end
            end
        end
    end

    Macro { area="Shell"; key="Ctrl+Shift+P"; description="FastCopy: Пауза"; action=function()
        Plugin.Call(FASTCOPY_PLUGIN_GUID, CMD_PAUSE, "")
    end }
    )lua";
        out.close();
    }
#if PLATFORM_WINDOWS
    ForceReloadFarMacros();
#endif
}

// ============================================================================
// 10. АВТОМАТИЧЕСКАЯ УТИЛИЗАЦИЯ И ОБНОВЛЕНИЕ ЛОКАЛИЗАЦИЙ (ВЕРСИОНИРОВАНИЕ)
// ============================================================================
const std::string CURRENT_PLUGIN_VERSION = "1.2.0";

fs::path GetCurrentPluginDirectory(const wchar_t* winModuleName) {
#if PLATFORM_WINDOWS
    return fs::path(winModuleName).parent_path();
#else
    Dl_info info;
    if (dladdr((void*)GetCurrentPluginDirectory, &info) && info.dli_fname) {
        return fs::path(info.dli_fname).parent_path();
    }
    return fs::current_path();
#endif
}

void DeployAllLocalizationFiles(const wchar_t* winModuleName) {
    fs::path pluginDir = GetCurrentPluginDirectory(winModuleName);
    fs::path versionFile = pluginDir / "version.txt";
    bool forceUpdate = false;

    if (fs::exists(versionFile)) {
        std::ifstream vIn(versionFile);
        std::string savedVersion;
        if (vIn >> savedVersion && savedVersion != CURRENT_PLUGIN_VERSION) forceUpdate = true;
        vIn.close();
    } else {
        forceUpdate = true;
    }

    const char* utf8_bom = "\xEF\xBB\xBF";

#if PLATFORM_WINDOWS
    fs::path lngRu = pluginDir / L"FastCopyPluginRof.lng";
    fs::path lngEn = pluginDir / L"FastCopyPluginEng.lng";
    fs::path hlfRu = pluginDir / L"FastCopyPluginRof.hlf";
    fs::path hlfEn = pluginDir / L"FastCopyPluginEng.hlf";

    if (forceUpdate) {
        fs::remove(lngRu); fs::remove(lngEn); fs::remove(hlfRu); fs::remove(hlfEn);
        wchar_t appDataPath[MAX_PATH];
        if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath) == S_OK) {
            fs::remove(fs::path(appDataPath) / L"Far Manager" / L"Profile" / L"Macros" / L"scripts" / L"fastcopy_intercept.lua");
        }
    }

    if (!fs::exists(lngRu)) {
        std::ofstream out(lngRu, std::ios::binary);
        if (out.is_open()) { out.write(utf8_bom, 3); out << ".Language=Russian,Russian\n\n\"FastCopy\"\n\"Очередь FastCopy\"\n\"Фоновое копирование\"\n\"Конфликт файлов\"\n\"[ Перезаписать ]\"\n\"[ Пропустить ]\"\n\"[ Дописать ]\"\n"; }
    }
    if (!fs::exists(lngEn)) {
        std::ofstream out(lngEn, std::ios::binary);
        if (out.is_open()) { out.write(utf8_bom, 3); out << ".Language=English,English\n\n\"FastCopy\"\n\"FastCopy Queue\"\n\"Background Copy\"\n\"File Conflict\"\n\"[ Overwrite ]\"\n\"[ Skip ]\"\n\"[ Append ]\"\n"; }
    }
#else
    fs::path far2l_lng = pluginDir / "FastCopyPlugin.lng";
    fs::path far2l_hlf = pluginDir / "FastCopyPlugin.hlf";

    if (forceUpdate) { fs::remove(far2l_lng); fs::remove(far2l_hlf); }

    if (!fs::exists(far2l_lng)) {
        std::ofstream out(far2l_lng, std::ios::binary);
        if (out.is_open()) {
            out << "#ru\n.Language=Russian,Russian\n\n\"FastCopy\"\n\"Очередь FastCopy\"\n\"Фоновое копирование\"\n\"Конфликт файлов\"\n\"[ Перезаписать ]\"\n\"[ Пропустить ]\"\n\"[ Дописать ]\"\n\n"
                   "#en\n.Language=English,English\n\n\"FastCopy\"\n\"FastCopy Queue\"\n\"Background Copy\"\n\"File Conflict\"\n\"[ Overwrite ]\"\n\"[ Skip ]\"\n\"[ Append ]\"\n";
        }
    }
#endif

    if (forceUpdate) {
        std::ofstream vOut(versionFile);
        if (vOut.is_open()) { vOut << CURRENT_PLUGIN_VERSION; vOut.close(); }
    }
}

// ============================================================================
// 11. КРОСС ПЛАТФОРМЕННЫЕ ТОЧКИ ВХОДА SDK FAR MANAGER / FAR2L
// ============================================================================
#if PLATFORM_WINDOWS
extern "C" void WINAPI SetStartupInfoW(const PluginStartupInfo* Info) {
    if (Info) { GInfo = *Info; g_FarInfoInitialized = true; DeployAllLocalizationFiles(Info->ModuleName); DeployMacroAndRefresh(); }
}
extern "C" HANDLE WINAPI OpenW(const struct OpenInfo* Info) {
    if (Info && Info->OpenFrom == 10 && Info->Data) {
        const wchar_t* luaPayload = reinterpret_cast<const wchar_t*>(Info->Data);
        if (luaPayload && (luaPayload[0] == L'1' || luaPayload[0] == L'2' || luaPayload[0] == L'3' || luaPayload[0] == L'4' || luaPayload[0] == L'5')) {
            int cmd = luaPayload[0] - L'0';
            if (luaPayload[1] == L':') ParseAndQueueTasks(cmd, luaPayload + 2);
        }
    }
    return nullptr;
}
#else
extern "C" void WINAPI SetStartupInfo(const PluginStartupInfo* Info) {
    if (Info) { GInfo = *Info; g_FarInfoInitialized = true; DeployAllLocalizationFiles(nullptr); DeployMacroAndRefresh(); }
}
extern "C" HANDLE WINAPI Open(const struct OpenInfo* Info) {
    return nullptr;
}
#endif
