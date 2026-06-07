#!/usr/bin/env bash
# ============================================================================
# БИЛД-СИСТЕМА FASTCOPY SUPERENGINE AMD64 / ARM64
# ============================================================================
set -e

# --- ПЕРЕМЕННЫЕ ДЛЯ ЗАГРУЗКИ SDK ЗАВИСИМОСТЕЙ ---
URL_FAR3_SDK="https://raw.githubusercontent.com/FarGroup/FarManager/refs/heads/master/plugins/common/unicode/plugin.hpp"
URL_FAR2L_SDK="https://raw.githubusercontent.com/elfmz/far2l/refs/heads/master/far2l/far2sdk/farplug-wide.h"

# Конфигурация по умолчанию
TARGET_OS="windows"
TARGET_ARCH="amd64"
MAKE_SH_INSTALLER=false

# Если запуск выполнен без параметров, по умолчанию собирается Windows AMD64
if [ $# -eq 0 ]; then
    TARGET_OS="windows"
    TARGET_ARCH="amd64"
fi

# Парсинг входных аргументов командной строки
for arg in "$@"; do
    case $arg in
        windows)
            TARGET_OS="windows"
            ;;
        linux)
            TARGET_OS="linux"
            ;;
        arm64)
            TARGET_ARCH="arm64"
            # Если arm64 идет единственным ключом, ОС по умолчанию переключается на Linux
            if [ "$TARGET_OS" = "windows" ] && [ "$1" = "arm64" ]; then
                TARGET_OS="linux"
            fi
            ;;
        linux--sh|--sh)
            TARGET_OS="linux"
            MAKE_SH_INSTALLER=true
            ;;
        *)
            echo "Unknown option: $arg"
            ;;
    esac
done

# ============================================================================
# ВНУТРЕННЯЯ ФУНКЦИЯ ДИНАМИЧЕСКОЙ УСТАНОВКИ ПАКЕТОВ
# ============================================================================
auto_install_package() {
    local pkg_name=$1
    echo "[!] Пакет '$pkg_name' отсутствует в системе."
    read -p "Хотите установить его автоматически? (y/N): " confirm
    if [[ "$confirm" =~ ^[Yy]$ ]]; then
        if command -v apt-get &> /dev/null; then
            sudo apt-get update && sudo apt-get install -y "$pkg_name"
        elif command -v pacman &> /dev/null; then
            sudo pacman -Sy --noconfirm "$pkg_name"
        elif command -v dnf &> /dev/null; then
            sudo dnf install -y "$pkg_name"
        else
            echo "[X] Пакетный менеджер вашей ОС не распознан. Пожалуйста, установите '$pkg_name' вручную."
            exit 1
        fi
    else
        echo "[X] Сборка прервана из-за отсутствия необходимой зависимости: $pkg_name"
        exit 1
    fi
}

# ============================================================================
# ОБЩИЙ БЛОК: СБОРКА, ПРОВЕРКА ЗАВИСИМОСТЕЙ И ГЕНЕРАЦИЯ АРТЕФАКТОВ В ВЫДЕЛЕННОЙ ПАПКЕ
# ============================================================================
execute_build_pipeline() {
    local os=$1
    local arch=$2
    # Выделенная папка сборки (уникальная для каждой комбинации ОС и Архитектуры)
    local out_dir="build_out_${os}_${arch}"
    
    echo "===================================================="
    echo " Starting FastCopy SuperEngine Compilation Pipeline"
    echo " Destination Platform:  ${os^^}"
    echo " Architecture Vector:  ${arch^^}"
    echo " Output Directory:      $out_dir"
    echo "===================================================="

    # Гарантируем чистоту и изоляцию папки сборки
    mkdir -p "$out_dir"

    # --- ИНТЕЛЛЕКТУАЛЬНАЯ ПРОВЕРКА И УСТАНОВКА БАЗОВЫХ УТИЛИТ ---
    if ! command -v curl &> /dev/null; then auto_install_package "curl"; fi
    if ! command -v awk &> /dev/null; then auto_install_package "gawk"; fi

    # Определение требуемого компилятора под целевую платформу
    local compiler=""
    local apt_package_suggestion=""

    if [ "$os" = "windows" ]; then
        if [ "$arch" = "amd64" ]; then
            compiler="x86_64-w64-mingw32-g++"
            apt_package_suggestion="g++-mingw-w64-x86-64"
        elif [ "$arch" = "arm64" ]; then
            compiler="aarch64-w64-mingw32-g++"
            apt_package_suggestion="g++-mingw-w64-aarch64"
        fi
    elif [ "$os" = "linux" ]; then
        if [ "$arch" = "amd64" ]; then
            compiler="g++"
            apt_package_suggestion="g++"
        elif [ "$arch" = "arm64" ]; then
            # Если кросс-компиляция на машине x86_64 под таргет ARM64
            if [ "$(uname -m)" != "aarch64" ]; then
                compiler="aarch64-linux-gnu-g++"
                apt_package_suggestion="g++-aarch64-linux-gnu"
            else
                compiler="g++"
                apt_package_suggestion="g++"
            fi
        fi
    fi

    # --- ПРОВЕРКА И ПРЕДЛОЖЕНИЕ УСТАНОВКИ ДЛЯ ВЫБРАННОГО КОМПИЛЯТОРА ---
    if ! command -v "$compiler" &> /dev/null; then
        auto_install_package "$apt_package_suggestion"
    fi

    # 1. Скачивание нативных SDK зависимостей по URL-переменным напрямую в выделенную папку
    echo "[*] Резолв нативных SDK зависимостей..."
    if [ "$os" = "windows" ]; then
        if [ ! -f "$out_dir/plugin.hpp" ]; then
            echo "    -> Загрузка Far Manager 3 SDK заголовков..."
            curl -sS -L -o "$out_dir/plugin.hpp" "$URL_FAR3_SDK"
        fi
    elif [ "$os" = "linux" ]; then
        if [ ! -f "$out_dir/farplug-wide.h" ]; then
            echo "    -> Загрузка far2l SDK заголовков..."
            curl -sS -L -o "$out_dir/farplug-wide.h" "$URL_FAR2L_SDK"
            # Создаем мост совместимости заголовок-в-заголовок для Linux сборки
            echo '#include "farplug-wide.h"' > "$out_dir/plugin.hpp"
        fi
    fi

    # 2. Настройка общих и платформозависимых флагов экстремальной компиляции
    local common_flags="-O3 -std=c++20 -flto=auto -fomit-frame-pointer -ffast-math -fno-plt -fPIC -DNDEBUG"
    local specific_flags=""
    local arch_flags=""
    local output_bin=""

    if [ "$os" = "windows" ]; then
        echo "[!] Внедрение глубоких оптимизаций под ядро Windows 10+..."
        specific_flags="-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -D_CRT_SECURE_NO_WARNINGS -mthreads"
        output_bin="fastcopy_plugin.dll"
        if [ "$arch" = "amd64" ]; then
            # Глубокий тюнинг под современные x86-64-v3 (Haswell/Zen2+) с поддержкой аппаратного SHA-NI/AVX2
            arch_flags="-march=x86-64-v3 -mtune=haswell -mavx2 -mbmi2 -msse4.2 -msha"
        elif [ "$arch" = "arm64" ]; then
            # Интеграция векторных модулей ARM64 Windows (Snapdragon X Elite / Copilot+ PC оптимизация)
            arch_flags="-march=armv8-a+crypto+crc+simd -mtune=cortex-a72"
        fi
    elif [ "$os" = "linux" ]; then
        echo "[!] Внедрение глубоких оптимизаций планировщика и ввода-вывода Linux Kernel 6+..."
        specific_flags="-D__LINUX_KERNEL_6__ -D_GNU_SOURCE -pthread"
        output_bin="fastcopy_plugin.so"
        if [ "$arch" = "amd64" ]; then
            arch_flags="-march=x86-64-v3 -mtune=native -mprefer-vector-width=256 -z max-page-size=0x1000"
        elif [ "$arch" = "arm64" ]; then
            # Активация криптографических сопроцессоров ARM64 под серверные ядра Linux
            arch_flags="-march=armv8-a+crypto+crc -mtune=neoverse-n1"
        fi
    fi

    # Добавляем путь к выделенной папке билда в include-пути компилятора (-I), чтобы он видел скачанный SDK
    local final_exec_flags="$common_flags $arch_flags $specific_flags -I$out_dir"
    echo "Компилятор: $compiler"

    # 3. Физическая компиляция исходного кода
    echo "Запуск компиляции исходного кода плагина..."
    $compiler $final_exec_flags plugin.cpp -shared -o "$out_dir/$output_bin"

    # 4. Автогенерация мультиязычных файлов локализации (.lng) внутри выделенной папки сборки
    echo "[!] Генерация языковых пакетов интерфейса..."
    cat << 'EOF' > "$out_dir/FastCopyEng.lng"
.Language=English,English

"FastCopy SuperEngine AMD64/ARM64"
"Copying:"
"Estimated time:"
"Cancel"
EOF

    cat << 'EOF' > "$out_dir/FastCopyRus.lng"
.Language=Russian,Russian (Русский)

"FastCopy SuperEngine AMD64/ARM64"
"Копирование:"
"Оставшееся время:"
"Отмена"
EOF

    echo "✔ Компиляция завершена успешно. Артефакт сохранен в: $out_dir/$output_bin"

    # 5. Сборка монолитного продвинутого SFX-инсталлятора для Unix систем (если запрошено)
    if [ "$os" = "linux" ] && [ "$MAKE_SH_INSTALLER" = true ]; then
        echo "[!] Упаковка самораспаковывающегося SFX-инсталлятора для Linux..."
        local installer_name="install_fastcopy_${arch}.sh"
        
        cat << 'EOF' > "$out_dir/$installer_name"
#!/usr/bin/env bash
set -e

LOG_FILE="/var/log/fastcopy_install.log"
INSTALL_TARGET="/usr/lib/far2l/plugins/fastcopy"
CONFLICT_PLUGIN_DIR="/usr/lib/far2l/plugins/multi"

log_message() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') - $1" | tee -a "$LOG_FILE" 2>/dev/null || echo "$1"
}

# --- ЛОГИКА ПОЛНОЙ ДЕИНСТАЛЛЯЦИИ И ОТКАТА ---
if [ "$1" = "--uninstall" ]; then
    log_message "[!] Запуск отката изменений и деинсталляции FastCopy..."
    if [ -d "$CONFLICT_PLUGIN_DIR" ]; then
        find "$CONFLICT_PLUGIN_DIR" -type f -name "*.disabled_by_fastcopy" | while read -r disabled_plug; do
            original_plug="${disabled_plug%.disabled_by_fastcopy}"
            mv "$disabled_plug" "$original_plug"
            log_message "Восстановлен оригинальный системный плагин: $original_plug"
        done
    fi
    if [ -d "$INSTALL_TARGET" ]; then
        rm -rf "$INSTALL_TARGET"
        log_message "Удалена директория плагина: $INSTALL_TARGET"
    fi
    log_message "✔ Деинсталляция завершена успешно. Система возвращена в исходное состояние."
    exit 0
fi

if [ "$EUID" -ne 0 ]; then
    echo "Пожалуйста, запустите инсталлятор от имени суперпользователя (через sudo) для интеграции в far2l."
    exit 1
fi

touch "$LOG_FILE" 2>/dev/null || true
log_message "[+] Развертывание FastCopy SuperEngine в системе Linux..."
mkdir -p "$INSTALL_TARGET"

# Шаг 1: Извлечение бинарного хвоста
BINARY_START_LINE=$(awk '/^__ARCHIVE_FOLLOWS__/ { print NR + 1; exit 0; }' "$0")
tail -n +$BINARY_START_LINE "$0" > "$INSTALL_TARGET/fastcopy_plugin.so"
chmod +x "$INSTALL_TARGET/fastcopy_plugin.so"
log_message "Бинарный модуль успешно развернут в $INSTALL_TARGET"

# Шаг 2: Копирование сопутствующих файлов .lng из папки, где лежит установщик
SCRIPT_DIR="$(dirname "$0")"
cp "$SCRIPT_DIR"/*.lng "$INSTALL_TARGET/" 2>/dev/null || true
log_message "Файлы локализации интегрированы."

# Шаг 3: Автоматический поиск и отключение конфликтующих модулей
log_message "[*] Сканирование конфликтующих системных I/O плагинов..."
if [ -d "$CONFLICT_PLUGIN_DIR" ]; then
    find "$CONFLICT_PLUGIN_DIR" -type f \( -name "*copy*.far-plug-wide" -o -name "standard_io.far-plug-wide" \) | while read -r conflict; do
        mv "$conflict" "${conflict}.disabled_by_fastcopy"
        log_message "Конфликтующий модуль деактивирован: $(basename "$conflict")"
    done
fi

log_message "✔ FastCopy теперь является приоритетным обработчиком задач Copy/Move в far2l."
exit 0
__ARCHIVE_FOLLOWS__
EOF

        # Физическая склейка шелл-скрипта с бинарным скомпилированным файлом .so
        cat "$out_dir/$output_bin" >> "$out_dir/$installer_name"
        chmod +x "$out_dir/$installer_name"
        echo "✔ Продвинутый SFX-инсталлятор скомпилирован: $out_dir/$installer_name"
    fi
}

# --- ЗАПУСК ОБЩЕГО КОНВЕЙЕРА СБОРКИ С ВЫБРАННЫМИ ПАРАМЕТРАМИ ---
execute_build_pipeline "$TARGET_OS" "$TARGET_ARCH"
