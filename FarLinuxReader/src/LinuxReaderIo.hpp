#pragma once
#include <windows.h>
#include <stdint.h>

// ===========================================================================
// 1. СТРУКТУРЫ МЕТАДАННЫХ ДИНАМИЧЕСКИХ ДИСКОВ WINDOWS LDM (libldm паттерн)
// ===========================================================================
#pragma pack(push, 1)

struct ldm_priv_head {
    char     magic[8];       // Сигнатура "PRIVHEAD"
    uint64_t logical_start;
    uint64_t logical_size;
    uint64_t config_start;   // Стартовый сектор базы данных LDM
    uint64_t config_size;    // Размер конфигурации
};

struct ldm_vmdb_hdr {
    char     magic[4];       // Сигнатура "VMDB"
    uint32_t sequence_num;
    uint32_t size;
    uint32_t status;
};

#pragma pack(pop)

struct LdmComponentStrip {
    uint64_t PhysicalStartOffset;
    uint64_t SizeSectors;
    uint32_t DiskId;
};

// Прототипы функций трансляции слоев LDM
bool LdmParseVolumeConfiguration(HANDLE hPhysicalDrive, uint64_t totalDiskSize);
uint64_t LdmTranslateLogicalToPhysicalOffset(uint64_t desiredLogicalOffset);

// ===========================================================================
// 2. ИНТЕРФЕЙС ПАКЕТНЫХ ЗАПРОСОВ ВВОДА-ВЫВОДА (Batch I/O Requests)
// ===========================================================================

struct LinuxIoRequest {
    uint64_t DiskOffset;     // Логическое смещение в байтах от начала тома
    uint32_t Length;         // Размер считываемого блока
    uint8_t* Buffer;         // Выровненный целевой буфер в ОЗУ
    HRESULT  Result;         // Статус завершения конкретной операции (S_OK / E_FAIL)
};

// ===========================================================================
// КЛАСС: Движок низкоуровневого асинхронного чтения физических накопителей
// ===========================================================================
class LinuxDiskReader {
private:
    HANDLE hDiskHandle;      // Дескриптор открытого устройства

public:
    LinuxDiskReader() : hDiskHandle(INVALID_HANDLE_VALUE) {}
    ~LinuxDiskReader() {
        if (hDiskHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(hDiskHandle);
        }
    }

    // Скрытое открытие блочного устройства (\.\PhysicalDriveX) без статического импорта
    HANDLE OpenDeviceSecure(const wchar_t* targetDevicePath);

    // Пакетный асинхронный ввод-вывод: Базовый механизм Overlapped I/O (Windows 10+)
    bool ReadBlocksAsync(LinuxIoRequest* requests, size_t count);

    // Пакетный асинхронный ввод-вывод: Высокоскоростной IoRing (Windows 11+)
    bool ReadBlocksBatchIoRing(LinuxIoRequest* requests, size_t count);

    // Статические bare-metal аллокаторы для NVMe-совместимого выравнивания страниц
    static uint8_t* AllocateAlignedBuffer(size_t size);
    static void FreeAlignedBuffer(uint8_t* buffer);
};
