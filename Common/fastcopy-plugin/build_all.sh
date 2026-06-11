#!/usr/bin/env bash
# =========================================================================
# FastCopy Plugin Build Automation System (C++20, Kernel 6+, Win10+)
# Поддержка платформ: Windows (AMD64), Linux (AMD64, ARM64, Odroid HC4)
# Целевые менеджеры: FAR Manager 3, far2l, far2m
# Среда запуска: MSYS2 UCRT64 (Windows) или Native Linux Терминал
# Генератор сборки: Ninja
# Кодировка файла: UTF-8 без BOM
# =========================================================================
set -e

# Глобальный block путей скачивания официальных SDK файлов из Интернета
URL_FAR3_SDK="https://raw.githubusercontent.com/FarGroup/FarManager/refs/heads/master/plugins/common/unicode/plugin.hpp"
URL_FAR2L_SDK="https://raw.githubusercontent.com/elfmz/far2l/refs/heads/master/far2l/far2sdk/farplug-wide.h"

# Функции логирования на экран
log() { echo -e "[\e[1;34m*\e[0m] $1"; }
log_ok() { echo -e "[\e[1;32mSUCCESS\e[0m] $1"; }
log_err() { echo -e "[\e[1;31mERROR\e[0m] $1"; }

# --- ИНТЕЛЛЕКТУАЛЬНЫЙ МОДУЛЬ ПРОВЕРКИ И УСТАНОВКИ ПАКЕТОВ ---
check_and_install_dependencies() {
    log "Проверка операционного окружения и системных зависимостей..."
    
    local missing_packages=()
    local is_msys2=false
    local is_linux=false

    # Определяем тип операционной системы/окружения запуска
    if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" ]]; then
        is_msys2=true
        log "Обнаружена среда: MSYS2 Windows"
    else
        is_linux=true
        log "Обнаружена среда: Native Linux"
    fi

    # Базовые утилиты, необходимые для всех типов сборок
    local deps_commands=("cmake" "ninja" "curl" "tar" "xz")
    for cmd in "${deps_commands[@]}"; do
        if ! command -v "$cmd" &> /dev/null; then
            missing_packages+=("$cmd")
        fi
    done

    # Специфичные проверки компиляторов в зависимости от аргументов командной строки
    if [ "$1" == "arm64" ] || [ "$1" == "ohc4" ] || [ "$1" == "--installer" ]; then
        if ! command -v aarch64-linux-gnu-gcc &> /dev/null; then
            missing_packages+=("cross-compiler-arm64")
        fi
    fi

    # Проверка компиляторов для сборки AMD64 (x86_64)
    if [ -z "$1" ] || [ "$1" == "amd64" ]; then
        if $is_msys2; then
            if ! command -v gcc &> /dev/null; then missing_packages+=("mingw-w64-ucrt-x86_64-gcc"); fi
            if ! command -v g++ &> /dev/null; then missing_packages+=("mingw-w64-ucrt-x86_64-g++"); fi
        else
            if ! command -v gcc &> /dev/null; then missing_packages+=("gcc"); fi
            if ! command -v g++ &> /dev/null; then missing_packages+=("g++"); fi
            if ! ldconfig -p | grep -q liburing; then missing_packages+=("liburing-dev"); fi
        fi
    fi

    # Если список пуст — все зависимости удовлетворены, выходим из функции
    if [ ${#missing_packages[@]} -eq 0 ]; then
        log_ok "Все необходимые пакеты и зависимости обнаружены в системе."
        return 0
    fi

    log_err "В системе отсутствуют критически важные компоненты для сборки проекта:"
    for pkg in "${missing_packages[@]}"; do
        echo -e "  - \e[1;33m$pkg\e[0m"
    done

    # Интерактивный запрос на автоматическую установку пакетов
    echo -n "Желаете установить недостающие пакеты автоматически? [Y/n]: "
    read -r response
    
    if [[ -z "$response" || "$response" =~ ^[yY]([eE][sS])?$ ]]; then
        if $is_msys2; then
            log "Запуск менеджера пакетов pacman в среде MSYS2..."
            local pacman_pkgs=()
            for pkg in "${missing_packages[@]}"; do
                case "$pkg" in
                    cmake) pacman_pkgs+=("mingw-w64-ucrt-x86_64-cmake") ;;
                    ninja) pacman_pkgs+=("mingw-w64-ucrt-x86_64-ninja") ;;
                    curl)  pacman_pkgs+=("curl") ;;
                    tar)   pacman_pkgs+=("tar") ;;
                    xz)    pacman_pkgs+=("xz") ;;
                    # ИСПРАВЛЕНО: Нативное имя пакета в репозитории MSYS2 для Linux ARM64
                    cross-compiler-arm64) pacman_pkgs+=("aarch64-linux-gnu-gcc") ;;
                    *)     pacman_pkgs+=("$pkg") ;;
                esac
            done
            pacman -S --noconfirm "${pacman_pkgs[@]}"
        elif $is_linux; then
            log "Запуск менеджера пакетов apt (требуются права sudo)..."
            sudo apt-get update
            local apt_pkgs=()
            for pkg in "${missing_packages[@]}"; do
                case "$pkg" in
                    ninja)                apt_pkgs+=("ninja-build") ;;
                    cross-compiler-arm64) apt_pkgs+=("gcc-aarch64-linux-gnu" "g++-aarch64-linux-gnu") ;;
                    liburing-dev)         apt_pkgs+=("liburing-dev") ;;
                    *)                    apt_pkgs+=("$pkg") ;;
                esac
            done
            sudo apt-get install -y "${apt_pkgs[@]}"
        fi
        log_ok "Установка пакетов завершена. Перезапуск сборщика..."
    else
        log_err "Сборка проекта отменена пользователем из-за отсутствия необходимых пакетов."
        exit 1
    fi
}

clean_build_dir() {
    log "Очистка и подготовка рабочей директории перед началом сборки..."
    rm -rf build_out target_dist sdk
    mkdir -p build_out target_dist sdk/far3 sdk/far2l
}

download_sdks() {
    log "Загрузка официальных SDK файлов из сети Интернет..."
    curl -sSL "$URL_FAR3_SDK" -o sdk/far3/PluginW.hpp
    curl -sSL "$URL_FAR2L_SDK" -o sdk/far2l/pluginw16.hpp
}

build_win_amd64() {
    log "Глубокая оптимизация: Windows x86_64 (AMD64) [AVX2 + FMA + LTCG] с использованием Ninja..."
    cd build_out 
    cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release .. 
    cmake --build . --config Release
    
    if [ -f "libFarFastCopyPlugin.dll" ]; then
        cp libFarFastCopyPlugin.dll ../target_dist/FastCopy.dll
    elif [ -f "FarFastCopyPlugin.dll" ]; then
        cp FarFastCopyPlugin.dll ../target_dist/FastCopy.dll
    else
        log_err "Скомпилированный бинарник Windows не найден!"
        exit 1
    fi
    cd ..
    log_ok "Монолитный плагин для Windows успешно собран: target_dist/FastCopy.dll"
}

build_linux_x86_64() {
    log "Глубокая оптимизация: Linux x86_64 (AMD64) [AVX2 + FMA + LTO + io_uring] с использованием Ninja..."
    cd build_out 
    cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DARM_TARGET="amd64" .. 
    cmake --build .
    cp libFarFastCopyPlugin.so ../target_dist/FastCopy.so 
    cd ..
    log_ok "Монолитный плагин для Linux AMD64 успешно собран: target_dist/FastCopy.so"
}

build_linux_arm64() {
    local arch_flag=$1
    log "Кросс-компиляция: Linux ARM64 (${arch_flag}) [NEON + Аппаратный CRC32/Crypto] с использованием Ninja..."
    cd build_out
    export CC=aarch64-linux-gnu-gcc
    export CXX=aarch64-linux-gnu-g++
    
    # ИСПРАВЛЕНО: Добавлен обязательный флаг -DCMAKE_SYSTEM_NAME=Linux для предотвращения ошибок путей в MSYS2
    cmake -G "Ninja" -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_BUILD_TYPE=Release -DARM_TARGET="${arch_flag}" .. 
    cmake --build .
    cp libFarFastCopyPlugin.so ../target_dist/FastCopy.so 
    cd ..
    log_ok "Монолитный плагин для Linux ARM64 (${arch_flag}) успешно собран: target_dist/FastCopy.so"
}

generate_sh_installer() {
    log "Упаковка нативного самораспаковывающегося SH-инсталлятора (far2l + far2m)..."
    cat << 'EOF' > target_dist/install.sh
#!/usr/bin/env bash
# Фоновый высоконадежный установщик / деинсталлятор FastCopy в far2l и far2m
LOG_FILE="/var/log/fastcopy_install.log"
exec 3>&1 4>&2 >>"$LOG_FILE" 2>&1
log() { echo "[$1] $2" >&3; echo "[$1] $2"; }

TARGET_FAR2L="$HOME/.config/far2l/plugins/fastcopy"
TARGET_FAR2M="$HOME/.config/far2m/plugins/fastcopy"

if [ "$1" == "--uninstall" ]; then
    log "INFO" "Запуск процедуры полной деинсталляции плагина..."
    rm -rf "$TARGET_FAR2L"
    rm -rf "$TARGET_FAR2M"
    log "SUCCESS" "Удаление завершено. Изменения из far2l и far2m успешно отменены."
    exit 0
fi

installed=false

if [ -d "$HOME/.config/far2l" ]; then
    mkdir -p "$TARGET_FAR2L"
    tail -n +43 "$0" | tar -xJ -C "$TARGET_FAR2L"
    log "SUCCESS" "Монолитный плагин успешно внедрен в far2l!"
    installed=true
fi

if [ -d "$HOME/.config/far2m" ]; then
    mkdir -p "$TARGET_FAR2M"
    tail -n +43 "$0" | tar -xJ -C "$TARGET_FAR2M"
    log "SUCCESS" "Монолитный плагин успешно внедрен в far2m!"
    installed=true
fi

if [ "$installed" = false ]; then
    log "ERROR" "Ни far2l, ни far2m не обнаружены в конфигурациях пользователя!"
    exit 1
fi

exit 0
__ARCHIVE_FOLLOWS__
EOF
    cd target_dist 
    tar -cJf - FastCopy.so | cat >> install.sh
    chmod +x install.sh 
    cd ..
    log_ok "Самораспаковывающийся инсталлятор (far2l/far2m) собран: target_dist/install.sh"
}

# --- ЦЕНТРАЛЬНЫЙ ДИСПЕТЧЕР КОМАНДНОЙ СТРОКИ ---
check_and_install_dependencies "$1"
clean_build_dir
download_sdks

if [ $# -eq 0 ]; then
    build_win_amd64
elif [ "$1" == "amd64" ]; then
    build_linux_x86_64
elif [ "$1" == "arm64" ] || [ "$1" == "ohc4" ]; then
    build_linux_arm64 "$1"
elif [ "$1" == "--installer" ]; then
    build_linux_arm64 "arm64"
    generate_sh_installer
else
    log_err "Ошибка: Неизвестный аргумент командной строки."
    echo "Использование: "
    echo "  ./build_all.sh             - Сборка под Windows AMD64 (по умолчанию)"
    echo "  ./build_all.sh amd64       - Сборка под Linux AMD64 (far2l/far2m)"
    echo "  ./build_all.sh arm64       - Кросс-компиляция под универсальный Linux ARM64"
    echo "  ./build_all.sh ohc4        - Глубокая оптимизация под Odroid HC4"
    echo "  ./build_all.sh --installer - Сборка ARM64 и упаковка в SH-инсталлятор"
    exit 1
fi
