#!/bin/bash

# ============================================================================
# АДРЕСА СЕТЕВЫХ РЕПОЗИТОРИЕВ FAR SDK (ПЕРЕМЕННЫЕ)
# ============================================================================
URL_FAR3_SDK="https://raw.githubusercontent.com/FarGroup/FarManager/refs/heads/master/plugins/common/unicode/plugin.hpp"
URL_FAR2L_SDK="https://raw.githubusercontent.com/elfmz/far2l/refs/heads/master/far2l/far2sdk/farplug-wide.h"

# ПАРАМЕТРЫ ПО УМОЛЧАНИЮ
ARCH="amd64"
OS="windows"
BUILD_INSTALLER=false

# ФУНКЦИЯ ВЫВОДА СПРАВКИ
show_help() {
    echo "Использование: $0 [архитектура] [ос] [--installer]"
    echo "Архитектуры: amd64, arm64, odroid_hc4"
    echo "Операционные системы: windows, linux"
    echo "Флаги: --installer (сборка .sh инсталлятора для Linux)"
    echo "Примеры:"
    echo "  $0                         -> Сборка под Windows AMD64 (По умолчанию)"
    echo "  $0 arm64                   -> Сборка под Linux ARM64 (По умолчанию для arm64)"
    echo "  $0 odroid_hc4 linux        -> Сборка под Linux для Odroid HC4"
    echo "  $0 amd64 linux --installer -> Сборка Linux AMD64 + создание инсталлятора"
    exit 0
}

# ОБРАБОТКА АРГУМЕНТОВ КОМАНДНОЙ СТРОКИ
if [[ "$1" == "-h" || "$1" == "--help" ]]; then show_help; fi

if [ $# -eq 0 ]; then
    ARCH="amd64"
    OS="windows"
elif [ $# -eq 1 ] && [[ "$1" == "arm64" || "$1" == "odroid_hc4" ]]; then
    ARCH="$1"
    OS="linux"
else
    for arg in "$@"; do
        case $arg in
            amd64|arm64|odroid_hc4) ARCH=$arg ;;
            windows|linux) OS=$arg ;;
            --installer) BUILD_INSTALLER=true ;;
        esac
    done
fi

echo "=== Инициализация сборки: Архитектура [$ARCH], ОС [$OS] ==="

# ============================================================================
# ОБЩИЙ БЛОК ДЛЯ СРЕДЫ MSYS2 (ПОДГОТОВКА И СКАН ПАКЕТОВ)
# ============================================================================
prepare_common_env() {
    echo "--> Проверка необходимых локальных пакетов MSYS2 UCRT64..."
    pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja curl p7zip xz > /dev/null

    mkdir -p sdk_headers
    if [ "$OS" == "windows" ]; then
        if [ ! -f "sdk_headers/plugin.hpp" ]; then
            echo "--> Загрузка заголовков Far3 SDK..."
            curl -sL "$URL_FAR3_SDK" -o sdk_headers/plugin.hpp
        fi
    else
        if [ ! -f "sdk_headers/plugin.hpp" ]; then
            echo "--> Загрузка заголовков far2l SDK..."
            curl -sL "$URL_FAR2L_SDK" -o sdk_headers/plugin.hpp
        fi
    fi
}

# ============================================================================
# КОНФИГУРАЦИЯ ТУЛЧЕЙНОВ И КРОСС-КОМПИЛЯЦИИ
# ============================================================================
configure_toolchain() {
    CMAKE_FLAGS="-G Ninja -DCMAKE_BUILD_TYPE=Release"

    if [ "$OS" == "windows" ]; then
        CMAKE_FLAGS="$CMAKE_FLAGS -DTARGET_OS_WINDOWS=TRUE"
        if [ "$ARCH" == "amd64" ]; then
            pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc > /dev/null
            CMAKE_FLAGS="$CMAKE_FLAGS -DTARGET_ARCH_AMD64=TRUE"
        elif [[ "$ARCH" == "arm64" || "$ARCH" == "odroid_hc4" ]]; then
            pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-clang > /dev/null
            CMAKE_FLAGS="$CMAKE_FLAGS -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=ARM64 -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS='-target aarch64-w64-mingw32'"
            [ "$ARCH" == "odroid_hc4" ] && CMAKE_FLAGS="$CMAKE_FLAGS -DTARGET_ARCH_ODROID=TRUE" || CMAKE_FLAGS="$CMAKE_FLAGS -DTARGET_ARCH_ARM64=TRUE"
        fi
    else
        # Настройка кросс-компиляции под Linux из MSYS2 окружения
        if [ "$ARCH" == "amd64" ]; then
            CMAKE_FLAGS="$CMAKE_FLAGS -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=x86_64 -DTARGET_ARCH_AMD64=TRUE"
        elif [[ "$ARCH" == "arm64" || "$ARCH" == "odroid_hc4" ]]; then
            CMAKE_FLAGS="$CMAKE_FLAGS -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64"
            if [ "$ARCH" == "odroid_hc4" ]; then
                CMAKE_FLAGS="$CMAKE_FLAGS -DTARGET_ARCH_ODROID=TRUE"
            else
                CMAKE_FLAGS="$CMAKE_FLAGS -DTARGET_ARCH_ARM64=TRUE"
            fi
        fi
    fi
}

# СТАРТ ПОДГОТОВКИ
prepare_common_env
configure_toolchain

BUILD_DIR="build_${OS}_${ARCH}"
rm -rf "$BUILD_DIR" && mkdir -p "$BUILD_DIR" && cd "$BUILD_DIR"

echo "--> Генерация CMake файлов..."
cmake $CMAKE_FLAGS ..

echo "--> Сборка проекта через Ninja..."
ninja

# ============================================================================
# ГЕНЕРАЦИЯ САМОРАСПАКОВЫВАЮЩЕГОСЯ LINUX-ИНСТАЛЛЯТОРА (.sh)
# ============================================================================
if [ "$OS" == "linux" ] && [ "$BUILD_INSTALLER" == true ]; then
    echo "=== Сборка дистрибутивного Linux-инсталлятора ==="
    cd ..
    
    OUTPUT_BIN="$BUILD_DIR/FastCopyPlugin.so"
    if [ ! -f "$OUTPUT_BIN" ]; then
        echo "Ошибка: Бинарный файл плагина не найден. Генерация отменена."
        exit 1
    fi

    mkdir -p payload_dist
    cp "$OUTPUT_BIN" payload_dist/

    # Сжатие данных с максимальным коэффициентом XZ (XZ-Ultra -9e)
    tar -cf - -C payload_dist . | xz -9e > payload.tar.xz

    # РАЗВЕРТЫВАНИЕ СТАБА ИНСТАЛЛЯТОРА С ANSI ЦВЕТАМИ И ПРОВЕРКОЙ SUDO
    cat << 'EOF' > install_stub.sh
#!/bin/bash
# Самораспаковывающийся инсталлятор / деинсталлятор FastCopy для far2l

# ЦВЕТОВЫЕ ANSI КОНСТАНТЫ
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m'

SHOW_HELP=false
MODE="install"

for arg in "$@"; do
    case $arg in
        --uninstall) MODE="uninstall" ;;
        -h|--help) SHOW_HELP=true ;;
    esac
done

if [ "$SHOW_HELP" == true ]; then
    echo -e "${BLUE}Использование инсталлятора:${NC}"
    echo "  ./install.sh             -> Автоматическая установка и интеграция макросов"
    echo "  ./install.sh --uninstall -> Полное удаление плагина, макросов и очистка кэша"
    exit 0
fi

# АВТОМАТИЧЕСКАЯ ПРОВЕРКА ПРАВ СУПЕРПОЛЬЗОВАТЕЛЯ (root / sudo)
if [ "$EUID" -eq 0 ]; then
    echo -e "${YELLOW}[!] Запуск выполнен с правами root. Режим ГЛОБАЛЬНОЙ установки...${NC}"
    FAR2L_BASE_DIR="/usr/lib/far2l"
    FAR2L_PLUGINS_DIR="$FAR2L_BASE_DIR/plugins"
    FAR2L_CONF_DIR="/etc/far2l"
    TARGET_PLUGIN_PATH="$FAR2L_PLUGINS_DIR/FastCopyPlugin"
    MACRO_FILE="$FAR2L_CONF_DIR/key_macros.ini"
else
    echo -e "${BLUE}[i] Обычный запуск. Режим ЛОКАЛЬНОЙ установки для текущего пользователя...${NC}"
    FAR2L_CONF_DIR="$HOME/.config/far2l"
    FAR2L_PLUGINS_DIR="$FAR2L_CONF_DIR/plugins"
    TARGET_PLUGIN_PATH="$FAR2L_PLUGINS_DIR/FastCopyPlugin"
    MACRO_FILE="$FAR2L_CONF_DIR/settings/key_macros.ini"
fi

# Функция для сброса кэша и горячей перезагрузки конфигурации far2l
reset_far2l_cache() {
    echo -e "${BLUE}--> Выполнение автоматической очистки кэша конфигурации far2l...${NC}"
    
    if command -v far2l &> /dev/null; then
        far2l --clear-cache &> /dev/null
    fi

    if [ "$EUID" -ne 0 ]; then
        rm -f "$FAR2L_CONF_DIR/history" 2>/dev/null
        rm -f "$FAR2L_CONF_DIR/viewers" 2>/dev/null
    fi
    
    PID_LIST=$(pgrep -x far2l)
    if [ ! -z "$PID_LIST" ]; then
        echo -e "${BLUE}Обнаружены активные процессы far2l (PIDs: $PID_LIST). Отправка сигнала обновления...${NC}"
        kill -10 $PID_LIST 2>/dev/null # SIGUSR1 (10) заставляет far2l перечитать макросы и плагины
    fi
}

if [ "$MODE" == "uninstall" ]; then
    echo -e "${YELLOW}=== ЗАПУСК ДЕИНСТАЛЛЯЦИИ ПЛАГИНА FASTCOPY ===${NC}"
    if [ -d "$TARGET_PLUGIN_PATH" ]; then
        rm -rf "$TARGET_PLUGIN_PATH"
        echo -e "${GREEN}-> Бинарные файлы плагина успешно удалены.${NC}"
    else
        echo -e "${YELLOW}-> Папка плагина не найдена, пропуск.${NC}"
    fi
    
    if [ -f "$MACRO_FILE" ]; then
        sed -i '/; --- FASTCOPY START ---/,/; --- FASTCOPY END ---/d' "$MACRO_FILE"
        echo -e "${GREEN}-> Макросы FastCopy вырезаны из конфигурации far2l.${NC}"
    fi
    
    reset_far2l_cache
    echo -e "${GREEN}[+] Деинсталляция успешно завершена!${NC}"
    exit 0
fi

echo -e "${BLUE}=== ЗАПУСК АВТОМАТИЧЕСКОЙ УСТАНОВКИ И КОНФИГУРАЦИИ ===${NC}"

mkdir -p "$TARGET_PLUGIN_PATH" 2>/dev/null
if [ $? -ne 0 ]; then
    echo -e "${RED}[Критическая ошибка] Нет прав на запись в директорию $TARGET_PLUGIN_PATH.${NC}"
    echo -e "${RED}Пожалуйста, запустите инсталлятор через sudo: sudo $0${NC}"
    exit 1
fi

SKIP=$(awk '/^__ARCHIVE_FOLLOWS__/ { print NR + 1; exit 0; }' "$0")
tail -n +$SKIP "$0" | tar -xJf - -C "$TARGET_PLUGIN_PATH" 2>/dev/null

if [ $? -eq 0 ]; then
    echo -e "${GREEN}-> Компоненты плагина успешно скопированы в: $TARGET_PLUGIN_PATH${NC}"
else
    echo -e "${RED}[Критическая ошибка] Не удалось распаковать архив компонентов.${NC}"
    exit 1
fi

# Отключение конфликтующих плагинов
CONFLICT_PLUGINS=("filecopyex" "altcopy" "customcopy")
for conf_plug in "${CONFLICT_PLUGINS[@]}"; do
    if [ -d "$FAR2L_PLUGINS_DIR/$conf_plug" ]; then
        echo -e "${YELLOW}[!] Обнаружен конфликтующий плагин [$conf_plug]. Отключение...${NC}"
        mv "$FAR2L_PLUGINS_DIR/$conf_plug" "$FAR2L_PLUGINS_DIR/${conf_plug}.disabled" 2>/dev/null
    fi
done

# Интеграция и автоматическая пропись макросов в key_macros.ini
mkdir -p "$(dirname "$MACRO_FILE")" 2>/dev/null
touch "$MACRO_FILE" 2>/dev/null

if ! grep -q "FASTCOPY START" "$MACRO_FILE"; then
    echo -e "${BLUE}-> Пропись обновленного макрокомплекса интеграции в $MACRO_FILE...${NC}"
    cat << 'MACRO_EOF' >> "$MACRO_FILE"

; --- FASTCOPY START ---
; Поддержка нативных окон F5/F6 с обработкой диалогов через макроядро
[Shell/CtrlShiftF5]
Sequence=F11 $if (menu.Select("FastCopy", 2) == 1) Enter $else Esc $endif
Description=FastCopy: Прямая постановка в FIFO-очередь копирования

[Shell/CtrlShiftF6]
Sequence=F11 $if (menu.Select("FastCopy", 2) == 1) Down Enter $else Esc $endif
Description=FastCopy: Прямая постановка в FIFO-очередь переноса
; --- FASTCOPY END ---
MACRO_EOF
fi

reset_far2l_cache

echo -e "${GREEN}[+] Установка и глубокая интеграция в far2l успешно завершены!${NC}"
exit 0
__ARCHIVE_FOLLOWS__
EOF

    # Конкатенация текстового стаба и бинарного архива
    cat install_stub.sh payload.tar.xz > FastCopy_Linux_Installer.sh
    chmod +x FastCopy_Linux_Installer.sh
    
    rm -rf payload_dist payload.tar.xz install_stub.sh
    echo -e "\033[0;32mФинальный дистрибутив создан: FastCopy_Linux_Installer.sh (Сжатие: XZ-Ultra)\033[0m"
fi
