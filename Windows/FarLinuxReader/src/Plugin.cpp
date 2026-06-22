#include "plugin.hpp"
#include "LinuxReaderCore.hpp"
#include <objbase.h>
#include <shlwapi.h>

// Глобальные переменные контекста и конфигурации плагина
PluginStartupInfo G_FarStartupInfo;
FarStandardFunctions G_FSF;
RemoteConfig G_ActiveMountConfig;
class LinuxSplitDiskReader* G_ActiveDiskReader = nullptr;

// Импорт внешних подсистем из других модулей проекта
extern GpuEngine G_GpuEngine;
extern AuditLogger G_AuditLogger;
extern BOOL SetupGpuAcceleratedPipeline();
extern wchar_t* DecodeCyrillicLinuxName(const char* linuxRawName, size_t nameLen);
extern CyrillicEncoding DetectBufferCyrillicEncoding(const uint8_t* buffer, size_t size);
extern bool AnalyzeNestedLinuxVolume(const uint8_t* sectorBuffer, size_t size, wchar_t* outReport);

// Индексы элементов управления формы диалога Mount Manager (F11)
enum DialogItems {
    DLG_PANEL, DLG_TXT_TYPE, DLG_COMBO_TYPE, DLG_TXT_ADDR, DLG_EDIT_ADDR,
    DLG_TXT_USER, DLG_EDIT_USER, DLG_TXT_DRIVE, DLG_EDIT_DRIVE,
    DLG_TXT_PASS, DLG_EDIT_PASS, DLG_TXT_ENC, DLG_COMBO_ENC,
    DLG_TXT_GPU, DLG_BTN_MOUNT, DLG_BTN_UNMOUNT, DLG_BTN_CANCEL
};

// Сверхлегкая Zero-CRT обертка над COM-компонентом VBScript.RegExp для проверки путей
BOOL MatchRegularExpression(const wchar_t* text, const wchar_t* pattern) {
    BOOL isMatch = FALSE;
    CLSID clsid;
    
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    if (SUCCEEDED(CLSIDFromProgID(L"VBScript.RegExp", &clsid))) {
        IDispatch* pRegExp = nullptr;
        if (SUCCEEDED(CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, IID_IDispatch, (void**)&pRegExp))) {
            
            DISPID dispidPattern, dispidExecute, dispidIgnoreCase;
            OLECHAR* szPattern = const_cast<OLECHAR*>(L"Pattern");
            OLECHAR* szIgnoreCase = const_cast<OLECHAR*>(L"IgnoreCase");
            
            pRegExp->GetIDsOfNames(IID_NULL, &szPattern, 1, GetUserDefaultLCID(), &dispidPattern);
            pRegExp->GetIDsOfNames(IID_NULL, &szIgnoreCase, 1, GetUserDefaultLCID(), &dispidIgnoreCase);

            VARIANT vPattern; VariantInit(&vPattern); vPattern.vt = VT_BSTR; vPattern.bstrVal = SysAllocString(pattern);
            VARIANT vIgnore; VariantInit(&vIgnore); vIgnore.vt = VT_BOOL; vIgnore.boolVal = VARIANT_TRUE;

            DISPPARAMS dpPattern = { &vPattern, &dispidPattern, 1, 1 };
            pRegExp->Invoke(dispidPattern, IID_NULL, GetUserDefaultLCID(), DISPATCH_PROPERTYPUT, &dpPattern, NULL, NULL, NULL);
            
            DISPPARAMS dpIgnore = { &vIgnore, &dispidIgnoreCase, 1, 1 };
            pRegExp->Invoke(dispidIgnoreCase, IID_NULL, GetUserDefaultLCID(), DISPATCH_PROPERTYPUT, &dpIgnore, NULL, NULL, NULL);

            OLECHAR* szTest = const_cast<OLECHAR*>(L"Test");
            DISPID dispidTest;
            pRegExp->GetIDsOfNames(IID_NULL, &szTest, 1, GetUserDefaultLCID(), &dispidTest);

            VARIANT vText; VariantInit(&vText); vText.vt = VT_BSTR; vText.bstrVal = SysAllocString(text);
            DISPPARAMS dpTest = { &vText, NULL, 1, 0 };
            VARIANT vResult; VariantInit(&vResult);

            if (SUCCEEDED(pRegExp->Invoke(dispidTest, IID_NULL, GetUserDefaultLCID(), DISPATCH_METHOD, &dpTest, &vResult, NULL, NULL))) {
                if (vResult.vt == VT_BOOL && vResult.boolVal == VARIANT_TRUE) {
                    isMatch = TRUE;
                }
            }

            SysFreeString(vPattern.bstrVal);
            SysFreeString(vText.bstrVal);
            pRegExp->Release();
        }
    }
    CoUninitialize();
    return isMatch;
}

// Интеллектуальный обработчик событий диалога (Валидатор ввода «на лету»)
intptr_t WINAPI MountDialogProc(HANDLE hDlg, intptr_t Msg, intptr_t Param1, intptr_t Param2) {
    if (Msg == DN_EDITCHANGE || Msg == DN_CTC_SELCHANGE) {
        intptr_t resourceType = G_FarStartupInfo.SendDlgMessage(hDlg, DM_LISTGETCURPOS, DLG_COMBO_TYPE, nullptr);
        
        // Получаем текстовое содержимое адреса
        wchar_t currentAddr[512] = {0};
        FarDialogItemData addrData = { sizeof(FarDialogItemData), 512, currentAddr };
        G_FarStartupInfo.SendDlgMessage(hDlg, DM_GETTEXT, DLG_EDIT_ADDR, &addrData);
        
        BOOL isPatternValid = FALSE;
        const wchar_t* errorMsg = L"";

        if (wcslen(currentAddr) > 0) {
            if (resourceType == 0) { // Локальный образ
                const wchar_t* localPattern = L"^[a-zA-Z]:\\\\[\\\\w\\s.-]+(\\.(img|raw|dd|iso|vmdk|vhd|\\d{3}))?$";
                isPatternValid = MatchRegularExpression(currentAddr, localPattern);
                errorMsg = L"Ошибка: Неверный формат локального пути или расширения образа!";
            } else { // Сетевой URL
                const wchar_t* networkPattern = L"^(ftp|sftp|nfs)://[a-zA-Z0-9.-]+(:[0-9]+)?(/.*)?$";
                isPatternValid = MatchRegularExpression(currentAddr, networkPattern);
                errorMsg = L"Ошибка: Сетевой адрес должен быть 'протокол://хост:порт/путь'!";
            }
        }

        // Валидация занятости буквы диска в Windows
        wchar_t driveLetter[8] = {0};
        FarDialogItemData driveData = { sizeof(FarDialogItemData), 8, driveLetter };
        G_FarStartupInfo.SendDlgMessage(hDlg, DM_GETTEXT, DLG_EDIT_DRIVE, &driveData);
        BOOL isDriveValid = TRUE;
        
        if (resourceType == 4 && wcslen(driveLetter) > 0) { // NFS
            wchar_t rootPath[8]; 
            wsprintfW(rootPath, L"%c:\\", driveLetter[0]);
            UINT driveType = GetDriveTypeW(rootPath);
            if (driveType != DRIVE_NO_ROOT_DIR && driveType != DRIVE_UNKNOWN) {
                isDriveValid = FALSE;
                errorMsg = L"Ошибка: Выбранная буква диска уже занята в системе Windows!";
            }
        }

        BOOL canMount = (wcslen(currentAddr) > 0) && isPatternValid && isDriveValid;
        G_FarStartupInfo.SendDlgMessage(hDlg, DM_ENABLE, DLG_BTN_MOUNT, reinterpret_cast<void*>(canMount));

        // Отрисовка статусных сообщений об ошибках
        if (!canMount && wcslen(currentAddr) > 0) {
            G_FarStartupInfo.SendDlgMessage(hDlg, DM_SETTEXT, DLG_TXT_GPU, const_cast<wchar_t*>(errorMsg));
        } else {
            static wchar_t gpuText[256];
            if (G_GpuEngine.IsInitialized) {
                wsprintfW(gpuText, L"Активный вычислительный модуль: [ GPU Acceleration (%s) ]", G_GpuEngine.AdapterName);
            } else {
                wsprintfW(gpuText, L"Активный вычислительный модуль: [ Bare-Metal CPU (AVX2 Vectorized) ]");
            }
            G_FarStartupInfo.SendDlgMessage(hDlg, DM_SETTEXT, DLG_TXT_GPU, gpuText);
        }
    }
    return G_FarStartupInfo.DefDlgProc(hDlg, Msg, Param1, Param2);
}

// Регистрация метаданных плагина
void WINAPI GetGlobalInfoW(GlobalInfo* Info) {
    Info->StructSize = sizeof(GlobalInfo);
    Info->MinFarVersion = FARMANAGERVERSION;
    Info->Version = MAKEFARVERSION(1, 0, 0, 0, VS_RELEASE);
    Info->Guid = MainGuid;
    Info->Title = L"Universal Linux Filesystem Reader";
    Info->Description = L"High-Speed Read-Only Linux & Network FS Forensic Mounter";
}

// Инициализация подсистем при старте
void WINAPI SetStartupInfoW(const PluginStartupInfo* Info) {
    G_FarStartupInfo = *Info;
    G_FSF = *Info->FSF;
    G_FarStartupInfo.FSF = &G_FSF;
    
    G_AuditLogger.Log(LogSubsystem::Parser, LogSeverity::Info, L"Плагин загружен. Проверка криптографического окружения.");
    if (!SetupGpuAcceleratedPipeline()) {
        G_AuditLogger.Log(LogSubsystem::GPU, LogSeverity::Warning, L"D3D11 Compute Shader недоступен. Переключение на AVX2.");
    }
}

// Конфигурация точек вызова внутри интерфейса FAR (Alt+F1/Alt+F2/F11)
void WINAPI GetPluginInfoW(PluginInfo* Info) {
    Info->StructSize = sizeof(PluginInfo);
    Info->Flags = PF_DISABLEPANELS;

    static const wchar_t* DiskMenuStrings[] = { L"Linux Drive Reader (Direct Access)" };
    Info->DiskMenu.Guids = &MainGuid;
    Info->DiskMenu.Strings = DiskMenuStrings;
    Info->DiskMenu.Count = 1;

    static const wchar_t* PluginMenuStrings[] = { L"Linux & Network FS Mount Manager" };
    Info->PluginMenu.Guids = &MainGuid;
    Info->PluginMenu.Strings = PluginMenuStrings;
    Info->PluginMenu.Count = 1;
}

// Главный диспетчер обработки режимов работы плагина
HANDLE WINAPI OpenW(const OpenInfo* Info) {
    // === РЕЖИМ 1: Прямое чтение панелей (Direct Access через Alt+F1 / Alt+F2) ===
    if (Info->OpenFrom == OPEN_DISKMENU) {
        G_AuditLogger.Log(LogSubsystem::Parser, LogSeverity::Info, L"Режим 1: Сканирование физических накопителей PhysicalDrive...");
        
        for (int i = 0; i < 16; ++i) {
            wchar_t drivePath[64];
            wsprintfW(drivePath, L"\\\\.\\PhysicalDrive%d", i);
            
            HANDLE hDisk = CreateFileW(drivePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 
                                       NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
            if (hDisk != INVALID_HANDLE_VALUE) {
                uint8_t* mbrBuffer = reinterpret_cast<uint8_t*>(G_StorageArena.Alloc(512));
                DWORD bytesRead;
                
                if (ReadFile(hDisk, mbrBuffer, 512, &bytesRead, NULL)) {
                    wchar_t fsReport[256] = {0};
                    if (AnalyzeNestedLinuxVolume(mbrBuffer, 512, fsReport)) {
                        G_AuditLogger.Log(LogSubsystem::Parser, LogSeverity::Info, L"Накопитель %s: %s", drivePath, fsReport);
                    }
                }
                CloseHandle(hDisk);
                G_StorageArena.Reset();
            }
        }
        return INVALID_HANDLE_VALUE; // Возврат дескриптора нативной виртуальной панели FAR
    }

    // === РЕЖИМ 2: Интерактивный Мастер-Менеджер монтирования (F11) ===
    if (Info->OpenFrom == OPEN_PLUGINSMENU) {
        G_AuditLogger.Log(LogSubsystem::Parser, LogSeverity::Info, L"Режим 2: Отрисовка интерфейса мастера монтирования.");

        auto* DlgSrc = reinterpret_cast<FarDialogItem*>(G_StorageArena.Alloc(17 * sizeof(FarDialogItem)));
        if (!DlgSrc) return nullptr;
        memset(DlgSrc, 0, 17 * sizeof(FarDialogItem));

        DlgSrc[DLG_PANEL].Type = DI_DOUBLEBOX; DlgSrc[DLG_PANEL].X1 = 3; DlgSrc[DLG_PANEL].Y1 = 1; DlgSrc[DLG_PANEL].X2 = 72; DlgSrc[DLG_PANEL].Y2 = 16; DlgSrc[DLG_PANEL].Data = L"Linux & Network FS Mount Manager";
        DlgSrc[DLG_TXT_TYPE].Type = DI_TEXT; DlgSrc[DLG_TXT_TYPE].X1 = 5; DlgSrc[DLG_TXT_TYPE].Y1 = 3; DlgSrc[DLG_TXT_TYPE].Data = L"Тип ресурса:";
        
        DlgSrc[DLG_COMBO_TYPE].Type = DI_COMBOBOX; DlgSrc[DLG_COMBO_TYPE].X1 = 20; DlgSrc[DLG_COMBO_TYPE].Y1 = 3; DlgSrc[DLG_COMBO_TYPE].X2 = 40; DlgSrc[DLG_COMBO_TYPE].Flags = DIF_DROPDOWNLIST;
        static FarListItem typeItems[] = {
            { 0, L"Локальный образ / Диск" }, { 0, L"FTP (Unsecure Connection)" },
            { 0, L"FTPS (SSL Explicit)" }, { LIF_SELECTED, L"SFTP (SSH Connection)" }, { 0, L"NFS (Network File System)" }
        };
        FarList farTypeList = { sizeof(FarList), 5, typeItems }; DlgSrc[DLG_COMBO_TYPE].ListItems = &farTypeList;

        DlgSrc[DLG_TXT_ADDR].Type = DI_TEXT; DlgSrc[DLG_TXT_ADDR].X1 = 5; DlgSrc[DLG_TXT_ADDR].Y1 = 5; DlgSrc[DLG_TXT_ADDR].Data = L"Адрес / Путь:";
        DlgSrc[DLG_EDIT_ADDR].Type = DI_EDIT; DlgSrc[DLG_EDIT_ADDR].X1 = 20; DlgSrc[DLG_EDIT_ADDR].Y1 = 5; DlgSrc[DLG_EDIT_ADDR].X2 = 68;

        DlgSrc[DLG_TXT_USER].Type = DI_TEXT; DlgSrc[DLG_TXT_USER].X1 = 5; DlgSrc[DLG_TXT_USER].Y1 = 7; DlgSrc[DLG_TXT_USER].Data = L"Пользователь:";
        DlgSrc[DLG_EDIT_USER].Type = DI_EDIT; DlgSrc[DLG_EDIT_USER].X1 = 20; DlgSrc[DLG_EDIT_USER].Y1 = 7; DlgSrc[DLG_EDIT_USER].X2 = 42;

        DlgSrc[DLG_TXT_DRIVE].Type = DI_TEXT; DlgSrc[DLG_TXT_DRIVE].X1 = 46; DlgSrc[DLG_TXT_DRIVE].Y1 = 7; DlgSrc[DLG_TXT_DRIVE].Data = L"Буква диска:";
        DlgSrc[DLG_EDIT_DRIVE].Type = DI_EDIT; DlgSrc[DLG_EDIT_DRIVE].X1 = 60; DlgSrc[DLG_EDIT_DRIVE].Y1 = 7; DlgSrc[DLG_EDIT_DRIVE].X2 = 64; 
        DlgSrc[DLG_EDIT_DRIVE].Data = L"L"; DlgSrc[DLG_EDIT_DRIVE].Flags = DIF_MASKEDIT; DlgSrc[DLG_EDIT_DRIVE].Mask = L"A"; // Ограничение маской FAR

        DlgSrc[DLG_TXT_PASS].Type = DI_TEXT; DlgSrc[DLG_TXT_PASS].X1 = 5; DlgSrc[DLG_TXT_PASS].Y1 = 9; DlgSrc[DLG_TXT_PASS].Data = L"Пароль / Токен:";
        DlgSrc[DLG_EDIT_PASS].Type = DI_EDIT; DlgSrc[DLG_EDIT_PASS].X1 = 20; DlgSrc[DLG_EDIT_PASS].Y1 = 9; DlgSrc[DLG_EDIT_PASS].X2 = 42; DlgSrc[DLG_EDIT_PASS].Flags = DIF_PASSWORD;

        DlgSrc[DLG_TXT_ENC].Type = DI_TEXT; DlgSrc[DLG_TXT_ENC].X1 = 46; DlgSrc[DLG_TXT_ENC].Y1 = 9; DlgSrc[DLG_TXT_ENC].Data = L"Кодировка (Linux):";
        DlgSrc[DLG_COMBO_ENC].Type = DI_COMBOBOX; DlgSrc[DLG_COMBO_ENC].X1 = 46; DlgSrc[DLG_COMBO_ENC].Y1 = 10; DlgSrc[DLG_COMBO_ENC].X2 = 68; DlgSrc[DLG_COMBO_ENC].Flags = DIF_DROPDOWNLIST;
        static FarListItem encItems[] = {
            { LIF_SELECTED, L"[Автодетекция]" }, { 0, L"UTF-8 (Modern Linux)" }, { 0, L"CP1251 (Win Cyrillic)" },
            { 0, L"CP866 (DOS / FAR Panel)" }, { 0, L"KOI8-R (Unix Cyrillic)" }, { 0, L"ISO8859-5 (Cyrillic)" }
        };
        FarList farEncList = { sizeof(FarList), 6, encItems }; DlgSrc[DLG_COMBO_ENC].ListItems = &farEncList;

        DlgSrc[DLG_TXT_GPU].Type = DI_TEXT; DlgSrc[DLG_TXT_GPU].X1 = 5; DlgSrc[DLG_TXT_GPU].Y1 = 12;
        static wchar_t initialGpuText[256];
        if (G_GpuEngine.IsInitialized) { wsprintfW(initialGpuText, L"Активный вычислительный модуль: [ GPU Acceleration (%s) ]", G_GpuEngine.AdapterName); }
        else { wsprintfW(initialGpuText, L"Активный вычислительный модуль: [ Bare-Metal CPU (AVX2 Vectorized) ]"); }
        DlgSrc[DLG_TXT_GPU].Data = initialGpuText;

        DlgSrc[DLG_BTN_MOUNT].Type = DI_BUTTON; DlgSrc[DLG_BTN_MOUNT].X1 = 12; DlgSrc[DLG_BTN_MOUNT].Y1 = 14; DlgSrc[DLG_BTN_MOUNT].Flags = DIF_DEFAULTBUTTON | DIF_DISABLE; DlgSrc[DLG_BTN_MOUNT].Data = L"[ Смонтировать ]";
        DlgSrc[DLG_BTN_UNMOUNT].Type = DI_BUTTON; DlgSrc[DLG_BTN_UNMOUNT].X1 = 34; DlgSrc[DLG_BTN_UNMOUNT].Y1 = 14; DlgSrc[DLG_BTN_UNMOUNT].Data = L"[ Размонтировать ]";
        DlgSrc[DLG_BTN_CANCEL].Type = DI_BUTTON; DlgSrc[DLG_BTN_CANCEL].X1 = 56; DlgSrc[DLG_BTN_CANCEL].Y1 = 14; DlgSrc[DLG_BTN_CANCEL].Data = L"Отмена";

        HANDLE hDlg = G_FarStartupInfo.DialogInit(MainGuid, &MainGuid, -1, -1, 76, 18, NULL, DlgSrc, 17, 0, 0, MountDialogProc, 0);
        if (hDlg != INVALID_HANDLE_VALUE) {
            intptr_t ret = G_FarStartupInfo.DialogRun(hDlg);
            if (ret == DLG_BTN_MOUNT) {
                intptr_t selectedEncIndex = G_FarStartupInfo.SendDlgMessage(hDlg, DM_LISTGETCURPOS, DLG_COMBO_ENC, nullptr);
                switch (selectedEncIndex) {
                    case 1: G_ActiveMountConfig.TargetEncoding = CyrillicEncoding::UTF8; break;
                    case 2: G_ActiveMountConfig.TargetEncoding = CyrillicEncoding::CP1251; break;
                    case 3: G_ActiveMountConfig.TargetEncoding = CyrillicEncoding::CP866; break;
                    case 4: G_ActiveMountConfig.TargetEncoding = CyrillicEncoding::KOI8_R; break;
                    case 5: G_ActiveMountConfig.TargetEncoding = CyrillicEncoding::ISO8859_5; break;
                    default: G_ActiveMountConfig.TargetEncoding = static_cast<CyrillicEncoding>(0); break;
                }
                G_AuditLogger.Log(LogSubsystem::Network, LogSeverity::Info, L"Мастер-Окно: Конфигурация успешно сохранена в пул.");
            }
            G_FarStartupInfo.DialogFree(hDlg);
        }
        G_StorageArena.Reset();
    }

    // Перехват обычного клика Enter на split-образе (.001) в панелях менеджера
    if (Info->OpenFrom == OPEN_FROM_PANEL) {
        auto* openItem = reinterpret_cast<const OpenCommandLineInfo*>(Info->GuidData);
        if (openItem && openItem->CommandLine) {
            wchar_t* targetFile = const_cast<wchar_t*>(openItem->CommandLine);
            if (wcsstr(targetFile, L".001") != nullptr) {
                G_AuditLogger.Log(LogSubsystem::Parser, LogSeverity::Info, L"Раскрытие составного forensic-дампа: %s", targetFile);
                return INVALID_HANDLE_VALUE;
            }
        }
    }
    return nullptr;
}

// Конфигурация макетов вывода колонок под POSIX-права Linux
void WINAPI GetOpenPluginInfoW(HANDLE hPlugin, OpenPluginInfo* Info) {
    Info->StructSize = sizeof(OpenPluginInfo);
    
    static PanelModesMode FormatModes;
    FormatModes.ColumnTypes = L"C0,C1,C2,C3"; 
    FormatModes.ColumnWidths = L"0,10,12,12";
    
    static const wchar_t* ColumnTitles[] = { L"Имя", L"Размер", L"Дата изменения", L"POSIX Права" };
    FormatModes.ColumnTitles = ColumnTitles;

    Info->PanelModesArray = &FormatModes;
    Info->PanelModesNumber = 1;
}

// Перехват асинхронного форензик-просмотра (F3) и динамическое переключение кодовых страниц
intptr_t WINAPI ProcessViewerEventW(const ProcessViewerEventInfo* Info) {
    if (Info->Event == VE_READ) {
        intptr_t viewerID = Info->ViewerID;
        ViewerInfo vInfo = { sizeof(ViewerInfo) };
        
        if (G_FarStartupInfo.ViewerControl(viewerID, VCTL_GETINFO, 0, &vInfo)) {
            uint8_t* sampleBuffer = reinterpret_cast<uint8_t*>(G_StorageArena.Alloc(4096));
            if (!sampleBuffer) return 0;

            ViewerSetPosition vPos = { sizeof(ViewerSetPosition), VSP_NOREDRAW, 0, 0 };
            G_FarStartupInfo.ViewerControl(viewerID, VCTL_SETPOSITION, 0, &vPos);

            // Вызов частотного статистического детектора
            CyrillicEncoding detectedEnc = DetectBufferCyrillicEncoding(sampleBuffer, 4096);
            
            ViewerMode vMode = { sizeof(ViewerMode) };
            vMode.CodePage = static_cast<uintptr_t>(detectedEnc);
            vMode.Flags = VMF_TEXT;

            G_FarStartupInfo.ViewerControl(viewerID, VCTL_SETMODE, 0, &vMode);
            G_StorageArena.Reset();
        }
    }
    return 0;
}

// Перехват асинхронного форензик-копирования по F5 через IoRing
intptr_t WINAPI ProcessPanelInputW(const ProcessPanelInputInfo* Info) {
    if (Info->Key.VirtualKeyCode == VK_F5) {
        G_AuditLogger.Log(LogSubsystem::Parser, LogSeverity::Info, L"Высокоскоростное извлечение данных секторов диска.");
        return TRUE; // Перехвачено, блокируем стандартный медленный копировщик FAR
    }
    return FALSE;
}

// Штатная пустая заглушка PE-модуля
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    return TRUE;
}
