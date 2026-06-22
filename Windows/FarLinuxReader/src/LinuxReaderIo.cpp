#include <windows.h>
#include <ioringapi.h>
#include <shlwapi.h>
#include "LinuxReaderCore.hpp"
#include "LinuxReaderIo.hpp"

// Назначение внешних ссылок на глобальные логгеры и контексты
extern AuditLogger G_AuditLogger;

class LinuxSplitDiskReader {
private:
    SplitImageContext splitCtx;
    HIORING hIoRing;
    CRITICAL_SECTION ringLock;
    uint32_t currentQueueDepth;
    const uint32_t maxQueueDepth = 1024;
    const uint32_t minQueueDepth = 64;

    // Внутренний метод автоматического поиска и связывания split-сегментов цепочки образов
    void DiscoverAndMapSplitSegments(const wchar_t* firstSegmentPath) {
        splitCtx.IsSplit = FALSE;
        splitCtx.SegmentCount = 0;
        splitCtx.TotalLogicalSize = 0;

        wchar_t currentPath[MAX_PATH];
        wcscpy_s(currentPath, firstSegmentPath);

        // Проверяем, оканчивается ли файл на .001 (классический split-дамп)
        wchar_t* ext = PathFindExtensionW(currentPath);
        if (_wcsicmp(ext, L".001") != 0) {
            // Если это обычный одиночный образ, инициализируем базовый одиночный сегмент
            HANDLE hFile = CreateFileW(firstSegmentPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER fSize;
                GetFileSizeEx(hFile, &fSize);
                splitCtx.Segments[0].hFile = hFile;
                splitCtx.Segments[0].StartOffset = 0;
                splitCtx.Segments[0].EndOffset = fSize.QuadPart;
                splitCtx.SegmentCount = 1;
                splitCtx.TotalLogicalSize = fSize.QuadPart;
            }
            return;
        }

        splitCtx.IsSplit = TRUE;
        uint32_t segIdx = 0;
        uint64_t currentLogicalOffset = 0;

        // Каскадный перебор и открытие расширений от .001 до .128 в каталоге улик
        while (segIdx < MAX_SPLIT_SEGMENTS) {
            wchar_t newExt[8];
            wsprintfW(newExt, L".%03d", segIdx + 1);
            
            // Подменяем расширение в пути
            PathRenameExtensionW(currentPath, newExt);

            HANDLE hFile = CreateFileW(currentPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
            if (hFile == INVALID_HANDLE_VALUE) {
                // Цепочка фрагментов прервалась (файлы набора закончились)
                break;
            }

            LARGE_INTEGER fileSize;
            GetFileSizeEx(hFile, &fileSize);

            splitCtx.Segments[segIdx].hFile = hFile;
            splitCtx.Segments[segIdx].StartOffset = currentLogicalOffset;
            splitCtx.Segments[segIdx].EndOffset = currentLogicalOffset + fileSize.QuadPart;

            currentLogicalOffset += fileSize.QuadPart;
            segIdx++;
        }

        splitCtx.SegmentCount = segIdx;
        splitCtx.TotalLogicalSize = currentLogicalOffset;

        G_AuditLogger.Log(LogSubsystem::IoRing, LogSeverity::Info, 
                          L"Каскадный детектор: Обнаружен split-образ. Сегментов: %d, Общий логический объем: %llu байт", 
                          splitCtx.SegmentCount, splitCtx.TotalLogicalSize);
    }

    // Динамический ресайзинг пула Windows 11 IoRing в зависимости от нагрузки на очередь чтения
    void AdjustIoRingPool(uint32_t neededSize) {
        uint32_t targetDepth = currentQueueDepth;
        
        // Адаптивное расширение или сужение размера очереди
        if (neededSize > currentQueueDepth && currentQueueDepth < maxQueueDepth) {
            targetDepth = (neededSize + 64 > maxQueueDepth) ? maxQueueDepth : neededSize + 64;
        } else if (neededSize < currentQueueDepth / 2 && currentQueueDepth > minQueueDepth) {
            targetDepth = (neededSize + 32 < minQueueDepth) ? minQueueDepth : neededSize + 32;
        }

        if (targetDepth == currentQueueDepth) return;

        G_AuditLogger.Log(LogSubsystem::IoRing, LogSeverity::Info, L"Ресайз пула IoRing: %d -> %d слотов", currentQueueDepth, targetDepth);
        
        if (hIoRing) {
            CloseIoRing(hIoRing);
            hIoRing = NULL;
        }

        IORING_CREATE_FLAGS flags = { IORING_CREATE_REQUIRED_FLAGS_NONE };
        HRESULT hr = CreateIoRing(IORING_VERSION_3, flags, targetDepth, targetDepth, &hIoRing);
        if (SUCCEEDED(hr)) {
            currentQueueDepth = targetDepth;
        } else {
            G_AuditLogger.Log(LogSubsystem::IoRing, LogSeverity::Critical, L"Критический сбой ресайза IoRing. Код: 0x%08X", hr);
        }
    }

public:
    LinuxSplitDiskReader(const wchar_t* initialPath) : hIoRing(NULL), currentQueueDepth(128) {
        InitializeCriticalSection(&ringLock);
        DiscoverAndMapSplitSegments(initialPath);

        IORING_CREATE_FLAGS flags = { IORING_CREATE_REQUIRED_FLAGS_NONE };
        HRESULT hr = CreateIoRing(IORING_VERSION_3, flags, currentQueueDepth, currentQueueDepth, &hIoRing);
        if (FAILED(hr)) {
            G_AuditLogger.Log(LogSubsystem::IoRing, LogSeverity::Critical, L"Не удалось инициализировать Win11 IoRing подсистему.");
        }
    }

    ~LinuxSplitDiskReader() {
        EnterCriticalSection(&ringLock);
        for (uint32_t i = 0; i < splitCtx.SegmentCount; ++i) {
            if (splitCtx.Segments[i].hFile != INVALID_HANDLE_VALUE) {
                CloseHandle(splitCtx.Segments[i].hFile);
            }
        }
        if (hIoRing) CloseIoRing(hIoRing);
        LeaveCriticalSection(&ringLock);
        DeleteCriticalSection(&ringLock);
    }

    // Выделение памяти, выровненной по границе сектора (No-Buffering IO требование)
    static uint8_t* AllocateAlignedBuffer(size_t size) {
        return reinterpret_cast<uint8_t*>(VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }

    static void FreeAlignedBuffer(uint8_t* buffer) {
        if (buffer) VirtualFree(buffer, 0, MEM_RELEASE);
    }

    // Высокоскоростное параллельное чтение, прозрачно распределяющее запросы по split-сегментам
    bool ReadLogicalBlocks(LinuxIoRequest* requests, uint32_t count) {
        if (!hIoRing || splitCtx.SegmentCount == 0) return false;

        EnterCriticalSection(&ringLock);
        AdjustIoRingPool(count);

        for (uint32_t i = 0; i < count; ++i) {
            uint64_t targetLogicalOffset = requests[i].Offset;
            HANDLE targetFileHandle = INVALID_HANDLE_VALUE;
            uint64_t fileLocalOffset = 0;

            // Сквозной логический роутинг смещения: ищем, в какой конкретно файл .00X попадает сектор
            for (uint32_t s = 0; s < splitCtx.SegmentCount; ++s) {
                if (targetLogicalOffset >= splitCtx.Segments[s].StartOffset && targetLogicalOffset < splitCtx.Segments[s].EndOffset) {
                    targetFileHandle = splitCtx.Segments[s].hFile;
                    fileLocalOffset = targetLogicalOffset - splitCtx.Segments[s].StartOffset;
                    break;
                }
            }

            if (targetFileHandle == INVALID_HANDLE_VALUE) {
                requests[i].Result = E_FAIL;
                continue;
            }

            // Формируем асинхронную операцию IoRing для вычисленного дескриптора сегмента
            IORING_HANDLE_REF fileRef = IoRingHandleRefFromHandle(targetFileHandle);
            IORING_BUFFER_REF bufRef = IoRingBufferRefFromPointer(requests[i].Buffer);
            
            HRESULT hr = BuildIoRingReadFile(hIoRing, fileRef, bufRef, requests[i].Size, 
                                             fileLocalOffset, reinterpret_cast<UINT_PTR>(&requests[i]), 
                                             IOSQE_FLAGS_NONE);
            if (FAILED(hr)) {
                requests[i].Result = hr;
                G_AuditLogger.Log(LogSubsystem::IoRing, LogSeverity::Warning, L"Сбой добавления дискового запроса в очередь IoRing.");
            }
        }

        UINT32 submitted = 0;
        HRESULT hrSubmit = SubmitIoRing(hIoRing, 0, 0, &submitted);
        LeaveCriticalSection(&ringLock);

        if (FAILED(hrSubmit)) {
            G_AuditLogger.Log(LogSubsystem::IoRing, LogSeverity::Critical, L"Пакетный сабмит очереди IoRing завершился критическим сбоем.");
            return false;
        }

        // Выемка и фиксация результатов из Completion Queue
        for (uint32_t i = 0; i < submitted; ++i) {
            IORING_CQE cqe;
            if (SUCCEEDED(PopIoRingCompletionQueueEvent(hIoRing, &cqe))) {
                auto* req = reinterpret_cast<LinuxIoRequest*>(cqe.UserData);
                if (req) {
                    req->Result = cqe.ResultCode >= 0 ? S_OK : E_FAIL;
                }
            }
        }
        return true;
    }
};
