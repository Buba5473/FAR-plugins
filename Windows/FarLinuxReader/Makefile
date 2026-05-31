# =========================================================================
#  Makefile под Intel Core i5 с изоляцией артефактов в подкаталоге build/
#  Проект: Монолитный User-Mode драйвер-плагин "linux_fs_reader.dll"
#  Особенности: Межмодульная LTO-компоновка, Loop Unrolling, Zero-CRT, AVX2
#  Дата актуализации спецификаций: Май 2026 года
# =========================================================================

# Настройка инструментов и компиляторов среды MSYS2 UCRT64
CXX      = g++
CC       = gcc
WINDRES  = windres
STRIP    = strip

# Определение подкаталога сборки для изоляции мусора
BUILD_DIR = build

# ФЛАГИ ОПТИМИЗАЦИИ ПОД INTEL CORE i5 (С полной аппаратной поддержкой amd64):
# -march=core-avx2     : оптимизация генерации инструкций под конвейер процессоров Intel Core.
# -mtune=core-avx2     : тонкая балансировка планировщика команд под кэш-линейки и декодеры Intel.
# -fomit-frame-pointer : освобождает регистр общего назначения RBP под нужды математики трансляции.
# -funroll-loops       : разворачивает циклы, утилизируя буфер Loop Stream Detector (LSD) Intel.
# -flto=auto           : сквозная Link-Time оптимизация кода на этапе межмодульной линковки.
# -fno-stack-protector : отключает оверхед системных проверок стека для сборки без привязки к libc.
INTEL_FLAGS  = -O3 -march=core-avx2 -mtune=core-avx2 -mavx2 -msse4.2 -flto=auto \
               -nostdlib -fno-stack-protector -fno-common -ffast-math \
               -fomit-frame-pointer -funroll-loops

CXXFLAGS     = $(INTEL_FLAGS) -std=c++17 -Wall -m64 -fno-rtti -fno-exceptions
CFLAGS       = $(INTEL_FLAGS) -Wall -m64

# Флаги компоновщика (Обязателен -flto для проведения финальной глобальной сквозной оптимизации)
LDFLAGS      = -shared -Wl,--entry=DllMain -Wl,--kill-at -flto=auto \
               -lkernel32 -luser32 -lole32 -lws2_32

# Целевой бинарник временно собирается внутри изолированной папки build/
TARGET       = $(BUILD_DIR)/linux_fs_reader.dll

# Компактный список исходных файлов C++ монолитной архитектуры
SRCS         = Plugin.cpp \
               LinuxReaderIo.cpp \
               LinuxReaderFsParsers.cpp \
               LinuxFsNetwork.cpp

# Исходные файлы C сторонних зависимостей (Zstd, LZ4, xxHash, miniz, minilzo)
C_SRCS       = zstd/zstd_decompress.c \
               zstd/huf_decompress.c \
               xxhash.c \
               lz4.c \
               miniz.c \
               minilzo.c

# Файл ресурсов (Манифесты совместимости Windows 10/11 и таблицы версий)
RC_SRC       = Resource.rc

# Автоматическое перенаправление путей всех объектных файлов в подкаталог build/
OBJS         = $(addprefix $(BUILD_DIR)/, $(SRCS:.cpp=.o) $(C_SRCS:.c=.o) $(RC_SRC:.rc=.o))

.PHONY: all clean

all: $(BUILD_DIR) clean $(TARGET)

# Создание структуры каталогов внутри build/ перед началом компиляции
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BUILD_DIR)/zstd

# Линковка объектных файлов внутри каталога сборки и глубокий стриппинг
$(TARGET): $(OBJS)
	@echo "Запуск укрупненной LTO-линковки монолитного бинарника в $(BUILD_DIR)..."
	$(CXX) $(OBJS) -o $(TARGET) $(LDFLAGS)
	@echo "Глубокий стриппинг отладочной информации из готовой DLL..."
	$(STRIP) --strip-all $(TARGET)

# Компиляция файлов C++ с размещением .o в подкаталоге build/
$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Компиляция файлов C с размещением .o в подкаталоге build/
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Компиляция ресурсов Windows (.rc) внутри подкаталога build/
$(BUILD_DIR)/%.o: %.rc
	@echo "Компиляция легитимного манифеста и ресурсов версии..."
	$(WINDRES) $< -o $@

# Полная очистка строго внутри изолированного подкаталога
clean:
	@echo "Очистка подкаталога $(BUILD_DIR) от объектных файлов и старых DLL..."
	rm -rf $(BUILD_DIR)/*
