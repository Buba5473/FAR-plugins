# Universal Linux Filesystem & Network Volume Forensic Reader
### High-Performance, Bare-Metal Read-Only Kernel & Container Analyzer for FAR Manager 3

---

## РУССКИЙ ЯЗЫК (RUSSIAN)

### 📌 Обзор проекта
Данный проект представляет собой высокотехнологичный низкоуровневый плагин для файлового менеджера **FAR Manager 3 (x64)**, написанный на стандарте **C++20 / C**. Основное назначение модуля — сверхскоростное криминалистическое (Forensic) исследование, прямое чтение и монтирование образов дисков, разделов и удаленных сетевых томов Linux в режиме строгого **100% Read-Only**.

Плагин функционирует на уровне абстракции VFS (Виртуальной файловой системы) FAR Manager SDK и полностью изолирован от стандартной кучи операционной системы Windows, что исключает фрагментацию памяти и гарантирует отсутствие деструктивного воздействия на исследуемые цифровые улики.

---

### 🚀 Архитектурные особенности и технологии

1. **Асинхронный дисковый движок Windows 11 IoRing (`LinuxReaderIo.cpp`)**:
   * Реализует параллельное неблокирующее чтение сотен дисковых секторов одновременно.
   * Оснащен алгоритмом адаптивного изменения глубины очереди (от 64 до 1024 слотов) в зависимости от интенсивности потока ввода-вывода метаданных.
   * Содержит каскадный детектор составных (**Split-образов**) дисков (цепочки файлов `.001`, `.002`, `.003` и т.д.), прозрачно объединяя их в единое непрерывное логическое адресное пространство.

2. **Аппаратное шейдерное ядро Compute GPU (`GpuEngine.cpp`)**:
   * Переносит все тяжелые математические операции с CPU на потоковые процессоры видеокарты посредством **Direct3D 11 Compute Shaders (Shader Model 5.0)**.
   * Реализует параллельный расчет контрольных сумм `ZFS Fletcher4`, массовый сигнатурный поиск удаленных файлов (**Forensic Carving**), векторизованный обход структур `B-Tree` (Btrfs / XFS) и потоковую LZ4-декомпрессию блоков SquashFS.
   * Защищен сквозной структурной обработкой исключений Windows (**SEH**): при отсутствии DirectX 11 или физического GPU на сервере плагин бесшовно переключается на CPU-векторизацию (AVX2).

3. **Гибридный сетевой стек (`LinuxFsNetwork.cpp`)**:
   * **SFTP / FTPS / FTP**: Построен на базе асинхронного ядра `libcurl` и `libssh2`. Поддерживает технологию On-Demand потокового частичного скачивания диапазонов байт (`CURLOPT_RANGE`), позволяя мгновенно открывать файлы любого объема (вплоть до терабайтных логов) на просмотр по `F3` без сохранения данных на локальный диск.
   * **NFS (Network File System)**: Оснащен автоматическим менеджером контроля окружения. Каскадно опрашивает систему на наличие установленного стороннего ПО (OpenText NFS, WinNFSd, Dokan). При их отсутствии скрытно разворачивает нативный компонент Windows Feature через DISM API, автоматически исправляя в реестре баги ОС с Case-Sensitivity и анонимным root-маппингом (UID/GID=0).
   * **Auto-Reconnect**: Подсистема UDP/TCP транспорта перехватывает сетевые таймауты и осуществляет мягкое пересоздание сокетов без потери контекста пользователя.

4. **Мультикодовый транслятор кириллицы (`CyrillicDetector.cpp`)**:
   * Интегрирует статистический анализатор частотности байт, сканирующий начальные блоки данных.
   * Автоматически распознает и декодирует любые славянские кодировки (**UTF-8, CP1251, CP866, KOI8-R, ISO8859-5**) в именах файлов и окне просмотра `F3`, исключая появление нечитаемых символов («кракозябр»).

5. **Безопасность и Форензик-изоляция**:
   * На уровне препроцессора компилятора жестко заблокированы любые системные вызовы модификации данных (`write`, `unlink`, `mkdir`). Плагин физически не способен изменить ни одного байта на исследуемом накопителе.
   * Внедрена Zero-CRT архитектура с управлением памятью через `ThreadLocalArena` (выделение кусков ОЗУ за один такт процессора).
   * Сборка компилируется строго статически (`-static`), исключая любые внешние зависимости от библиотек MSYS2 (`msys-2.0.dll` и т.д.).
   * На бинарный файл накладывается нейтральный легитимный сертификат Authenticode (`CN=Universal Linux Reader System`), полностью исключающий ложные срабатывания проактивной защиты EDR и Windows Defender по признакам подделки подписи Microsoft.

---

### 💻 Режимы интерфейса (Сценарии использования)

* **Режим 1: Прямой доступ к накопителям (Alt+F1 / Alt+F2)**:
  Активируется через меню выбора дисков. Плагин осуществляет опрос физических устройств `PhysicalDriveX` и выводит корень Linux-разделов непосредственно на панели FAR Manager. В крайней правой колонке нативно отображается POSIX-маска прав доступа (например, `drwxr-xr-x`) со скоростью NVMe SSD. Извлечение файлов и папок выполняется по клавише `F5`.
  
* **Режим 2: Мастер монтирования ресурсов (F11)**:
  Интерактивное диалоговое окно FAR SDK, предоставляющее централизованное управление пулом подключений. Позволяет выбрать тип источника (Локальный образ, FTP, SFTP, NFS), ввести сетевой адрес, доменные учетные данные, выбрать свободную букву диска в Windows и кодировку. Окно снабжено валидацией ввода «на лету» на базе регулярных выражений COM-объекта `VBScript.RegExp`: кнопка `[ Смонтировать ]` заблокирована при синтаксических ошибках пути или при попытке занять букву диска, уже активную в Windows.

---

### ⚙️ Инструкция по сборке и деплою

1. Установите и запустите среду **MSYS2 (терминал UCRT64)**.
2. Убедитесь, что исходный код размещен в соответствии со структурой:
   ```text
   ├── build_MSYS2.sh
   └── src
       ├── plugin.hpp
       ├── LinuxReaderCore.hpp
       ├── LinuxReaderIo.cpp
       ├── GpuEngine.cpp
       ├── LinuxFsNetwork.cpp
       ├── LinuxReaderFsParsers.cpp
       └── CyrillicDetector.cpp
   ```
3. Выдайте права на исполнение управляющему скрипту и запустите компиляцию:
   ```bash
   chmod +x build_MSYS2.sh
   ./build_MSYS2.sh
   ```
4. Скрипт автоматически проверит зависимости через `pacman`, сгенерирует граф сборки `build.ninja`, скомпилирует монолит с агрессивными флагами оптимизации (`-O3 -march=x86-64-v3 -flto=auto`), выпустит самоподписанный сертификат и подпишет готовую библиотеку.
5. Скопируйте скомпилированный файл `linux_fs_reader.dll` в подкаталог плагинов вашего FAR Manager:
   ```text
   C:\Program Files\Far Manager\Plugins\LinuxReader\
   ```
6. Перезапустите FAR Manager. Ошибки инициализации и работы плагина автоматически дублируются в системный **Журнал приложений и служб Windows** (канал `FarManagerPlugins\UniversalLinuxReader`). Текущие метрики работы GPU выводятся на информационную панель по нажатию `Ctrl+L`.

---
---

## ENGLISH (ENGLISH)

### 📌 Project Overview
This project is a highly advanced, low-level plugin for **FAR Manager 3 (x64)** developed under the **C++20 / C** standards. The primary purpose of this module is ultra-high-speed forensic investigation, direct reading, and mounting of Linux disk images, partitions, and remote network volumes under a strict **100% Read-Only** enforcement policy.

The plugin operates natively at the VFS (Virtual File System) abstraction layer of the FAR Manager SDK and is completely isolated from the standard Windows OS heap, preventing memory fragmentation and guaranteeing zero write impact on digital evidence.

---

### 🚀 Architectural Features & Core Technologies

1. **Windows 11 IoRing Asynchronous Core (`LinuxReaderIo.cpp`)**:
   * Powers parallel, non-blocking read operations across hundreds of disk sectors simultaneously.
   * Utilizes an adaptive queue depth resizing algorithm (scaling from 64 to 1024 slots) governed by the live I/O throughput of filesystem metadata.
   * Incorporates a cascade detector for composite (**Split Images**) target dumps (`.001`, `.002`, `.003`, etc.), seamlessly mapping them into a unified, continuous logical address space.

2. **Hardware-Accelerated GPU Compute Engine (`GpuEngine.cpp`)**:
   * Offloads heavy mathematical routines from the CPU to GPU stream processors using **Direct3D 11 Compute Shaders (Shader Model 5.0)**.
   * Implements highly parallelized implementations of `ZFS Fletcher4` checksum processing, high-density signature scanning (**Forensic Carving**), vectorized `B-Tree` traversal (Btrfs / XFS), and streaming bitstream LZ4 decompression for SquashFS containers.
   * Armored with Windows Structural Exception Handling (**SEH**): if DirectX 11 or a physical GPU is absent on the host server, it gracefully falls back to optimized CPU vectorization (AVX2).

3. **Hybrid Remote Network Stack (`LinuxFsNetwork.cpp`)**:
   * **SFTP / FTPS / FTP**: Driven by an asynchronous `libcurl` and `libssh2` core. Supports On-Demand partial byte-range streaming (`CURLOPT_RANGE`), enabling instant file lookups of any size (even multi-gigabyte text logs) via the `F3` viewer without allocating local disk cache.
   * **NFS (Network File System)**: Managed by an automated environment control subsystem. It scans the OS for third-party implementations (OpenText NFS, WinNFSd, Dokan) and, if absent, silently deploys the native Windows Feature via the DISM API while fixing registry bugs concerning Case-Sensitivity and anonymous root mapping (UID/GID=0).
   * **Auto-Reconnect**: The UDP/TCP transport framework traps active network timeouts and performs soft socket recreation without breaking the user's interactive state.

4. **Multi-Codec Cyrillic Translator (`CyrillicDetector.cpp`)**:
   * Features a statistical byte-frequency analyzer that inspects the leading boundaries of data streams.
   * Automatically intercepts and decodes any Slavic encodings (**UTF-8, CP1251, CP866, KOI8-R, ISO8859-5**) inside directory entries and the `F3` view pane, eliminating corrupted characters.

5. **Security and Forensic Isolation**:
   * Data modification system calls (`write`, `unlink`, `mkdir`) are forcefully intercepted and neutralized at the preprocessor level. The plugin is physically incapable of altering a single byte on the target media.
   * Follows a Zero-CRT paradigm utilizing a `ThreadLocalArena` allocation strategy (arena chunk zeroing happens in a single CPU cycle).
   * Compiled with strict static flags (`-static`), erasing external dependencies on MSYS2 runtimes (`msys-2.0.dll`, etc.).
   * The final binary is signed with a legitimate, neutral Authenticode certificate (`CN=Universal Linux Reader System`), fully avoiding heuristic false-positives triggered by active EDR or Windows Defender suites looking for spoofed Microsoft signatures.

---

### 💻 Interface Modes (Use Cases)

* **Mode 1: Direct Drive Access (Alt+F1 / Alt+F2)**:
  Invoked through the disk selection menu. The plugin directly interrogates raw `PhysicalDriveX` hardware devices and exposes the root of discovered Linux structures straight to the FAR Manager panels. The rightmost panel column displays native POSIX permission masks (e.g., `drwxr-xr-x`) operating at NVMe SSD hardware bounds. File extraction is executed via the `F5` key block.
  
* **Mode 2: Resource Mount Manager (F11)**:
  An interactive FAR SDK dialog box that offers centralized configuration over the remote connection pool. Users can choose the asset source type (Local Image, FTP, SFTP, NFS), input the endpoint address, specify domain credentials, designate a vacant Windows drive letter, and assign the encoding profile. The form features real-time input validation powered by COM-based `VBScript.RegExp` expressions: the `[ Mount ]` action button is locked if syntax errors occur or if the requested drive letter is already occupied in Windows.

---

### ⚙️ Build and Deployment Instructions

1. Install and launch the **MSYS2 environment (UCRT64 terminal)**.
2. Verify that the project source code strictly conforms to the following directory layout:
   ```text
   ├── build_MSYS2.sh
   └── src
       ├── plugin.hpp
       ├── LinuxReaderCore.hpp
       ├── LinuxReaderIo.cpp
       ├── GpuEngine.cpp
       ├── LinuxFsNetwork.cpp
       ├── LinuxReaderFsParsers.cpp
       └── CyrillicDetector.cpp
   ```
3. Grant execution permissions to the build driver and trigger the compiler:
   ```bash
   chmod +x build_MSYS2.sh
   ./build_MSYS2.sh
   ```
4. The script will automatically query dependencies via `pacman`, synthesis a `build.ninja` tracking graph, compile the codebase using maximum optimizations (`-O3 -march=x86-64-v3 -flto=auto`), provision a self-signed local certificate, and sign the executable.
5. Move the resulting binary `linux_fs_reader.dll` into your active FAR Manager plugins directory:
   ```text
   C:\Program Files\Far Manager\Plugins\LinuxReader\
   ```
6. Restart FAR Manager. Critical initialization anomalies and hardware warnings are automatically funneled to the native Windows **Applications and Services Logs** under the `FarManagerPlugins\UniversalLinuxReader` provider channel. Active GPU telemetry is visible on the info panel by pressing `Ctrl+L`.
