# #️⃣ FastCopy Far Manager Plugin (Far3 & far2l)

[English version](#-english-version)

Высокопроизводительный кроссплатформенный плагин для **Far Manager 3 (Windows)** и **far2l (Linux)**, интегрирующий возможности асинхронного небуферизированного ввода-вывода и очередей FIFO с глубоким перехватом нативных диалогов менеджера.

---

## 🇷🇺 Русский язык

### 🚀 Ключевые особенности и низкоуровневые оптимизации

*   **⚡ Технология Zero-Copy (Linux 6.0+):** Использование системных вызовов `copy_file_range` и `FICLONE` (Reflink) на файловых системах Btrfs/XFS. Копирование тяжелых файлов в пределах одного диска происходит мгновенно на уровне метаданных без циклов чтения-записи в ОЗУ.
*   **💾 Небуферизированный асинхронный ввод-вывод (Windows 10+):** Прямая работа с контроллерами дисков через `CreateFile2` с флагами `FILE_FLAG_NO_BUFFERING` и `FILE_FLAG_OVERLAPPED`. Исключает кэш страниц ОС (Page Cache), высвобождая оперативную память и раскрывая 100% скорости NVMe/SSD накопителей.
*   **🏎️ Адаптивная буферизация (HDD vs SSD):** Плагин на лету опрашивает тип накопителя через `StorageDeviceSeekPenaltyProperty` (Win) или `/sys/block` (Linux). Для SSD применяется мелкокластерный параллельный стриминг буферами по 4 МБ, а для шпиндельных HDD — блочный bulk-сброс по 64 МБ, что минимизирует перемещение магнитных головок.
*   **🔄 Бесшовное возобновление (Resume/Append):** Автоматическая дозапись файлов при обрыве копирования на основе сверки смещений и размеров. Если целевой файл меньше исходного, плагин мгновенно продолжит запись с места обрыва.
*   **🔐 Паритет функций с FileCopyEx3:** Нативная поддержка принудительного NTFS-сжатия (`FSCTL_SET_COMPRESSION`), шифрования Windows EFS (`EncryptFileW`), а также корректный перенос альтернативных потоков данных (ADS) и прав доступа (ACL/Security Descriptors).
*   **🔍 Инкрементальная xxHash-синхронизация:** Сверхбыстрое хэширование файлов по алгоритму xxHash. Если файлы идентичны, плагин мгновенно пропускает их, избавляя от лишних операций и вызовов TUI-диалогов.
*   **🧵 Двухуровневая многопоточность:** Общая FIFO-очередь на базе `std::condition_variable` для последовательных фоновых операций и мгновенные независимые потоки (`.detach()`) для параллельного асинхронного копирования.

---

### 🎹 Описание использования макросов и горячих клавиш

Плагин полностью автоматизирован и разворачивает комплекс макросов самостоятельно при первом запуске (в Windows через LuaMacro, в Linux через интеграцию в `key_macros.ini`). Больше нет жесткого «слепого» перехвата стандартных клавиш — поведение плагина полностью предсказуемо.

#### 1. Стандартные операции (F5 / F6) с нативным перехватом
*   При нажатии клавиш **`F5` (Копирование)** или **`F6` (Перенос)** на активной панели открывается **штатный, нативный диалог Far Manager / far2l**.
*   Плагин динамически внедряет в этот диалог новую официальную кнопку **`[ FastCopy Очередь ]`**.
*   **Если вы нажимаете `Enter` (или кнопку «Копировать»/«Перенести»):** Плагин перехватывает операцию, закрывает нативный диалог и выполняет асинхронное копирование **немедленно в отдельном потоке**, минуя общую очередь. Панели менеджера не блокируются, вы можете продолжать работу.
*   **Если вы нажимаете кнопку `[ FastCopy Очередь ]`:** Задача считывает целевой путь из поля ввода диалога, закрывает окно и встает в **общую фоновую FIFO-очередь**, где задачи выполняются последовательно одна за другой.

#### 2. Прямые горячие клавиши (миная диалоги)
*   **`Ctrl+Shift+F5`** — Мгновенно пакует выделенные элементы и отправляет их в фоновую FIFO-очередь на копирование в целевую директорию пассивной панели, не выводя на экран никаких окон.
*   **`Ctrl+Shift+F6`** — Мгновенно отправляет выделенные элементы в фоновую FIFO-очередь на перемещение (с последующим удалением источника) в папку пассивной панели.
*   **`Ctrl+Shift+P`** — Глобальный хоткей паузы. Переводит фоновый поток диспетчера очереди в спящий режим ядра ОС. Потребление тактов CPU и диска падает до нуля.

---

### 💻 Управление фоновыми приоритетами и интерфейсом

*   **🚦 Динамический Background I/O Throttling:** Поток копирования переводится в режим `IoPriorityHintBackground` (Windows) и `IOPRIO_CLASS_IDLE` (Linux). Операция автоматически замедляется, если система нагружена другими задачами, предотвращая микрофризы интерфейса ОС.
*   **🟩 Интеграция с Taskbar Windows 10/11:** Прогресс немедленных операций транслируется на иконку Far Manager на панели задач Windows через `ACTL_SETPROGRESSVALUE`.
*   **💬 Информативные TUI-уведомления:** Всплывающие тосты Far Dialog API с отображением размера пула задач и индикатором `[PAUSED]`.
*   **🔔 Умный акустический сигнал:** Мелодичный двойной `Beep` по окончании всей очереди. Плагин считывает реестр Windows Focus Assist (режим «Не беспокоить») и глушит звук в ночное время.
*   **📦 Автообновление ресурсов:** Плагин контролирует свою версию через маркер `version.txt`. При обновлении бинарника старые `.lng` и `.hlf` файлы автоматически зачищаются и создаются заново.

---

### 🛠️ Сборка проекта (MSYS2 UCRT64)

Для сборки требуется запустить консоль **MSYS2 UCRT64** и выполнить скрипт `build_all.sh` с аргументами:

```bash
# Сборка под Windows AMD64 UCRT (по умолчанию):
./build_all.sh amd64 windows

# Кросс-компиляция под Linux ARM64 (для микрокомпьютеров):
./build_all.sh arm64 linux

# Экстремальная оптимизация под ядра Cortex-A55 (Odroid HC4):
./build_all.sh odroid_hc4 linux --installer
```

Флаг `--installer` генерирует готовый цветной самораспаковывающийся Linux-скрипт `FastCopy_Linux_Installer.sh` с ультра-сжатием **XZ-Ultra (-9e)**, который умеет отключать конфликтующие плагины (`filecopyex`, `altcopy`) и поддерживает флаг деинсталляции `--uninstall`.

***

## 🇬🇧 English Version

### 🚀 Key Features & Low-Level Optimizations

*   **⚡ Zero-Copy Technology (Linux 6.0+):** Utilizes `copy_file_range` and `FICLONE` (Reflink) system calls on Btrfs/XFS filesystems. Large file cloning within the same drive happens instantly at the metadata level without CPU/RAM I/O cycles.
*   **💾 Unbuffered Asynchronous I/O (Windows 10+):** Direct disk controller interaction via `CreateFile2` with `FILE_FLAG_NO_BUFFERING` and `FILE_FLAG_OVERLAPPED` tokens. Bypasses the OS Page Cache, freeing RAM and unlocking 100% of NVMe/SSD speeds.
*   **🏎️ Adaptive Buffering (HDD vs SSD):** Dynamically detects drive types using `StorageDeviceSeekPenaltyProperty` (Win) or `/sys/block` (Linux). Allocates 4 MB parallel streaming buffers for SSDs and 64 MB bulk blocks for HDDs to minimize drive head thrashing.
*   **🔄 Seamless Resumption (Resume/Append):** Automatically appends partially copied files based on size and offset verification after any unexpected transfer interruption. If the destination file is smaller, the plugin continues recording from the exact breakdown point.
*   **🔐 FileCopyEx3 Feature Parity:** Native support for forced NTFS compression (`FSCTL_SET_COMPRESSION`), Windows EFS encryption (`EncryptFileW`), as well as accurate preservation of Alternative Data Streams (ADS) and ACL permissions.
*   **🔍 Incremental xxHash Synchronization:** Ultra-fast file hashing using the xxHash algorithm. Identical files are instantly skipped, saving storage lifespan and bypassing unnecessary TUI dialog triggers.
*   **🧵 Dual-level Multithreading:** A shared FIFO queue powered by `std::condition_variable` for sequential background processing, paired with immediate detached threads (`.detach()`) for parallel asynchronous cloning.

---

### 🎹 Macros & Hotkeys Usage Guide

The plugin is fully autonomous and deploys its macro pack on its first startup (via LuaMacro in Windows, and inside `key_macros.ini` in Linux). No more hard blind overriding of standard keys — the plugin's behavior is completely transparent.

#### 1. Standard Operations (F5 / F6) with Native Interception
*   Pressing **`F5` (Copy)** or **`F6` (Move)** on an active panel opens the **standard, native Far Manager / far2l dialog box**.
*   The plugin dynamically injects a new official button into this native interface: **`[ FastCopy Queue ]`** (or `[ FastCopy Очередь ]`).
*   **If you press `Enter` (or click the native Copy/Move button):** The plugin intercepts the operation, closes the native dialog, and executes asynchronous copying **immediately in a detached thread**, bypassing the queue. The panels are not blocked; you can continue navigating.
*   **If you click the `[ FastCopy Queue ]` button:** The task reads the target path from the dialog's input text field, closes the window, and joins the **global background FIFO queue** to be processed sequentially.

#### 2. Direct Global Hotkeys (Bypassing Dialogs)
*   **`Ctrl+Shift+F5`** — Instantly grabs selected items and pushes them straight into the background FIFO copy queue using the passive panel's current directory as a destination, without spawning any dialog prompt.
*   **`Ctrl+Shift+F6`** — Instantly passes selected items into the background FIFO move queue (with subsequent source deletion) directed to the passive panel's path.
*   **`Ctrl+Shift+P`** — Global pause hotkey. Puts the background queue manager thread into the OS kernel sleep mode. CPU and disk usage drop to zero.

---

### 💻 Background Priorities & UI Management

*   **🚦 Dynamic Background I/O Throttling:** Transfers the copying process to `IoPriorityHintBackground` (Windows) and `IOPRIO_CLASS_IDLE` (Linux). Limits I/O speed if the system is busy with other workflows, preventing any OS micro-stutters.
*   **🟩 Windows 10/11 Taskbar Integration:** Immediate operation progress is streamed directly onto the Far Manager taskbar icon via `ACTL_SETPROGRESSVALUE`.
*   **💬 Informative TUI Notifications:** Clean Far Dialog API overlay windows displaying current task pool sizes and the `[PAUSED]` state indicator.
*   **🔔 Smart Audio Alerts:** Melodic dual-tone hardware `Beep` upon queue completion. The plugin queries Windows Focus Assist (Do Not Disturb) registry variables to silence itself at night.
*   **📦 Asset Auto-Purging:** The plugin monitors its lifecycle using a `version.txt` file. When the binary is upgraded, old `.lng` and `.hlf` resources are automatically wiped and redeployed from scratch.

---

### 🛠️ Building the Project (MSYS2 UCRT64)

To compile the plugin, open the **MSYS2 UCRT64** environment shell and run `build_all.sh` with the target arguments:

```bash
# Windows AMD64 UCRT Compilation (Default target):
./build_all.sh amd64 windows

# Linux ARM64 Cross-compilation (For microcomputers):
./build_all.sh arm64 linux

# Extreme microarchitecture optimization for Cortex-A55 (Odroid HC4):
./build_all.sh odroid_hc4 linux --installer
```

The `--installer` flag creates an executable self-extracting Linux script `FastCopy_Linux_Installer.sh` packed with **XZ-Ultra (-9e)** compression. It features smart ANSI logging, automatically deactivates conflicting copy utilities (`filecopyex`, `altcopy`), and accepts the `--uninstall` flag to cleanly revert all adjustments.
