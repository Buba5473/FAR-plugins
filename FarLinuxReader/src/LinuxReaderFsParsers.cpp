#include <windows.h>
#include <string.h>
#include "LinuxReaderCore.hpp"

// Назначение внешних ссылок на глобальные контексты проекта
extern ThreadLocalArena G_StorageArena;
extern RemoteConfig G_ActiveMountConfig;

// Двухпроходной безопасный транслятор любых кодировок кириллицы с защитой памяти Арены
wchar_t* DecodeCyrillicLinuxName(const char* linuxRawName, size_t nameLen) {
    if (nameLen == 0 || !linuxRawName) return const_cast<wchar_t*>(L"");

    // Выбор кодовой страницы на основе параметров сессии из Мастер-Окна (F11)
    UINT codePage = static_cast<UINT>(G_ActiveMountConfig.TargetEncoding);
    if (codePage == 0) {
        // Если в форме выбрана [Автодетекция], вызываем частотный статистический анализатор
        extern CyrillicEncoding DetectBufferCyrillicEncoding(const uint8_t* buffer, size_t size);
        codePage = static_cast<UINT>(DetectBufferCyrillicEncoding(reinterpret_cast<const uint8_t*>(linuxRawName), nameLen));
    }

    // Шаг 1: Тестовый проход для расчета точного размера буфера UTF-16 с учетом суррогатных пар [0.30, 0.34]
    int expectedChars = MultiByteToWideChar(codePage, 0, linuxRawName, static_cast<int>(nameLen), nullptr, 0);
    if (expectedChars <= 0) {
        // Жесткий фолбэк на CP866 (кодировку консоли FAR), если системный вызов вернул сбой
        expectedChars = MultiByteToWideChar(866, 0, linuxRawName, static_cast<int>(nameLen), nullptr, 0);
        codePage = 866;
    }

    // Шаг 2: Безопасная аллокация в изолированном пул-пространстве Арены [0.27, 0.30]
    auto* wideNameBuffer = reinterpret_cast<wchar_t*>(G_StorageArena.Alloc((expectedChars + 1) * sizeof(wchar_t)));
    if (!wideNameBuffer) return const_cast<wchar_t*>(L"[Ошибка: Нехватка памяти Арены]");

    // Шаг 3: Финальная атомарная конвертация в UTF-16 строку
    MultiByteToWideChar(codePage, 0, linuxRawName, static_cast<int>(nameLen), wideNameBuffer, expectedChars);
    wideNameBuffer[expectedChars] = L'\0';

    return wideNameBuffer;
}

// Многоступенчатый каскадный детектор файловых систем и вложенных forensic-контейнеров
bool AnalyzeNestedLinuxVolume(const uint8_t* sectorBuffer, size_t size, wchar_t* outReport) {
    if (!sectorBuffer || size < 512) return false;

    // Слой 1: Автодетекция контейнеров SquashFS (LiveCD / Snap-пакеты / Docker-слои)
    const auto* squash = reinterpret_cast<const squashfs_super_block*>(sectorBuffer);
    if (squash->s_magic == SQUASHFS_MAGIC) {
        uint64_t exactSize = squash->bytes_used; // Вычисление точного физического размера контейнера
        wsprintfW(outReport, L"Контейнер: SquashFS v%d.%d [Точный размер: %llu байт, Блок: %d КБ]", 
                  squash->s_major, squash->s_minor, exactSize, squash->block_size / 1024);
        return true;
    }

    // Слой 2: Форензик-детекция зашифрованных разделов LUKS (dm-crypt)
    const auto* luks = reinterpret_cast<const luks_header*>(sectorBuffer);
    // Проверка магического числа 'LUKS' на физическом уровне (Big Endian / Little Endian инвариант)
    if (sectorBuffer[0] == 'L' && sectorBuffer[1] == 'U' && sectorBuffer[2] == 'K' && sectorBuffer[3] == 'S') {
        char cipherTmp[32] = {0};
        memcpy(cipherTmp, &luks->cipher_name, 31);
        
        // Преобразование версии с учетом сетевого порядка байт (Big Endian на диске)
        uint16_t luksVersion = _byteswap_ushort(luks->version);
        
        wsprintfW(outReport, L"Контейнер: Зашифрованный раздел LUKS v%d [Алгоритм: %S, Хэш: %S]", 
                  luksVersion, cipherTmp, &luks->hash_spec);
        return true;
    }

    // Слой 3: Парсинг вложенных структур Logical Volume Manager (LVM2)
    // Сигнатура LVM2 "LABELONE" по стандартам Linux всегда находится со смещением 24 байта от начала сектора
    if (memcmp(sectorBuffer + 24, "LABELONE", 8) == 0) {
        char lvmType[16] = {0};
        memcpy(lvmType, sectorBuffer + 24 + 16, 8); // Извлечение типа метаданных (например, 'LVM2 001')
        wsprintfW(outReport, L"Контейнер: Менеджер логических томов Linux LVM2 [%S]", lvmType);
        return true;
    }

    // Слой 4: Автодетекция вложенных баз данных SQLite / Aria
    if (memcmp(sectorBuffer, "SQLite format 3", 15) == 0) {
        wsprintfW(outReport, L"База данных: Встроенное хранилище SQLite v3");
        return true;
    }

    // Слой 5: Автодетекция текстовых логов системного демона Linux Syslog
    // Проверка характерного паттерна приоритетаfacility (например, "<13>", "<34>") в начале сектора
    if (sectorBuffer[0] == '<' && (sectorBuffer[1] >= '0' && sectorBuffer[1] <= '9') && (sectorBuffer[2] == '>' || (sectorBuffer[2] >= '0' && sectorBuffer[2] <= '9' && sectorBuffer[3] == '>'))) {
        wsprintfW(outReport, L"Поток логов: Стандартный текстовый журнал Linux Syslog Core");
        return true;
    }

    // Слой 6: Базовые нативные файловые системы Linux (Ext2 / Ext3 / Ext4)
    const auto* ext4 = reinterpret_cast<const ext4_super_block*>(sectorBuffer);
    if (ext4->s_magic == EXT4_SUPER_MAGIC) {
        wsprintfW(outReport, L"Файловая система: Linux Native Ext2/Ext3/Ext4 [Индексы Inodes: %d]", ext4->s_inodes_count);
        return true;
    }

    return false; // Каскадный фильтр сигнатур не определил известных метаструктур
}
