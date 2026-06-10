#include "core.h"
#include <vector>

PluginStartupInfo Info; 
FarStandardFunctions FSF;

// --- РЕАЛИЗАЦИЯ ФИЛЬТРАЦИИ FAR 3 SDK (Исправляет ошибку линкера) ---
bool IsFileAllowedByFarFilter(HANDLE hFilter, const std::wstring& fileName, uint32_t attributes, uint64_t fileSize) {
    if (!hFilter) return true;

    PluginPanelItem item{};
    // Защищаем память: делаем копию строки, так как SDK требует неконстантный wchar_t*
    std::vector<wchar_t> nameBuf(fileName.begin(), fileName.end());
    nameBuf.push_back(L'\0');

    item.FileName = nameBuf.data();
    item.FileAttributes = attributes;
    item.FileSize = fileSize;

    // В FAR 3 вызов проверки элемента в созданном фильтре делается через FileFilterControl
    return Info.FileFilterControl(hFilter, FFCTL_ISFILEINFILTER, 0, &item) != 0;
}

// Функция проверки: является ли путь сетевым или удаленной точкой монтирования FUSE
bool IsNetworkPathStr(const std::wstring& path) {
    if (path.length() >= 2 && path[0] == L'\\' && path[1] == L'\\') return true;
    if (path.find(L"/mnt/") == 0 || path.find(L"/media/") == 0 || 
        path.find(L"/.local/share/far2l/mnt/") == 0 || path.find(L"/.local/share/far2m/mnt/") == 0) { 
        return true; 
    }
    return false;
}

// Динамический контроль версий ресурсов (.lng/.hlf) и макросов
void CheckAndCleanPluginResources() {
    std::string v = ""; std::ifstream f("version.txt");
    if (f.is_open()) { std::getline(f, v); f.close(); }
    if (v != CURRENT_PLUGIN_VERSION) {
#if defined(_WIN32)
        DeleteFileW(L"FastCopyEng.lng"); DeleteFileW(L"scripts\\FastCopyExtensions.lua");
#else
        std::remove("FastCopyEng.lng"); std::remove("FastCopyExtensions.lua");
#endif
        std::ofstream out("version.txt"); out << CURRENT_PLUGIN_VERSION; out.close();
    }
}

// Автогенерация файлов локализации и справки при первом старте из внутренних строк
void DeployResourcesOnDemand() {
    CheckAndCleanPluginResources();
    std::ifstream check("FastCopyEng.lng");
    if (!check.is_open()) {
        std::ofstream lng("FastCopyEng.lng");
        lng << ".Language=English,English\n\"FastCopy Engine\"\n\"[ FastCopy Async ]\"\n\"[ FastMove Inode ]\"\n";
        lng.close();
    }
#if defined(_WIN32)
    CreateDirectoryW(L"scripts", nullptr); std::string macroPath = "scripts\\FastCopyExtensions.lua";
#else
    std::string macroPath = "FastCopyExtensions.lua";
#endif
    std::ifstream checkLua(macroPath);
    if (!checkLua.is_open()) {
        std::ofstream luaFile(macroPath, std::ios::binary | std::ios::out);
        if (luaFile.is_open()) { luaFile.write(EMBEDDED_LUA_MACRO.data(), EMBEDDED_LUA_MACRO.size()); luaFile.close(); }
    }
}

DLL_EXPORT void WINAPI SetStartupInfoW(const PluginStartupInfo *pInfo) { Info = *pInfo; FSF = *pInfo->FSF; DeployResourcesOnDemand(); }
DLL_EXPORT void WINAPI GetPluginInfoW(PluginInfo *pInfo) { pInfo->StructSize = sizeof(PluginInfo); pInfo->Flags = PF_DISABLEPANELS; }
DLL_EXPORT void WINAPI ExitFARW(const ExitInfo * /*pInfo*/) { FastCopyQueueManager::Instance().Shutdown(); }

// Нативная отправка строки макроса в FAR 3
void HandleSynchroEvent(void* param) {
    if (!param) return; 
    wchar_t* scr = reinterpret_cast<wchar_t*>(param);
    
    MacroSendMacroText mt{};
    mt.StructSize = sizeof(MacroSendMacroText);
    mt.SequenceText = scr; 
    Info.MacroControl(&MainGuid, MCTL_SENDSTRING, 0, &mt);
    delete[] scr;
}

// Синхро-вызов сообщений под Windows
DLL_EXPORT intptr_t WINAPI ProcessSynchroEventW(const struct ProcessSynchroEventInfo *InfoStruct) {
    if (InfoStruct) { 
        HandleSynchroEvent(InfoStruct->Param); 
        return TRUE; 
    }
    return FALSE;
}

extern bool GetFileAttributesFast(const std::wstring&, FileTimePoint&, uint64_t&);
extern void DiscoverDirectoryRecursive(const std::wstring&, const std::wstring&, FastCopyBatchOptions&);

// Сбор данных для фонового выполнения по глобальным хоткеям панелей менеджера
void HandleGlobalHotkey(TaskType type) {
    FastCopyBatchOptions b; b.type = type;
    PanelInfo passivePanel{}; passivePanel.StructSize = sizeof(PanelInfo);
    Info.PanelControl(PANEL_PASSIVE, FCTL_GETPANELINFO, 0, &passivePanel);
    
    size_t dirSize = Info.PanelControl(PANEL_PASSIVE, FCTL_GETPANELDIRECTORY, 0, nullptr);
    std::vector<char> dirBuf(dirSize); FarPanelDirectory* fpd = reinterpret_cast<FarPanelDirectory*>(dirBuf.data());
    fpd->StructSize = sizeof(FarPanelDirectory);
    Info.PanelControl(PANEL_PASSIVE, FCTL_GETPANELDIRECTORY, dirSize, fpd);
    b.destDirectory = fpd->Name;

    PanelInfo activePanel{}; activePanel.StructSize = sizeof(PanelInfo);
    Info.PanelControl(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, &activePanel);
    for (size_t i = 0; i < activePanel.SelectedItemsNumber; ++i) {
        size_t size = Info.PanelControl(PANEL_ACTIVE, FCTL_GETSELECTEDPANELITEM, i, nullptr);
        std::vector<char> itemBuf(size); PluginPanelItem* item = reinterpret_cast<PluginPanelItem*>(itemBuf.data());
        FarGetPluginPanelItem gpi{}; gpi.StructSize = sizeof(FarGetPluginPanelItem); gpi.Size = size; gpi.Item = item;
        Info.PanelControl(PANEL_ACTIVE, FCTL_GETSELECTEDPANELITEM, i, &gpi);
        
        // Безопасное конструирование строки из сырого указателя SDK
        std::wstring fName = item->FileName ? item->FileName : L"";
        if (fName.empty() || fName == L"..") continue;
        
        if (item->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) { DiscoverDirectoryRecursive(fName, L"", b); }
        else { TaskItem task; task.srcPath = fName; task.fileSize = item->FileSize; b.items.push_back(task); }
    }
    if (!b.items.empty()) FastCopyQueueManager::Instance().PushTask(std::move(b));
}

DLL_EXPORT intptr_t WINAPI ProcessConsoleInputW(ProcessConsoleInputInfo *pInfo) {
    if (pInfo && pInfo->Rec.EventType == KEY_EVENT && pInfo->Rec.Event.KeyEvent.bKeyDown) {
        WORD vk = pInfo->Rec.Event.KeyEvent.wVirtualKeyCode; DWORD ctrl = pInfo->Rec.Event.KeyEvent.dwControlKeyState;
        bool isCtrlShift = (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) && (ctrl & SHIFT_PRESSED);
        if (isCtrlShift) {
            if (vk == VK_F5) { HandleGlobalHotkey(TaskType::Copy); return TRUE; }
            if (vk == VK_F6) { HandleGlobalHotkey(TaskType::Move); return TRUE; }
            if (vk == 'P') {
                FastCopyQueueManager::Instance().TogglePause();
                const wchar_t* msg = FastCopyQueueManager::Instance().IsPaused() ? L"FastCopy: [PAUSED]" : L"FastCopy: [RESUMED]";
                Info.Message(&MainGuid, nullptr, FMSG_LEFTALIGN, nullptr, &msg, 1, 0); return TRUE;
            }
        }
    }
    return FALSE;
}

void RouteBatchTask(HANDLE hDlg, TaskType type) {
    FastCopyBatchOptions b; b.type = type;
    size_t len = Info.SendDlgMessage(hDlg, DM_GETTEXT, 2, nullptr);
    if (len > 0) {
        std::vector<wchar_t> buf(len + 1); FarDialogItemData id{ sizeof(FarDialogItemData), buf.size(), buf.data() };
        Info.SendDlgMessage(hDlg, DM_GETTEXT, 2, &id); b.destDirectory = buf.data();
    }
    b.useFilter = (Info.SendDlgMessage(hDlg, DM_GETCHECK, 8, nullptr) == 1);
    if (b.useFilter) Info.FileFilterControl(INVALID_HANDLE_VALUE, FFCTL_CREATEFILEFILTER, FFT_COPY, &b.hFarFilter);

    PanelInfo pi{}; pi.StructSize = sizeof(PanelInfo); Info.PanelControl(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, &pi);
    for (size_t i = 0; i < pi.SelectedItemsNumber; ++i) {
        size_t sz = Info.PanelControl(PANEL_ACTIVE, FCTL_GETSELECTEDPANELITEM, i, nullptr);
        std::vector<char> buf(sz); PluginPanelItem* item = reinterpret_cast<PluginPanelItem*>(buf.data());
        FarGetPluginPanelItem gpi{}; gpi.StructSize = sizeof(FarGetPluginPanelItem); gpi.Size = sz; gpi.Item = item;
        Info.PanelControl(PANEL_ACTIVE, FCTL_GETSELECTEDPANELITEM, i, &gpi);
        
        std::wstring fName = item->FileName ? item->FileName : L"";
        if (fName.empty() || fName == L"..") continue;
        
        if (item->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) { DiscoverDirectoryRecursive(fName, L"", b); }
        else { TaskItem t; t.srcPath = fName; t.fileSize = item->FileSize; b.items.push_back(t); }
    }
    Info.SendDlgMessage(hDlg, DM_CLOSE, -1, nullptr);
    if (b.hFarFilter) { Info.FileFilterControl(b.hFarFilter, FFCTL_FREEFILEFILTER, 0, nullptr); b.hFarFilter = nullptr; }
    if (!b.items.empty()) FastCopyQueueManager::Instance().PushTask(std::move(b));
}

intptr_t WINAPI FastCopyDlgProc(HANDLE hDlg, intptr_t Msg, intptr_t Param1, void* Param2, TaskType type) {
    if (Msg == DN_INITDIALOG) {
        RECT rc{};
        if (Info.SendDlgMessage(hDlg, DM_GETDLGRECT, 0, &rc)) {
            COORD sz;
            sz.X = static_cast<SHORT>(rc.right - rc.left + 1);
            sz.Y = static_cast<SHORT>(rc.bottom - rc.top + 3); 
            Info.SendDlgMessage(hDlg, DM_RESIZEDIALOG, 0, &sz);
            
            FarDialogItem it{}; it.Type = DI_BUTTON; it.X1 = 5; it.Y1 = sz.Y - 3; it.X2 = 24; it.Y2 = sz.Y - 3;
            it.Flags = DIF_BTNNOCLOSE; it.Data = (type == TaskType::Move) ? L"[ FastMove Inode ]" : L"[ FastCopy Async ]";
            Info.SendDlgMessage(hDlg, DM_SETDLGITEM, 100, &it);
        }
    } else if (Msg == DN_BTNCLICK && Param1 == 100) { RouteBatchTask(hDlg, type); return TRUE; }
    return Info.DefDlgProc(hDlg, Msg, Param1, Param2);
}

DLL_EXPORT intptr_t WINAPI ProcessDialogEventW(const ProcessDialogEventInfo *pInfo) {
    if (pInfo && pInfo->Event == DE_DLGPROCINIT) {
        FarDialogEvent* de = reinterpret_cast<FarDialogEvent*>(pInfo->Param);
        if (de && de->hDlg) {
            GUID g;
            if (Info.SendDlgMessage(de->hDlg, DM_GETDIALOGINFO, 0, &g)) {
                TaskType t; bool target = false;
                if (g == FarCopyDlgGuid) { t = TaskType::Copy; target = true; }
                else if (g == FarMoveDlgGuid) { t = TaskType::Move; target = true; }
                if (target) {
                    return FastCopyDlgProc(de->hDlg, de->Msg, de->Param1, de->Param2, t);
                }
            }
        }
    }
    return FALSE;
}

// --- СУПЕР-РАСШИРЕННЫЙ ОБРАБОТЧИК ДЛЯ МАКРОСОВ (ПЛОСКИЙ СКАН АРГУМЕНТОВ FAR 3 SDK) ---
DLL_EXPORT HANDLE WINAPI OpenW(const OpenInfo *pInfo) {
    if (pInfo && pInfo->OpenFrom == OPEN_FROMMACRO) {
        const OpenMacroInfo* omi = reinterpret_cast<const OpenMacroInfo*>(pInfo->Guid);
        if (omi && omi->Count > 0 && omi->Values[0].Type == FMVT_STRING) {
            std::wstring cmd = omi->Values[0].String;
            if (cmd == L"add_task_batch" && omi->Count >= 3) {
                FastCopyBatchOptions customBatch;
                customBatch.type = TaskType::Copy;
                if (omi->Values[1].Type == FMVT_STRING) {
                    customBatch.destDirectory = omi->Values[1].String;
                }
                if (IsNetworkPathStr(customBatch.destDirectory)) {
                    customBatch.useFilter = true; 
                }

                for (size_t idx = 2; idx < omi->Count; ++idx) {
                    if (omi->Values[idx].Type == FMVT_STRING) {
                        TaskItem item;
                        item.srcPath = omi->Values[idx].String;
                        FileTimePoint dummyTime;
                        GetFileAttributesFast(item.srcPath, dummyTime, item.fileSize);
                        customBatch.items.push_back(item);
                    }
                }
                if (!customBatch.items.empty()) {
                    FastCopyQueueManager::Instance().PushTask(std::move(customBatch));
                }
                return reinterpret_cast<HANDLE>(1);
            }
            if (cmd == L"toggle_pause") { FastCopyQueueManager::Instance().TogglePause(); }
        }
    }
    return nullptr;
}
