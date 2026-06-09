#pragma once

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// 1. ОПРЕДЕЛЕНИЕ ПЛАТФОРМЫ И ПОДКЛЮЧЕНИЕ СИСТЕМНЫХ ЗАГОЛОВКОВ
// ============================================================================
#if defined(_WIN32) || defined(_WIN64) || defined(PLATFORM_WINDOWS)
    #ifndef PLATFORM_WINDOWS
        #define PLATFORM_WINDOWS 1
    #endif
    
    // Принудительно включаем поддержку современных API Windows 10+
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0A00 // Windows 10
    #endif
    
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    
    using FarChar = wchar_t;
    #define FAR_TEXT(str) L##str

    // Обеспечение совместимости со старыми версиями Windows SDK для функций I/O и приоритетов
    #ifndef THREAD_INFORMATION_CLASS
    typedef enum _THREAD_INFORMATION_CLASS {
        ThreadMemoryPriority,
        ThreadAbsoluteCpuPriority,
        ThreadDynamicCpuPriority,
        ThreadIoPriority,
        ThreadSchedulingPolicy,
        ThreadCountInformation,
        ThreadFlushInformation,
        ThreadSetTlsConstant,
        ThreadMaxInformation
    } THREAD_INFORMATION_CLASS;
    #endif

    #ifndef IoPriorityHintBackground
    typedef enum _IO_PRIORITY_HINT {
        IoPriorityHintVeryLow = 0,
        IoPriorityHintLow,
        IoPriorityHintNormal,
        IoPriorityHintBackground,
        IoPriorityHintHigh,
        MaxIoPriorityHintType
    } IO_PRIORITY_HINT;
    #endif

#else
    #ifndef PLATFORM_LINUX
        #define PLATFORM_LINUX 1
    #endif
    
    #include <unistd.h>
    #include <sys/syscall.h>
    
    using FarChar = char;
    #define FAR_TEXT(str) str

    // Системные константы Linux для ручного управления приоритетами ввода-вывода (ioprio_set)
    #define IOPRIO_CLASS_SHIFT 13
    #define IOPRIO_PRIO_VALUE(class, data) (((class) << IOPRIO_CLASS_SHIFT) | (data))
    
    enum {
        IOPRIO_CLASS_NONE,
        IOPRIO_CLASS_RT,     // Real-time
        IOPRIO_CLASS_BE,     // Best-effort
        IOPRIO_CLASS_IDLE,   // Низший фоновый приоритет диска (Background/Idle)
    };

    enum {
        IOPRIO_WHO_PROCESS = 1,
        IOPRIO_WHO_PGRP,
        IOPRIO_WHO_USER,
    };
#endif

// ============================================================================
// 2. УПРАВЛЕНИЕ ВЫРОВНЕННОЙ ПАМЯТЬЮ (ALIGNED MEMORY ALLOCATION)
// ============================================================================
/**
 * Выделяет блок памяти, выровненный по заданной границе.
 * Критически важно для No-Buffering (Windows) / O_DIRECT (Linux) и SIMD (AVX2/NEON).
 * 
 * @param alignment Граница выравнивания в байтах (например, 64 для кэш-линий и AVX-512)
 * @param size Размер выделяемой памяти в байтах
 * @return Указатель на выделенную выровненную память или nullptr при ошибке
 */
inline void* aligned_alloc_buffer(size_t alignment, size_t size) {
#if PLATFORM_WINDOWS
    // Использование нативной функции выравнивания Microsoft UCRT
    return _aligned_malloc(size, alignment);
#else
    // Использование стандартного POSIX механизма для Linux (GCC/Clang)
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    return ptr;
#endif
}

/**
 * Освобождает блок выровненной памяти, выделенный через aligned_alloc_buffer.
 * 
 * @param ptr Указатель на освобождаемый блок памяти
 */
inline void aligned_free_buffer(void* ptr) {
    if (!ptr) return;

#if PLATFORM_WINDOWS
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}
