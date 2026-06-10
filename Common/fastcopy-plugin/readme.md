# FastCopy Plugin for FAR Manager 3, far2l & far2m

[Русский] | [English]

---

## Русский

Высокопроизводительный кроссплатформенный плагин для **FAR Manager 3 (Windows 10+)**, **far2l** и **far2m (Linux Kernel 6.0+)**, спроектированный для обеспечения максимальной скорости копирования и перемещения файлов. Плагин полностью заменяет или дополняет стандартные дисковые операции, утилизируя современные асинхронные интерфейсы ввода-вывода и технологии Zero-Copy на уровне ядер операционных систем.

### Ключевые особенности и низкоуровневые оптимизации

*   **Linux (Ядро 6.0+ / far2l / far2m):**
    *   **Технология Zero-Copy:** Использование системных вызовов `FICLONE` (мгновенное клонирование рефлинков на файловых системах Btrfs и XFS) и `copy_file_range(2)` для перекачки данных внутри адресного пространства ядра без копирования в user-space оперативную память плагина.
    *   **Асинхронный движок `io_uring`:** Пул асинхронного ввода-вывода с флагами ядра `IORING_SETUP_COOP_TASKRUN` и `IORING_SETUP_SINGLE_ISSUER`, обеспечивающий максимальную утилизацию очередей NVMe/SSD накопителей без накладных расходов на переключение контекста потоков.
    *   **Бесконтактный пре-чек (`statx`):** Быстрая проверка метаданных файлов через сисколл `statx(2)` с флагом `AT_STATX_DONT_SYNC`. Плагин запрашивает строго время модификации и размер файла, исключая сетевые задержки (round-trips) на NFS/Samba шарах и не открывая дескрипторы файлов (защита от ошибки `EMFILE`).
    *   **Сверхбыстрый Move:** Атомарное перемещение папок и файлов через системный вызов `renameat2(2)` на уровне метаданных inode (занимает ~1 микросекунду внутри одного раздела). При междисковом переносе (`EXDEV`) прозрачно перетекает в конвейер `io_uring` -> `unlink`.
    *   **Умное FUSE-ветвление:** Автоматическое распознавание путей локального монтирования сетевых ресурсов, контейнеров и FUSE-драйверов (sshfs, rclone) в каталогах `~/.local/share/far2l(far2m)/mnt/`.
    *   **Динамический размер буфера (Dynamic Chunk Size):** Плагин анализирует размер файла перед стартом операции. Для мелких файлов буфер сжимается, предотвращая лишние аллокации. Для крупных файлов (`> 50 МБ`) буфер расширяется до `8 МБ`, снижая нагрузку на CPU при переключении контекста в очередях `io_uring`.
    *   **Очистка Page Cache (`posix_fadvise`):** После завершения каждой асинхронной итерации записи данных ядру отдается команда `POSIX_FADV_DONTNEED`, принудительно освобождающая оперативную память от этого блока и защищающая ОЗУ от забивания дисковым кэшем страниц при переносе терабайтных массивов.
*   **Windows (10+ / Server 2016+):**
    *   **Прямой асинхронный I/O (`NO_BUFFERING`):** Использование `CreateFileW` с флагами `FILE_FLAG_NO_BUFFERING` и `FILE_FLAG_OVERLAPPED` совместно с современным асинхронным API `CopyFile2`. Полностью обходит системный кэш страниц ОС (Page Cache), исключая забивание ОЗУ «мусором» при переносе терабайтных массивов данных.
    *   **Паритет функций NTFS (FileCopyEx3):** Полная поддержка альтернативных потоков данных (**ADS**) через `FindFirstStreamW`, сохранение прозрачного NTFS сжатия (`FSCTL_SET_COMPRESSION`) и шифрования Windows EFS (`EncryptFileW`).
    *   **Технология `\\?\` (MAX_PATH Bypass):** Автоматическое экранирование путей перед передачей в Win32 API. Снимает ограничение Windows в 260 символов, расширяя лимит длины пути до 32 767 символов.
*   **Глобальные архитектурные фичи:**
    *   **Технология №1: Drive Coalescing (Контроль коллизий накопителей):** Плагин автоматически выстраивает задачи в строгую последовательную очередь (FIFO), если они пытаются одновременно писать или читать с одного физического шпинделя или контроллера (детекция букв дисков в Windows и точек монтирования в Linux), защищая накопители от падения скорости из-за коллизий.
    *   **Технология №3: Верификация на лету (Inline Hashing XXH3):** Контрольная сумма рассчитывается инкрементально в процессе передачи буфера в фоновом потоке, полностью исключая необходимость повторного чтения файлов диском после копирования. Нагрузка на дисковую шину падает ровно на 50%.
    *   **Санация путей:** Встроенная функция-санация на лету заменяет недопустимые для NTFS/FAT символы (`:`, `*`, `?`, `"`, `<`, `>`, `|`) на безопасные дефисы, предотвращая падение копирования с ошибкой `ERROR_INVALID_NAME`.
    *   **Монолитная архитектура:** Весь исполняемый код плагина, включая языковые ресурсы (`.lng`) и пакет смарт-макросов Lua, полностью вшит внутрь бинарника (`.dll` / `.so`) в виде сырых UTF-8 литералов и автоматически разворачивается на диске при первом запуске или обновлении версии (по маркеру `version.txt`).
    *   **Умный Focus Assist акустический сигнал:** Финальный звуковой сигнал окончания очереди автоматически глушится, если в реестре Windows активен режим «Не беспокоить» (Focus Assist).

---

### Архитектура и структура файлов проекта

Проект сведен к минимально возможному и безопасному количеству файлов (**4 исходных файла C++20**), что ускоряет компиляцию и позволяет компиляторам проводить агрессивный инлайнинг кода.

```text
my_fastcopy_plugin/
├── CMakeLists.txt              # Скрипт сборки с глубоким разгоном (AVX2, FMA, LTO, Ninja)
├── plugin.def                  # Файл экспорта функций для линкера Windows
├── build_all.sh                # Автоматический скрипт компиляции и сборки инсталлятора (Ninja)
└── src/                        # Оптимизированный слой исходного кода C++20
    ├── core.h                  # Очередь коллизий, xxHash, Logger, Focus Assist, встроенный Lua-код
    ├── main.cpp                # Точка входа, нативный перехват ввода, хоткеи, API Plugin.Call (add_task_batch)
    ├── win32_impl.cpp          # Системный монолит Windows 10+ (CopyFile2, \\?\, ADS, EFS, XXH3 на лету)
    └── linux_impl.cpp          # Системный монолит Linux 6+ (statx, io_uring, renameat2, FICLONE, fadvise, XXH3)
```

---

### Нативное управление (Встроенные хоткеи)

Плагин осуществляет глубокую интеграцию в TUI-интерфейс менеджеров FAR 3, far2l и far2m через перехват функций SDK:

1.  **Инжекция в стандартные диалоги (F5 / F6):** При нажатии обычных `F5` или `F6` плагин перехватывает событие `DE_DLGPROCINIT`, динамически расширяет окно вниз с помощью сообщения `DM_RESIZEDIALOG` (на основе данных `DM_GETDLGRECT`) и добавляет кастомную интерактивную кнопку `[ FastCopy Async ]` или `[ FastMove Inode ]` через `DM_SETDLGITEM`. При клике на неё плагин считывает все настроенные вами галочки (включая штатный фильтр FAR `Ctrl+I`), закрывает окно и забирает управление.
2.  **Прямые глобальные хоткеи (с панелей):**
    *   `Ctrl + Shift + F5` — Мгновенно запустить асинхронное копирование выделенных файлов/папок в каталог пассивной панели (без вывода диалоговых окон).
    *   `Ctrl + Shift + F6` — Мгновенно запустить фоновый перенос (сдвиг inode / атомарный Move) в каталог пассивной панели.
    *   `Ctrl + Shift + P` — Перевести фоновую FIFO-очередь в режим **Паузы** (или снять с нее). Фоновый поток мгновенно засыпает на уровне ядра ОС, снижая потребление CPU и диска до 0%. На панели выводится нативное TUI-уведомление `FastCopy: [PAUSED]`.

---

### Скриптовое расширение возможностей (API макросов)

Вы можете бесконечно расширять логику плагина (добавлять Telegram-уведомления, кастомные меню, интеграцию с буфером обмена ОС, гибкие регулярные выражения), не перекомпилируя C++ код. Плагин экспортирует в макро-движки функцию `OpenW` (команда `Plugin.Call`) с поддержкой GUID `4A9E71F3-12A4-4D62-BC1F-72864C62F18A`.

Плагин автоматически разворачивает встроенную эталонную версию макросов на диске при первом старте (в подпапку `scripts/` под Windows или в корень под Linux). Ниже подробно описаны встроенные скриптовые сценарии, к которым вы можете привязать свои хоткеи или пункты меню. Для стопроцентной совместимости с нативным **FAR 3 SDK**, Lua-макросы упаковывают массивы на месте через `table.unpack()`, а C++ ядро принимает их через плоскую команду **`add_task_batch`**.

#### 1. Сценарий: Раскладка файлов по подпапкам (Alt + Shift + F5)
Позволяет распределить выделенные на панели файлы в каталог, на котором в данный момент установлен курсор, без переходов на соседнюю панель.
*   **Как это работает:** Макрос считывает атрибуты элемента под курсором, проверяет, что это папка, и вызывает C++ API командной строкой: `Plugin.Call(GUID, "add_task_batch", target_dir, target_item.FileName)`.

#### 2. Сценарий: Умное бэкапирование и Сетевое / FUSE ветвление (Ctrl + Shift + B)
Создает резервную копию файлов на пассивной панели внутри папки с уникальным таймштампом вида `backup_YYYY-MM-DD_HH-MM-SS`.
*   **Как это работает:** Скрипт генерирует имя папки через `os.date` и шлет задачу в плагин. Нативное C++ ядро через функцию `IsNetworkPathStr` анализирует пути (включая локальные точки монтирования FUSE, такие как `sshfs`, `rclone` в каталогах `~/.local/share/far2l(far2m)/mnt/` или Windows UNC). Если обнаруживается сетевая ФС или удаленная точка монтирования, плагин автоматически переключает планировщик на щадящий последовательный режим, а дисковую подсистему — в безопасный монопоточный режим для стабильности сетевого стека.

#### 3. Сценарий: Смарт-фильтр + Динамический Git Skip-лист (.gitignore) (Ctrl + Alt + F5)
Осуществляет глубокую предварительную фильтрацию выделенных массивов файлов силами скриптового движка с автоматическим парсингом правил локальных репозиториев.
*   **Как это работает:** Скрипт проверяет наличие локального файла `.gitignore` в текущем каталоге активной панели. В случае его обнаружения, макрос динамически парсит его строки, трансформирует правила Git в регулярные выражения Lua и объединяет их со встроенным Skip-листом (блокировка `.git`, `node_modules`, `__pycache__`, `.DS_Store`). Сформированный чистый массив строк распаковывается через `table.unpack()` и скармливается C++ ядру за один вызов: `Plugin.Call(GUID, "add_task_batch", passive_dir, table.unpack(clean_paths))`. Это исключает избыточные дисковые итерации по заблокированным или мусорным файлам проекта.

#### 4. Сценарий: Вставка файлов из буфера обмена ОС (Ctrl + Shift + V)
Обеспечивает бесшовную интеграцию консольного интерфейса FAR/far2l/far2m и графической оболочки операционной системы (Проводник Windows, Рабочий стол GNOME/KDE).
*   **Как это работает:** Перехватывает комбинацию вставки, вызывает функцию `far.PasteFromClipboard()`, извлекает системные пути файлов (формат `CF_HDROP`), разбивает их по строкам, формирует массив, распаковывает его и передает плоским списком в фоновую асинхронную FIFO-очередь плагина: `Plugin.Call(GUID, "add_task_batch", current_dir, table.unpack(paths))`.

#### 5. Событийные макро-хуки (Plugin-to-Macro)
Внутри C++ конвейера обработки ошибок (`win32_impl.cpp` / `linux_impl.cpp`) принудительно зашит вызов внешних скриптов через `ACTL_SYNCHRO`. Если файл жестко заблокирован другой программой, плагин шлет в главный интерфейсный поток строку: `if FastCopy_OnLockedFile then FastCopy_OnLockedFile(path, code) end`.
Вы можете объявить в своем общем макро-файле функцию:
```lua
function FastCopy_OnLockedFile(filePath, errCode)
    -- Пример: автоматическое уничтожение процесса, заблокировавшего файл
    if not win then os.execute("fuser -k " .. filePath) end
end
```

---

### Сборка и деплой

Для сборки используется кроссплатформенный инструмент автоматизации **`build_all.sh`**. Запуск производится из терминала **MSYS2 UCRT64** (под Windows) или из стандартной Linux-консоли / WSL2. Скрипт полностью интерактивен: он проверит наличие пакетов `cmake`, `ninja`, `curl`, `tar`, `xz`, и в случае их отсутствия — сам предложит установить их через `pacman` или `apt`.

*   `./build_all.sh` — Сборка под Windows AMD64 с флагами глубокой оптимизации (`-O3 -flto -mavx2 -mfma`) с использованием генератора **Ninja**. Выходной монолитный файл: `target_dist/FastCopy.dll` (типичный размер ~231 КБ со вшитыми макросами).
*   `./build_all.sh amd64` — Сборка под Linux x86_64 (AVX2, FMA, LTO, `liburing`). Выходной файл: `target_dist/FastCopy.so`.
*   `./build_all.sh arm64` — Кросс-компиляция под общую архитектуру Linux ARM64 (Векторные инструкции NEON + аппаратный CRC32/Crypto для хэшей xxHash). Автоматически подтягивает пакет `aarch64-linux-gnu-gcc` в MSYS2.
*   `./build_all.sh ohc4` — Глубоко оптимизированная сборка под плату **Odroid HC4** с принудительным тюнингом планировщика под архитектуру ядер `Cortex-A73+Cortex-A53`.
*   `./build_all.sh --installer` — Компилирует версию для Linux ARM64 и упаковывает её со сжатием **XZ Extreme** в единый самораспаковывающийся скрипт установки `target_dist/install.sh`.

#### Работа с SH-инсталлятором на Linux (far2l & far2m):
*   **Установка:** Скопируйте `install.sh` на целевую машину (например, Odroid HC4) и запустите: `./install.sh`. Инсталлятор сам найдет пути far2l и far2m, создаст директории `~/.config/far2l(far2m)/plugins/fastcopy`, развернет туда монолитный бинарник и активирует перехват ввода.
*   **Деинсталляция (Откат изменений):** Запустите инсталлятор с флагом деинсталляции: `./install.sh --uninstall`. Он чисто сотрет папки плагина из обеих сред, вернув стандартное поведение файлового менеджера без ручной очистки «хвостов».

---

## English

A high-performance, cross-platform plugin for **FAR Manager 3 (Windows 10+)**, **far2l**, and **far2m (Linux Kernel 6.0+)**, engineered to deliver maximum file copying and moving speeds. The plugin fully replaces or complements standard disk operations by utilizing modern asynchronous I/O interfaces and Zero-Copy technologies directly at the OS kernel level.

### Key Features and Low-Level Optimizations

*   **Linux (Kernel 6.0+ / far2l / far2m):**
    *   **Zero-Copy Technology:** Leverages `FICLONE` system calls (instant reflink cloning on Btrfs and XFS file systems) and `copy_file_range(2)` to transfer data inside the kernel address space, entirely bypassing copying into the plugin's user-space RAM.
    *   **Asynchronous `io_uring` Engine:** An asynchronous I/O pool powered by kernel flags `IORING_SETUP_COOP_TASKRUN` and `IORING_SETUP_SINGLE_ISSUER`, ensuring maximum utilization of NVMe/SSD drive queues without thread context-switching overhead.
    *   **Contactless Pre-check (`statx`):** Fast file metadata verification via the `statx(2)` syscall with the `AT_STATX_DONT_SYNC` flag. The plugin requests strictly the modification time and file size, eliminating network round-trips on NFS/Samba shares and preventing `EMFILE` errors by not opening file descriptors.
    *   **Ultra-fast Move:** Atomic directory and file relocation using the `renameat2(2)` syscall at the inode metadata level (takes ~1 microsecond within the same partition). For cross-disk moves (`EXDEV`), it transparently falls back to the `io_uring` -> `unlink` pipeline.
    *   **Smart FUSE Branching:** Automatically maps local POSIX mount paths for network shares, containers, and FUSE drivers (sshfs, rclone) located inside `~/.local/share/far2l(far2m)/mnt/` directories.
    *   **Dynamic Chunk Size:** The plugin inspects individual file sizes before processing. Small files utilize shrunk memory limits to avoid allocation overhead, whereas massive files (`> 50 MB`) expand buffers up to `8 MB`, significantly mitigating CPU context-switch stress inside `io_uring` worker ring loops.
    *   **Page Cache Throttling (`posix_fadvise`):** After each asynchronous block iteration write commits to disk, the plugin forces a kernel `POSIX_FADV_DONTNEED` call. This flushes pages out of the OS Page Cache, preventing the operating system's RAM from being clogged with cached garbage during terabyte-scale operations.
*   **Windows (10+ / Server 2016+):**
    *   **Direct Asynchronous I/O (`NO_BUFFERING`):** Utilizes `CreateFileW` with `FILE_FLAG_NO_BUFFERING` and `FILE_FLAG_OVERLAPPED` flags combined with the modern asynchronous `CopyFile2` API. It completely bypasses the OS Page Cache, preventing RAM from being cluttered with cached garbage when transferring terabyte-scale datasets.
    *   **NTFS Feature Parity (FileCopyEx3):** Full support for Alternate Data Streams (**ADS**) via `FindFirstStreamW`, preservation of transparent NTFS compression (`FSCTL_SET_COMPRESSION`), and Windows EFS encryption (`EncryptFileW`).
    *   **`\\?\` Long Path Prefix Technology:** Forcibly formats path structures using the `\\?\` system escape namespace before hitting Win32 file execution APIs. This completely bypasses the restrictive 260-character Win32 `MAX_PATH` boundary, elevating processing limits up to 32,767 Unicode symbols.
*   **Global Architectural Features:**
    *   **Drive Coalescing (Storage Collision Prevention):** The thread scheduler dynamically analyzes drive lettering (Windows volumes) or POSIX mount targets (Linux nodes). If multiple batch copy sessions hit the same physical mechanical disk spindle or host controller lane concurrently, the backend forces an ordered sequential queue (FIFO), protecting hard drives from speed degradation caused by heads thrashing.
    *   **Inline Hashing (Real-Time XXH3 Verification):** Executed via a stream-driven **xxHash (XXH3)** instance to map file chunks directly from memory inside asynchronous background I/O progress hooks. The file verification checksum is computed entirely on-the-fly, dropping necessary read-back operations by exactly 50% and protecting bus bandwidth.
    *   **Path Sanitization:** An intelligent string sanitizer translates illegal Windows destination path characters (`:`, `*`, `?`, `"`, `<`, `>`, `|`) into standard hyphens on-the-fly, blocking immediate I/O failures triggered by `ERROR_INVALID_NAME`.
    *   **Monolithic Architecture:** The entire executable code, including language resources (`.lng`) and the Lua smart-macro pack, is completely embedded inside the binary (`.dll` / `.so`) as raw UTF-8 string literals and automatically deploys on the disk upon the first launch or version update (tracked via `version.txt`).
    *   **Smart Focus Assist Acoustic Signal:** The final queue completion sound chime is automatically muted if Windows "Focus Assist" (Do Not Disturb) mode is active in the registry.

---

### Project File Structure and Architecture

The codebase is streamlined into a minimal and safe number of files (**4 source files using C++20**), which significantly accelerates compilation and allows compilers to perform aggressive inline code expansion.

```text
my_fastcopy_plugin/
├── CMakeLists.txt              # Build script with deep optimization (AVX2, FMA, LTO, Ninja)
├── plugin.def                  # Function export file for the Windows linker
├── build_all.sh                # Automated compilation and SH installer build script (Ninja)
└── src/                        # Optimized C++20 source code layer
    ├── core.h                  # Queue, xxHash, Logger, Focus Assist, embedded Lua code
    ├── main.cpp                # Entry point, native input hook, hotkeys, Plugin.Call API (add_task_batch)
    ├── win32_impl.cpp          # Windows 10+ system monolith (CopyFile2, \\?\, ADS, EFS, real-time XXH3)
    └── linux_impl.cpp          # Linux 6+ system monolith (statx, io_uring, renameat2, FICLONE, fadvise, XXH3)
```

---

### Native Controls (Built-in Hotkeys)

The plugin provides deep integration into the TUI interface of FAR 3, far2l, and far2m managers by hooking SDK events:

1.  **Standard Dialog Injection (F5 / F6):** When pressing regular `F5` or `F6`, the plugin intercepts the `DE_DLGPROCINIT` event, dynamically resizes the dialog box downwards using `DM_RESIZEDIALOG` (driven by `DM_GETDLGRECT`), and injects a custom interactive button `[ FastCopy Async ]` or `[ FastMove Inode ]` via `DM_SETDLGITEM`. Clicking it reads all user-configured checkboxes (including native FAR filters `Ctrl+I`), closes the window, and hands execution over to the optimized engine.
2.  **Direct Global Hotkeys (Panel-wide):**
    *   `Ctrl + Shift + F5` — Instantly triggers asynchronous copying of selected files/folders to the passive panel directory (bypassing all dialog windows).
    *   `Ctrl + Shift + F6` — Instantly triggers background relocation (inode shifting / atomic Move) to the passive panel directory.
    *   `Ctrl + Shift + P` — Toggles the background FIFO queue into **Pause** mode (or resumes it). The background thread instantly falls asleep at the OS kernel level, reducing CPU and disk utilization to absolute 0%. A native TUI notification `FastCopy: [PAUSED]` is shown on the screen.

---

### Scripted Extensibility (Macro API)

You can infinitely extend the plugin's logic (adding Telegram notifications, custom TUI menus, OS clipboard integration, flexible regular expression parsing) without recompiling the C++ code. The plugin exports the `OpenW` function to macro engines (via the `Plugin.Call` method) using the plugin GUID `4A9E71F3-12A4-4D62-BC1F-72864C62F18A`.

The plugin automatically extracts its embedded reference macro file onto the disk upon the first start (into the `scripts/` subfolder under Windows or the root directory under Linux). To maintain absolute compliance with **FAR 3 SDK specifications**, Lua scripts unpack array values natively via `table.unpack()`, feeding the backend a flat list of arguments evaluated by a streamlined **`add_task_batch`** command hook.

#### 1. Scenario: File Sorting into Subfolders (Alt + Shift + F5)
Allows distribution of selected files directly into the folder currently under the cursor, without switching to the opposite panel.
*   **How it works:** The macro reads the attributes of the item under the cursor, verifies that it is a directory, and executes the C++ API command: `Plugin.Call(GUID, "add_task_batch", target_dir, target_item.FileName)`.

#### 2. Scenario: Smart Backup and Network / FUSE Branching (Ctrl + Shift + B)
Creates a backup copy of files on the passive panel inside a folder labeled with a unique timestamp like `backup_YYYY-MM-DD_HH-MM-SS`.
*   **How it works:** The script generates the folder name using `os.date` and posts the task to the plugin. The native C++ core uses the `IsNetworkPathStr` function to analyze paths (checking for local FUSE mount points like `sshfs` or `rclone` mapped inside `~/.local/share/far2l(far2m)/mnt/` directories or Windows UNC). If a network filesystem or remote share is discovered, the plugin automatically throttles the architecture into a safe sequential planning lane, establishing singular I/O pipelines to guarantee network stack stability.

#### 3. Scenario: Dynamic Regex-based Git Skip-list (.gitignore) (Ctrl + Alt + F5)
Performs deep pre-filtering of selected file arrays using the macro engine's scripting power backed by local pattern configuration files.
*   **How it works:** The script looks for a local `.gitignore` configuration within the current folder of the active panel. If found, the macro dynamically parses its parameters, converts raw Git patterns into valid Lua regular expressions, and appends them to the internal static Skip-list (blocking `.git`, `node_modules`, `__pycache__`, and `.DS_Store` files). The cleaned string values are unfolded using `table.unpack()` and passed inside a singular operational transaction: `Plugin.Call(GUID, "add_task_batch", passive_dir, table.unpack(clean_paths))`. This completely eliminates redundant disk seeking and tree walking overhead.

#### 4. Scenario: Paste Files from OS Clipboard (Ctrl + Shift + V)
Provides seamless integration between the console interface of FAR/far2l/far2m and the graphical desktop environment (Windows Explorer, GNOME/KDE Desktop).
*   **How it works:** Intercepts the paste key combination, calls `far.PasteFromClipboard()`, extracts system file paths (`CF_HDROP` format), splits them by newline characters, formats an array, unlocks it, and streams it into the plugin's background async FIFO worker queue: `Plugin.Call(GUID, "add_task_batch", current_dir, table.unpack(paths))`.

#### 5. Event-Driven Macro Hooks (Plugin-to-Macro)
The C++ error-handling pipeline (`win32_impl.cpp` / `linux_impl.cpp`) features built-in notification routing via `ACTL_SYNCHRO`. If a file is locked by another program, the plugin pushes a string to the main thread: `if FastCopy_OnLockedFile then FastCopy_OnLockedFile(path, code) end`.
You can define a global hook function in your main macro files:
```lua
function FastCopy_OnLockedFile(filePath, errCode)
    -- Example: Automatically kill the process holding the file lock
    if not win then os.execute("fuser -k " .. filePath) end
end
```

---

### Compilation and Deployment

The cross-platform automation script **`build_all.sh`** handles the entire compilation pipeline. It must be executed from an **MSYS2 UCRT64** terminal (on Windows) or a standard native Linux console / WSL2 container. The script is interactive: it checks for the presence of `cmake`, `ninja`, `curl`, `tar`, `xz`, and cross-compilers. If any dependencies are missing, it prompts the user to install them via `pacman` or `apt` automatically.

*   `./build_all.sh` — Standard build for Windows AMD64 with deep vector optimizations enabled (`-O3 -flto -mavx2 -mfma`) driving the **Ninja** engine. Output monolith: `target_dist/FastCopy.dll` (standard file weight ~231 KB with compiled macros included).
*   `./build_all.sh amd64` — Standard build for Linux x86_64 (AVX2, FMA, LTO, `liburing`). Output monolith: `target_dist/FastCopy.so`.
*   `./build_all.sh arm64` — Cross-compilation for generic Linux ARM64 architectures (utilizing ARM NEON vector extensions + hardware CRC32/Crypto for ultra-fast xxHash computing). Automatically provisions `aarch64-linux-gnu-gcc` packages inside MSYS2.
*   `./build_all.sh ohc4` — Deeply optimized build targeted specifically at the **Odroid HC4** board, tuning the compilation parameters around its dual-cluster `Cortex-A73+Cortex-A53` core topology.
*   `./build_all.sh --installer` — Compiles the Linux ARM64 binary and bundles it into a standalone self-extracting shell script `target_dist/install.sh` using **XZ Extreme** compression.

#### Managing the SH Installer on Linux (far2l & far2m):
*   **Installation:** Copy `install.sh` to the target device (e.g., Odroid HC4) and run: `./install.sh`. The installer automatically maps configuration folders, deploys the directory structure at `~/.config/far2l(far2m)/plugins/fastcopy`, extracts the monolithic binary, and binds input hooks.
*   **Uninstallation (Rollback):** Execute the script with the uninstallation flag: `./install.sh --uninstall`. It cleanly deletes target plugin structures out of both environments, reverting file manager behaviors to default values without leaving any configuration fragments in the file system.
