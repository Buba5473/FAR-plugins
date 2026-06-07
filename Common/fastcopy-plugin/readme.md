# FastCopy SuperEngine AMD64

[English version below](#fastcopy-superengine-amd64-en)

## Описание (RU)

**FastCopy SuperEngine AMD64** — это монументальный, глубоко оптимизированный кроссплатформенный (Windows и Linux) плагин для файловых менеджеров **Far Manager 3** и **far2l**, написанный на современном стандарте **C++23**. Плагин полностью замещает стандартные процедуры копирования и перемещения файлов, выжимая максимум производительности из подсистемы ввода-вывода современных процессоров архитектуры AMD64 (x86-64-v3).

### Ключевые архитектурные особенности

1. **Аппаратное ускорение SHA-256 (SHA-NI):** Полный отказ от медленного CRC32. Расчет полноценного криптографического хэша SHA-256 переложен на аппаратный сопроцессор кремния AMD64 с помощью векторных интринсиков (`_mm_sha256rnds2_epu32`). Расчет идет «на лету» на полной скорости работы интерфейса накопителя.
2. **PMR Buffer Pool (Zero Allocations):** Полное исключение фрагментации оперативной памяти. Плагин работает поверх заранее выделенной статической структуры `std::pmr::monotonic_buffer_resource`, нарезая буферы обмена за 0 наносекунд без обращений к аллокатору ОС во время копирования.
3. **Double Buffering Конвейер:** Реализация упреждающего асинхронного чтения/записи. Пока один 4-мегабайтный буфер сбрасывается на физический диск, второй параллельно вычитывает следующий блок данных из источника, минимизируя простои контроллера диска.
4. **Борьба с фрагментацией (Из FileCopyEx3):** Плагин производит жесткую преаллокацию секторов на диске (`SetEndOfFile` в Windows и нативный `fallocate` в Linux) под размер файла до начала итераций записи. Это снижает фрагментацию таблиц MFT/Extents и увеличивает линейную скорость.
5. **Поддержка NTFS Streams и атрибутов:** Перенос альтернативных потоков данных (ADS), а также нативная активация аппаратных флагов сжатия и шифрования NTFS при создании целевого файла на Windows.
6. **Token Bucket Rate Limiter:** Динамический ограничитель скорости, отслеживающий тайминги с точностью до микросекунд. Позволяет разгрузить шину накопителя для комфортной работы за ПК.
7. **Lock-Free Очередь Задач:** Межпотоковое взаимодействие между UI-потоком Far SDK и воркером ввода-вывода построено на атомарных индексах без использования тяжелых мьютексов блокировки.

---

## Управление и горячие клавиши

Во время активной операции копирования или перемещения плагин выводит двухстрочное графическое окно прогресса (прогресс текущего файла + общий прогресс всей пачки файлов).

* **`B`** (или **`b`** на Linux) — Мгновенно убрать операцию в **фоновый режим**. Окно прогресса скроется, управление вернется панелям Far Manager.
* **`Ctrl + Alt + F`** (в Windows) или **`Ctrl + F`** (в Linux терминале) — **Вернуть плагин из фона** обратно на передний план с актуализацией всех счетчиков.
* **`L`** (или **`l`** на Linux) — Мгновенно активировать **лимит скорости** на уровне 50 МБ/с.
* **`Esc`** — **Отмена операции**. Поток ввода-вывода корректно прервется, а текущий недописанный файл будет удален с диска для предотвращения появления битых данных.

> По завершении фоновой операции плагин издает звуковой сигнал (`MessageBeep` / ASCII Bell `\a`) и выбрасывает системное Toast-уведомление поверх рабочего стола (в Linux задействуется демон `notify-send`).

---

## Сборка и развертывание

Вся сборка полностью автоматизирована и изолирована с помощью управляющего скрипта `build_system.sh` внутри среды **MSYS2 UCRT64** или нативного Linux. 

### Параметры скрипта сборки:
* `--platform <windows|linux>` — Выбор целевой платформы (по умолчанию `windows`).
* `--installer` — Создание самораспаковывающегося `.sh` инсталлятора (применимо только для платформы `linux`).

### Пошаговые команды компиляции:

1. Откройте консоль **MSYS2 UCRT64** и перейдите в каталог проекта.
2. Выдайте права на исполнение скрипту:
   ```bash
   chmod +x build_system.sh
   ```
3. **Сборка под Windows (Far Manager 3):**
   ```bash
   ./build_system.sh --platform windows
   ```
   *Результат:* Скрипт очистит папку `build_out`, скачает `plugin.hpp` от FarGroup, скомпилирует `fastcopy.dll` и запишет файлы контекстной справки `fastcopy_ru.hlf` / `fastcopy_en.hlf` в кодировке **UTF-8 с BOM**.
4. **Сборка под Linux (far2l) + Инсталлятор:**
   ```bash
   ./build_system.sh --platform linux --installer
   ```
   *Результат:* Скрипт скачает `farplug-wide.h` от elfmz, скомпилирует `fastcopy.plg`, запишет HLF-справки в **UTF-8 без BOM** и упакует всё в монолитный SFX-архив `fastcopy_installer.sh`.

---

## Установка и удаление в Linux

### Установка:
Перенесите сгенерированный file `fastcopy_installer.sh` на целевую Linux-систему и запустите:
```bash
./fastcopy_installer.sh
```
Инсталлятор автоматически:
* Сделает бэкап конфликтующих стандартных плагинов копирования в `~/.config/far2l/plugins_backup/`.
* Развернет плагин и файлы мультиязычной справки в каталог плагинов `far2l`.
* Запишет лог установки в `/var/log/fastcopy_install.log`.

### Удаление (Откат изменений):
Для полного удаления плагина и возврата дефолтных модулей менеджера на место запустите инсталлятор с флагом деинсляции:
```bash
./fastcopy_installer.sh --uninstall
```

---

# FastCopy SuperEngine AMD64 (EN)

## Description (EN)

**FastCopy SuperEngine AMD64** is a monumental, deeply optimized, cross-platform (Windows and Linux) plugin for **Far Manager 3** and **far2l** file managers, written in compliance with the modern **C++23** standard. The plugin completely supersedes standard file copying and moving procedures, squeezing maximum I/O subsystem performance out of modern AMD64 (x86-64-v3) architecture processors.

### Key Architectural Features

1. **Hardware-Accelerated SHA-256 (SHA-NI):** Complete removal of slow CRC32 algorithms. Calculation of a cryptographically secure SHA-256 hash is offloaded to the AMD64 hardware silicon co-processor via vector intrinsics (`_mm_sha256rnds2_epu32`). The calculation runs on-the-fly at the maximum interface bandwidth of the storage drive.
2. **PMR Buffer Pool (Zero Allocations):** Complete elimination of RAM fragmentation. The plugin operates on top of a pre-allocated static `std::pmr::monotonic_buffer_resource` structure, slicing swap buffers in 0 nanoseconds without hitting the OS allocator during active copying operations.
3. **Double Buffering Pipeline:** Implementation of look-ahead asynchronous read/write operations. While one 4MB buffer is being flushed onto the physical disk, the second buffer simultaneously reads the next block of data from the source, minimizing disk controller downtime.
4. **Anti-Fragmentation Logic (From FileCopyEx3):** The plugin performs strict drive sector pre-allocation (`SetEndOfFile` on Windows and native `fallocate` on Linux) matching the file size prior to running write iterations. This reduces MFT/Extents table fragmentation and maximizes linear transfer speed.
5. **NTFS Streams and Attributes Support:** Preserves alternative data streams (ADS) and natively activates NTFS hardware compression and encryption flags when creating target files on Windows.
6. **Token Bucket Rate Limiter:** A dynamic speed limiter tracking precise timings down to microseconds. It unloads the storage bus, keeping the PC responsive for user multi-tasking.
7. **Lock-Free Task Queue:** Inter-thread communication between the Far SDK UI thread and the I/O worker is built on atomic indexes without utilizing heavy locking mutexes.

---

## Operation and Hotkeys

During an active copy or move operation, the plugin displays a dual-bar graphical progress window (current file progress + total progress of the entire task queue).

* **`B`** (or **`b`** on Linux) — Instantly push the operation into the **background**. The progress window hides, and control returns to the Far Manager panels.
* **`Ctrl + Alt + F`** (on Windows) or **`Ctrl + F`** (in Linux terminal) — **Restore the plugin from the background** back to the foreground, bringing up all active counters.
* **`L`** (or **`l`** on Linux) — Instantly activate a **speed limit** capped at 50 MB/s.
* **`Esc`** — **Cancel the operation**. The I/O pipeline safely terminates, and the current incomplete file is deleted from the drive to prevent corrupt data generation.

> Upon completion of a background task, the plugin emits an audio signal (`MessageBeep` / ASCII Bell `\a`) and triggers a system Toast notification over the desktop environment (uses the `notify-send` daemon on Linux).

---

## Building and Deployment

The build process is fully automated and isolated using the `build_system.sh` control script within an **MSYS2 UCRT64** environment or native Linux.

### Build Script Parameters:
* `--platform <windows|linux>` — Choose target platform (defaults to `windows`).
* `--installer` — Generate a self-extracting `.sh` installer script (applicable only to `linux` platform targets).

### Step-by-Step Compilation Commands:

1. Open the **MSYS2 UCRT64** terminal and navigate to the project directory.
2. Grant execution permissions to the script:
   ```bash
   chmod +x build_system.sh
   ```
3. **Build for Windows (Far Manager 3):**
   ```bash
   ./build_system.sh --platform windows
   ```
   *Result:* The script purges the `build_out` directory, downloads `plugin.hpp` from FarGroup, compiles `fastcopy.dll`, and writes the context help files `fastcopy_ru.hlf` / `fastcopy_en.hlf` encoded in **UTF-8 with BOM**.
4. **Build for Linux (far2l) + Installer Pack:**
   ```bash
   ./build_system.sh --platform linux --installer
   ```
   *Result:* The script downloads `farplug-wide.h` from elfmz, compiles `fastcopy.plg`, generates HLF help maps in **UTF-8 without BOM**, and bundles everything into a monolithic SFX archive named `fastcopy_installer.sh`.

---

## Installation and Uninstallation in Linux

### Installation:
Transfer the generated `fastcopy_installer.sh` file to the target Linux system and run:
```bash
./fastcopy_installer.sh
```
The installer automatically executes the following:
* Backs up conflicting default copy plugins into `~/.config/far2l/plugins_backup/`.
* Deploys the plugin binary and multi-language help systems into the `far2l` plugins directory.
* Appends installation logs into `/var/log/fastcopy_install.log`.

### Uninstallation (Rollback):
To completely remove the plugin and restore the file manager's default modules to their original state, execute the installer using the uninstall flag:
```bash
./fastcopy_installer.sh --uninstall
```
