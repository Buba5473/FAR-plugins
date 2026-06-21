#pragma once
#include <windows.h>
#include <winternl.h>

// ===========================================================================
// 1. АЛГОРИТМЫ ХЭШИРОВАНИЯ СТРОК ВРЕМЕНИ КОМПИЛЯЦИИ (Compile-Time ROR13)
// ===========================================================================

constexpr uint32_t HashRor13(const char* str) {
    uint32_t hash = 0;
    while (*str) {
        hash = (hash >> 13) | (hash << (32 - 13));
        hash += static_cast<uint32_t>(*str >= 'A' && *str <= 'Z' ? *str + 32 : *str); // Регистронезависимый
        str++;
    }
    return hash;
}

constexpr uint32_t HashRor13W(const wchar_t* str) {
    uint32_t hash = 0;
    while (*str) {
        hash = (hash >> 13) | (hash << (32 - 13));
        hash += static_cast<uint32_t>(*str >= L'A' && *str <= L'Z' ? *str + 32 : *str); // Регистронезависимый
        str++;
    }
    return hash;
}

// ===========================================================================
// 2. ДИНАМИЧЕСКИЙ РАЗБОР PEB И ТАБЛИЦ ЭКСПОРТА (Bare-Metal Модульное разрешение)
// ===========================================================================

// Скрытый перехват базового адреса загруженной библиотеки DLL в памяти процесса
inline HMODULE GetModuleHandleByHash(uint32_t moduleHash) {
    // Архитектурная фиксация строго под платформу AMD64 (x86_64) Windows 10/11
    #if defined(_M_X64) || defined(__x86_64__)
    auto* peb = reinterpret_cast<PEB*>(__readgsqword(0x60));
    #else
    return NULL; 
    #endif

    auto* ldr = peb->Ldr;
    auto* head = &ldr->InMemoryOrderModuleList;
    auto* curr = head->Flink;

    // Итерируемся по двусвязному списку загруженных модулей операционной системы
    while (curr != head) {
        auto* tableEntry = CONTAINING_RECORD(curr, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        if (tableEntry->BaseDllName.Buffer != nullptr) {
            if (HashRor13W(tableEntry->BaseDllName.Buffer) == moduleHash) {
                return reinterpret_cast<HMODULE>(tableEntry->DllBase);
            }
        }
        curr = curr->Flink;
    }
    return NULL;
}

// Поиск физического адреса WinAPI-функции внутри таблицы экспорта (EAT) структуры PE-файла
inline void* GetProcAddressByHash(HMODULE hModule, uint32_t funcHash) {
    if (!hModule) return nullptr;

    auto* base = reinterpret_cast<BYTE*>(hModule);
    auto* dosHdr = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* ntHdrs = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dosHdr->e_lfanew);
    
    // Извлекаем директорию экспорта
    auto expDirAddr = ntHdrs->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (!expDirAddr) return nullptr;

    auto* expDir = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + expDirAddr);
    auto* functions = reinterpret_cast<uint32_t*>(base + expDir->AddressOfFunctions);
    auto* names = reinterpret_cast<uint32_t*>(base + expDir->AddressOfNames);
    auto* ordinals = reinterpret_cast<uint16_t*>(base + expDir->AddressOfNameOrdinals);

    // Прямой перебор имен функций и сверка по посчитанным хэшам
    for (uint32_t i = 0; i < expDir->NumberOfNames; ++i) {
        const char* funcName = reinterpret_cast<const char*>(base + names[i]);
        if (HashRor13(funcName) == funcHash) {
            uint16_t ord = ordinals[i];
            return reinterpret_cast<void*>(base + functions[ord]);
        }
    }
    return nullptr;
}
