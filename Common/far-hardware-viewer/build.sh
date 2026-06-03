#!/usr/bin/env bash
# ==============================================================================
# Кросс-платформенный скрипт сборки и генерации инсталлятора для Far Hardware Viewer
# Поддерживаемые платформы: Windows (MSYS2 UCRT64) / Linux (far2l)
# ==============================================================================
set -e

TARGET_OS="windows"
BUILD_TYPE="Release"
CREATE_INSTALLER=false

# Вывод справки по использованию
show_help() {
    echo "Использование: ./build.sh [параметры]"
    echo "Параметры:"
    echo "  --os=windows           Сборка для Windows 10/11 (Far Manager 3)"
    echo "  --os=linux             Сборка для Linux (far2l)"
    echo "  --installer            Создать самораспаковывающийся .sh инсталлятор (только для Linux)"
    echo "  --help                 Показать эту справку"
    exit 0
}

# Парсинг аргументов командной строки
while [[ \$# -gt 0 ]]; do
  case \$1 in
    --os=*)
      TARGET_OS="\${1#*=}"
      if [[ "\(TARGET_OS" != "windows" && "\)TARGET_OS" != "linux" ]]; then
          echo "Ошибка: Неверная ОС. Допустимы: windows, linux."
          exit 1
      fi
      shift
      ;;
    --installer)
      CREATE_INSTALLER=true
      shift
      ;;
    --help)
      show_help
      ;;
    *)
      echo "Ошибка: Неизвестный параметр: \$1"
      show_help
      ;;
  esac
done

echo "======================================================================"
echo " Настройка окружения сборки: ОС -> [\(TARGET_OS], Конфигурация -> [\)BUILD_TYPE]"
echo "======================================================================"

# --- Функция проверки и установки пакетов-зависимостей ---
check_and_install_deps() {
  if [ "\$TARGET_OS" == "windows" ]; then
    # Проверка, что сборка идет в правильном окружении MSYS2
    if [ -z "\(MSYSTEM" ] \vert{}\vert{} [ "\)MSYSTEM" != "UCRT64" ]; then
      echo "Критическая ошибка: Для Windows сборка должна запускаться строго внутри консоли MSYS2 UCRT64!"
      exit 1
    fi
    
    # Список необходимых пакетов в MSYS2 UCRT64 (включая YASM для компиляции нашего .asm файла)
    PACKAGES=(
        mingw-w64-ucrt-x86_64-cmake 
        mingw-w64-ucrt-x86_64-gcc 
        mingw-w64-ucrt-x86_64-yasm
        mingw-w64-ucrt-x86_64-mupdf 
        mingw-w64-ucrt-x86_64-libraw
        mingw-w64-ucrt-x86_64-freeimage
        mingw-w64-ucrt-x86_64-lunasvg
        mingw-w64-ucrt-x86_64-libarchive
        mingw-w64-ucrt-x86_64-libdxfrw
    )
    MISSING_PACKAGES=()

    for pkg in "\${PACKAGES[@]}"; do
      if ! pacman -Q "\$pkg" &>/dev/null; then
        MISSING_PACKAGES+=("\$pkg")
      fi
    done

    if [ \${#MISSING_PACKAGES[@]} -gt 0 ]; then
      echo "Обнаружены отсутствующие системные пакеты: \${MISSING_PACKAGES[*]}"
      read -p "Установить их сейчас автоматически через pacman? (y/n): " confirm
      if [[ \(confirm == [yY] \vert{}\vert{}\)confirm == [yY][eE][sS] ]]; then
        pacman -S --noconfirm "\${MISSING_PACKAGES[@]}"
      else
        echo "Сборка отменена пользователем из-за отсутствия необходимых библиотек."
        exit 1
      fi
    fi

  else
    # Проверка пакетов для Linux (Деривативы Debian/Ubuntu)
    if command -v apt-get &>/dev/null; then
      echo "Проверка системных библиотек через apt..."
      PACKAGES=(cmake yasm libmupdf-dev libraw-dev libfreeimage-dev liblunasvg-dev libarchive-dev libdxfrw-dev)
      MISSING_PACKAGES=()
      
      for pkg in "\${PACKAGES[@]}"; do
        if ! dpkg -s "\$pkg" &>/dev/null; then
          MISSING_PACKAGES+=("\$pkg")
        fi
      done

      if [ \${#MISSING_PACKAGES[@]} -gt 0 ]; then
        echo "В системе отсутствуют библиотеки разработки: \${MISSING_PACKAGES[*]}"
        echo "Для продолжения требуется установить их командой:"
        echo "sudo apt-get install -y \${MISSING_PACKAGES[*]}"
        read -p "Попробовать установить автоматически через sudo apt-get? (y/n): " confirm
        if [[ \(confirm == [yY] \vert{}\vert{}\)confirm == [yY][eE][sS] ]]; then
          sudo apt-get update && sudo apt-get install -y "\${MISSING_PACKAGES[@]}"
        else
          echo "Сборка отменена: Установите пакеты вручную и перезапустите скрипт."
          exit 1
        fi
      fi
    else
       echo "[Предупреждение]: Менеджер пакетов 'apt' не найден. Убедитесь, что все dev-библиотеки установлены вручную."
    fi
  fi
}

# Запускаем валидацию системного окружения
check_and_install_deps

# --- Изолированная сборка в отдельном каталоге ---
BUILD_DIR="build_\${TARGET_OS}"
rm -rf "\$BUILD_DIR"
mkdir -p "\$BUILD_DIR"
cd "\$BUILD_DIR"

# Глубокая оптимизация под микроархитектуру AMD64 (AVX2, FMA3, BMI2)
CXX_FLAGS="-O3 -march=x86-64-v3 -flto -ffast-math"

echo "Конфигурирование проекта через CMake..."
if [ "\$TARGET_OS" == "windows" ]; then
  cmake -G "MinGW Makefiles" \
        -DCMAKE_BUILD_TYPE=\$BUILD_TYPE \
        -DCMAKE_CXX_FLAGS="\$CXX_FLAGS" \
        -DTARGET_PLATFORM=WINDOWS ..
else
  cmake -DCMAKE_BUILD_TYPE=\$BUILD_TYPE \
        -DCMAKE_CXX_FLAGS="\$CXX_FLAGS" \
        -DTARGET_PLATFORM=LINUX ..
fi

echo "Компиляция бинарных модулей плагина..."
cmake --build . --config \(BUILD_TYPE -- -j\)(nproc)

echo "Фиксация сборки..."
cd ..

# ==============================================================================
# БЛОК СБОРКИ САМОРАСПАКОВЫВАЮЩЕГОСЯ SFX ИНСТАЛЛЯТОРА ДЛЯ LINUX (far2l)
# ==============================================================================
if [ "\(TARGET_OS" == "linux" ] && [ "\)CREATE_INSTALLER" == true ]; then
    echo "======================================================================"
    echo "  Генерация SFX инсталлятора с экстремальным сжатием XZ (-9e)"
    echo "======================================================================"
    
    DIST_DIR="dist_linux_package"
    rm -rf "\$DIST_DIR"
    mkdir -p "\$DIST_DIR/payload"
    
    if [ -f "\$BUILD_DIR/far_image_viewer.so" ]; then
        cp "\(BUILD_DIR/far_image_viewer.so" "\)DIST_DIR/payload/"
        # Интеграция конфигурационного INI-файла и локализации в инсталлятор
        cp config.ini "\$DIST_DIR/payload/"
        cp Amd64ViewerEng.lng "\$DIST_DIR/payload/"
        cp Amd64ViewerRus.lng "\$DIST_DIR/payload/"
        cp Amd64ViewerEng.hlf "\$DIST_DIR/payload/"
        cp Amd64ViewerRus.hlf "\$DIST_DIR/payload/"
    else
        echo "Критическая ошибка: Бинарный файл far_image_viewer.so не найден в \$BUILD_DIR!"
        exit 1
    fi
    
    # Инжектируем внутренний скрипт установки с механизмами резервного копирования
    cat << 'EOF' > "\$DIST_DIR/payload/install_logic.sh"
#!/usr/bin/env bash
LOG_FILE="\$HOME/.config/far2l/plugins/amd64_viewer_install.log"
PLUGIN_DIR="\$HOME/.config/far2l/plugins/amd64_viewer"
FAR2L_PLUGINS_ROOT="\$HOME/.config/far2l/plugins"

log_msg() {
    echo "\$(date '+%Y-%m-%d %H:%M:%S') - \(1" >> "\)LOG_FILE"
}

run_install() {
    mkdir -p "\$PLUGIN_DIR"
    mkdir -p "\((dirname "\)LOG_FILE")"
    echo "=== ЖУРНАЛ УСТАНОВКИ FAR AMD64 HARDWARE VIEWER ===" > "\$LOG_FILE"
    log_msg "Старт процедуры развертывания плагина."

    # Копирование бинарных файлов, файлов конфигурации и ресурсов плагина
    cp far_image_viewer.so "\$PLUGIN_DIR/"
    cp config.ini "\$PLUGIN_DIR/"
    cp Amd64Viewer*.lng "\$PLUGIN_DIR/"
    cp Amd64Viewer*.hlf "\$PLUGIN_DIR/"
    log_msg "Файлы, конфиг и локализация успешно скопированы в: \$PLUGIN_DIR"

    # Список известных плагинов просмотра far2l для безопасного отключения
    CONFLICTING_PLUGINS=("imagine" "pictureview" "pv" "imageview")

    for conflicting in "\${CONFLICTING_PLUGINS[@]}"; do
        TARGET_PATH="\(FAR2L_PLUGINS_ROOT/\)conflicting"
        if [ -d "\$TARGET_PATH" ]; then
            log_msg "Внимание: Обнаружен конфликтующий плагин просмотра: [\$conflicting]"
            mv "\(TARGET_PATH" "\){TARGET_PATH}.disabled"
            log_msg "Успешно отключен: [\$conflicting] (Путь изменен на .disabled)"
            echo "DISABLED_PLUGIN:\({TARGET_PATH}" >> "\)LOG_FILE"
        fi
    done
    
    echo "------------------------------------------------------------------"
    echo "  Установка завершена успешно!"
    echo "  Плагин и файл config.ini развернуты в: \$PLUGIN_DIR"
    echo "  Отключенные конфликты зафиксированы в системном логе."
    echo "  Пожалуйста, перезапустите far2l для применения изменений."
    echo "------------------------------------------------------------------"
    log_msg "Процесс установки штатно завершен."
}

run_uninstall() {
    if [ ! -f "\$LOG_FILE" ]; then
        echo "Ошибка деинсталляции: Системный лог \$LOG_FILE не найден."
        exit 1
    fi

    log_msg "Запущен процесс полного удаления плагина с откатом изменений."

    while read -r line; do
        if [[ "\$line" == DISABLED_PLUGIN:* ]]; then
            ORIGINAL_PATH="\${line#DISABLED_PLUGIN:}"
            if [ -d "\${ORIGINAL_PATH}.disabled" ]; then
                mv "\({ORIGINAL_PATH}.disabled" "\)ORIGINAL_PATH"
                log_msg "Откат выполнен успешно: Восстановлен плагин [\$ORIGINAL_PATH]"
                echo "Восстановлен старый плагин: \$ORIGINAL_PATH"
            fi
        fi
    done < "\$LOG_FILE"

    if [ -d "\$PLUGIN_DIR" ]; then
        rm -rf "\$PLUGIN_DIR"
        log_msg "Файлы плагина Amd64 Hardware Viewer полностью стерты."
    fi

    echo "------------------------------------------------------------------"
    echo "  Деинсталляция успешно выполнена!"
    echo "  Все конфликтующие плагины возвращены в исходное рабочее состояние."
    echo "------------------------------------------------------------------"
    rm -f "\$LOG_FILE"
}

if [ "\$1" == "--uninstall" ]; then
    run_uninstall
else
    run_install
fi
EOF

    chmod +x "\$DIST_DIR/payload/install_logic.sh"

    echo "Упаковка и компрессия бинарников алгоритмом XZ Extreme..."
    cd "\$DIST_DIR"
    tar -cf - payload | xz -9e > payload.tar.xz

    cat << 'EOF' > install.sh
#!/usr/bin/env bash
# ==============================================================================
# Самораспаковывающийся дистрибутив Far Hardware Viewer под архитектуру AMD64
# Используйте ключ --uninstall для полной очистки плагина и отката бэкапов
# ==============================================================================

UNINSTALL_FLAG=""
if [ "\$1" == "--uninstall" ]; then
    UNINSTALL_FLAG="--uninstall"
    echo "Инициализация деинсталлятора..."
else
    echo "Инициализация инсталлятора..."
fi

TMP_UNPACK_DIR=\$(mktemp -d)
ARCHIVE_START_LINE=\$(awk '/^__ARCHIVE_DATA_MARKER__/ {print NR + 1; exit 0;}' "\$0")

tail -n +"\$ARCHIVE_START_LINE" "\$0" | tar -xJf - -C "\$TMP_UNPACK_DIR"

cd "\$TMP_UNPACK_DIR/payload"
./install_logic.sh \$UNINSTALL_FLAG

rm -rf "\$TMP_UNPACK_DIR"
exit 0
__ARCHIVE_DATA_MARKER__
EOF

    cat payload.tar.xz >> install.sh
    chmod +x install.sh
    mv install.sh ../amd64_viewer_installer.sh
    
    cd ..
    rm -rf "\$DIST_DIR"
    
    echo "======================================================================"
    echo "  Готово! Монолитный SFX-инсталлятор собран: ./amd64_viewer_installer.sh"
    echo "======================================================================"
fi

echo "Процесс завершен. Бинарные файлы готовы."

