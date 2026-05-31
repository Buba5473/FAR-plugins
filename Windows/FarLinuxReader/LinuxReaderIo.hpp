// =========================================================================
#pragma once
#include <windows.h>
#include <stdint.h>

#pragma pack(push, 1)

/**
 * Структура для пакетирования (батчинга) асинхронных запросов ввода-вывода.
 * Используется локальным Direct I/O движком дисков и сетевым асинхронным конвейером.
 */
struct LinuxIoRequest {
    uint64_t   DiskOffset;   // Физическое смещение на диске/образе в байтах (кратно размеру сектора)
    uint32_t   Size;         // Размер считываемого блока в байтах (выровнен по 4КБ)
    uint8_t*   Buffer;       // Прямой указатель на выровненный буфер памяти (VirtualAlloc)
    OVERLAPPED Overlapped;   // Системный контекст асинхронного трекинга ввода-вывода Windows
    DWORD      BytesRead;    // Реальное количество прочитанных аппаратных байт
    HRESULT    Result;       // Атомарный код статуса выполнения транзакции (S_OK или код ошибки)
};

#pragma pack(pop)

class LinuxDiskReader {
private:
    HANDLE   hDisk;          // Нативный системный дескриптор открытого PhysicalDrive или раздела
    uint32_t sectorSize;     // Физический размер сектора дисковой геометрии (512 или 4096 байт)

public:
    LinuxDiskReader();
    ~LinuxDiskReader();

    /**
     * Открытие локального физического устройства или логического тома Windows.
     * Автоматически взводит флаги FILE_FLAG_NO_BUFFERING и FILE_FLAG_OVERLAPPED
     * для аппаратного обхода системного кэша и активации Zero-Copy DMA переноса данных.
     */
    bool OpenDevice(const wchar_t* volumePath);
    
    // Безопасное закрытие дескрипторов ввода-вывода
    void CloseDevice();

    /**
     * Выделение системных страниц памяти, гарантированно выровненных по границе 4 КБ.
     * Критически важно для стабильной работы контроллера в режиме Direct Access (без буферизации).
     */
    static uint8_t* AllocateAlignedBuffer(size_t size);
    
    // Освобождение выровненных страниц памяти через VirtualFree
    static void FreeAlignedBuffer(uint8_t* buffer);

    /**
     * ГЛАВНЫЙ КОНВЕЙЕР ЧТЕНИЯ: Параллельный асинхронный ввод-вывод пачки блоков.
     * Реализует NCQ/NVMe батчинг на локальных дисках, а также прозрачный сквозной
     * перехват и перенаправление запросов на сетевой IOCP-стек, полностью изолируя
     * логику парсеров всех 11 файловых систем от специфики транспорта.
     */
    bool ReadBlocksAsync(LinuxIoRequest* requests, uint32_t count);

    uint32_t GetSectorSize() const { return sectorSize; }
    bool IsOpen() const { return hDisk != INVALID_HANDLE_VALUE; }
};

// Глобальный экземпляр дискового I/O движка, разделяемый между модулями плагина
extern LinuxDiskReader GlobalDiskReader;
