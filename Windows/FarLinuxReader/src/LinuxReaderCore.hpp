#pragma once
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>

// ==============================================================================
#define write(fd, buf, count)  (-1)
#define pwrite(fd, buf, count) (-1)
#define unlink(path)           (-1)
#define mkdir(path, mode)      (-1)
#define rmdir(path)            (-1)

// ==============================================================================
#define EXT4_SUPER_MAGIC     0xEF53
#define XFS_DIR3_BLOCK_MAGIC 0x58444233 // 'XDB3'
#define ZFS_ZAP_MAGIC_LE     0x2f52415a // 'ZAR/'
#define EROFS_SUPER_MAGIC_V1 0xE0F5E1E2
#define BCACHEFS_MAGIC       0x4a10314a
#define SQUASHFS_MAGIC       0x73717368 // 'hsqs'
#define LUKS_MAGIC           0x534b554c // 'LUKS' в байтовом представлении памяти
#define LVM2_MAGIC           0x324d564c // 'LVM2'

#define MAX_AUDIT_LOGS       4096
#define MAX_SPLIT_SEGMENTS   128

// Глобальный GUID плагина для нативной регистрации в структурах FAR Manager 3 SDK [0.26]
static const GUID MainGuid = { 0x7a3e921b, 0x4a2d, 0x4e10, { 0x8b, 0x1c, 0x3d, 0x4f, 0x5e, 0x6a, 0x7b, 0x8c } };

// ==============================================================================
enum class LogSubsystem : uint32_t {
    Network = 1,
    IoRing  = 2,
    GPU     = 3,
    Parser  = 4
};

enum class LogSeverity : uint32_t {
    Info     = 1,
    Warning  = 2,
    Critical = 3
};

enum class CyrillicEncoding : uint32_t {
    UTF8      = CP_UTF8,
    CP1251    = 1251,     // Windows Cyrillic
    CP866     = 866,      // DOS Cyrillic (Родная кодировка панелей FAR)
    KOI8_R    = 20866,    // Unix Cyrillic
    ISO8859_5 = 28555     // ISO Cyrillic
};

enum class NetProtocol : uint32_t {
    FTP,
    FTPS,
    SFTP,
    NFS
};

// ==============================================================================
struct RemoteConfig {
    NetProtocol Protocol;
    wchar_t Hostname[256];
    uint16_t Port;
    wchar_t Username[128];
    wchar_t Password[128];
    char RemoteImagePath[512];
    CyrillicEncoding TargetEncoding;
    wchar_t CustomMountLetter; // Назначаемая буква диска для NFS-монтирования (например, L':')
};

struct AuditEvent {
    uint64_t Timestamp;
    LogSubsystem Subsystem;
    LogSeverity Severity;
    wchar_t Message[512];
    float GpuTemperature;
    float GpuThrottling;
};

struct GpuEngine {
    BOOL IsInitialized;
    ID3D11Device* Device;
    ID3D11DeviceContext* Context;
    ID3D11ComputeShader* Fletcher4Shader;
    ID3D11ComputeShader* CarvingShader;
    ID3D11ComputeShader* BTreeShader;
    ID3D11ComputeShader* SquashFsShader;
    wchar_t AdapterName[128];
};

struct SplitSegment {
    HANDLE hFile;
    uint64_t StartOffset; // Сквозное логическое смещение начала сегмента
    uint64_t EndOffset;   // Сквозное логическое смещение конца сегмента
};

struct SplitImageContext {
    BOOL IsSplit;
    uint32_t SegmentCount;
    uint64_t TotalLogicalSize;
    SplitSegment Segments[MAX_SPLIT_SEGMENTS];
};

struct LinuxIoRequest {
    uint64_t Offset;      // Сквозное логическое смещение в виртуальном диске
    uint32_t Size;
    uint8_t* Buffer;
    HRESULT Result;
};

// Структуры для расчета контрольных сумм ZFS
struct zfs_zio_cksum {
    uint64_t zc_word[4];
};

// ==============================================================================
#pragma pack(push, 1)
struct ext4_super_block {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_feature_incompat;
    uint16_t s_inode_size;
};

struct luks_header {
    uint8_t  magic[4];     // 'L','U','K','S'
    uint16_t version;
    char     cipher_name[32];
    char     cipher_mode[32];
    char     hash_spec[32];
    uint32_t payload_offset;
    uint32_t key_bytes;
};

struct lvm2_disk_loc {
    uint64_t offset;
    uint64_t size;
};

struct lvm2_pv_header {
    uint8_t  id[32];
    uint64_t device_size;
    lvm2_disk_loc disk_areas;
    lvm2_disk_loc metadata_areas;
};

struct squashfs_super_block {
    uint32_t s_magic;
    uint32_t inodes;
    uint32_t mkfs_time;
    uint32_t block_size;
    uint32_t fragments;
    uint16_t compression;
    uint16_t block_log;
    uint16_t flags;
    uint16_t no_ids;
    uint16_t s_major;
    uint16_t s_minor;
    uint64_t root_inode;
    uint64_t bytes_used;
};
#pragma pack(pop)

// ==============================================================================
// Высокоскоростная Bare-Metal арена памяти исключающая фрагментацию кучи Windows
class ThreadLocalArena {
private:
    uint8_t* buffer;
    size_t capacity;
    size_t offset;
public:
    ThreadLocalArena() : capacity(16 * 1024 * 1024), offset(0) {
        buffer = reinterpret_cast<uint8_t*>(VirtualAlloc(NULL, capacity, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }
    ~ThreadLocalArena() { if (buffer) VirtualFree(buffer, 0, MEM_RELEASE); }
    void* Alloc(size_t size) {
        size_t alignedSize = (size + 7) & ~7;
        if (offset + alignedSize > capacity) return nullptr;
        void* ptr = buffer + offset;
        offset += alignedSize;
        return ptr;
    }
    void Reset() { offset = 0; }
};

// Высокоскоростной потокобезопасный кольцевой буфер логирования сопряженный с Event Viewer Windows
class AuditLogger {
private:
    CRITICAL_SECTION cs;
    AuditEvent logs[MAX_AUDIT_LOGS];
    uint32_t head;
    uint32_t count;
    HANDLE hEventSource;

    void EnsureEventLogGroupExists() {
        HKEY hKey;
        const wchar_t* logPath = L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\FarManagerPlugins\\UniversalLinuxReader";
        
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, logPath, 0, NULL, REG_OPTION_NON_VOLATILE, 
                            KEY_WRITE | KEY_QUERY_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            
            DWORD typesSupported = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_INFORMATION_TYPE;
            RegSetValueExW(hKey, L"TypesSupported", 0, REG_DWORD, reinterpret_cast<BYTE*>(&typesSupported), sizeof(DWORD));
            
            wchar_t modPath[MAX_PATH];
            GetModuleFileNameW(reinterpret_cast<HINSTANCE>(&__ImageBase), modPath, MAX_PATH);
            RegSetValueExW(hKey, L"EventMessageFile", 0, REG_EXPAND_SZ, reinterpret_cast<BYTE*>(modPath), 
                           static_cast<DWORD>((wcslen(modPath) + 1) * sizeof(wchar_t)));
            
            RegCloseKey(hKey);
        }
    }

public:
    AuditLogger() : head(0), count(0), hEventSource(NULL) { 
        InitializeCriticalSection(&cs); 
        EnsureEventLogGroupExists();
        hEventSource = RegisterEventSourceW(NULL, L"UniversalLinuxReader");
    }

    ~AuditLogger() { 
        if (hEventSource) DeregisterEventSource(hEventSource);
        DeleteCriticalSection(&cs); 
    }

    void Log(LogSubsystem sub, LogSeverity sev, const wchar_t* fmt, ...) {
        EnterCriticalSection(&cs);
        uint32_t idx = (head + count) % MAX_AUDIT_LOGS;
        logs[idx].Timestamp = GetTickCount64();
        logs[idx].Subsystem = sub;
        logs[idx].Severity = sev;
        
        va_list args;
        va_start(args, fmt);
        wvsprintfW(logs[idx].Message, fmt, args);
        va_end(args);

        logs[idx].GpuTemperature = 42.0f;
        logs[idx].GpuThrottling = 0.0f;

        if (count < MAX_AUDIT_LOGS) count++;
        else head = (head + 1) % MAX_AUDIT_LOGS;
        LeaveCriticalSection(&cs);

        if (hEventSource && (sev == LogSeverity::Warning || sev == LogSeverity::Critical)) {
            WORD winType = (sev == LogSeverity::Critical) ? EVENTLOG_ERROR_TYPE : EVENTLOG_WARNING_TYPE;
            DWORD eventID = static_cast<DWORD>(sub); 
            const wchar_t* strings = logs[idx].Message;
            ReportEventW(hEventSource, winType, 0, eventID, NULL, 1, 0, &strings, NULL);
        }
    }

    uint32_t GetCount() { return count; }
};

// ==============================================================================
extern "C" IMAGE_DOS_HEADER __ImageBase; // Ссылка на заголовок текущего PE-модуля для получения путей
extern ThreadLocalArena G_StorageArena;
extern GpuEngine G_GpuEngine;
extern AuditLogger G_AuditLogger;
