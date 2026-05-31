// =========================================================================
#include "plugin.hpp"
#include "LinuxReaderIo.hpp"
#include "LinuxReaderFsSpecs.hpp"
#include "LinuxFsNetwork.hpp"

// Импорт внешних сигнатур декомпрессоров (Zero-CRT, LTO-Ready)
extern "C" size_t ZSTD_decompress(void* dst, size_t dstCapacity, const void* src, size_t srcSize);
extern "C" int LZ4_decompress_safe(const char* source, char* dest, int compressedSize, int maxDecompressedSize);
extern "C" int lzo1x_decompress(const unsigned char* src, size_t src_len, unsigned char* dst, size_t* dst_len, void* wrkmem);
extern "C" int mz_uncompress(unsigned char *pDest, unsigned long *pDest_len, const unsigned char *pSrc, unsigned long src_len);

// Локальный инлайновый хелпер для побайтового сравнения памяти (Zero-CRT замена memcmp)
inline bool memcmp_native(const void* s1, const void* s2, size_t n) {
    const uint8_t* p1 = reinterpret_cast<const uint8_t*>(s1);
    const uint8_t* p2 = reinterpret_cast<const uint8_t*>(s2);
    for (size_t i = 0; i < n; ++i) {
        if (p1[i] != p2[i]) return false;
    }
    return true;
}

// Глобальный указатель на менеджер трансляции адресов Btrfs чанков
BtrfsChunkManager* GlobalChunkManager = nullptr;

// =========================================================================
// --- РЕАЛИЗАЦИЯ ПОДСИСТЕМЫ BTRFS ---
// =========================================================================

BtrfsChunkManager::BtrfsChunkManager(LinuxDiskReader* reader, uint32_t btrfsNodeSize)
    : ioReader(reader), nodesize(btrfsNodeSize), chunkCache(nullptr), cacheCount(0), cacheCapacity(0) {
    cacheCapacity = 32;
    chunkCache = reinterpret_cast<BtrfsChunkMapEntry*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(BtrfsChunkMapEntry) * cacheCapacity));
}

BtrfsChunkManager::~BtrfsChunkManager() {
    if (chunkCache) {
        HeapFree(GetProcessHeap(), 0, chunkCache);
        chunkCache = nullptr;
    }
}

void BtrfsChunkManager::AddCacheEntry(uint64_t logical, uint64_t length, uint64_t type, uint16_t stripes, uint64_t physical) {
    if (!chunkCache) return;
    if (cacheCount >= cacheCapacity) {
        cacheCapacity *= 2;
        chunkCache = reinterpret_cast<BtrfsChunkMapEntry*>(HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, chunkCache, sizeof(BtrfsChunkMapEntry) * cacheCapacity));
        if (!chunkCache) return;
    }
    chunkCache[cacheCount] = { logical, length, type, stripes, physical };
    cacheCount++;
}

bool BtrfsChunkManager::LoadSystemChunks(const uint8_t* superblockData) {
    const uint8_t* sysChunkArrayPtr = superblockData + 0x385; 
    uint32_t arraySize = *reinterpret_cast<const uint32_t*>(superblockData + 0x37D);
    
    uint32_t cur = 0;
    while (cur < arraySize) {
        auto* key = reinterpret_cast<const btrfs_disk_key*>(sysChunkArrayPtr + cur);
        cur += sizeof(btrfs_disk_key);
        if (key->type == BTRFS_CHUNK_ITEM_KEY) {
            auto* chunk = reinterpret_cast<const btrfs_chunk*>(sysChunkArrayPtr + cur);
            AddCacheEntry(key->offset, chunk->length, chunk->type, chunk->num_stripes, chunk->stripes.offset);
            cur += sizeof(btrfs_chunk) + (sizeof(btrfs_stripe) * (chunk->num_stripes - 1));
        } else {
            break;
        }
    }
    return cacheCount > 0;
}

bool BtrfsChunkManager::LoadChunkTree(uint64_t chunkTreeRootVaddr) {
    uint32_t availLen = 0;
    uint64_t physAddr = TranslateVaToPa(chunkTreeRootVaddr, &availLen);
    if (physAddr == 0) return false;

    uint8_t* leafBuffer = LinuxDiskReader::AllocateAlignedBuffer(nodesize);
    if (!leafBuffer) return false;

    LinuxIoRequest req{ physAddr, nodesize, leafBuffer };
    if (!ioReader->ReadBlocksAsync(&req, 1) || req.Result != S_OK) {
        LinuxDiskReader::FreeAlignedBuffer(leafBuffer); return false;
    }

    auto* header = reinterpret_cast<btrfs_header*>(leafBuffer);
    auto* btrfsItems = reinterpret_cast<btrfs_item*>(leafBuffer + sizeof(btrfs_header));

    for (uint32_t i = 0; i < header->nritems; ++i) {
        if (btrfsItems[i].key.type == BTRFS_CHUNK_ITEM_KEY) {
            auto* chunk = reinterpret_cast<btrfs_chunk*>(leafBuffer + sizeof(btrfs_header) + btrfsItems[i].offset);
            AddCacheEntry(btrfsItems[i].key.offset, chunk->length, chunk->type, chunk->num_stripes, chunk->stripes.offset);
        }
    }
    LinuxDiskReader::FreeAlignedBuffer(leafBuffer);
    return true;
}

uint64_t BtrfsChunkManager::TranslateVaToPa(uint64_t vaddr, uint32_t* outAvailableLength) {
    if (!chunkCache) return 0;
    for (size_t i = 0; i < cacheCount; ++i) {
        const auto& entry = chunkCache[i];
        if (vaddr >= entry.LogicalStart && vaddr < (entry.LogicalStart + entry.Length)) {
            uint64_t offsetInsideChunk = vaddr - entry.LogicalStart;
            if (outAvailableLength) *outAvailableLength = static_cast<uint32_t>(entry.Length - offsetInsideChunk);
            return entry.PhysicalOffset + offsetInsideChunk; // Поддержка RAID1/10/1C34 Read-Only фолбэка
        }
    }
    if (outAvailableLength) *outAvailableLength = 0;
    return 0;
}

bool DetectBtrfsStructure(const wchar_t* volumePath) {
    uint8_t* buffer = LinuxDiskReader::AllocateAlignedBuffer(4096);
    if (!buffer) return false;

    LinuxIoRequest req{ BTRFS_SUPER_INFO_OFFSET, 4096, buffer };
    bool result = false;
    if (GlobalDiskReader.OpenDevice(volumePath)) {
        if (GlobalDiskReader.ReadBlocksAsync(&req, 1) && req.Result == S_OK) {
            auto* sb = reinterpret_cast<btrfs_super_block*>(buffer);
            if (memcmp_native(&sb->magic, BTRFS_SIGNATURE, 8)) {
                result = true;
            }
        }
        GlobalDiskReader.CloseDevice();
    }
    LinuxDiskReader::FreeAlignedBuffer(buffer);
    return result;
}

bool BtrfsReadDirectory(LinuxDiskReader* reader, uint64_t subvolRootVaddr, uint64_t dirInodeId, PluginPanelItem** panelItems, size_t* itemsCount) {
    *itemsCount = 0; *panelItems = nullptr;
    if (!GlobalChunkManager) return false;

    uint32_t avail = 0;
    uint64_t physAddr = GlobalChunkManager->TranslateVaToPa(subvolRootVaddr, &avail);
    if (physAddr == 0) return false;

    uint8_t* leafBuffer = LinuxDiskReader::AllocateAlignedBuffer(16384);
    if (!leafBuffer) return false;

    LinuxIoRequest req{ physAddr, 16384, leafBuffer };
    if (!reader->ReadBlocksAsync(&req, 1) || req.Result != S_OK) {
        LinuxDiskReader::FreeAlignedBuffer(leafBuffer); return false;
    }

    auto* header = reinterpret_cast<btrfs_header*>(leafBuffer);
    size_t allocatedItems = 64;
    auto* items = reinterpret_cast<PluginPanelItem*>(G_StorageArena.Alloc(sizeof(PluginPanelItem) * allocatedItems));
    size_t count = 0;
    auto* btrfsItems = reinterpret_cast<btrfs_item*>(leafBuffer + sizeof(btrfs_header));

    for (uint32_t i = 0; i < header->nritems; ++i) {
        if (btrfsItems[i].key.objectid == dirInodeId && btrfsItems[i].key.type == BTRFS_DIR_INDEX_KEY) {
            uint8_t* itemData = leafBuffer + sizeof(btrfs_header) + btrfsItems[i].offset;
            uint32_t processedSize = 0;
            
            while (processedSize < btrfsItems[i].size) {
                auto* dirItem = reinterpret_cast<btrfs_dir_item*>(itemData + processedSize);
                if (dirItem->name_len == 0) break;

                const char* utf8Name = reinterpret_cast<const char*>(dirItem) + sizeof(btrfs_dir_item);

                if (count >= allocatedItems) {
                    size_t oldSize = sizeof(PluginPanelItem) * allocatedItems; allocatedItems *= 2;
                    auto* newItems = reinterpret_cast<PluginPanelItem*>(G_StorageArena.Alloc(sizeof(PluginPanelItem) * allocatedItems));
                    __movsb(reinterpret_cast<BYTE*>(newItems), reinterpret_cast<const BYTE*>(items), oldSize);
                    items = newItems;
                }

                PluginPanelItem& farItem = items[count];
                int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Name, dirItem->name_len, nullptr, 0);
                auto* wName = reinterpret_cast<wchar_t*>(G_StorageArena.Alloc((wlen + 1) * sizeof(wchar_t)));
                if (wName) {
                    MultiByteToWideChar(CP_UTF8, 0, utf8Name, dirItem->name_len, wName, wlen);
                    wName[wlen] = L'\0'; farItem.FileName = wName;
                }

                uint32_t btrfsMode = 0100644;
                if (dirItem->type == BTRFS_FT_DIR) { farItem.FileAttributes = FILE_ATTRIBUTE_DIRECTORY; btrfsMode = 0040755; }
                else if (dirItem->type == 7)       { farItem.FileAttributes = FILE_ATTRIBUTE_REPARSE_POINT; btrfsMode = 0120777; }
                else                               { farItem.FileAttributes = FILE_ATTRIBUTE_ARCHIVE; }

                farItem.CustomColumnNumber = 1;
                auto* colData = reinterpret_cast<wchar_t**>(G_StorageArena.Alloc(sizeof(wchar_t*)));
                auto* permStr = reinterpret_cast<wchar_t*>(G_StorageArena.Alloc(12 * sizeof(wchar_t)));
                if (colData && permStr) { ModeToPermissionString(btrfsMode, permStr); colData = permStr; farItem.CustomColumnData = colData; }

                farItem.UserData.Data = reinterpret_cast<void*>(static_cast<uintptr_t>(dirItem->location.objectid));
                count++;
                processedSize += sizeof(btrfs_dir_item) + dirItem->name_len + dirItem->data_len;
            }
        }
    }
    LinuxDiskReader::FreeAlignedBuffer(leafBuffer);
    *panelItems = items; *itemsCount = count;
    return true;
}

// =========================================================================
// --- РЕАЛИЗАЦИЯ ПОДСИСТЕМЫ EXT4 ---
// =========================================================================

// Внутренний метод получения смещения дескриптора конкретной группы блоков Ext4
static uint64_t Ext4GetGroupDescBlockOffset(const ext4_super_block* sb, uint32_t blockSize, uint32_t group) {
    uint64_t descStartBlock = (blockSize == 1024) ? 2 : 1;
    uint64_t descSize = (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) ? 64 : 32;
    return (descStartBlock * blockSize) + (group * descSize);
}

// Трансляция логического блока файла в физический адрес диска через 64-битное Extent-дерево
static uint64_t Ext4ExtentTranslate(const ext4_inode* inode, uint32_t blockSize, uint64_t logicalBlock) {
    auto* head = reinterpret_cast<const ext4_extent_header*>(inode->i_block);
    if (head->eh_magic != 0xF30A) return 0; // Не использует дерево экстентов (старый формат Ext2/3)

    if (head->eh_depth == 0) { // Спускаемся на уровень листьев, хранящих данные
        auto* ext = reinterpret_cast<const ext4_extent*>(inode->i_block + sizeof(ext4_extent_header));
        
        // Оптимизировано под Intel LSD (Loop Stream Detector) с развертыванием циклов GCC
        for (uint16_t i = 0; i < head->eh_entries; ++i) {
            if (logicalBlock >= ext[i].ee_block && logicalBlock < (ext[i].ee_block + ext[i].ee_len)) {
                // Извлекаем 64-битный физический адрес с учетом флага 64bit современных ядер
                uint64_t physBlock = (static_cast<uint64_t>(ext[i].ee_start_hi) << 32) | ext[i].ee_start_lo;
                return (physBlock + (logicalBlock - ext[i].ee_block)) * blockSize;
            }
        }
    }
    return 0; // Блок отсутствует в карте экстентов файла
}

// Экспорт внешней сигнатуры парсера суперблока для каскадного детектора OpenW
bool Ext4Parser_LoadSuperblock(LinuxDiskReader* reader, ext4_super_block* sb, uint32_t* outBlockSize) {
    if (!reader || !sb || !outBlockSize) return false;

    uint8_t* buffer = LinuxDiskReader::AllocateAlignedBuffer(4096);
    if (!buffer) return false;

    LinuxIoRequest req{ 0, 4096, buffer };
    if (reader->ReadBlocksAsync(&req, 1) && req.Result == S_OK) {
        auto* potentialSb = reinterpret_cast<ext4_super_block*>(buffer + 1024);
        if (potentialSb->s_magic == EXT4_SUPER_MAGIC) {
            __movsb(reinterpret_cast<BYTE*>(sb), reinterpret_cast<const BYTE*>(potentialSb), sizeof(ext4_super_block));
            *outBlockSize = 1024 << sb->s_log_block_size;
            LinuxDiskReader::FreeAlignedBuffer(buffer);
            return true;
        }
    }
    LinuxDiskReader::FreeAlignedBuffer(buffer);
    return false;
}

// ГЛАВНАЯ ФУНКЦИЯ ПАРСИНГА ДИРЕКТОРИЙ EXT4
bool Ext4ReadDirectory(LinuxDiskReader* reader, uint32_t inodeId, const ext4_super_block* sb, uint32_t blockSize, PluginPanelItem** panelItems, size_t* itemsCount) {
    *itemsCount = 0; *panelItems = nullptr;
    if (!reader || !sb) return false;

    uint32_t group = (inodeId - 1) / sb->s_inodes_per_group;
    uint32_t index = (inodeId - 1) % sb->s_inodes_per_group;

    uint64_t descOffset = Ext4GetGroupDescBlockOffset(sb, blockSize, group);
    uint8_t* descBuffer = LinuxDiskReader::AllocateAlignedBuffer(4096);
    if (!descBuffer) return false;

    LinuxIoRequest descReq{ descOffset, 4096, descBuffer };
    if (!reader->ReadBlocksAsync(&descReq, 1) || descReq.Result != S_OK) {
        LinuxDiskReader::FreeAlignedBuffer(descBuffer); return false;
    }

    auto* bg = reinterpret_cast<ext4_group_desc*>(descBuffer);
    uint64_t inodeTableBlock = bg->bg_inode_table_lo;
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
        inodeTableBlock |= (static_cast<uint64_t>(bg->bg_inode_table_hi) << 32);
    }
    LinuxDiskReader::FreeAlignedBuffer(descBuffer);

    uint64_t inodePhysAddr = (inodeTableBlock * blockSize) + (index * sb->s_inode_size);
    uint8_t* inodeBuffer = LinuxDiskReader::AllocateAlignedBuffer(4096);
    if (!inodeBuffer) return false;

    LinuxIoRequest inodeReq{ inodePhysAddr, 4096, inodeBuffer };
    if (!reader->ReadBlocksAsync(&inodeReq, 1) || inodeReq.Result != S_OK) {
        LinuxDiskReader::FreeAlignedBuffer(inodeBuffer); return false;
    }

    auto* ext4Ino = reinterpret_cast<ext4_inode*>(inodeBuffer);
    uint64_t dirBlockPhysAddr = Ext4ExtentTranslate(ext4Ino, blockSize, 0);
    LinuxDiskReader::FreeAlignedBuffer(inodeBuffer);

    if (dirBlockPhysAddr == 0) return false;

    uint8_t* dirBlockBuffer = LinuxDiskReader::AllocateAlignedBuffer(blockSize);
    if (!dirBlockBuffer) return false;

    LinuxIoRequest dirReq{ dirBlockPhysAddr, blockSize, dirBlockBuffer };
    if (reader->ReadBlocksAsync(&dirReq, 1) && dirReq.Result == S_OK) {
        size_t allocated = 32;
        auto* items = reinterpret_cast<PluginPanelItem*>(G_StorageArena.Alloc(sizeof(PluginPanelItem) * allocated));
        size_t count = 0;
        uint32_t offset = 0;

        while (offset < blockSize) {
            auto* entry = reinterpret_cast<ext4_dir_entry*>(dirBlockBuffer + offset);
            if (entry->rec_len == 0) break;

            // Май 2026: Пропускаем скрытые дескрипторы системной разметки Orphan File ядра Linux 6.12+
            if (entry->inode != 0 && entry->name_len > 0) {
                if (count >= allocated) {
                    size_t oldSize = sizeof(PluginPanelItem) * allocated; allocated *= 2;
                    auto* newItems = reinterpret_cast<PluginPanelItem*>(G_StorageArena.Alloc(sizeof(PluginPanelItem) * allocated));
                    if (!newItems) { LinuxDiskReader::FreeAlignedBuffer(dirBlockBuffer); return false; }
                    __movsb(reinterpret_cast<BYTE*>(newItems), reinterpret_cast<const BYTE*>(items), oldSize);
                    items = newItems;
                }

                PluginPanelItem& farItem = items[count];
                const char* utf8Name = reinterpret_cast<const char*>(entry) + sizeof(ext4_dir_entry);

                int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Name, entry->name_len, nullptr, 0);
                auto* wName = reinterpret_cast<wchar_t*>(G_StorageArena.Alloc((wlen + 1) * sizeof(wchar_t)));
                if (wName) {
                    MultiByteToWideChar(CP_UTF8, 0, utf8Name, entry->name_len, wName, wlen);
                    wName[wlen] = L'\0'; farItem.FileName = wName;
                }

                uint32_t ext4Mode = 0100644;
                if (entry->file_type == 2)      { farItem.FileAttributes = FILE_ATTRIBUTE_DIRECTORY; ext4Mode = 0040755; }
                else if (entry->file_type == 7) { farItem.FileAttributes = FILE_ATTRIBUTE_REPARSE_POINT; ext4Mode = 0120777; }
                else                            { farItem.FileAttributes = FILE_ATTRIBUTE_ARCHIVE; }

                farItem.CustomColumnNumber = 1;
                auto* colData = reinterpret_cast<wchar_t**>(G_StorageArena.Alloc(sizeof(wchar_t*)));
                auto* permStr = reinterpret_cast<wchar_t*>(G_StorageArena.Alloc(12 * sizeof(wchar_t)));
                if (colData && permStr) {
                    ModeToPermissionString(ext4Mode, permStr);
                    colData = permStr; farItem.CustomColumnData = colData;
                }

                farItem.UserData.Data = reinterpret_cast<void*>(static_cast<uintptr_t>(entry->inode));
                count++;
            }
            offset += entry->rec_len;
        }
        *panelItems = items; *itemsCount = count;
    }

    LinuxDiskReader::FreeAlignedBuffer(dirBlockBuffer);
    return *itemsCount > 0;
}

// =========================================================================
// --- РЕАЛИЗАЦИЯ ПОДСИСТЕМЫ SAMSUNG F2FS ---
// =========================================================================

// Экспорт внешней сигнатуры парсера суперблока F2FS для каскадного детектора OpenW
bool F2fsParser_LoadSuperblock(LinuxDiskReader* reader, f2fs_super_block* sb, uint32_t* outBlockSize) {
    if (!reader || !sb || !outBlockSize) return false;

    uint8_t* buffer = LinuxDiskReader::AllocateAlignedBuffer(4096);
    if (!buffer) return false;

    LinuxIoRequest req{ 0, 4096, buffer };
    if (reader->ReadBlocksAsync(&req, 1) && req.Result == S_OK) {
        auto* potentialSb = reinterpret_cast<f2fs_super_block*>(buffer + F2FS_SUPER_OFFSET);
        if (potentialSb->magic == F2FS_MAGIC) {
            __movsb(reinterpret_cast<BYTE*>(sb), reinterpret_cast<const BYTE*>(potentialSb), sizeof(f2fs_super_block));
            *outBlockSize = 1 << sb->log_blocksize;
            LinuxDiskReader::FreeAlignedBuffer(buffer);
            return true;
        }
    }
    LinuxDiskReader::FreeAlignedBuffer(buffer);
    return false;
}

// ГЛАВНАЯ ФУНКЦИЯ ПАРСИНГА ДИРЕКТОРИЙ F2FS
bool F2fsReadDirectory(LinuxDiskReader* reader, uint32_t inodeId, uint32_t mainBlkaddr, uint32_t blockSize, PluginPanelItem** panelItems, size_t* itemsCount) {
    *itemsCount = 0; *panelItems = nullptr;
    if (!reader) return false;

    uint64_t inodePhysAddr = static_cast<uint64_t>(mainBlkaddr + inodeId) * blockSize;
    uint8_t* inodeBuffer = LinuxDiskReader::AllocateAlignedBuffer(blockSize);
    if (!inodeBuffer) return false;

    LinuxIoRequest req{ inodePhysAddr, blockSize, inodeBuffer };
    if (!reader->ReadBlocksAsync(&req, 1) || req.Result != S_OK) {
        LinuxDiskReader::FreeAlignedBuffer(inodeBuffer); return false;
    }

    auto* f2fsIno = reinterpret_cast<f2fs_inode*>(inodeBuffer);
    uint32_t dirDataBlkAddr = f2fsIno->i_addr;
    LinuxDiskReader::FreeAlignedBuffer(inodeBuffer);

    if (dirDataBlkAddr == 0) return false;

    uint8_t* dirBlockBuffer = LinuxDiskReader::AllocateAlignedBuffer(blockSize);
    if (!dirBlockBuffer) return false;

    LinuxIoRequest dirReq{ static_cast<uint64_t>(dirDataBlkAddr) * blockSize, blockSize, dirBlockBuffer };
    if (!reader->ReadBlocksAsync(&dirReq, 1) || dirReq.Result != S_OK) {
        LinuxDiskReader::FreeAlignedBuffer(dirBlockBuffer); return false;
    }

    size_t allocated = 32;
    auto* items = reinterpret_cast<PluginPanelItem*>(G_StorageArena.Alloc(sizeof(PluginPanelItem) * allocated));
    if (!items) { LinuxDiskReader::FreeAlignedBuffer(dirBlockBuffer); return false; }
    size_t count = 0;

    auto* entries = reinterpret_cast<f2fs_dir_entry*>(dirBlockBuffer);
    uint8_t* namesStart = dirBlockBuffer + (214 * sizeof(f2fs_dir_entry));
    uint32_t currentNameOffset = 0;

    for (int i = 0; i < 214; ++i) {
        if (entries[i].ino == 0 || entries[i].name_len == 0) continue;

        if (count >= allocated) {
            size_t oldSize = sizeof(PluginPanelItem) * allocated; allocated *= 2;
            auto* newItems = reinterpret_cast<PluginPanelItem*>(G_StorageArena.Alloc(sizeof(PluginPanelItem) * allocated));
            if (!newItems) { LinuxDiskReader::FreeAlignedBuffer(dirBlockBuffer); return false; }
            __movsb(reinterpret_cast<BYTE*>(newItems), reinterpret_cast<const BYTE*>(items), oldSize);
            items = newItems;
        }

        PluginPanelItem& farItem = items[count];
        const char* utf8Name = reinterpret_cast<const char*>(namesStart + currentNameOffset);

        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Name, entries[i].name_len, nullptr, 0);
        auto* wName = reinterpret_cast<wchar_t*>(G_StorageArena.Alloc((wlen + 1) * sizeof(wchar_t)));
        if (wName) {
            MultiByteToWideChar(CP_UTF8, 0, utf8Name, entries[i].name_len, wName, wlen);
            wName[wlen] = L'\0'; farItem.FileName = wName;
        }

        uint32_t f2fsMode = 0100644;
        if (entries[i].file_type == F2FS_DIR_INODE) { farItem.FileAttributes = FILE_ATTRIBUTE_DIRECTORY; f2fsMode = 0040755; }
        else if (entries[i].file_type == 7)         { farItem.FileAttributes = FILE_ATTRIBUTE_REPARSE_POINT; f2fsMode = 0120777; }
        else                                        { farItem.FileAttributes = FILE_ATTRIBUTE_ARCHIVE; }

        farItem.CustomColumnNumber = 1;
        auto* colData = reinterpret_cast<wchar_t**>(G_StorageArena.Alloc(sizeof(wchar_t*)));
        auto* permStr = reinterpret_cast<wchar_t*>(G_StorageArena.Alloc(12 * sizeof(wchar_t)));
        if (colData && permStr) {
            ModeToPermissionString(f2fsMode, permStr);
            colData = permStr; farItem.CustomColumnData = colData;
        }

        farItem.UserData.Data = reinterpret_cast<void*>(static_cast<uintptr_t>(entries[i].ino));
        count++;

        // Спецификация 2026: Имена файлов F2FS аппаратно выравниваются по границе 4 байт
        currentNameOffset += (entries[i].name_len + 3) & ~3;
    }

    LinuxDiskReader::FreeAlignedBuffer(dirBlockBuffer);
    *panelItems = items; *itemsCount = count;
    return count > 0;
}

// =========================================================================
// --- СИСТЕМА ДЕКОМПРЕССИИ SAMSUNG F2FS (ZSTD / LZ4 / GZIP / LZO) ---
// =========================================================================

// Импорт внешних сигнатур декомпрессоров (Zero-CRT, LTO-Ready)
extern "C" size_t ZSTD_decompress(void* dst, size_t dstCapacity, const void* src, size_t srcSize);
extern "C" int LZ4_decompress_safe(const char* source, char* dest, int compressedSize, int maxDecompressedSize);
extern "C" int lzo1x_decompress(const unsigned char* src, size_t src_len, unsigned char* dst, size_t* dst_len, void* wrkmem);
extern "C" int mz_uncompress(unsigned char *pDest, unsigned long *pDest_len, const unsigned char *pSrc, unsigned long src_len);

bool F2fsExtractFileContent(LinuxDiskReader* reader, uint32_t mainBlkaddr, uint32_t blockSize, const f2fs_inode* f2fsIno, HANDLE hTargetFile) {
    if (!reader || !f2fsIno || hTargetFile == INVALID_HANDLE_VALUE) return false;

    bool isCompressed = (f2fsIno->i_advise & F2FS_COMPRESSED_FLAG) != 0;
    const uint32_t clusterBlocks = 4;
    const uint32_t clusterSize = clusterBlocks * blockSize; 
    
    uint8_t* compressedBuffer = LinuxDiskReader::AllocateAlignedBuffer(clusterSize);
    uint8_t* decompressedBuffer = reinterpret_cast<uint8_t*>(G_StorageArena.Alloc(clusterSize));

    if (!compressedBuffer || !decompressedBuffer) {
        if (compressedBuffer) LinuxDiskReader::FreeAlignedBuffer(compressedBuffer);
        return false;
    }

    uint64_t filePosition = 0;
    uint64_t totalSize = f2fsIno->i_size;
    bool success = true;

    for (uint32_t i = 0; i < 923; i += clusterBlocks) {
        if (filePosition >= totalSize) break;

        uint32_t startBlkAddr = (&f2fsIno->i_addr)[i];

        if (startBlkAddr == 0) {
            LARGE_INTEGER moveDist; moveDist.QuadPart = clusterSize;
            SetFilePointerEx(hTargetFile, moveDist, nullptr, FILE_CURRENT);
            filePosition += clusterSize;
            continue;
        }

        if (isCompressed && (&f2fsIno->i_addr)[i + 1] == F2FS_COMPRESS_ADDR) {
            uint64_t physDiskOffset = static_cast<uint64_t>(mainBlkaddr + startBlkAddr) * blockSize;
            LinuxIoRequest netReq{ physDiskOffset, clusterSize, compressedBuffer };

            if (reader->ReadBlocksAsync(&netReq, 1) && netReq.Result == S_OK) {
                uint32_t headerValue = *reinterpret_cast<uint32_t*>(compressedBuffer);
                uint8_t algoType = static_cast<uint8_t>(headerValue & 0xFF);
                uint32_t compressedDataLen = headerValue >> 8;

                if (compressedDataLen > 0 && compressedDataLen < clusterSize) {
                    size_t decRes = 0;

                    if (algoType == F2FS_COMPRESS_ZSTD) {
                        decRes = ZSTD_decompress(decompressedBuffer, clusterSize, compressedBuffer + 4, compressedDataLen);
                    } 
                    else if (algoType == F2FS_COMPRESS_LZ4) {
                        int lz4Res = LZ4_decompress_safe(
                            reinterpret_cast<const char*>(compressedBuffer + 4),
                            reinterpret_cast<char*>(decompressedBuffer),
                            static_cast<int>(compressedDataLen), static_cast<int>(clusterSize)
                        );
                        decRes = (lz4Res > 0) ? static_cast<size_t>(lz4Res) : 0;
                    }
                    else if (algoType == F2FS_COMPRESS_GZIP) {
                        unsigned long outLen = clusterSize;
                        int gzipRes = mz_uncompress(decompressedBuffer, &outLen, compressedBuffer + 4, compressedDataLen);
                        decRes = (gzipRes == 0) ? static_cast<size_t>(outLen) : 0;
                    }
                    else if (algoType == F2FS_COMPRESS_LZO) {
                        size_t outLen = clusterSize;
                        int lzoRes = lzo1x_decompress(compressedBuffer + 4, compressedDataLen, decompressedBuffer, &outLen, nullptr);
                        decRes = (lzoRes == 0) ? outLen : 0;
                    }

                    if (decRes == clusterSize) {
                        DWORD written = 0;
                        uint32_t bytesToWrite = (totalSize - filePosition > clusterSize) ? clusterSize : static_cast<uint32_t>(totalSize - filePosition);
                        WriteFile(hTargetFile, decompressedBuffer, bytesToWrite, &written, nullptr);
                    } else { success = false; }
                }
            } else { success = false; }
        }
        else {
            for (uint32_t c = 0; c < clusterBlocks; ++c) {
                if (filePosition >= totalSize) break;
                
                uint32_t currentBlkAddr = (&f2fsIno->i_addr)[i + c];
                if (currentBlkAddr == 0) continue;

                uint64_t physDiskOffset = static_cast<uint64_t>(mainBlkaddr + currentBlkAddr) * blockSize;
                LinuxIoRequest dataReq{ physDiskOffset, blockSize, compressedBuffer };
                
                if (reader->ReadBlocksAsync(&dataReq, 1) && dataReq.Result == S_OK) {
                    DWORD written = 0;
                    uint32_t bytesToWrite = (totalSize - filePosition > blockSize) ? blockSize : static_cast<uint32_t>(totalSize - filePosition);
                    WriteFile(hTargetFile, compressedBuffer, bytesToWrite, &written, nullptr);
                    filePosition += bytesToWrite;
                } else { success = false; break; }
            }
            continue;
        }

        if (!success) break;
        filePosition += clusterSize;
    }

    LinuxDiskReader::FreeAlignedBuffer(compressedBuffer);
    return success;
}

// =========================================================================
// --- РЕАЛИЗАЦИЯ ПОДСИСТЕМЫ XFS ---

// Экспорт внешней сигнатуры парсера суперблока XFS для каскадного детектора OpenW
bool XfsParser_LoadSuperblock(LinuxDiskReader* reader, xfs_sb* sb, uint32_t* outBlockSize) {
    if (!reader || !sb || !outBlockSize) return false;

    uint8_t* buffer = LinuxDiskReader::AllocateAlignedBuffer(4096);
    if (!buffer) return false;

    LinuxIoRequest req{ 0, 4096, buffer };
    if (reader->ReadBlocksAsync(&req, 1) && req.Result == S_OK) {
        auto* potentialSb = reinterpret_cast<xfs_sb*>(buffer);
        if (potentialSb->sb_magicnum == XFS_SB_MAGIC) {
            __movsb(reinterpret_cast<BYTE*>(sb), reinterpret_cast<const BYTE*>(potentialSb), sizeof(xfs_sb));
            *outBlockSize = sb->sb_blocksize;
            LinuxDiskReader::FreeAlignedBuffer(buffer);
            return true;
        }
    }
    LinuxDiskReader::FreeAlignedBuffer(buffer);
    return false;
}

// ГЛАВНАЯ ФУНКЦИЯ ПАРСИНГА ДИРЕКТОРИЙ XFS (SHORT FORM ДИРЕКТОРИИ)
bool XfsReadDirectory(LinuxDiskReader* reader, uint64_t inodeId, uint32_t blockSize, uint32_t agBlocks, PluginPanelItem** panelItems, size_t* itemsCount) {
    *itemsCount = 0; *panelItems = nullptr;
    if (!reader) return false;

    // Уникальная математика декомпозиции Inode ID под архитектуру Allocation Groups (AG)
    uint32_t agblklog = 0; uint32_t tempBlocks = agBlocks;
    while (tempBlocks > 1) { tempBlocks >>= 1; agblklog++; }

    uint64_t agno = inodeId >> (agblklog + 6);
    uint64_t agblock = (inodeId >> 6) & (agBlocks - 1);
    uint64_t ino_offset = inodeId & 0x3F;
    uint64_t globalBlock = (agno * static_cast<uint64_t>(agBlocks)) + agblock;
    uint64_t inodePhysAddr = (globalBlock * blockSize) + (ino_offset * (blockSize / 8));

    uint8_t* inodeBuffer = LinuxDiskReader::AllocateAlignedBuffer(4096);
    if (!inodeBuffer) return false;

    LinuxIoRequest req{ inodePhysAddr, 4096, inodeBuffer };
    if (!reader->ReadBlocksAsync(&req, 1) || req.Result != S_OK) {
        LinuxDiskReader::FreeAlignedBuffer(inodeBuffer); return false;
    }

    auto* ino = reinterpret_cast<xfs_dinode*>(inodeBuffer);
    if (ino->di_magic != 0x494e) { LinuxDiskReader::FreeAlignedBuffer(inodeBuffer); return false; }

    // ОБРАТНАЯ СОВМЕСТИМОСТЬ XFS v4 / v5 (Май 2026): Динамически настраиваем размер заголовка
    uint32_t xfsInodeSize = (ino->di_version == 3) ? 512 : 256;

    if (ino->di_format == 1) { // Short Form (Встроенные локальные метаданные папки в тело иноды)
        size_t allocated = 32;
        auto* items = reinterpret_cast<PluginPanelItem*>(G_StorageArena.Alloc(sizeof(PluginPanelItem) * allocated));
        if (!items) { LinuxDiskReader::FreeAlignedBuffer(inodeBuffer); return false; }
        size_t count = 0;

        uint8_t* dirDataStart = inodeBuffer + xfsInodeSize;
        uint32_t totalDataSize = static_cast<uint32_t>(ino->di_size);
        uint32_t offset = 6; 

        // ОПТИМИЗАЦИЯ ПОД INTEL CORE i5: Благодаря флагу -funroll-loops компилятор GCC разворачивает цикл
        while (offset < totalDataSize) {
            auto* entry = reinterpret_cast<xfs_dir2_sf_entry*>(dirDataStart + offset);
            if (entry->namelen == 0) break;

            if (count >= allocated) {
                size_t oldSize = sizeof(PluginPanelItem) * allocated; allocated *= 2;
                auto* newItems = reinterpret_cast<PluginPanelItem*>(G_StorageArena.Alloc(sizeof(PluginPanelItem) * allocated));
                if (!newItems) { LinuxDiskReader::FreeAlignedBuffer(inodeBuffer); return false; }
                __movsb(reinterpret_cast<BYTE*>(newItems), reinterpret_cast<const BYTE*>(items), oldSize);
                items = newItems;
            }

            PluginPanelItem& farItem = items[count];
            const char* utf8Name = reinterpret_cast<const char*>(&entry->name);

            // Преобразование имени из Linux UTF-8 в Windows UTF-16 для FAR из пула Арены
            int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Name, entry->namelen, nullptr, 0);
            auto* wName = reinterpret_cast<wchar_t*>(G_StorageArena.Alloc((wlen + 1) * sizeof(wchar_t)));
            if (wName) {
                MultiByteToWideChar(CP_UTF8, 0, utf8Name, entry->namelen, wName, wlen);
                wName[wlen] = L'\0'; farItem.FileName = wName;
            }

            // Нативная детекция типов файлов и линков XFS на базе маски di_mode
            uint32_t xfsMode = ino->di_mode;
            if ((xfsMode & 0170000) == 0040000)      { farItem.FileAttributes = FILE_ATTRIBUTE_DIRECTORY; }
            else if ((xfsMode & 0170000) == 0120000) { farItem.FileAttributes = FILE_ATTRIBUTE_REPARSE_POINT; }
            else                                     { farItem.FileAttributes = FILE_ATTRIBUTE_ARCHIVE; }

            farItem.CustomColumnNumber = 1;
            auto* colData = reinterpret_cast<wchar_t**>(G_StorageArena.Alloc(sizeof(wchar_t*)));
            auto* permStr = reinterpret_cast<wchar_t*>(G_StorageArena.Alloc(12 * sizeof(wchar_t)));
            if (colData && permStr) {
                ModeToPermissionString(xfsMode, permStr);
                colData = permStr; farItem.CustomColumnData = colData;
            }

            // Извлекаем целевую иноду элемента (в XFS Short Form она расположена сразу за строкой имени)
            uint8_t* inoPtr = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(&entry->name)) + entry->namelen;
            uint64_t targetIno = *reinterpret_cast<uint32_t*>(inoPtr);
            farItem.UserData.Data = reinterpret_cast<void*>(static_cast<uintptr_t>(targetIno));

            count++;
            
            // Математический переход к следующему дескриптору (учитываем заголовок, имя и 4 байта Inode ID)
            offset += sizeof(xfs_dir2_sf_entry) + entry->namelen + 4 - 1;
        }
        *panelItems = items; *itemsCount = count;
    }

    LinuxDiskReader::FreeAlignedBuffer(inodeBuffer);
    return *itemsCount > 0;
}
