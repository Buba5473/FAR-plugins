// =========================================================================
#pragma once
#include <windows.h>
#include <winsock2.h>
#include <stdint.h>

// Константы типов источников монтирования слотов
#define MOUNT_TYPE_LOCAL  0
#define MOUNT_TYPE_IMAGE  1
#define MOUNT_TYPE_NET    2

// Коды операций (Opcodes) протокола TFTP согласно спецификации RFC 1350
#define TFTP_OP_RRQ   1 
#define TFTP_OP_DATA  3 
#define TFTP_OP_ACK   4 
#define TFTP_OP_ERROR 5 

// Константы идентификаторов диалогового интерфейса Mount Manager
#define ID_BOX         0
#define ID_LIST_TITLE  1
#define ID_MOUNT_LIST  2
#define ID_TYPE_TEXT   3
#define ID_RADIO_LOCAL 4
#define ID_RADIO_IMAGE 5
#define ID_RADIO_NET   6
#define ID_PATH_TEXT   7
#define ID_EDIT_PATH   8
#define ID_USER_TEXT   9
#define ID_EDIT_USER   10
#define ID_PASS_TEXT   11
#define ID_EDIT_PASS   12
#define ID_LETTER_TEXT 13
#define ID_EDIT_LETTER 14
#define ID_FS_TEXT     15
#define ID_COMBO_FS    16
#define ID_BTN_MOUNT   17
#define ID_BTN_UNMOUNT 18
#define ID_BTN_CLOSE   19

#pragma pack(push, 1)

// Настройки сетевого сопряжения (Заполняются из интерактивного GUI)
struct NetworkConfig {
    BOOL     UseNetwork;     // Флаг режима: TRUE = сетевой образ, FALSE = локальный накопитель
    wchar_t  ServerIp[64];   // Строка IP-адреса удаленного хранилища образов
    uint16_t ServerPort;     // UDP-порт (по умолчанию стандартный порт TFTP: 69)
    char     ImagePath[260]; // Относительный ASCII-путь к файлу-образу на сервере
};

// Универсальная структура контекста активного монтирования (Слот подключения)
struct MountSlot {
    BOOL      IsActive;           // Флаг состояния: TRUE = диск активен в ядре Windows
    int       SourceType;         // Тип подключения (MOUNT_TYPE_LOCAL, IMAGE или NET)
    wchar_t   MountLetter[4];     // Буква смонтированного FUSE-диска (например, L"X:")
    wchar_t   FsType[12];         // Строковое имя файловой системы (L"Ext4", L"Btrfs", L"XFS"...)
    wchar_t   SourcePath[260];    // Путь к устройству, локальному файлу или URL узла
    wchar_t   Username[64];       // Логин для авторизации на удаленном сетевом ресурсе
    wchar_t   Password[64];       // Замаскированный пароль для авторизации
    HANDLE    FspThreadHandle;    // Индивидуальный дескриптор фонового FUSE-потока WinFSP
    void*     IoBackendPrivate;   // Указатель на структуру сетевого сокета или локального I/O хэндла
};

// Перечисление типов операций асинхронного IOCP контекста Winsock2
enum class NetOpType {
    DataReceive,
    DataSend
};

// Расширенная структура OVERLAPPED для трекинга асинхронных UDP пакетов через IOCP
struct NetOverlapped {
    OVERLAPPED   Overlapped;        // Нативная Win32 структура асинхронного ожидания
    WSABUF       WsaBuf;            // Структура Winsock буферизации
    NetOpType    OpType;            // Тип сетевой транзакции
    uint64_t     TargetDiskOffset;  // Смещение на диске Linux, куда предназначены данные пакета
    uint32_t     ExpectedBytes;     // Сколько байт полезной нагрузки ожидает ядро парсера
    uint8_t*     DirectBuffer;      // Ссылка на FUSE-память Арены для Zero-Copy DMA переноса данных
};

#pragma pack(pop)

// Глобальные переменные сетевой конфигурации
extern NetworkConfig NetCfg;

// =========================================================================
// --- КЛАСС АСИНХРОННОГО ДВИЖКА IOCP WINSOCK2 ---
// =========================================================================
class AsyncNetworkMounter {
private:
    SOCKET       netSocket;      // Нативный UDP-сокет Winsock2 в режиме OVERLAPPED
    HANDLE       hIOCP;          // Дескриптор Порта Завершения Ввода-Вывода Windows API
    sockaddr_in  serverAddr;     // Скоординированная структура адреса удаленного сервера
    HANDLE*      workerThreads;  // Пул системных потоков ожидания прерываний сети
    uint32_t     threadCount;    // Количество ядер в пуле потоков
    volatile BOOL isLooping;     // Флаг активности бесконечного цикла воркеров IOCP

    // Потоковая процедура воркера пула IOCP, обрабатывающая пакеты «на лету»
    static DWORD WINAPI NetworkWorkerProc(LPVOID lpParam);

public:
    AsyncNetworkMounter();
    ~AsyncNetworkMounter();

    // Инициализация Winsock2, биндинг сокета на IOCP-порт и масштабирование пула под ядра CPU Intel
    bool StartNetworkEngine(const wchar_t* serverIp, uint16_t port);
    
    // Безопасная остановка асинхронных потоков с посылкой терминирующих PostQueuedCompletionStatus сигналов
    void StopNetworkEngine();

    // Отправка неблокирующего UDP-кадра запроса блока данных по сети в рамках скользящего окна
    bool QueueBlockRead(uint64_t diskOffset, uint32_t size, uint8_t* outFuseBuffer);
};

extern AsyncNetworkMounter G_AsyncNetEngine;

// =========================================================================
// --- КЛАСС СИНХРОННОГО СЕТЕВОГО ФОЛБЭК РИДЕРА ---
// =========================================================================
class NetworkDiskReader {
private:
    SOCKET      sock;            // Синхронный UDP-сокет Winsock2
    sockaddr_in serverAddr;      // Контекст сетевого адреса сервера
    BOOL        isInitialized;   // Маркер успешного WSAStartup рантайма

public:
    NetworkDiskReader();
    ~NetworkDiskReader();

    // Первичный запуск Winsock и установка SO_RCVTIMEO таймаутов сокета во избежание фризов FAR
    bool InitNetwork(const wchar_t* ip, uint16_t port);
    void CloseNetwork();

    // Потоковая синхронная выкачка секторов удаленного образа по TFTP протоколу
    bool ReadRemoteBlock(uint64_t diskOffset, uint32_t size, uint8_t* outBuffer);
};

extern NetworkDiskReader GlobalNetReader;

// =========================================================================
// --- КЛАСС МЕНЕДЖЕРА СЛОТОВ ВИРТУАЛЬНЫХ ДИСКОВ WINDOWS ---
// =========================================================================
class MountManager {
private:
    MountSlot        slots[26];  // Ограничено алфавитным пулом букв A-Z ОС Windows
    size_t           activeCount;// Счетчик активных FUSE сессий монтирования
    CRITICAL_SECTION syncLock;   // Спин-блокировка для потокобезопасного создания подключений

public:
    MountManager();
    ~MountManager();

    // Создание точки монтирования тома и инициализация сетевого стека при MOUNT_TYPE_NET
    bool CreateMount(const MountSlot& newMount);
    
    // Безопасное отключение FUSE диска Windows, остановка IOCP-воркеров и зачистка слота
    bool DeleteMount(const wchar_t* letter);

    // Считывание зарегистрированных слотов для вывода в ListBox форму диалога FAR Manager 3
    size_t GetSlots(MountSlot** outSlots);

    // Потокобезопасный перехватчик: возвращает TRUE, если текущий опрашиваемый FAR диск — сетевой
    bool IsCurrentSourceNetwork();
};

extern MountManager G_MountManager;

// =========================================================================
// --- КЛАСС АСИНХРОННОГО РЕГИСТРАТОРА СЕТЕВЫХ СБОЕВ (LOG SYSTEM) ---
// =========================================================================
class NetLogger {
private:
    wchar_t          lastErrorMsg[256]; // Защищенный буфер для текста ошибки
    uint32_t         errorCount;        // Общий счетчик зафиксированных сетевых аномалий
    uint32_t         lastErrorTime;     // Таймштамп ошибки (GetTickCount) для механизма auto-decay
    CRITICAL_SECTION cs;                // Спин-локер межпоточного взаимодействия (WSA Workers -> FAR)

    inline HWND GetFarWindowHandle() { return FindWindowW(L"Far", nullptr); }

public:
    NetLogger() : errorCount(0), lastErrorTime(0) {
        InitializeCriticalSection(&cs);
        lastErrorMsg[0] = L'\0';
    }

    ~NetLogger() { DeleteCriticalSection(&cs); }

    // Потокобезопасная фиксация сетевой ошибки из IOCP-потока с посылкой WM_SETCURSOR сообщения в FAR GUI
    void LogNetworkError(const wchar_t* format, ...) {
        EnterCriticalSection(&cs);
        va_list args;
        #if defined(_M_X64) || defined(__x86_64__)
        args = reinterpret_cast<va_list>(&format) + sizeof(format);
        #else
        args = (va_list)(&format + 1);
        #endif
        wvsprintfW(lastErrorMsg, format, args);
        errorCount++;
        lastErrorTime = GetTickCount();
        LeaveCriticalSection(&cs);

        HWND hFarWnd = GetFarWindowHandle();
        if (hFarWnd) PostMessageW(hFarWnd, WM_SETCURSOR, 0, 0); // Форсируем мгновенную отрисовку статус-бара
    }

    // Вычитывание ошибки главным потоком FAR. Включает auto-decay очистку экрана через 7 секунд.
    bool GetLastErrorString(wchar_t* outBuffer, uint32_t maxLen) {
        if (!outBuffer || maxLen == 0) return false;
        EnterCriticalSection(&cs);
        if (lastErrorMsg[0] == L'\0') { LeaveCriticalSection(&cs); return false; }

        if (GetTickCount() - lastErrorTime > 7000) {
            lastErrorMsg[0] = L'\0'; LeaveCriticalSection(&cs); return false;
        }

        wchar_t* dst = outBuffer; wchar_t* src = lastErrorMsg; uint32_t count = 0;
        while (*src != L'\0' && count < (maxLen - 1)) { *dst++ = *src++; count++; }
        *dst = L'\0';
        LeaveCriticalSection(&cs);
        return true;
    }
};

extern NetLogger G_NetLogger;
