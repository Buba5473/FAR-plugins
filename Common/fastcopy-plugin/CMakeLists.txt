cmake_minimum_required(VERSION 3.19)
project(FastCopyFarPlugin CXX)

# Обязательный стандарт C++17 для работы с std::filesystem и атомиками
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Отключаем внешние динамические зависимости MSVC runtime при сборке, 
# гарантируя чистую линковку с Universal CRT (UCRT)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Определение путей к заголовкам Far SDK (загруляются скриптом build_all.sh)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/sdk_headers)

# Подключение Windows-специфичного файла экспорта определений (.def)
# Он предотвращает name mangling функций OpenW/SetStartupInfoW в MinGW-w64 GCC
if(TARGET_OS_WINDOWS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugin.def")
    set(PLUGIN_DEF_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/plugin.def")
endif()

# Определение структуры выходной динамической библиотеки плагина
add_library(${PROJECT_NAME} SHARED fastcopy_core.cpp ${PLUGIN_DEF_SOURCE})

# ============================================================================
# ГЛУБОКАЯ ОПТИМИЗАЦИЯ И НАСТРОЙКА ФЛАГОВ КОМПИЛЯТОРА (GCC / CLANG)
# ============================================================================
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Базовые флаги экстремальной производительности для MSYS2 UCRT64 / Linux
    # -O3: Максимальный уровень оптимизации скорости кода компилятором
    # -flto: Link-Time Optimization (сквозная оптимизация между модулями на этапе линковки)
    # -ffast-math: Высокопроизводительные математические вычисления (для расчетов блоков)
    # -fomit-frame-pointer: Освобождает регистр стека для нужд общей оптимизации
    # -fPIC: Нативная генерация позиционно-независимого кода (критично для .so в Linux)
    target_compile_options(${PROJECT_NAME} PRIVATE -O3 -flto -ffast-math -fomit-frame-pointer -fPIC)
    
    # Флаги линковщика: -s удаляет таблицы символов и отладку, максимально облегчая бинарник
    target_link_options(${PROJECT_NAME} PRIVATE -flto -Wl,--strip-all)

    # 1. Специфические оптимизации под целевые Операционные Системы
    if(TARGET_OS_WINDOWS)
        # -municode: Переводит MinGW тулчейн на нативную работу с Unicode (wmain / wchar_t)
        target_compile_options(${PROJECT_NAME} PRIVATE -municode)
        target_link_options(${PROJECT_NAME} PRIVATE -municode)
        
        # Линкуем базовые системные библиотеки Windows и рантайм UCRT
        target_link_libraries(${PROJECT_NAME} PRIVATE kernel32 ucrt)
        target_compile_definitions(${PROJECT_NAME} PRIVATE PLATFORM_WINDOWS=1)
    else()
        # Линкуем POSIX библиотеку динамической загрузки (необходима для dladdr в Linux)
        target_link_libraries(${PROJECT_NAME} PRIVATE dl)
        target_compile_definitions(${PROJECT_NAME} PRIVATE PLATFORM_LINUX=1)
    endif()

    # 2. Специфические оптимизации под Процессорные Микроархитектуры
    if(TARGET_ARCH_AMD64)
        # Включаем векторные расширения AVX2, аппаратное умножение FMA и инструкции BMI2
        # Это дает максимальный буст при расчете контрольных сумм xxHash на x86_64
        target_compile_options(${PROJECT_NAME} PRIVATE -msse4.2 -mavx2 -mfma -mbmi2)
        target_compile_definitions(${PROJECT_NAME} PRIVATE ARCH_AMD64_AVX2=1)
        
    elif(TARGET_ARCH_ODROID)
        # Экстремальная оптимизация под энергоэффективный чип Odroid HC4 (Cortex-A55)
        # march=armv8.2-a+crypto: Включает векторный блок NEON и аппаратные крипто-инструкции AES/SHA
        # mcpu=cortex-a55: Оптимизирует планировщик конвейера под in-order архитектуру ядер A55
        target_compile_options(${PROJECT_NAME} PRIVATE 
            -march=armv8.2-a+crypto+fp+simd 
            -mcpu=cortex-a55 
            -mfpu=neon-fp-armv8
        )
        target_compile_definitions(${PROJECT_NAME} PRIVATE TARGET_ARCH_ARM64=1)
        
    elif(TARGET_ARCH_ARM64)
        # Общая оптимизация под архитектуру ARM64 (Generic ARMv8-A + NEON блок)
        target_compile_options(${PROJECT_NAME} PRIVATE -march=armv8-a+simd -mfpu=neon)
        target_compile_definitions(${PROJECT_NAME} PRIVATE TARGET_ARCH_ARM64=1)
    endif()
    
else()
    # Резервный фоллбэк на случай сборки через оригинальный MSVC (Visual Studio)
    target_compile_options(${PROJECT_NAME} PRIVATE /O2 /Oi /Ot /Gy /GL /Gw)
    target_link_options(${PROJECT_NAME} PRIVATE /LTCG /OPT:REF /OPT:ICF)
    if(TARGET_OS_WINDOWS)
        add_compile_definitions(PLATFORM_WINDOWS=1)
    endif()
endif()

# ============================================================================
# НАСТРОЙКА ИМЕН И РАСШИРЕНИЙ ВЫХОДНЫХ БИНАРНЫХ ФАЙЛОВ ПЛАГИНА
# ============================================================================
# Избавляемся от стандартного префикса "lib", который GCC принудительно ставит в Linux.
# На выходе получаем чистое имя файла, жестко требуемое спецификацией SDK Far.
if(TARGET_OS_WINDOWS)
    set_target_properties(${PROJECT_NAME} PROPERTIES 
        PREFIX "" 
        OUTPUT_NAME "FastCopyPlugin" 
        SUFFIX ".dll"
    )
else()
    set_target_properties(${PROJECT_NAME} PROPERTIES 
        PREFIX "" 
        OUTPUT_NAME "FastCopyPlugin" 
        SUFFIX ".so"
    )
endif()
