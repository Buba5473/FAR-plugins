// =========================================================================
#include <winsock2.h> // Обязательно первым во избежание конфликтов макросов Windows
#include <windows.h>
#include <initguid.h>
#include "plugin.hpp"
#include "LinuxReaderCore.hpp" // Импортирует ThreadLocalArena, маски POSIX и хэши Crossmeta
#include "LinuxReaderIo.hpp"
#include "LinuxFsNetwork.hpp"

// Реализация перечисления типов для каскадного автодетектора (FAR 3)
enum class LinuxFsType {
    Unknown, Btrfs, Ext4, XFS, ZFS, F2FS, UFS2, HFSPlus, APFS, ReiserFS, Reiser4, SquashFS
};

// Контекст инстанса открытой панели FAR
struct PluginInstance {
    HANDLE            hPanel;
    LinuxDiskReader*  DiskReader;
    LinuxFsType       DetectedFs;
    uint64_t          BtrfsSubvolumeVaddr;
    uint32_t          Ext4CurrentInode;
    uint32_t          Ext4BlockSize;
    uint32_t          XfsAgBlocksCached;
    uint32_t          F2fsMainBlkaddrCached;
    ext4_super_block  Ext4SbCached;
    uint8_t           DevicePathBuf[260];
};

// Инстанцирование глобального экземпляра Thread-Local Арены памяти текущего потока
thread_local ThreadLocalArena G_StorageArena;

// Уникальные GUID элементов плагина (Сгенерированы для интеграции в FAR 3)
// {A5E397B1-4A5D-4F18-B5B0-1F0A6BDE0F60}
DEFINE_GUID(MainGuid, 0xa5e397b1, 0x4a5d, 0x4f18, 0xb5, 0xb0, 0x1f, 0x0a, 0x6b, 0xde, 0xf, 0x60);
// {B2F4E211-19BE-4B17-A1C8-D8C57088D162}
DEFINE_GUID(MenuGuid, 0xb2f4e211, 0x19be, 0x4b17, 0xa1, 0xc8, 0xd8, 0xc5, 0x70, 0x88, 0xd1, 0x62);
// {C3A144F2-3F1E-4D11-BD77-A96C2084E71A}
DEFINE_GUID(DiskMenuGuid, 0xc3a144f2, 0x3f1e, 0x4d11, 0xbd, 0x77, 0xa9, 0x6c, 0x20, 0x84, 0xe7, 0x1a);
// {D4B255E3-4F2A-4E12-CE88-B07D3095F82B}
DEFINE_GUID(ConfigDialogGuid, 0xd4b255e3, 0x4f2a, 0x4e12, 0xce, 0x88, 0xb0, 0x7d, 0x30, 0x95, 0xf8, 0x2b);

// Глобальные переменные контекста FAR SDK
PluginStartupInfo PSI;
FarStandardFunctions FSF;

struct PluginSettings {
    BOOL ShowInDiskMenu;
    BOOL SIMD_Checksum;
} Config = { TRUE, TRUE };

// Декларации внешних модулей из LinuxFsParsers
bool DetectBtrfsStructure(const wchar_t* volumePath);
bool Ext4Parser_LoadSuperblock(LinuxDiskReader* reader, ext4_super_block* sb, uint32_t* outBlockSize);
bool XfsParser_LoadSuperblock(LinuxDiskReader* reader, xfs_sb* sb, uint32_t* outBlockSize);
bool ZfsDetector_DetectZfsPool(LinuxDiskReader* reader, uint64_t* outHighestTxg, uint64_t* outActiveUberblockOffset);
bool F2fsParser_LoadSuperblock(LinuxDiskReader* reader, f2fs_super_block* sb, uint32_t* outBlockSize);
bool DetectUfs2(LinuxDiskReader* reader);
bool DetectHfsPlus(LinuxDiskReader* reader);
bool DetectApfs(LinuxDiskReader* reader);
bool DetectReiserFs(LinuxDiskReader* reader, bool* isReiser4);
bool DetectSquashFs(LinuxDiskReader* reader);

bool BtrfsReadDirectory(LinuxDiskReader* reader, uint64_t subvolRootVaddr, uint64_t dirInodeId, PluginPanelItem** panelItems, size_t* itemsCount);
bool Ext4ReadDirectory(LinuxDiskReader* reader, uint32_t inodeId, const ext4_super_block* sb, uint32_t blockSize, PluginPanelItem** panelItems, size_t* itemsCount);
bool F2fsReadDirectory(LinuxDiskReader* reader, uint32_t inodeId, uint32_t mainBlkaddr, uint32_t blockSize, PluginPanelItem** panelItems, size_t* itemsCount);
bool XfsReadDirectory(LinuxDiskReader* reader, uint64_t inodeId, uint32_t blockSize, uint32_t agBlocks, PluginPanelItem** panelItems, size_t* itemsCount);

#ifdef _MSC_VER
#pragma comment(linker, "/ENTRY:DllMain")
#endif

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    return TRUE;
}

void WINAPI SetStartupInfoW(const struct PluginStartupInfo *Info) {
    PSI = *Info;
    FSF = *Info->FSF;
    PSI.FSF = &FSF;
}

void WINAPI GetPluginInfoW(struct PluginInfo *Info) {
    Info->StructSize = sizeof(struct PluginInfo);
    Info->Flags = PF_DISABLEPANELS;

    static const wchar_t* PluginMenuStrings[] = { L"Linux & Network FS Mount Manager" };
    Info->PluginMenu.Guids = &MenuGuid;
    Info->PluginMenu.Strings = PluginMenuStrings;
    Info->PluginMenu.Count = 1;

    static const wchar_t* DiskMenuStrings[] = { L"Linux Drive Reader" };
    Info->DiskMenu.Guids = &DiskMenuGuid;
    Info->DiskMenu.Strings = DiskMenuStrings;
    Info->DiskMenu.Count = Config.ShowInDiskMenu ? 1 : 0;
}

// Приведено к строгому типу FARWINDOWPROC: четвертый параметр void* Param2 вместо intptr_t
intptr_t WINAPI MountDlgProc(HANDLE hDlg, intptr_t Msg, intptr_t Param1, void* Param2) {
    if (Msg == DN_INITDIALOG) {
        PSI.SendDlgMessage(hDlg, DM_ENABLE, ID_EDIT_USER, reinterpret_cast<void*>(FALSE));
        PSI.SendDlgMessage(hDlg, DM_ENABLE, ID_EDIT_PASS, reinterpret_cast<void*>(FALSE));
    }
    else if (Msg == DN_BTNCLICK) {
        if (Param1 == ID_RADIO_NET) {
            PSI.SendDlgMessage(hDlg, DM_ENABLE, ID_EDIT_USER, reinterpret_cast<void*>(TRUE));
            PSI.SendDlgMessage(hDlg, DM_ENABLE, ID_EDIT_PASS, reinterpret_cast<void*>(TRUE));
        } else if (Param1 == ID_RADIO_LOCAL || Param1 == ID_RADIO_IMAGE) {
            PSI.SendDlgMessage(hDlg, DM_ENABLE, ID_EDIT_USER, reinterpret_cast<void*>(FALSE));
            PSI.SendDlgMessage(hDlg, DM_ENABLE, ID_EDIT_PASS, reinterpret_cast<void*>(FALSE));
        }
    }
    return PSI.DefDlgProc(hDlg, Msg, Param1, Param2);
}

intptr_t WINAPI ConfigureW(const struct ConfigureInfo *Info) {
    MountSlot* activeSlots = nullptr;
    size_t count = G_MountManager.GetSlots(&activeSlots);

    FarListItem* listItems = reinterpret_cast<FarListItem*>(G_StorageArena.Alloc(sizeof(FarListItem) * (count + 1)));
    wchar_t* itemBuffers = reinterpret_cast<wchar_t*>(G_StorageArena.Alloc(sizeof(wchar_t) * 128 * (count + 1)));

    for (size_t i = 0; i < count; ++i) {
        wchar_t* buf = &itemBuffers[i * 128];
        FSF.sprintf(buf, L"[%s]   %s     %s   %s", 
                    activeSlots[i].IsActive ? L"Актив" : L"Офлайн", 
                    activeSlots[i].MountLetter, activeSlots[i].FsType, activeSlots[i].SourcePath);
        listItems[i].Text = buf;
    }
    if (count == 0) {
        listItems[0].Text = L" Нет active монтирований. Добавьте новый источник ниже.";
    }

    FarList farList{ sizeof(FarList), static_cast<size_t>(count == 0 ? 1 : count), listItems };

    FarDialogItem DialogItems[] = {
        {DI_DOUBLEBOX, 3, 1, 74, 19, 0, nullptr, nullptr, 0, L" Linux & Network FS Mount Manager "},
        {DI_TEXT,      5, 3, 0,  3, 0, nullptr, nullptr, 0, L"Статус  Буква  Тип ФС    Источник / Удаленный адрес"},
        {DI_LISTBOX,   5, 4, 72, 7, 0, nullptr, nullptr, DIF_LISTNOBOX | DIF_LISTTRACKMOUSE, L""},
        
        {DI_TEXT,      5, 9, 0,  9, 0, nullptr, nullptr, 0, L"Тип источника:"},
        {DI_RADIOBUTTON, 21,9, 0,  9, DIF_GROUP, nullptr, nullptr, 1, L"Диск"},
        {DI_RADIOBUTTON, 30,9, 0,  9, 0,         nullptr, nullptr, 0, L"Образ"},
        {DI_RADIOBUTTON, 40,9, 0,  9, 0,         nullptr, nullptr, 0, L"Сеть (SFTP)"},
        
        {DI_TEXT,      5, 11, 0, 11, 0, nullptr, nullptr, 0, L"Путь/Адрес:"},
        {DI_EDIT,      18,11,72, 11, 0, nullptr, nullptr, DIF_EDITPATH, L""},
        
        {DI_TEXT,      5, 13, 0, 13, 0, nullptr, nullptr, 0, L"Логин:"},
        {DI_EDIT,      13,13,32, 13, 0, nullptr, nullptr, 0, L""},
        {DI_TEXT,      35,13, 0, 13, 0, nullptr, nullptr, 0, L"Пароль:"},
        {DI_EDIT,      44,13,72, 13, 0, nullptr, nullptr, DIF_EDITPATHWITHPASSWORD, L""}, // Исправлено под FAR 3
        
        {DI_TEXT,      5, 15, 0, 15, 0, nullptr, nullptr, 0, L"Буква:"},
        {DI_EDIT,      13,15,18, 15, 0, nullptr, nullptr, 0, L"Z:"},
        {DI_TEXT,      23,15, 0, 15, 0, nullptr, nullptr, 0, L"Тип ФС:"},
        {DI_EDIT,      32,15,45, 15, 0, nullptr, nullptr, 0, L"Ext4"}, 
        
        {DI_BUTTON,    5, 17, 0, 17, 0, nullptr, nullptr, DIF_DEFAULTBUTTON, L"Смонтировать"},
        {DI_BUTTON,    25,17, 0, 17, 0, nullptr, nullptr, 0, L"Размонтировать"},
        {DI_BUTTON,    50,17, 0, 17, 0, nullptr, nullptr, 0, L"Закрыть"}
    };

    DialogItems[ID_MOUNT_LIST].ListItems = &farList;

    // Передаем &MainGuid вместо удаленного ModuleNumber
    HANDLE hDlg = PSI.DialogInit(&MainGuid, &ConfigDialogGuid, -1, -1, 78, 21, 
                                 nullptr, DialogItems, sizeof(DialogItems)/sizeof(DialogItems), 
                                 0, 0, MountDlgProc, nullptr);
    
    if (hDlg != INVALID_HANDLE_VALUE) {
        intptr_t ExitCode = PSI.DialogRun(hDlg);
        
        if (ExitCode == ID_BTN_MOUNT) {
            MountSlot newSlot{};
            newSlot.IsActive = TRUE;
            
            if (PSI.SendDlgMessage(hDlg, DM_GETCHECK, ID_RADIO_LOCAL, nullptr)) newSlot.SourceType = MOUNT_TYPE_LOCAL;
            else if (PSI.SendDlgMessage(hDlg, DM_GETCHECK, ID_RADIO_IMAGE, nullptr)) newSlot.SourceType = MOUNT_TYPE_IMAGE;
            else newSlot.SourceType = MOUNT_TYPE_NET;

            FarDialogItemData fData{ sizeof(FarDialogItemData), 259, newSlot.SourcePath };
            PSI.SendDlgMessage(hDlg, DM_GETTEXT, ID_EDIT_PATH, &fData);

            fData.PtrData = newSlot.Username; fData.PtrLength = 63;
            PSI.SendDlgMessage(hDlg, DM_GETTEXT, ID_EDIT_USER, &fData);

            fData.PtrData = newSlot.Password; fData.PtrLength = 63;
            PSI.SendDlgMessage(hDlg, DM_GETTEXT, ID_EDIT_PASS, &fData);

            fData.PtrData = newSlot.MountLetter; fData.PtrLength = 3;
            PSI.SendDlgMessage(hDlg, DM_GETTEXT, ID_EDIT_LETTER, &fData);

            fData.PtrData = newSlot.FsType; fData.PtrLength = 11;
            PSI.SendDlgMessage(hDlg, DM_GETTEXT, ID_COMBO_FS, &fData);

            G_MountManager.CreateMount(newSlot);
        }
        else if (ExitCode == ID_BTN_UNMOUNT) {
            // Исправлено под макрос FAR 3: DM_LISTGETCURINDEX вместо DM_GETLISTINDEX
            intptr_t listIdx = PSI.SendDlgMessage(hDlg, DM_LISTGETCURINDEX, ID_MOUNT_LIST, nullptr);
            if (listIdx >= 0 && listIdx < static_cast<intptr_t>(count)) {
                G_MountManager.DeleteMount(activeSlots[listIdx].MountLetter);
            }
        }
        PSI.DialogFree(hDlg);
    }
    
    G_StorageArena.Reset();
    return TRUE;
}

HANDLE WINAPI OpenW(const struct OpenInfo *Info) {
    if (Info->OpenFrom != OPEN_LEFTDISKMENU && Info->OpenFrom != OPEN_RIGHTDISKMENU) return INVALID_HANDLE_VALUE;

    auto* Instance = reinterpret_cast<PluginInstance*>(G_StorageArena.Alloc(sizeof(PluginInstance)));
    if (!Instance) return INVALID_HANDLE_VALUE;

    lstrcpyW(reinterpret_cast<wchar_t*>(Instance->DevicePathBuf), L"\\\\.\\PhysicalDrive0"); 
    GlobalDiskReader.OpenDevice(reinterpret_cast<const wchar_t*>(Instance->DevicePathBuf));
    Instance->DiskReader = &GlobalDiskReader;
    Instance->DetectedFs = LinuxFsType::Unknown;

    ext4_super_block ext4_sb_data;
    xfs_sb           xfs_sb_data;
    f2fs_super_block f2fs_sb_data;
    uint32_t         computedBlockSize = 0;

    if (DetectBtrfsStructure(reinterpret_cast<const wchar_t*>(Instance->DevicePathBuf))) {
        Instance->DetectedFs = LinuxFsType::Btrfs;
        Instance->BtrfsSubvolumeVaddr = 0x500000;
    } 
    else if (Ext4Parser_LoadSuperblock(Instance->DiskReader, &ext4_sb_data, &computedBlockSize)) {
        Instance->DetectedFs = LinuxFsType::Ext4;
        Instance->Ext4CurrentInode = 2;
        Instance->Ext4BlockSize = computedBlockSize;
        __movsb(reinterpret_cast<BYTE*>(&Instance->Ext4SbCached), reinterpret_cast<const BYTE*>(&ext4_sb_data), sizeof(ext4_super_block));
    } 
    else if (XfsParser_LoadSuperblock(Instance->DiskReader, &xfs_sb_data, &computedBlockSize)) {
        Instance->DetectedFs = LinuxFsType::XFS;
        Instance->BtrfsSubvolumeVaddr = xfs_sb_data.sb_rootino;
        Instance->Ext4BlockSize = computedBlockSize;
        Instance->XfsAgBlocksCached = xfs_sb_data.sb_agblocks;
    } 
    else if (ZfsDetector_DetectZfsPool(Instance->DiskReader, nullptr, &Instance->BtrfsSubvolumeVaddr)) {
        Instance->DetectedFs = LinuxFsType::ZFS;
    } 
    else if (F2fsParser_LoadSuperblock(Instance->DiskReader, &f2fs_sb_data, &computedBlockSize)) {
        Instance->DetectedFs = LinuxFsType::F2FS;
        Instance->Ext4CurrentInode = f2fs_sb_data.root_ino;
        Instance->Ext4BlockSize = computedBlockSize;
        Instance->F2fsMainBlkaddrCached = f2fs_sb_data.main_blkaddr;
    }
    else if (DetectUfs2(Instance->DiskReader))     { Instance->DetectedFs = LinuxFsType::UFS2; }
    else if (DetectHfsPlus(Instance->DiskReader))  { Instance->DetectedFs = LinuxFsType::HFSPlus; }
    else if (DetectApfs(Instance->DiskReader))     { Instance->DetectedFs = LinuxFsType::APFS; }
    else if (DetectSquashFs(Instance->DiskReader)) { Instance->DetectedFs = LinuxFsType::SquashFS; }
    else {
        bool isReiser4 = false;
        if (DetectReiserFs(Instance->DiskReader, &isReiser4)) {
            Instance->DetectedFs = isReiser4 ? LinuxFsType::Reiser4 : LinuxFsType::ReiserFS;
        }
    }

    if (Instance->DetectedFs == LinuxFsType::Unknown) {
        GlobalDiskReader.CloseDevice();
        return INVALID_HANDLE_VALUE;
    }

    auto* farInstance = reinterpret_cast<PluginInstance*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PluginInstance)));
    if (farInstance) {
        __movsb(reinterpret_cast<BYTE*>(farInstance), reinterpret_cast<const BYTE*>(Instance), sizeof(PluginInstance));
        return reinterpret_cast<HANDLE>(farInstance);
    }

    return INVALID_HANDLE_VALUE;
}

void WINAPI GetOpenPanelInfoW(struct OpenPanelInfo *Info) {
    auto* instance = reinterpret_cast<PluginInstance*>(Info->hPanel);
    if (!instance) return;

    Info->StructSize = sizeof(struct OpenPanelInfo);
    Info->Flags = OPIF_SHOWPRESERVECASE; 

    static wchar_t panelTitle;
    const wchar_t* fsNames[] = { L"Unknown", L"Btrfs", L"Ext4", L"XFS", L"ZFS", L"F2FS", L"UFS2", L"HFS+", L"APFS", L"ReiserFS", L"Reiser4", L"SquashFS" };
    FSF.sprintf(&panelTitle, L" %s Reader (2026 Build) ", fsNames[static_cast<int>(instance->DetectedFs)]);
    Info->PanelTitle = &panelTitle;

    static PanelMode modes;
    SecureZeroMemory(&modes, sizeof(modes));
    modes.ColumnTypes = L"N,S,C0";
    modes.ColumnWidths = L"0,10,11";
    
    static const wchar_t* columnTitles[] = { L"Name", L"Size", L"Rights" };
    modes.ColumnTitles = columnTitles;

    Info->PanelModesArray = &modes;
    Info->PanelModesNumber = 1;
    Info->StartPanelMode = L'0';

    static wchar_t statusBuffer;
    wchar_t netErrorMsg;

    if (G_NetLogger.GetLastErrorString(&netErrorMsg, 256)) {
        lstrcpyW(&statusBuffer, &netErrorMsg);
        Info->CurDir = &statusBuffer;
    } else {
        FSF.sprintf(&statusBuffer, L"Filesystem: %s | Mode: Read-Only", fsNames[static_cast<int>(instance->DetectedFs)]);
        Info->CurDir = &statusBuffer;
    }
}

intptr_t WINAPI GetFindDataW(struct GetFindDataInfo *Info) {
    auto* instance = reinterpret_cast<PluginInstance*>(Info->hPanel);
    if (!instance) return FALSE;

    PluginPanelItem* items = nullptr; size_t count = 0;

    if (instance->DetectedFs == LinuxFsType::Btrfs) {
        if (BtrfsReadDirectory(instance->DiskReader, instance->BtrfsSubvolumeVaddr, 5, &items, &count)) {
            Info->PanelItem = items; Info->ItemsNumber = count; return TRUE;
        }
    } 
    else if (instance->DetectedFs == LinuxFsType::Ext4) {
        if (Ext4ReadDirectory(instance->DiskReader, instance->Ext4CurrentInode, &instance->Ext4SbCached, instance->Ext4BlockSize, &items, &count)) {
            Info->PanelItem = items; Info->ItemsNumber = count; return TRUE;
        }
    }
    else if (instance->DetectedFs == LinuxFsType::F2FS) {
        if (F2fsReadDirectory(instance->DiskReader, instance->Ext4CurrentInode, instance->F2fsMainBlkaddrCached, instance->Ext4BlockSize, &items, &count)) {
            Info->PanelItem = items; Info->ItemsNumber = count; return TRUE;
        }
    }
    else if (instance->DetectedFs == LinuxFsType::XFS) {
        if (XfsReadDirectory(instance->DiskReader, instance->BtrfsSubvolumeVaddr, instance->Ext4BlockSize, instance->XfsAgBlocksCached, &items, &count)) {
            Info->PanelItem = items; Info->ItemsNumber = count; return TRUE;
        }
    }

    return FALSE;
}

void WINAPI FreeFindDataW(const struct FreeFindDataInfo *Info) {
    G_StorageArena.Reset();
}

void WINAPI ClosePanelW(const struct ClosePanelInfo *Info) {
    auto* Instance = reinterpret_cast<PluginInstance*>(Info->hPanel);
    if (Instance) {
        if (Instance->DiskReader) Instance->DiskReader->CloseDevice();
        HeapFree(GetProcessHeap(), 0, Instance);
    }
}
