cmake_minimum_required(VERSION 3.16)

# ==============================================================================
# Конфигурация проекта. Включаем поддержку CXX и ассемблера NASM/YASM
# ==============================================================================
project(FarMultiEngineViewer CXX ASM_NASM)

# Строгий таргет на стандарт C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Поиск системного менеджера пакетов PkgConfig
find_package(PkgConfig REQUIRED)

# Выполняем автоматический поиск и проверку всех легковесных декодеров
pkg_check_modules(MUPDF REQUIRED mupdf)
pkg_check_modules(LIBRAW REQUIRED libraw)
pkg_check_modules(FREEIMAGE REQUIRED freeimage)
pkg_check_modules(LUNASVG REQUIRED lunasvg)
pkg_check_modules(LIBARCHIVE REQUIRED libarchive)

# Поиск библиотеки для работы с векторными CAD чертежами DXF/DWG
find_library(LIBDXFRW_LIB NAMES dxfrw REQUIRED)

# ==============================================================================
# Формирование базового кроссплатформенного ядра исходного кода
# ==============================================================================
set(SOURCES
    src/plugin_core.cpp
    src/simd_amd64.asm
)

# ==============================================================================
# Платформо-зависимая диспетчеризация (Windows / Linux)
# ==============================================================================
if(WIN32)
    # Ориентация на подсистемы Windows 10/11
    add_definitions(-D_WIN32_WINNT=0x0A00)
    
    # Добавляем монолитный бэкэнд Windows (WIC + Direct2D + DirectWrite + FAR3)
    list(APPEND SOURCES src/win32_backend.cpp)
    
    # Системные графические библиотеки Windows 10
    set(PLATFORM_LIBS d2d1 dwrite windowscodecs ole32 shlwapi)
    
    # Флаг компиляции ассемблера для 64-битной Windows (Microsoft x64 ABI)
    set(CMAKE_ASM_NASM_FLAGS "-f win64")
    
    echo("== Настройка CMake: Выбрана целевая платформа WINDOWS (Far3) ==")
else()
    # Макрос для сборки под Linux-версию far2l
    add_definitions(-DLINUX_FAR2L)
    
    # Добавляем монолитный бэкэнд Linux (Chauvinist GUI VT + Рендерер AVX2)
    list(APPEND SOURCES src/linux_backend.cpp)
    
    # Системные библиотеки Linux (все зависимости закрываются через pkg-config)
    set(PLATFORM_LIBS "")
    
    # Флаг компиляции ассемблера для 64-битного Linux (System V AMD64 ABI)
    set(CMAKE_ASM_NASM_FLAGS "-f elf64")
    
    echo("== Настройка CMake: Выбрана целевая платформа LINUX (far2l) ==")
endif()

# ==============================================================================
# Создание целевого бинарного модуля (Динамическая библиотека / Плагин)
# ==============================================================================
add_library(far_image_viewer SHARED ${SOURCES})

# Настройка путей для поиска заголовочных файлов (.hpp / .h)
target_include_directories(far_image_viewer PRIVATE 
    src 
    ${MUPDF_INCLUDE_DIRS}
    ${LIBRAW_INCLUDE_DIRS}
    ${FREEIMAGE_INCLUDE_DIRS}
    ${LUNASVG_INCLUDE_DIRS}
    ${LIBARCHIVE_INCLUDE_DIRS}
)

# Интеграция флагов компиляции сторонних библиотек
target_compile_options(far_image_viewer PRIVATE
    ${MUPDF_CFLAGS_OTHER}
    ${LIBRAW_CFLAGS_OTHER}
    ${FREEIMAGE_CFLAGS_OTHER}
    ${LUNASVG_CFLAGS_OTHER}
    ${LIBARCHIVE_CFLAGS_OTHER}
)

# ==============================================================================
# Финальная линковка всех модулей и системных библиотек
# ==============================================================================
target_link_libraries(far_image_viewer PRIVATE 
    ${PLATFORM_LIBS}
    ${MUPDF_LIBRARIES}
    ${LIBRAW_LIBRARIES}
    ${FREEIMAGE_LIBRARIES}
    ${LUNASVG_LIBRARIES}
    ${LIBARCHIVE_LIBRARIES}
    ${LIBDXFRW_LIB}
)

# Выставляем выходное имя файла плагина в зависимости от операционной системы
if(WIN32)
    set_target_properties(far_image_viewer PROPERTIES PREFIX "" SUFFIX ".dll")
else()
    set_target_properties(far_image_viewer PROPERTIES PREFIX "" SUFFIX ".so")
endif()
