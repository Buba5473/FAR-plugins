cmake_minimum_required(VERSION 3.20)
project(FarFastCopyPlugin LANGUAGES CXX)

# Активируем стандарт C++20 для утилизации jthread, std::format и трехстороннего сравнения (<=>)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Структура исходного кода проекта (Оптимизированный компактный слой из 4 файлов)
set(SOURCES
    src/main.cpp
    src/core.h
)

# Настройка жестких флагов оптимизации релизных сборок под разные компиляторы
if(MSVC)
    # /O2 - макс. скорость, /Oi - генерация встроенных функций, /Ot - приоритет скорости кода,
    # /GL - глобальная оптимизация всей программы (LTCG), /Gy - включение функций на уровне строк
    set(GLOBAL_RELEASE_FLAGS "/O2 /Oi /Ot /GL /Gy /MD /EHa /arch:AVX2")
else()
    # -O3 - агрессивный разгон, -flto - межмодульная оптимизация на этапе линковки,
    # -fno-plt - обход процедурных таблиц линковки, -fomit-frame-pointer - освобождение регистра под нужды I/O
    # -mavx2 -mfma - глубокий разгон AMD64 (x86_64) под MinGW GCC / Linux GCC
    set(GLOBAL_RELEASE_FLAGS "-O3 -flto -fno-plt -fomit-frame-pointer -Wall -Wextra -fvisibility=hidden -mavx2 -mfma")
endif()

# Разделение логики сборки в зависимости от целевой операционной системы
if(WIN32)
    # --- СБОРКА ПОД WINDOWS 10+ (FAR 3) ---
    list(APPEND SOURCES src/win32_impl.cpp)
    add_library(${PROJECT_NAME} SHARED ${SOURCES})
    
    # Нацеливаемся на Windows 10+ и включаем нативную поддержку Юникода
    target_compile_definitions(${PROJECT_NAME} PRIVATE 
        _WIN32_WINNT=0x0A00 
        UNICODE 
        _UNICODE
    )
    
    # Подключаем скачанный SDK FAR 3
    target_include_directories(${PROJECT_NAME} PRIVATE sdk/far3)
    
    # Указываем файл экспорта функций (.def) и активируем Link-Time Code Generation
    if(MSVC)
        target_link_options(${PROJECT_NAME} PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/plugin.def" "/LTCG")
    else()
        # Для MinGW GCC передаем путь к .def файлу напрямую и включаем LTO на этапе линковки
        target_link_options(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/plugin.def" "-flto")
    endif()
    
    # Применяем сформированные флаги к сборке Windows-модуля
    set_target_properties(${PROJECT_NAME} PROPERTIES COMPILE_FLAGS "${GLOBAL_RELEASE_FLAGS}")

else()
    # --- СБОРКА ПОД LINUX (KERNEL 6+) (far2l / far2m) ---
    list(APPEND SOURCES src/linux_impl.cpp)
    add_library(${PROJECT_NAME} SHARED ${SOURCES})
    
    # Подключаем скачанный SDK far2l
    target_include_directories(${PROJECT_NAME} PRIVATE sdk/far2l)
    
    # Жестко линкуем системную библиотеку liburing для асинхронных дисковых колец Linux 6+
    target_link_libraries(${PROJECT_NAME} PRIVATE uring)
    
    # Ветвление глубокой архитектурной оптимизации «под железо» (Переменная ARM_TARGET управляется из build_all.sh)
    if(ARM_TARGET STREQUAL "ohc4")
        # Экстремальный разгон процессора Odroid HC4 (Cortex-A73/A53 + аппаратный CRC32/Crypto для хэшей xxHash)
        set(ARCH_FLAGS "-mcpu=cortex-a73+cortex-a53 -march=armv8-a+crc+crypto")
    elseif(ARM_TARGET STREQUAL "arm64")
        # Глубокая оптимизация под универсальные процессоры ARM64 (Векторные NEON инструкции + CRC32)
        set(ARCH_FLAGS "-march=armv8-a+crc+crypto")
    else()
        # Глубокий разгон AMD64 (x86_64) под Linux (Включаем векторные регистры Haswell/AVX2 + аппаратный FMA)
        set(ARCH_FLAGS "-march=haswell -mfma -mavx2") 
    endif()
    
    # Применяем сформированные флаги к сборке Linux-модуля
    set_target_properties(${PROJECT_NAME} PROPERTIES COMPILE_FLAGS "${GLOBAL_RELEASE_FLAGS} ${ARCH_FLAGS}")
endif()
