// =========================================================================
#include "LinuxReaderIo.hpp"
#include "LinuxFsNetwork.hpp" // Наш укрупненный сетевой стек (IOCP, Winsock2, Менеджер)

// Инициализация единого экземпляра дискового I/O движка
LinuxDiskReader GlobalDiskReader;

LinuxDiskReader::LinuxDiskReader() : hDisk(INVALID_HANDLE_VALUE), sectorSize(512) {}

LinuxDiskReader::~LinuxDiskReader() {
    CloseDevice();
}

bool LinuxDiskReader::OpenDevice(const wchar_t* volumePath) {
    CloseDevice();

    // Открываем локальный жесткий диск, NVMe SSD или раздел тома Windows.
    // FILE_FLAG_NO_BUFFERING — жесткое требование для обхода кэша Windows и активации DMA.
    // FILE_FLAG_OVERLAPPED    — включает аппаратную асинхронность на уровне контроллера диска.
    // Используем скрытый вызов CreateFileW из нашей обвязки маскировки импорта (Obfuscation)
    hDisk = DynamicCreateFileW(
        volumePath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED
    );

    if (hDisk == INVALID_HANDLE_VALUE) {
        return false;
    }

    // Динамически определяем физический размер сектора дисковой геометрии накопителя
    DISK_GEOMETRY geometry{};
    DWORD bytesReturned = 0;
    BOOL res = DeviceIoControl(
        hDisk,
        IOCTL_DISK_GET_DRIVE_GEOMETRY,
        NULL, 0,
        &geometry, sizeof(geometry),
        &bytesReturned,
        NULL
    );

    if (res && geometry.BytesPerSector > 0) {
        sectorSize = geometry.BytesPerSector;
    } else {
        sectorSize = 512; // Безопасный фолбэк для совместимости со старыми накопителями
    }

    return true;
}

void LinuxDiskReader::CloseDevice() {
    if (hDisk != INVALID_HANDLE_VALUE) {
        CloseHandle(hDisk);
        hDisk = INVALID_HANDLE_VALUE;
    }
}

uint8_t* LinuxDiskReader::AllocateAlignedBuffer(size_t size) {
    // VirtualAlloc всегда выделяет память, выравненную по границе 4 КБ системных страниц.
    // Это жесткое аппаратное требование контроллера накопителя для работы в режиме Direct Access.
    return reinterpret_cast<uint8_t*>(VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
}

void LinuxDiskReader::FreeAlignedBuffer(uint8_t* buffer) {
    if (buffer) {
        VirtualFree(buffer, 0, MEM_RELEASE);
    }
}

bool LinuxDiskReader::ReadBlocksAsync(LinuxIoRequest* requests, uint32_t count) {
    if (count == 0 || !requests) return false;

    // ПЕРЕХВАТ НА ВЫСОКОПРОИЗВОДИТЕЛЬНОЕ АСИНХРОННОЕ СЕТЕВОЕ ЯДРО (IOCP)
    // Если в глобальном пуловом менеджере подключений текущий источник помечен как сетевой
    if (G_MountManager.IsCurrentSourceNetwork()) {
        for (uint32_t i = 0; i < count; ++i) {
            // Отправляем запросы пакетами в асинхронное скользящее окно Winsock2, поток мгновенно идет дальше
            if (G_AsyncNetEngine.QueueBlockRead(requests[i].DiskOffset, requests[i].Size, requests[i].Buffer)) {
                requests[i].BytesRead = requests[i].Size;
                requests[i].Result = S_OK;
            } else {
                requests[i].Result = HRESULT_FROM_WIN32(ERROR_NET_WRITE_FAULT);
            }
        }
        return true;
    }

    // --- ЛОКАЛЬНЫЙ АСИНХРОННЫЙ WIN32 OVERLAPPED I/O (Работа со встроенными дисками) ---
    if (hDisk == INVALID_HANDLE_VALUE) return false;

    // Выделяем память под пул хэндлов событий Windows в куче процесса без вызова функций libc
    HANDLE* events = reinterpret_cast<HANDLE*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(HANDLE) * count));
    if (!events) return false;

    uint32_t submittedCount = 0;

    // ЭТАП 1: Массовая отправка асинхронных команд чтения в аппаратную очередь накопителя (NCQ/NVMe батчинг)
    // Благодаря оптимизации Intel -funroll-loops, данный цикл раскрывается компилятором для утилизации LSD
    for (uint32_t i = 0; i < count; ++i) {
        // Быстрое обнуление структуры без вызова memset
        __stosb(reinterpret_cast<BYTE*>(&requests[i].Overlapped), 0, sizeof(OVERLAPPED));
        
        // Создаем событие ручного сброса для атомарного отслеживания готовности конкретного сектора
        events[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
        requests[i].Overlapped.hEvent = events[i];
        
        // Разделяем 64-битное смещение Linux-разметки на младшую и старшую части для структуры Windows 10/11
        requests[i].Overlapped.Offset = static_cast<DWORD>(requests[i].DiskOffset & 0xFFFFFFFF);
        requests[i].Overlapped.OffsetHigh = static_cast<DWORD>((requests[i].DiskOffset >> 32) & 0xFFFFFFFF);
        requests[i].Result = S_OK;

        // Отправляем команду. ReadFile возвращает управление немедленно, не блокируя поток плагина.
        // Данные пойдут по шине через DMA прямо в RAM плагина.
        BOOL readRes = ReadFile(hDisk, requests[i].Buffer, requests[i].Size, NULL, &requests[i].Overlapped);
        
        if (!readRes) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                // Прямая ошибка отправки (например, передано невыровненное по границе секторов смещение диска)
                requests[i].Result = HRESULT_FROM_WIN32(err);
                CloseHandle(events[i]);
                events[i] = NULL;
                continue;
            }
        }
        submittedCount++;
    }

    // ЭТАП 2: Высокоэффективное ожидание завершения пачки операций ввода-вывода (Пул ожидания ядра)
    uint32_t completedCount = 0;
    while (completedCount < submittedCount) {
        // Поток плагина FAR засыпает в планировщике операционной системы и пробуждается прерыванием от контроллера
        DWORD waitRes = WaitForMultipleObjects(count, events, FALSE, INFINITE);
        
        if (waitRes >= WAIT_OBJECT_0 && waitRes < (WAIT_OBJECT_0 + count)) {
            uint32_t idx = waitRes - WAIT_OBJECT_0;
            
            if (events[idx] != NULL) {
                // Считываем финальный статус завершения и количество реально переданных байт для выполненной операции
                BOOL opRes = GetOverlappedResult(hDisk, &requests[idx].Overlapped, &requests[idx].BytesRead, FALSE);
                if (!opRes) {
                    requests[idx].Result = HRESULT_FROM_WIN32(GetLastError());
                } else {
                    requests[idx].Result = S_OK;
                }
                
                // Закрываем дескриптор события и убираем его из пула ожидания ядра Windows
                CloseHandle(events[idx]);
                events[idx] = NULL;
                completedCount++;
            }
        } else {
            break; // Критическая непредвиденная ошибка диспетчера потоков Windows
        }
    }

    // Экстренная превентивная зачистка оставшихся дескрипторов событий в случае аварийного сбоя внутри цикла ожидания
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i]) CloseHandle(events[i]);
    }
    
    HeapFree(GetProcessHeap(), 0, events);
    return true;
}
