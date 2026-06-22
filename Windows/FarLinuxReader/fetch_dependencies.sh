#!/usr/bin/env bash
# ===========================================================================
# ФАЙЛ: fetch_dependencies.sh
# НАЗНАЧЕНИЕ: Модуль загрузки Latest Stable исходных кодов и RO-модификации
# Среда исполнения: MSYS2 UCRT64 (Windows 10 / 11)
# Актуальность: Июнь 2026 года
# ===========================================================================
set -euo pipefail

# Цветовая палитра для форматированного вывода логов в консоль MSYS2
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
RED='\033[0;0m'
NC='\033[0;0m'

echo -e "${GREEN}===[ Старт фазы загрузки внешних компонентов, ATFTP и SDK ]===${NC}"

# Подготовка структуры изолированного каталога сборки
mkdir -p build/zstd build/atftp

# Унифицированная функция для перехвата URL архива последнего стабильного релиза через GitHub API
get_github_latest_release_tarball() {
    local repo=$1
    local api_url="https://api.github.com/repos/$repo/releases/latest"

    # Запрос к GitHub API
    local response=$(curl -s \
        -H "Accept: application/vnd.github.v3+json" \
        -H "X-GitHub-Api-Version: 2022-11-28" \
        --connect-timeout 10 \
        "$api_url")

    local tarball_url=$(echo "$response" | jq -r '.tarball_url')
    if [ "$tarball_url" = "null" ] || [ -z "$tarball_url" ]; then
        echo -e "${RED}Критическая ошибка: Не удалось получить метаданные релиза для ${repo}${NC}" >&2
        exit 1
    fi
    echo "$tarball_url"
}

# Функция для загрузки и распаковки архива с GitHub
download_and_extract() {
    local repo=$1 target_dir=$2 shift 2
    local url=$(get_github_latest_release_tarball "$repo")
    local archive="build/$(basename "$repo").tar.gz"

    curl -s -L "$url" -o "$archive"
    tar -xzf "$archive" -C "$target_dir" --strip-components=1
    rm -f "$archive"
}

# ---------------------------------------------------------------------------
# ПУНКТ 1: ЗАГРУЗКА FAR MANAGER 3 SDK
# ---------------------------------------------------------------------------
if [ ! -f "build/plugin.hpp" ]; then
    echo -e "${YELLOW}[1/7] Загрузка FAR3 SDK из FarGroup/FarManager...${NC}"
    FAR_API_RESP=$(curl -s -H "Accept: application/vnd.github.v3+json" -H "X-GitHub-Api-Version: 2022-11-28" "https://api.github.com/repos/FarGroup/FarManager/releases/latest")
    FAR_TAG=$(echo "$FAR_API_RESP" | jq -r '.tag_name')
    curl -s -L "https://raw.githubusercontent.com/FarGroup/FarManager/$FAR_TAG/far/plugin.hpp" -o build/plugin.hpp
fi

# ---------------------------------------------------------------------------
# ПУНКТ 2: ЗАГРУЗКА БИБЛИОТЕКИ СЖАТИЯ ZSTD
# ---------------------------------------------------------------------------
echo -e "${YELLOW}[2/7] Загрузка ZSTD из facebook/zstd...${NC}"
download_and_extract "facebook/zstd" "build/zstd"
cp build/zstd/lib/decompress/zstd_decompress.c build/zstd/
cp build/zstd/lib/decompress/huf_decompress.c build/zstd/
cp build/zstd/lib/common/fse_decompress.c build/zstd/
cp build/zstd/lib/common/entropy_common.c build/zstd/
cp build/zstd/lib/common/zstd_common.c build/zstd/
cp build/zstd/lib/common/error_private.c build/zstd/
cp build/zstd/lib/common/*.h build/zstd/
cp build/zstd/lib/decompress/*.h build/zstd/
cp build/zstd/lib/zstd.h build/zstd/

# ---------------------------------------------------------------------------
# ПУНКТ 3: ЗАГРУЗКА БИБЛИОТЕКИ LZ4
# ---------------------------------------------------------------------------
echo -e "${YELLOW}[3/7] Загрузка LZ4 из lz4/lz4...${NC}"
download_and_extract "lz4/lz4" "build"
tar -xzf build/lz4.tar.gz -C build --strip-components=3 "*/lib/lz4.c" "*/lib/lz4.h"
rm -f build/lz4.tar.gz

# ---------------------------------------------------------------------------
# ПУНКТ 4: ЗАГРУЗКА АЛГОРИТМА ХЭШИРОВАНИЯ xxHash
# ---------------------------------------------------------------------------
echo -e "${YELLOW}[4/7] Загрузка xxHash из Cyan4973/xxHash...${NC}"
download_and_extract "Cyan4973/xxHash" "build"
tar -xzf build/xxhash.tar.gz -C build --strip-components=1 "*/xxhash.c" "*/xxhash.h"
rm -f build/xxhash.tar.gz

# ---------------------------------------------------------------------------
# ПУНКТ 5: ЗАГРУЗКА СВЕРХЛЕГКОГО ДЕКОМПРЕССОРА MiniLZO
# ---------------------------------------------------------------------------
echo -e "${YELLOW}[5/7] Загрузка MiniLZO из max-ic/minilzo...${NC}"
curl -s -L "https://raw.githubusercontent.com/max-ic/minilzo/master/minilzo.c" -o build/minilzo.c
curl -s -L "https://raw.githubusercontent.com/max-ic/minilzo/master/minilzo.h" -o build/minilzo.h
curl -s -L "https://raw.githubusercontent.com/max-ic/minilzo/master/lzodefs.h" -o build/lzodefs.h

# ---------------------------------------------------------------------------
# ПУНКТ 6: ЗАГРУЗКА SINGLE-HEADER МИНИ-АРХИВАТОРА MiniZ (Gzip API)
# ---------------------------------------------------------------------------
echo -e "${YELLOW}[6/7] Загрузка MiniZ из richgel999/miniz...${NC}"
curl -s -L "https://raw.githubusercontent.com/richgel999/miniz/master/miniz.c" -o build/miniz.c
curl -s -L "https://raw.githubusercontent.com/richgel999/miniz/master/miniz.h" -o build/miniz.h

# ---------------------------------------------------------------------------
# ПУНКТ 7: ЗАГРУЗКА ИСХОДНОГО КОДА КЛИЕНТА ATFTP
# ---------------------------------------------------------------------------
echo -e "${YELLOW}[7/7] Загрузка ATFTP из madmartin/atftp...${NC}"
download_and_extract "madmartin/atftp" "build/atftp"

# Извлекаем и перемещаем только изолированные С-файлы протокола в директорию сборки
cp build/atftp/tftp_io.c build/atftp_io.c 2>/dev/null || cp build/atftp/tftp/tftp_io.c build/atftp_io.c
cp build/atftp/tftp_def.c build/tftp_def.c 2>/dev/null || cp build/atftp/tftp/tftp_def.c build/tftp_def.c
cp build/atftp/*.h build/ 2>/dev/null || cp build/atftp/tftp/*.h build/

# Полная зачистка временных tar.gz архивов перед фазой фильтрации
rm -f build/*.tar.gz

# ===========================================================================
# ФАЗА ФИЛЬТРАЦИИ: ГЛУБОКОЕ ВЫРЕЗАНИЕ ИНТЕРФЕЙСОВ ЗАПИСИ И ОПКОДОВ WRQ
# ===========================================================================
echo -e "${GREEN}===[ Синхронизация завершена. Запуск потокового вырезания кода записи ]===${NC}"

# 1. Затираем вызовы модификации данных в системных функциях С‑библиотек декомпрессоров
find build/ -type f \( -name "*.c" -o -name "*.h" \) -exec sed -i \
    -e 's/\bwrite\s*(/(-1); \/\/ Вырезано: write /g' \
    -e 's/\bpwrite\s*(/(-1); \/\/ Вырезано: pwrite /g' \
    -e 's/\bunlink\s*(/(-1); \/\/ Вырезано: unlink /g' \
    -e 's/\bmkdir\s*(/(-1); \/\/ Вырезано: mkdir /g' \
    -e 's/\brmdir\s*(/(-1); \/\/ Вырезано: rmdir /g' \
    -e 's/\bchown\s*(/(-1); \/\/ Вырезано: chown /g' \
    -e 's/\bchmod\s*(/(-1); \/\/ Вырезано: chmod /g' {} +

# 2. Вырезаем поддержку опкода WRQ (Write Request, Opcode 2) на уровне парсера ATFTP
if [ -f "build/atftp_io.c" ]; then
    sed -i 's/\bTFTP_WRQ\b/0xFFFF/g' build/atftp_io.c
    sed -i 's/tftp_send_wrq/NULL \/\/ Вырезано /g' build/atftp_io.c
fi

echo -e "${GREEN}===[ Все внешние исходные коды успешно переведены в режим Read-Only ]===${NC}"
