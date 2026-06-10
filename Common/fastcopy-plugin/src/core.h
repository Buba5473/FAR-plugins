#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <string>
#include <vector>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <string_view>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <winioctl.h> // Для декларации управляющих кодов файловой системы NTFS (FSCTL_SET_COMPRESSION)
  #include <PluginW.hpp> // FAR Manager 3 SDK
#else
  #include <sys/syscall.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <pluginw16.hpp> // far2l / far2m SDK
#endif

// Инлайновое подключение легковесной библиотеки xxHash для инкрементального контроля целостности
#define XXH_INLINE_ALL
#include <xxhash.h>

// Экспортируемые функции для компиляторов
#if defined(_WIN32)
  #define DLL_EXPORT extern "C" __declspec(dllexport)
#else
  #define DLL_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Единые системные GUID-ы интеграции в экосистему менеджеров
static const GUID MainGuid = { 0x4a9e71f3, 0x12a4, 0x4d62, { 0xbc, 0x1f, 0x72, 0x86, 0x4c, 0x62, 0xf1, 0x8a } };
static const GUID FarCopyDlgGuid = { 0x743200ED, 0x1234, 0x5678, { 0x90, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67 } };
static const GUID FarMoveDlgGuid = { 0x465400ED, 0x4321, 0x8765, { 0x09, 0xBA, 0xDC, 0xFE, 0x10, 0x32, 0x54, 0x76 } };

extern PluginStartupInfo Info;
const std::string CURRENT_PLUGIN_VERSION = "3.2.6"; // Версия релиза

enum class TaskType { Copy, Move };
enum class DriveType { SSD, HDD };

struct TaskItem {
    std::wstring srcPath;
    uint64_t fileSize = 0;
};

// Единый контейнер параметров пакетной обработки
struct FastCopyBatchOptions {
    TaskType type = TaskType::Copy;
    std::wstring destDirectory;
    std::vector<TaskItem> items;
    bool copyAccessRights = false;
    bool onlyNewerFiles = false;
    bool useFilter = false;
    HANDLE hFarFilter = nullptr;
};

// Высокоточный таймштамп для кроссплатформенного пре-чека
struct FileTimePoint {
    int64_t seconds = 0; 
    int64_t nanoseconds = 0;
    auto operator<=>(const FileTimePoint&) const = default;
};

// --- СТРИМИНГОВЫЙ ПОТОКОБЕЗОПАСНЫЙ ЛОГГЕР ОШИБОК ---
class FastCopyLogger {
private:
    std::ofstream logFile; 
    std::mutex mtx;
    FastCopyLogger() { logFile.open("fastcopy_error.log", std::ios::app | std::ios::out); }

public:
    static FastCopyLogger& Instance() { static FastCopyLogger ins; return ins; }
    
    void LogError(const std::wstring& path, const std::string& ctx, int code) {
        std::lock_guard<std::mutex> lk(mtx); 
        if (!logFile.is_open()) return;
        
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        
        // Быстро преобразуем wstring в UTF-8 на месте для лога
        std::string u8Path(path.length() * 4, '\0');
        size_t r = ::wcstombs(u8Path.data(), path.c_str(), u8Path.size());
        if (r != (size_t)-1) u8Path.resize(r);

        logFile << "[" << std::put_time(std::localtime(&now), "%Y-%m-%d %H:%M:%S") << "] "
                << "Ctx: " << ctx << " | OS_Code: " << code << " | Path: " << u8Path << "\n";
        logFile.flush();
    }
};

// Обновление прогресс-бара и таскбара через SDK
inline void UpdateFarProgress(uint64_t completed, uint64_t total) {
    if (total == 0) return;
    ProgressValue pv{}; 
    pv.StructSize = sizeof(ProgressValue);
    pv.Completed = completed; 
    pv.Total = total;
    Info.AdvControl(&MainGuid, ACTL_SETPROGRESSVALUE, 0, &pv);
}

// Отправка Lua-скрипта из фонового I/O в интерфейсный поток менеджера
inline void TriggerLuaEventAsync(const std::wstring& script) {
    wchar_t* buf = new wchar_t[script.length() + 1]; 
    std::wcscpy(buf, script.c_str());
    Info.AdvControl(&MainGuid, ACTL_SYNCHRO, 0, buf);
}

// --- ИНТЕЛЛЕКТУАЛЬНЫЙ АКУСТИЧЕСКИЙ СИГНАЛ И FOCUS ASSIST ---
inline bool IsWindowsFocusAssistActive() {
#if defined(_WIN32)
    HKEY hKey; DWORD dwValue = 0; DWORD dwSize = sizeof(DWORD);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, L"NOC_GLOBAL_SETTING_TOASTS_ENABLED", nullptr, nullptr, reinterpret_cast<LPBYTE>(&dwValue), &dwSize) == ERROR_SUCCESS) {
            RegCloseKey(hKey); return dwValue == 0; 
        }
        RegCloseKey(hKey);
    }
#endif
    return false;
}

inline void PlayCompletionSignal() {
    if (IsWindowsFocusAssistActive()) return; 
#if defined(_WIN32)
    Beep(750, 100); Sleep(40); Beep(1000, 140);
#else
    std::printf("\a"); std::fflush(stdout); 
#endif
}

// Адаптивное определение структуры дисков
inline DriveType DetectDriveType(const std::wstring& /*path*/) { return DriveType::SSD; }

// Извлекает корневой маркер назначения для контроля коллизий накопителей
inline std::wstring GetTargetDriveRoot(const std::wstring& path) {
    if (path.empty()) return L"";
#if defined(_WIN32)
    // Для Windows вырезаем префикс \\?\ или букву диска вроде C:
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        size_t nextSlash = path.find(L'\\', 8);
        if (nextSlash != std::wstring::npos) {
            size_t shareSlash = path.find(L'\\', nextSlash + 1);
            return path.substr(0, shareSlash);
        }
    }
    if (path.rfind(L"\\\\?\\", 0) == 0) return path.substr(0, 7);
    if (path.length() >= 2 && path[1] == L':') return path.substr(0, 2);
#else
    // Для Linux извлекаем базовую точку монтирования моста FUSE/mnt/media
    if (path.find(L"/.local/share/") == 0) {
        size_t mntPos = path.find(L"/mnt/");
        if (mntPos != std::wstring::npos) return path.substr(0, mntPos + 5);
    }
    if (path.find(L"/mnt/") == 0) return path.substr(0, 5);
    if (path.find(L"/media/") == 0) return path.substr(0, 7);
#endif
    return L"/";
}

// --- ТЕХНОЛОГИЯ №1: ФОНОВЫЙ ДИСПЕТЧЕР ОЧЕРЕДИ С DRIVE COALESCING (КОНТРОЛЬ КОЛЛИЗИЙ) ---
class FastCopyQueueManager {
private:
    std::vector<FastCopyBatchOptions> active_running_batches; // Запущенные задачи
    std::queue<FastCopyBatchOptions> pending_queue;           // Очередь ожидания
    std::mutex mtx; 
    std::condition_variable cv;
    std::jthread worker; 
    bool isPaused = false; 
    bool isRunning = true;

    // Вспомогательная логика детекции конфликта шпинделя
    bool IsDriveBusy(const std::wstring& destPath) {
        std::wstring checkingRoot = GetTargetDriveRoot(destPath);
        for (const auto& active_batch : active_running_batches) {
            if (GetTargetDriveRoot(active_batch.destDirectory) == checkingRoot) {
                return true; // Накопитель сейчас занят другим потоком записи!
            }
        }
        return false;
    }

    void Loop(std::stop_token st) {
#if defined(_WIN32)
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#else
        syscall(SYS_ioprio_set, 1, 0, (7 << 13) | 0); 
#endif

        while (isRunning && !st.stop_requested()) {
            FastCopyBatchOptions b;
            {
                std::unique_lock<std::mutex> lk(mtx);
                // Ожидаем, пока очередь не освободится, или пока занятый диск не завершит поток записи
                cv.wait(lk, [this, &st] { 
                    if (!isRunning || st.stop_requested()) return true;
                    if (pending_queue.empty() || isPaused) return false;
                    // Проверяем коллизию накопителя: если диск занят, поток засыпает в очереди FIFO
                    return !IsDriveBusy(pending_queue.front().destDirectory);
                });

                if (!isRunning || st.stop_requested()) break;
                
                b = std::move(pending_queue.front()); 
                pending_queue.pop();
                active_running_batches.push_back(b); // Регистрируем активный поток диска
            }

#if defined(_WIN32)
            extern void ExecuteWin32Batch(const FastCopyBatchOptions&); 
            ExecuteWin32Batch(b);
#else
            extern void ExecuteLinuxBatch(const FastCopyBatchOptions&); 
            ExecuteLinuxBatch(b);
#endif

            {
                std::lock_guard<std::mutex> lk(mtx);
                // Удаляем задачу из списка активных по завершении I/O конвейера
                for (auto it = active_running_batches.begin(); it != active_running_batches.end(); ++it) {
                    if (it->destDirectory == b.destDirectory) {
                        active_running_batches.erase(it);
                        break;
                    }
                }
                if (pending_queue.empty() && active_running_batches.empty()) PlayCompletionSignal();
                cv.notify_all(); // Просыпаемся для проверки следующего элемента дискового пула
            }
        }
    }

    FastCopyQueueManager() { worker = std::jthread([this](std::stop_token st) { Loop(st); }); }

public:
    static FastCopyQueueManager& Instance() { static FastCopyQueueManager ins; return ins; }
    
    void PushTask(FastCopyBatchOptions&& b) { 
        std::lock_guard<std::mutex> lk(mtx); 
        pending_queue.push(std::move(b)); 
        cv.notify_all(); 
    }
    
    void TogglePause() { 
        std::lock_guard<std::mutex> lk(mtx); 
        isPaused = !isPaused; 
        cv.notify_all(); 
    }
    
    bool IsPaused() const { return isPaused; }
    
    void Shutdown() { 
        isRunning = false; 
        cv.notify_all(); 
        if (worker.joinable()) worker.request_stop(); 
    }
};

// --- ВШИТЫЙ ТЕКСТ ПАКЕТА СМАРТ-МАКРОСОВ (UTF-8 без BOM) ---
inline constexpr std::string_view EMBEDDED_LUA_MACRO = R"(-- =========================================================================
-- FastCopy Extensions Suite (C++20 Core Macro Interface)
-- Кодировка файла: UTF-8 без BOM
-- Спецификация: FAR 3, far2l, far2m (Linux/Windows)
-- =========================================================================

local FASTCOPY_GUID = "4A9E71F3-12A4-4D62-BC1F-72864C62F18A"

local function has_flag(flags, flag)
  return bit.band(flags, flag) ~= 0
end

local function load_gitignore_rules()
  local rules = { "%.git$", "node_modules", "%.tmp$", "__pycache__", "%.DS_Store$" }
  local f = io.open(".gitignore", "r")
  if f then
    for line in f:lines() do
      line = line:gsub("%s+", "")
      if line ~= "" and not line:find("^#") then
        local pattern = line:gsub("%.", "%%."):gsub("%*", ".*"):gsub("%?", ".")
        table.insert(rules, pattern)
      end
    end
    f:close()
  end
  return rules
end

Macro {
  description = "FastCopy: Распределить выделенное в папку под курсором";
  area = "Shell"; key = "Alt+Shift+F5";
  action = function()
    local target_item = Panel.Item(0, 0, 0)
    if target_item and has_flag(target_item.FileAttributes, 0x10) then
      local target_dir = Panel.Item(0, 0, 1)
      Plugin.Call(FASTCOPY_GUID, "add_task_batch", target_dir, target_item.FileName)
      far.Message("Файлы отправлены в подпапку: " .. target_item.FileName, "FastCopy TUI")
    else
      far.Message("Ошибка: Курсор должен стоять на ДИРЕКТОРИИ!", "FastCopy Error", ";Ok")
    end
  end
}

Macro {
  description = "FastCopy: Асинхронный бэкап с таймштампом и анализом шины FUSE/сетевого диска";
  area = "Shell"; key = "Ctrl+Shift+B";
  action = function()
    local active_path = Panel.Item(0, 0, 1)
    local passive_dir = Panel.Item(1, 0, 1)
    if not active_path or not passive_dir then return end
    
    local timestamp = os.date("%Y-%m-%d_%H-%M-%S")
    local final_backup_path = passive_dir .. "/backup_" .. timestamp
    
    Plugin.Call(FASTCOPY_GUID, "add_task_batch", final_backup_path, active_path)
    far.Message("Бэкап-пакет сформирован:\n" .. final_backup_path, "FastCopy Backup Engine")
  end
}

Macro {
  description = "FastCopy: Копирование с автоматическим пропуском файлов по правилам .gitignore";
  area = "Shell"; key = "Ctrl+Alt+F5";
  action = function()
    local passive_dir = Panel.Item(1, 0, 1)
    local selected_count = panel.GetPanelInfo(nil, 0).SelectedItemsNumber
    local clean_paths = {}
    
    local skip_patterns = load_gitignore_rules()

    for i = 0, selected_count - 1 do
        local item_clean = panel.GetSelectedPanelItem(nil, 0, i)
        if item_clean and item_clean.FileName ~= ".." then
            local is_skipped = false
            for _, pattern in ipairs(skip_patterns) do
                if string.find(item_clean.FileName, pattern) then is_skipped = true break end
            end
            if not is_skipped then table.insert(clean_paths, item_clean.FileName) end
        end
    end

    if #clean_paths > 0 then
        Plugin.Call(FASTCOPY_GUID, "add_task_batch", passive_dir, table.unpack(clean_paths))
        far.Message("Отправлено в асинхронную очередь (с учетом Git-исключений): " .. #clean_paths, "FastCopy Git-Фильтр")
    else
        far.Message("Все выделенные файлы были отсеяны правилами .gitignore!", "FastCopy Git-Filter", ";Ok")
    end
  end
}

Macro {
  description = "FastCopy: Асинхронная вставка файлов из буфера обмена ОС Windows/Linux";
  area = "Shell"; key = "Ctrl+Shift+V";
  action = function()
    local current_dir = Panel.Item(0, 0, 1)
    local clip_data = far.PasteFromClipboard()
    if clip_data and type(clip_data) == "string" then
        local paths = {}
        for path in string.gmatch(clip_data, "[^\r\n]+") do
            if path ~= "" then table.insert(paths, path) end
        end
        if #paths > 0 then
            Plugin.Call(FASTCOPY_GUID, "add_task_batch", current_dir, table.unpack(paths))
            far.Message("Из буфера обмена ОС вставляется файлов: " .. #paths, "FastCopy Clipboard")
        end
    end
  end
}
)";
