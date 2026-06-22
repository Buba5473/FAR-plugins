#pragma once
#include <windows.h>
#include <winsock2.h>
#include <stdint.h>

// Константы для адаптивного контроля таймаутов
#define TFTP_BASE_TIMEOUT_MS   400
#define TFTP_MAX_TIMEOUT_MS    5000
#define TFTP_MAX_RETRIES       5

// Константы для Lock-Free Name Cache
#define CACHE_BUCKETS_COUNT    1024

// Типы операций для асинхронного Winsock2 IOCP контекста
enum class NetOpType {
    DataSend,
    DataReceive
};

// Расширенный транзакционный контекст OVERLAPPED для паттерна Zero-Copy
struct NetOverlapped {
    OVERLAPPED Overlapped;
    WSABUF     WsaBuf;
    NetOpType  OpType;
    uint64_t   TargetDiskOffset;
    uint32_t   ExpectedBytes;
    uint8_t*   DirectBuffer;
};

// Конфигурация удаленного подключения
struct NetworkConfig {
    BOOL     UseNetwork;
    wchar_t  ServerIp[64];
    uint16_t ServerPort;
    char     ImagePath[260];
};

// Базовая структура слота монтирования
#define MOUNT_TYPE_LOCAL 0
#define MOUNT_TYPE_NET   1

struct MountSlot {
    BOOL     IsActive;
    wchar_t  MountLetter;
    uint32_t SourceType;
    wchar_t  SourcePath[260];
};

// Структура ячейки Lock-Free кэша путей (FreeBSD Namecache Pattern)
struct CacheNode {
    uint64_t pathHash;
    uint64_t physicalBlockAddr;
    uint32_t inodeId;
    volatile LONG isOccupied;
};

// ===========================================================================
// КЛАСС: Потокобезопасный сетевой логгер общего назначения
// ===========================================================================
class NetLogger {
private:
    CRITICAL_SECTION cs;
    uint32_t         errorCount;
    DWORD            lastErrorTime;
    wchar_t          lastErrorMsg[256];

public:
    NetLogger();
    ~NetLogger();
    void LogNetworkError(const wchar_t* format, ...);
};

// ===========================================================================
// КЛАСС: Асинхронный TFTP/UDP сетевой движок на базе Winsock2 IOCP
// ===========================================================================
class AsyncNetworkMounter {
private:
    SOCKET           netSocket;
    HANDLE           hIOCP;
    HANDLE*          workerThreads;
    uint32_t         threadCount;
    sockaddr_in      serverAddr;

    static DWORD WINAPI NetworkWorkerProc(LPVOID lpParam);
    void SendAcknowledgeFrame(uint16_t blockNumber);

public:
    volatile BOOL    isLooping;
    NetworkConfig    NetCfg;

    AsyncNetworkMounter();
    ~AsyncNetworkMounter();

    bool StartNetworkEngine(const wchar_t* serverIp, uint16_t port);
    void StopNetworkEngine();
    bool QueueBlockRead(uint64_t diskOffset, uint32_t size, uint8_t* outFuseBuffer);
};

// ===========================================================================
// КЛАСС: Диспетчер управления слотами монтирования устройств
// ===========================================================================
class MountManager {
private:
    CRITICAL_SECTION syncLock;
    uint32_t         activeCount;
    MountSlot        slots[26]; // Слоты под буквы дисков A-Z

public:
    MountManager();
    ~MountManager();
    bool CreateMount(const MountSlot& newMount);
};

// Экспорт глобальных сущностей для линковки
extern NetLogger           G_NetLogger;
extern AsyncNetworkMounter G_AsyncNetEngine;

// Прототипы функций Lock-Free кэша путей
void InitializeVfsNameCache();
void VfsCacheInsert(const wchar_t* fullPath, uint32_t inodeId, uint64_t physBlockAddr);
bool VfsCacheLookup(const wchar_t* fullPath, uint32_t* outInodeId, uint64_t* outPhysBlockAddr);
