#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <tlhelp32.h>
#include <curl/curl.h>
#include <shlwapi.h>
#include "LinuxReaderCore.hpp"
#include "LinuxFsNetwork.hpp"

// Импорт глобальных контекстов аудита и конфигурации
extern AuditLogger G_AuditLogger;
extern RemoteConfig G_ActiveMountConfig;

class MountRemoteEngine {
private:
    CURL* curlHandle;
    RemoteConfig cfg;
    CRITICAL_SECTION sessionLock;
    BOOL isConnected;

    // Внутренний Zero-Copy callback для потоковой записи сетевого блока в буфер IoRing
    static size_t WriteBlockCallback(void* ptr, size_t size, size_t nmemb, void* stream) {
        auto* req = reinterpret_cast<LinuxIoRequest*>(stream);
        size_t totalSize = size * nmemb;
        
        if (req && req->Buffer) {
            memcpy(req->Buffer, ptr, totalSize);
        }
        return totalSize;
    }

    // Высокоскоростная проверка наличия запущенного процесса альтернативного NFS-клиента
    BOOL IsAlternativeNfsProcessRunning(const wchar_t* processName) {
        BOOL found = FALSE;
        HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe;
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(hSnapshot, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, processName) == 0) {
                        found = TRUE;
                        break;
                    }
                } while (Process32NextW(hSnapshot, &pe));
            }
            CloseHandle(hSnapshot);
        }
        return found;
    }

    // Каскадная проверка установленного в ОС стороннего ПО для NFS (предотвращение конфликтов)
    bool IsAnyThirdPartyNfsClientInstalled() {
        HKEY hKey;
        
        // 1. Проверка коммерческого клиента OpenText (Hummingbird) NFS Client
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\OpenText\\NFS Client", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            G_AuditLogger.Log(LogSubsystem::Network, LogSeverity::Info, L"Обнаружен сторонний сетевой провайдер: OpenText NFS Client.");
            return true;
        }

        // 2. Проверка Dokan-NFS драйверов монтирования
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Dokan", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            G_AuditLogger.Log(LogSubsystem::Network, LogSeverity::Info, L"Обнаружена подсистема Dokan (возможен маппинг пользовательских NFS-утилит).");
            return true;
        }

        // 3. Проверка активности легковесных юзерспейс-демонов в ОЗУ
        if (IsAlternativeNfsProcessRunning(L"winnfsd.exe")) {
            G_AuditLogger.Log(LogSubsystem::Network, LogSeverity::Info, L"Обнаружен запущенный фоновый процесс winnfsd.exe.");
            return true;
        }
        if (IsAlternativeNfsProcessRunning(L"nekodrive.exe")) {
            G_AuditLogger.Log(LogSubsystem::Network, LogSeverity::Info, L"Обнаружен запущенный фоновый процесс Nekodrive NFS.");
            return true;
        }

        return false;
    }

    // Интеллектуальный менеджер автоматического развертывания и тюнинга NFS-компонентов Windows
    bool EnsureNfsSubsystemReady() {
        // Если найдено стороннее ПО, используем его без вмешательства в системные компоненты
        if (IsAnyThirdPartyNfsClientInstalled()) {
            return true; 
        }

        HKEY hKey;
        const wchar_t* nfsRegPath = L"SOFTWARE\\Microsoft\\ClientForNFS\\CurrentVersion\\Default";
        LONG status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, nfsRegPath, 0, KEY_READ | KEY_WRITE, &hKey);
        
        if (status == ERROR_FILE_NOT_FOUND) {
            G_AuditLogger.Log(LogSubsystem::Network, LogSeverity::Warning, 
                              L"Сетевые NFS-клиенты отсутствуют. Запуск фоновой тихой установки Windows Feature через DISM...");
            
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi;
            wchar_t cmd[] = L"cmd.exe /c dism.exe /online /enable-feature /featurename:NFS-Client /norestart";
            
            if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, INFINITE);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                
                status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, nfsRegPath, 0, KEY_READ | KEY_WRITE, &hKey);
            }
        }

        if (status == ERROR_SUCCESS) {
            // Тюнинг нативного клиента: сопоставление root-прав (UID/GID = 0) для беспрепятственного Read-Only форензика
            DWORD anonymousId = 0;
            RegSetValueExW(hKey, L"AnonymousUid", 0, REG_DWORD, reinterpret_cast<BYTE*>(&anonymousId), sizeof(DWORD));
            RegSetValueExW(hKey, L"AnonymousGid", 0, REG_DWORD, reinterpret_cast<BYTE*>(&anonymousId), sizeof(DWORD));

            // Включение строгого Case-Sensitivity для полной совместимости с именами файлов Linux
            DWORD caseSensitive = 1;
            RegSetValueExW(hKey, L"CaseSensitive", 0, REG_DWORD, reinterpret_cast<BYTE*>(&caseSensitive), sizeof(DWORD));

            RegCloseKey(hKey);
            return true;
        }

        return false;
    }

    // Изолированная ветка автоматического восстановления сессии (Auto-Reconnect) при сбоях curl-соединений
    void TriggerNetworkAutoReconnect() {
        G_AuditLogger.Log(LogSubsystem::Network, LogSeverity::Warning, L"Фиксация сбоя в ветке Network/UDP/Critical. Инициализация Auto-Reconnect...");
        
        if (curlHandle) {
            curl_easy_cleanup(curlHandle);
            curlHandle = nullptr;
        }

        // Попытка мягкого восстановления транспортной сессии
        if (Connect(cfg)) {
            G_AuditLogger.Log(LogSubsystem::Network, LogSeverity::Info, L"Сетевая сессия успешно восстановлена, выполнение запросов возобновлено.");
        } else {
            G_AuditLogger.Log(LogSubsystem::Network, LogSeverity::Critical, L"Критический сетевой тайм-аут: восстановить сессию не удалось.");
        }
    }

public:
    MountRemoteEngine() : curlHandle(nullptr), isConnected(FALSE) {
        InitializeCriticalSection(&sessionLock);
        curl_global_init(CURL_GLOBAL_ALL);
    }

    ~MountRemoteEngine() {
        DisconnectActiveSession();
        curl_global_cleanup();
        DeleteCriticalSection(&sessionLock);
    }

    // Установка соединения с удаленным ресурсом
    bool Connect(const RemoteConfig& remoteConfig) {
        EnterCriticalSection(&sessionLock);
        cfg = remoteConfig;

        // --- Обработка протокола NFS ---
        if (cfg.Protocol == NetProtocol::NFS) {
            if (!EnsureNfsSubsystemReady()) {
                G_AuditLogger.Log(LogSubsystem::Network, LogSeverity::Critical, L"Сбой подсистемы: в ОС отсутствует рабочий NFS-компонент.");
                LeaveCriticalSection(&sessionLock);
                return false;
            }

            wchar_t uncPath[1024];
            wchar_t remotePathW[512];
            MultiByteToWideChar(CP_UTF8, 0, cfg.RemoteImagePath, -1, remotePathW, 512);
            
            // Нормализация путей: преобразуем прямые Unix-слеши в обратные для сетевого провайдера Windows
            for (int i = 0; remotePathW[i] != L'\0'; ++i) {
                if (remotePathW[i] == L'/') remotePathW[i] = L'\\';
            }

            wsprintfW(uncPath, L"\\\\%s%s", cfg.Hostname, remotePathW);
            wchar_t localDrive[] = { cfg.CustomMountLetter, L':', L'\0' };

            NETRESOURCEW nr = {0};
            nr.dwType = RESOURCETYPE_DISK;
            nr.lpLocalName = localDrive;
            nr.lpRemoteName = uncPath;
            nr.lpProvider = NULL; // Windows автоматически перенаправит вызов на приоритетный NFS-провайдер в MUP

            // Принудительно размонтируем букву, если она осталась занята от аварийно завершенных старых сессий
            WNetCancelConnection2W(localDrive, CONNECT_UPDATE_PROFILE, TRUE);

            DWORD res = WNetAddConnection2W(&nr, NULL, NULL, CONNECT_TEMPORARY);
            if (res == NO_ERROR) {
                isConnected = TRUE;
                G_AuditLogger.Log(LogSubsystem::Network, LogSeverity::Info, L"NFS Шара успешно смонтирована на диск %s.", localDrive);
            } else {
                // Жесткий системный фолбэк командной строки, если стандартный WNet API вернул отказ
                wchar_t mountCmd[1024];
                wsprintfW(mountCmd, L"cmd.exe /c mount.exe -o mtype=hard anon %s %s", uncPath, localDrive);
                
                STARTUPINFOW si = { sizeof(si) };
                PROCESS_INFORMATION pi;
                if (CreateProcessW(NULL, mountCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                    if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0) {
                        if (GetDriveTypeW(localDrive) == DRIVE_REMOTE) {
                            isConnected = TRUE;
                            G_AuditLogger.Log(LogSubsystem::Network, LogSeverity::Info, L"NFS Ресурс принудительно смонтирован через утилиту mount.exe на диск %s.", localDrive);
                        }
                    }
                    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
                }
            }

            LeaveCriticalSection(&sessionLock);
            return isConnected;
        }

        // --- Обработка протоколов FTP / FTPS / SFTP (Через libcurl) ---
        curlHandle = curl_easy_init();
        if (!curlHandle) {
            LeaveCriticalSection(&sessionLock);
            return false;
        }

        char url[1024];
        const char* protoStr = (cfg.Protocol == NetProtocol::SFTP) ? "sftp" : 
                               ((cfg.Protocol == NetProtocol::FTPS) ? "ftps" : "ftp");

        char host[256], user[128], pass[128];
        WideCharToMultiByte(CP_ACP, 0, cfg.Hostname, -1, host, 256, NULL, NULL);
        WideCharToMultiByte(CP_ACP, 0, cfg.Username, -1, user, 128, NULL, NULL);
        WideCharToMultiByte(CP_ACP, 0, cfg.Password, -1, pass, 128, NULL, NULL);

        sprintf_s(url, "%s://%s:%s@%s:%d%s", protoStr, user, pass, host, cfg.Port, cfg.RemoteImagePath);
        curl_easy_setopt(curlHandle, CURLOPT_URL, url);
        
        // Индустриальный Read-Only тюнинг curl-транспорта
        curl_easy_setopt(curlHandle, CURLOPT_NOBODY, 1L); // Опрашиваем исключительно метаданные существования образа
        curl_easy_setopt(curlHandle, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curlHandle, CURLOPT_TCP_NODELAY, 1L); // Отключаем алгоритм Нагла для мгновенного пинга блоков
        
        if (cfg.Protocol == NetProtocol::SFTP) {
            curl_easy_setopt(curlHandle, CURLOPT_SSH_AUTH_TYPES, CURLSSH_AUTH_PASSWORD);
        }

        CURLcode res = curl_easy_perform(curlHandle);
        isConnected = (res == CURLE_OK);
        
        if (!isConnected) {
            G_AuditLogger.Log(LogSubsystem::Network, LogSeverity::Critical, L"Не удалось установить curl-сессию с удаленным сервером. Код: %d", res);
        }

        LeaveCriticalSection(&sessionLock);
        return isConnected;
    }

    // Асинхронное On-Demand чтение произвольного блока удаленного образа по сети (Zero-Copy)
    bool QueueRemoteBlockRead(LinuxIoRequest* req) {
        if (!isConnected) return false;

        if (cfg.Protocol == NetProtocol::NFS) {
            // Для смонтированного NFS-диска чтение идет прозрачно силами ОС, плагин перехватывает управление на верхнем уровне
            return true;
        }

        if (!curlHandle) return false;

        EnterCriticalSection(&sessionLock);
        
        // Отключаем режим заголовков (NOBODY), переходя к скачиванию полезной нагрузки сектора
        curl_easy_setopt(curlHandle, CURLOPT_NOBODY, 0L);
        
        // Высокоскоростное частичное скачивание диска через HTTP/FTP Range-запросы
        char rangeStr[64];
        sprintf_s(rangeStr, "%llu-%llu", req->Offset, req->Offset + req->Size - 1);
        curl_easy_setopt(curlHandle, CURLOPT_RANGE, rangeStr);

        // Настройка прямого буферного приемника в ОЗУ Арены
        curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, WriteBlockCallback);
        curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, reinterpret_cast<void*>(req));

        CURLcode res = curl_easy_perform(curlHandle);
        
        if (res != CURLE_OK) {
            req->Result = E_FAIL;
            // Уход в аварийную ветку автоматического переподключения сессии при фиксации критической сетевой ошибки
            TriggerNetworkAutoReconnect();
            LeaveCriticalSection(&sessionLock);
            return false;
        }

        req->Result = S_OK;
        LeaveCriticalSection(&sessionLock);
        return true;
    }

    // Безопасное закрытие и очистка всех дескрипторов сессий
    void DisconnectActiveSession() {
        EnterCriticalSection(&sessionLock);
        if (isConnected) {
            if (cfg.Protocol == NetProtocol::NFS) {
                wchar_t localDrive[] = { cfg.CustomMountLetter, L':', L'\0' };
                WNetCancelConnection2W(localDrive, CONNECT_UPDATE_PROFILE, TRUE);
            } else if (curlHandle) {
                curl_easy_cleanup(curlHandle);
                curlHandle = nullptr;
            }
            isConnected = FALSE;
        }
        LeaveCriticalSection(&sessionLock);
    }
};
