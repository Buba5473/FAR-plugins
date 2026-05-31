// =========================================================================
#include <windows.h>
#include <winsock2.h>
#include <mswsock.h>
#include <stdint.h>
#include "LinuxReaderCore.hpp"
#include "LinuxFsNetwork.hpp"

// Жесткое линкование системного Winsock2 на уровне компилятора
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

// Глобальные экземпляры подсистем сетевого стека
AsyncNetworkMounter G_AsyncNetEngine;
NetworkDiskReader   GlobalNetReader;
MountManager        G_MountManager;
NetLogger           G_NetLogger;
NetworkConfig       NetCfg{ FALSE, L"127.0.0.1", 69, "" };

// Внешняя декларация функции скрытого импорта Windows API
extern HANDLE DynamicCreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes);

// =========================================================================
// --- РЕАЛИЗАЦИЯ ASYNC NETWORK ENGINE (IOCP + WINSOCK2 UDP) ---
// =========================================================================

AsyncNetworkMounter::AsyncNetworkMounter() 
    : netSocket(INVALID_SOCKET), hIOCP(NULL), workerThreads(NULL), threadCount(0), isLooping(FALSE) {}

AsyncNetworkMounter::~AsyncNetworkMounter() {
    StopNetworkEngine();
}

DWORD WINAPI AsyncNetworkMounter::NetworkWorkerProc(LPVOID lpParam) {
    auto* engine = reinterpret_cast<AsyncNetworkMounter*>(lpParam);
    DWORD bytesTransferred = 0;
    ULONG_PTR completionKey = 0;
    LPOVERLAPPED lpOverlapped = nullptr;

    // Воркер-поток засыпает в ядре ОС Windows и пробуждается прерыванием от сетевой карты
    while (engine->isLooping) {
        BOOL res = GetQueuedCompletionStatus(
            engine->hIOCP,
            &bytesTransferred,
            &completionKey,
            &lpOverlapped,
            INFINITE
        );

        if (!res || !lpOverlapped) {
            continue; // Пропуск ошибочных пакетов или системных сигналов зачистки
        }

        auto* netOver = reinterpret_cast<NetOverlapped*>(lpOverlapped);

        if (netOver->OpType == NetOpType::DataReceive) {
            // Валидация пришедшего фрагмента UDP-пакета на предмет целостности метаданных ядра
            if (bytesTransferred >= 4 && netOver->ExpectedBytes == (bytesTransferred - 4)) {
                // ПАТТЕРН ZERO-COPY: Прямой перенос полезной нагрузки сетевого буфера TFTP в FUSE ОЗУ
                __movsb(
                    reinterpret_cast<BYTE*>(netOver->DirectBuffer),
                    reinterpret_cast<const BYTE*>(netOver->WsaBuf.buf + 4), // Пропуск 4 байт заголовка TFTP
                    netOver->ExpectedBytes
                );
            } else {
                // Фиксация сетевой аномалии фрагментации в асинхронный регистратор сбоев
                G_NetLogger.LogNetworkError(
                    L"[Net Error] Фрагмент: получено %d из %d байт в блоке 0x%I64X", 
                    bytesTransferred - 4, netOver->ExpectedBytes, netOver->TargetDiskOffset
                );
            }
            // Освобождаем транзакционный контекст в куче процесса без использования libc-функций
            HeapFree(GetProcessHeap(), 0, netOver);
        }
    }
    return 0;
}

bool AsyncNetworkMounter::StartNetworkEngine(const wchar_t* serverIp, uint16_t port) {
    StopNetworkEngine();

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;

    // Создаем OVERLAPPED UDP-сокет
    netSocket = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (netSocket == INVALID_SOCKET) { WSACleanup(); return false; }

    // Конфигурируем и инициализируем Порт Завершения Ввода-Вывода Windows (IOCP)
    hIOCP = CreateIoCompletionPort(reinterpret_cast<HANDLE>(netSocket), NULL, 0, 0);
    if (!hIOCP) { closesocket(netSocket); WSACleanup(); return false; }

    // Ручная сборка сетевого IPv4 адреса удаленного Linux-хранилища
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = _byteswap_ushort(port);
    
    // Инлайновый ручной разбор IP-строки в байтовую маску (Zero-CRT)
    uint32_t ip = 0; int part = 0;
    const wchar_t* p = serverIp;
    while (*p) {
        if (*p >= L'0' && *p <= L'1') { part = part * 10 + (*p - L'0'); }
        else if (*p == L'.') { ip = (ip << 8) | part; part = 0; }
        p++;
    }
    ip = (ip << 8) | part;
    serverAddr.sin_addr.s_addr = _byteswap_ulong(ip);

    isLooping = TRUE;
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    threadCount = sysInfo.dwNumberOfProcessors; // Масштабируем пул под все ядра Intel Core i5

    workerThreads = reinterpret_cast<HANDLE*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(HANDLE) * threadCount));
    for (uint32_t i = 0; i < threadCount; ++i) {
        workerThreads[i] = CreateThread(NULL, 0, NetworkWorkerProc, this, 0, NULL);
    }

    return true;
}

void AsyncNetworkMounter::StopNetworkEngine() {
    isLooping = FALSE;
    if (hIOCP) {
        // Посылаем терминирующие пакеты по числу воркеров в пуле для деактивации циклов ожидания
        for (uint32_t i = 0; i < threadCount; ++i) {
            PostQueuedCompletionStatus(hIOCP, 0, 0, NULL);
        }
        for (uint32_t i = 0; i < threadCount; ++i) {
            if (workerThreads[i]) {
                WaitForSingleObject(workerThreads[i], 1000);
                CloseHandle(workerThreads[i]);
            }
        }
        HeapFree(GetProcessHeap(), 0, workerThreads); workerThreads = NULL;
        CloseHandle(hIOCP); hIOCP = NULL;
    }
    if (netSocket != INVALID_SOCKET) { closesocket(netSocket); netSocket = INVALID_SOCKET; }
    WSACleanup();
}

bool AsyncNetworkMounter::QueueBlockRead(uint64_t diskOffset, uint32_t size, uint8_t* outFuseBuffer) {
    if (netSocket == INVALID_SOCKET || !isLooping) return false;

    // Выделяем расширенную структуру асинхронного контекста на куче Windows API
    auto* netOver = reinterpret_cast<NetOverlapped*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(NetOverlapped)));
    if (!netOver) return false;

    netOver->OpType = NetOpType::DataReceive;
    netOver->TargetDiskOffset = diskOffset;
    netOver->ExpectedBytes = size;
    netOver->DirectBuffer = outFuseBuffer;

    // Формируем плоский сырой буфер TFTP-пакета запроса блока во внутренней памяти кучи
    uint8_t requestPayload[256];
    auto* opCode = reinterpret_cast<uint16_t*>(requestPayload);
    *opCode = _byteswap_ushort(TFTP_OP_RRQ); // Opcode 1 = Read Request

    // Ручная сборка ASCII строки виртуального пути (например, "rootfs.img")
    char* pathPtr = reinterpret_cast<char*>(requestPayload + 2);
    const char* srcPath = NetCfg.ImagePath;
    while (*srcPath) { *pathPtr++ = *srcPath++; }
    *pathPtr++ = '\0';
    
    // Взводим режим передачи "octet" согласно спецификации RFC 1350
    const char* mode = "octet";
    while (*mode) { *pathPtr++ = *mode++; }
    *pathPtr++ = '\0';

    netOver->WsaBuf.len = size + 4; // Буфер ОЗУ Winsock выделяется с учетом 4 байт заголовка UDP
    netOver->WsaBuf.buf = reinterpret_cast<char*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 4));

    DWORD bytesSent = 0;
    // Отправляем асинхронную команду сокета. Winsock2 регистрирует её в IOCP и мгновенно возвращает управление.
    int res = WSASendTo(
        netSocket,
        &netOver->WsaBuf, 1,
        &bytesSent, 0,
        reinterpret_cast<const sockaddr*>(&serverAddr), sizeof(serverAddr),
        &(netOver->Overlapped),
        NULL
    );

    if (res == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        HeapFree(GetProcessHeap(), 0, netOver->WsaBuf.buf);
        HeapFree(GetProcessHeap(), 0, netOver);
        return false;
    }

    return true;
}

// =========================================================================
// --- РЕАЛИЗАЦИЯ NETWORK DISK READER (СИНХРОННЫЙ СЕТЕВОЙ ФОЛБЭК) ---
// =========================================================================

NetworkDiskReader::NetworkDiskReader() : sock(INVALID_SOCKET), isInitialized(FALSE) {}

NetworkDiskReader::~NetworkDiskReader() {
    CloseNetwork();
}

bool NetworkDiskReader::InitNetwork(const wchar_t* ip, uint16_t port) {
    CloseNetwork();
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) { WSACleanup(); return false; }

    // Настраиваем жесткие таймауты сокета на 3 секунды для предотвращения зависания панелей FAR
    int timeout = 3000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = _byteswap_ushort(port);
    
    uint32_t rawIp = 0; int part = 0;
    const wchar_t* p = ip;
    while (*p) {
        if (*p >= L'0' && *p <= L'9') { part = part * 10 + (*p - L'0'); }
        else if (*p == L'.') { rawIp = (rawIp << 8) | part; part = 0; }
        p++;
    }
    rawIp = (rawIp << 8) | part;
    serverAddr.sin_addr.s_addr = _byteswap_ulong(rawIp);

    isInitialized = TRUE;
    return true;
}

void NetworkDiskReader::CloseNetwork() {
    if (sock != INVALID_SOCKET) { closesocket(sock); sock = INVALID_SOCKET; }
    if (isInitialized) { WSACleanup(); isInitialized = FALSE; }
}

bool NetworkDiskReader::ReadRemoteBlock(uint64_t diskOffset, uint32_t size, uint8_t* outBuffer) {
    if (!isInitialized || sock == INVALID_SOCKET) return false;

    uint8_t packet[516]; // Фолбэк буфер под стандартный блок TFTP (512 байт полезных данных + 4 байта заголовок)
    auto* op = reinterpret_cast<uint16_t*>(packet);
    *op = _byteswap_ushort(TFTP_OP_RRQ);

    char* p = reinterpret_cast<char*>(packet + 2);
    const char* img = NetCfg.ImagePath;
    while (*img) { *p++ = *img++; }
    *p++ = '\0';
    const char* m = "octet";
    while (*m) { *p++ = *m++; }
    *p++ = '\0';

    int sendRes = sendto(sock, reinterpret_cast<const char*>(packet), static_cast<int>(p - reinterpret_cast<char*>(packet)), 0,
                         reinterpret_cast<const sockaddr*>(&serverAddr), sizeof(serverAddr));
    if (sendRes == SOCKET_ERROR) return false;

    sockaddr_in fromAddr;
    int fromLen = sizeof(fromAddr);
    int recvRes = recvfrom(sock, reinterpret_cast<char*>(packet), sizeof(packet), 0,
                           reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);

    if (recvRes == SOCKET_ERROR) {
        G_NetLogger.LogNetworkError(L"[Net Error] Превышено время ожидания сервера хранения.");
        return false;
    }

    if (recvRes >= 4 && _byteswap_ushort(*op) == TFTP_OP_DATA) {
        __movsb(reinterpret_cast<BYTE*>(outBuffer), reinterpret_cast<const BYTE*>(packet + 4), recvRes - 4);
        return true;
    }

    return false;
}

// =========================================================================
// --- РЕАЛИЗАЦИЯ MOUNT MANAGER (ДИСПЕТЧЕР СЛОТОВ ВИРТУАЛЬНЫХ ДИСКОВ) ---
// =========================================================================

MountManager::MountManager() : activeCount(0) {
    InitializeCriticalSection(&syncLock);
    __stosb(reinterpret_cast<BYTE*>(slots), 0, sizeof(slots));
}

MountManager::~MountManager() {
    DeleteCriticalSection(&syncLock);
}

bool MountManager::CreateMount(const MountSlot& newMount) {
    EnterCriticalSection(&syncLock);

    // Вычисляем индекс буквы диска в массиве Windows (A = index 0, Z = index 25)
    wchar_t ch = newMount.MountLetter;
    int index = 0;
    if (ch >= L'A' && ch <= L'Z') index = ch - L'A';
    else if (ch >= L'a' && ch <= L'z') index = ch - L'a';
    else { LeaveCriticalSection(&syncLock); return false; }

    if (slots[index].IsActive) {
        LeaveCriticalSection(&syncLock);
        return false; // Слот уже занят активным монтированием
    }

    // Посимвольный перенос параметров структуры интринсиком
    __movsb(reinterpret_cast<BYTE*>(&slots[index]), reinterpret_cast<const BYTE*>(&newMount), sizeof(MountSlot));
    slots[index].IsActive = TRUE;

    // Если монтируется сетевой образ, активируем асинхронное ядро ввода-вывода Winsock2
    if (newMount.SourceType == MOUNT_TYPE_NET) {
        NetCfg.UseNetwork = TRUE;
        // Копируем IP-адрес
        const wchar_t* srcIp = newMount.SourcePath;
        wchar_t* dstIp = NetCfg.ServerIp;
        while (*srcIp && (srcIp - newMount.SourcePath) < 63) { *dstIp++ = *srcIp++; }
        *dstIp = L'\0';

        // Конвертируем Unicode URL в плоский ASCII-путь для сетевого кадра UDP
        char* dstImg = NetCfg.ImagePath;
        *dstImg++ = 'r'; *dstImg++ = 'o'; *dstImg++ = 't'; *dstImg++ = 'f';
        *dstImg++ = 's'; *dstImg++ = '.'; *dstImg++ = 'i'; *dstImg++ = 'm';
        *dstImg++ = 'g'; *dstImg = '\0';

        G_AsyncNetEngine.StartNetworkEngine(NetCfg.ServerIp, NetCfg.ServerPort);
    }

    activeCount++;
    LeaveCriticalSection(&syncLock);
    return true;
}

bool MountManager::DeleteMount(const wchar_t* letter) {
    EnterCriticalSection(&syncLock);

    wchar_t ch = letter;
    int index = 0;
    if (ch >= L'A' && ch <= L'Z') index = ch - L'A';
    else if (ch >= L'a' && ch <= L'z') index = ch - L'a';
    else { LeaveCriticalSection(&syncLock); return false; }

    if (!slots[index].IsActive) {
        LeaveCriticalSection(&syncLock);
        return false;
    }

    if (slots[index].SourceType == MOUNT_TYPE_NET) {
        G_AsyncNetEngine.StopNetworkEngine();
        NetCfg.UseNetwork = FALSE;
    }

    slots[index].IsActive = FALSE;
    activeCount--;

    LeaveCriticalSection(&syncLock);
    return true;
}

size_t MountManager::GetSlots(MountSlot** outSlots) {
    if (outSlots) *outSlots = slots;
    return 26; // Длина жестко завязана на размер алфавита Windows
}

bool MountManager::IsCurrentSourceNetwork() {
    return NetCfg.UseNetwork;
}
