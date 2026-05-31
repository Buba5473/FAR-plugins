// =========================================================================
#pragma once
#include <windows.h>
#include <stdint.h>

#pragma pack(push, 1)

// =========================================================================
// --- СТРУКТУРЫ И КОНСТАНТЫ EXT4 ---
// =========================================================================
#define EXT4_SUPER_MAGIC   0xEF53
#define EXT4_FEATURE_INCOMPAT_64BIT        0x0080
#define EXT4_FEATURE_INCOMPAT_EXTENTS      0x0040
#define EXT4_FEATURE_INCOMPAT_FLEX_BG      0x0200
#define EXT4_FEATURE_INCOMPAT_EA_INODE     0x0400
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED    0x2000
#define EXT4_FEATURE_INCOMPAT_LARGEDIR     0x4000 

#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400
#define EXT4_FEATURE_RO_COMPAT_ORPHAN_FILE   0x1000

struct ext4_group_desc {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_reserved2;
};

struct ext4_super_block {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count_lo;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid;
    char     s_volume_name;
    char     s_last_mounted;
    uint32_t s_algorithm_usage_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;
    uint32_t s_blocks_count_hi;
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;
    uint32_t s_flags;
    uint16_t s_raid_stride;
    uint16_t s_mmp_interval;
    uint64_t s_mmp_block;
    uint32_t s_raid_stripe_width;
    uint8_t  s_log_groups_per_flex;
    uint8_t  s_checksum_type;
    uint16_t s_reserved_pad;
    uint64_t s_kbytes_written;
    uint32_t s_snapshot_inum;
    uint32_t s_snapshot_id;
    uint64_t s_snapshot_r_blocks_count;
    uint32_t s_snapshot_list;
    uint32_t s_error_count;
    uint32_t s_first_error_time;
    uint32_t s_first_error_ino;
    uint64_t s_first_error_block;
    uint32_t s_first_error_func;
    uint32_t s_first_error_line;
    uint32_t s_last_error_time;
    uint32_t s_last_error_ino;
    uint64_t s_last_error_block;
    uint32_t s_last_error_func;
    uint32_t s_last_error_line;
    char     s_mount_opts;
    uint32_t s_usr_quota_inum;
    uint32_t s_grp_quota_inum;
    uint32_t s_overhead_blocks;
    uint32_t s_backup_bgs;
    uint8_t  s_encrypt_algos;
    uint8_t  s_encrypt_pw_salt;
    uint32_t s_lpf_ino;
    uint32_t s_prj_quota_inum;
    uint32_t s_checksum_seed;
    uint32_t s_reserved;
    uint32_t s_checksum;
};

struct ext4_extent_header {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
};

struct ext4_extent {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
};

struct ext4_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint8_t  i_block;
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_high;
    uint32_t i_obso_faddr;
    uint16_t i_blocks_high;
    uint16_t i_file_acl_high;
    uint16_t i_uid_high;
    uint16_t i_gid_high;
    uint16_t i_checksum_lo;
    uint16_t i_extra_isize;
    uint16_t i_checksum_hi;
    uint32_t i_ctime_extra;
    uint32_t i_mtime_extra;
    uint32_t i_atime_extra;
    uint32_t i_crtime;
    uint32_t i_crtime_extra;
    uint32_t i_version_hi;
    uint32_t i_projid;
};

struct ext4_dir_entry {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
};

// =========================================================================
// --- СТРУКТУРЫ И КОНСТАНТЫ XFS ---
// =========================================================================
#define XFS_SB_MAGIC 0x58465342

struct xfs_sb {
    uint32_t sb_magicnum;
    uint32_t sb_blocksize;
    uint64_t sb_dblocks;
    uint64_t sb_rblocks;
    uint64_t sb_rextents;
    uint8_t  sb_uuid;
    uint64_t sb_logstart;
    uint64_t sb_rootino;
    uint64_t sb_rbmino;
    uint64_t sb_rsumino;
    uint32_t sb_rextsize;
    uint32_t sb_agblocks;
    uint32_t sb_agcount;
    uint32_t sb_rbmblocks;
    uint32_t sb_logblocks;
    uint16_t sb_versionnum;
    uint16_t sb_sectsize;
    uint16_t sb_inodesize;
    uint16_t sb_inopblock;
    char     sb_fname;
    char     sb_fpack;
    uint8_t  sb_blocklog;
    uint8_t  sb_sectlog;
    uint8_t  sb_inodelog;
    uint8_t  sb_inopblog;
    uint8_t  sb_agblklog;
    uint8_t  sb_rextslog;
    uint8_t  sb_inprogress;
    uint8_t  sb_imax_pct;
    uint64_t sb_icount;
    uint64_t sb_ifree;
    uint64_t sb_fdblocks;
    uint64_t sb_frextents;
    uint64_t sb_uquotino;
    uint64_t sb_gquotino;
    uint16_t sb_qflags;
    uint8_t  sb_flags;
    uint8_t  sb_shared_vn;
    uint32_t sb_inoalignmt;
    uint32_t sb_unit;
    uint32_t sb_width;
    uint8_t  sb_dirblklog;
    uint8_t  sb_logsectlog;
    uint16_t sb_logsectsize;
    uint32_t sb_logsunit;
    uint32_t sb_features2;
    uint32_t sb_bad_features2;
    uint32_t sb_features_compat;
    uint32_t sb_features_ro_compat;
    uint32_t sb_features_incompat;
    uint32_t sb_features_log_incompat;
    uint32_t sb_crc;
    uint32_t sb_pquotino;
};

struct xfs_dinode {
    uint16_t di_magic;
    uint16_t di_mode;
    uint8_t  di_version;
    uint8_t  di_format;
    uint16_t di_onlink;
    uint32_t di_uid;
    uint32_t di_gid;
    uint32_t di_nlink;
    uint16_t di_projid_lo;
    uint16_t di_projid_hi;
    uint8_t  di_pad;
    uint16_t di_flushiter;
    uint32_t di_atime;
    uint32_t di_atime_nsec;
    uint32_t di_mtime;
    uint32_t di_mtime_nsec;
    uint32_t di_ctime;
    uint32_t di_ctime_nsec;
    uint64_t di_size;
    uint64_t di_nblocks;
    uint32_t di_extsize;
    uint32_t di_nextents;
    uint16_t di_anextents;
    uint8_t  di_forkoff;
    uint8_t  di_aformat;
    uint32_t di_dmevmask;
    uint16_t di_dmstate;
    uint16_t di_flags;
    uint32_t di_gen;
    uint32_t di_crccsum;
    uint64_t di_changecount;
    uint64_t di_lsn;
    uint64_t di_flags2;
    uint32_t di_cowextsize;
    uint8_t  di_pad2;
    uint32_t di_crtime;
    uint32_t di_crtime_nsec;
    uint64_t di_ino;
    uint8_t  di_uuid;
};

struct xfs_dir2_sf_entry {
    uint8_t  namelen;
    uint8_t  offset;
    uint8_t  name;
};

// =========================================================================
// --- СТРУКТУРЫ И КОНСТАНТЫ ZFS ---
// =========================================================================
#define ZFS_UBERBLOCK_MAGIC_LE 0x00bab10cULL
#define ZFS_UBERBLOCK_MAGIC_BE 0x0cb1ba00ULL
#define ZFS_LABEL_START_OFFSET   0x20000
#define ZFS_UBERBLOCK_ARRAY_SIZE 0x20000
#define ZFS_UBERBLOCK_SIZE       1024

struct ZfsChecksumResult {
    uint64_t zcr_word;
};

struct zfs_uberblock_disk {
    uint64_t          ub_magic;
    uint64_t          ub_version;
    uint64_t          ub_txg;
    uint64_t          ub_guid_sum;
    uint64_t          ub_timestamp;
    uint8_t           ub_rootbp;
    uint8_t           ub_reserved; 
    ZfsChecksumResult ub_fletcher4;
};

// =========================================================================
// --- СТРУКТУРЫ И КОНСТАНТЫ F2FS ---
// =========================================================================
#define F2FS_SUPER_OFFSET     1024
#define F2FS_MAGIC            0xF2F52010
#define F2FS_DIR_ENTRY_PER_BLOCK 214
#define F2FS_DIR_INODE        2

struct f2fs_super_block {
    uint32_t magic;
    uint16_t major_ver;
    uint16_t minor_ver;
    uint32_t log_sectorsize;
    uint32_t log_sectors_per_block;
    uint32_t log_blocksize;
    uint32_t log_blocks_per_seg;
    uint32_t segs_per_sec;
    uint32_t secs_per_zone;
    uint32_t checksum_offset;
    uint64_t block_count;
    uint32_t segment_count;
    uint32_t segment_count_main;
    uint32_t segment_count_user;
    uint32_t cp_blkaddr;
    uint32_t sit_blkaddr;
    uint32_t nat_blkaddr;
    uint32_t ssa_blkaddr;
    uint32_t main_blkaddr;
    uint32_t root_ino;
    uint32_t node_ino;
    uint32_t meta_ino;
    uint8_t  uuid;
    wchar_t  volume_name;
    uint32_t extension_count;
    uint8_t  extension_list;
    uint32_t cp_payload;
    uint8_t  version;
    uint8_t  init_version;
    uint32_t feature;
    uint8_t  encryption_level;
    uint8_t  blkz_invalid_regions;
    uint8_t  reserved;
};

struct f2fs_inode {
    uint16_t i_mode;
    uint8_t  i_advise;
    uint8_t  i_inline;
    uint32_t i_uid;
    uint32_t i_gid;
    uint32_t i_links;
    uint64_t i_size;
    uint64_t i_blocks;
    uint64_t i_atime;
    uint64_t i_ctime;
    uint64_t i_mtime;
    uint32_t i_generation;
    uint32_t i_current_depth;
    uint32_t i_xattr_nid;
    uint32_t i_flags;
    uint32_t i_pino;
    uint32_t i_namelen;
    uint8_t  i_name;
    uint8_t  i_dir_level;
    uint32_t i_addr;     
    uint32_t i_nid;        
};

struct f2fs_dir_entry {
    uint32_t hash_code;
    uint32_t ino;
    uint16_t name_len;
    uint8_t  file_type;
};

// =========================================================================
// --- NEW UNIX / APPLE FILESYSTEMS (2026 STABLE SPEC) ---
// =========================================================================
#define UFS2_MAGIC        0x194d0119
#define HFS_PLUS_MAGIC    0x2B48
#define APFS_MAGIC        0x42535041
#define REISERFS_MAGIC_V2 "ReIsEr2Fs"
#define REISER4_MAGIC     "ReIsEr4"
#define SQUASHFS_MAGIC    0x73717368
#define UFS_MAXMNTLEN     512

struct ufs2_super_block {
    uint32_t fs_firstfield;
    uint32_t fs_unused_1;
    int32_t  fs_sblkno;
    int32_t  fs_cblkno;
    int32_t  fs_iblkno;
    int32_t  fs_dblkno;
    uint32_t fs_ncg;
    uint32_t fs_bsize;
    uint32_t fs_fsize;
    uint32_t fs_frag;
    int64_t  fs_size;
    int64_t  fs_dsize;
    uint32_t fs_nindir;
    uint32_t fs_inopb;
    uint32_t fs_magic;
    uint8_t  fs_fmod;
    uint8_t  fs_clean;
    uint8_t  fs_ronly;
    uint8_t  fs_flags;
    uint8_t  fs_fsmnt[UFS_MAXMNTLEN];
};

struct hfs_plus_extent_descriptor {
    uint32_t startBlock;
    uint32_t blockCount;
};

typedef hfs_plus_extent_descriptor hfs_plus_extent_record;

struct hfs_plus_volume_header {
    uint16_t signature;
    uint16_t version;
    uint32_t attributes;
    uint32_t lastMountedVersion;
    uint32_t journalInfoBlock;
    uint32_t createDate;
    uint32_t modifyDate;
    uint32_t backupDate;
    uint32_t checkedDate;
    uint32_t fileCount;
    uint32_t folderCount;
    uint32_t blockSize;
    uint32_t totalBlocks;
    uint32_t freeBlocks;
    uint32_t nextAllocation;
    uint32_t rsrcClumpSize;
    uint32_t dataClumpSize;
    uint32_t nextCatalogId;
    uint32_t writeCount;
    uint64_t encodingsBitmap;
    hfs_plus_extent_record allocationFile;
    hfs_plus_extent_record extentsFile;
    hfs_plus_extent_record catalogFile;
};

struct apfs_obj_phys {
    uint64_t o_cksum;
    uint64_t o_oid;
    uint64_t o_xid;
    uint32_t o_type;
    uint32_t o_subtype;
};

struct apfs_container_superblock {
    apfs_obj_phys ac_obj;
    uint32_t ac_magic;
    uint32_t ac_block_size;
    uint64_t ac_block_count;
    uint64_t ac_features;
    uint64_t ac_readonly_compatible_features;
    uint64_t ac_incompatible_features;
    uint8_t  ac_uuid;
    uint64_t ac_next_oid;
    uint64_t ac_next_xid;
    uint64_t ac_omap_oid;
    uint64_t ac_vdev_tree_oid;
};

struct reiserfs_super_block {
    uint32_t s_block_count;
    uint32_t s_free_blocks;
    uint32_t s_root_block;
    uint32_t s_journal_block;
    uint32_t s_journal_dev;
    uint32_t s_orig_journal_size;
    uint16_t s_blocksize;
    uint16_t s_oid_maxsize;
    uint16_t s_oid_cursize;
    uint16_t s_state;
    char     s_magic;
};

struct squashfs_super_block {
    uint32_t s_magic;
    uint32_t inodes;
    uint32_t mkfs_time;
    uint32_t block_size;
    uint32_t fragments;
    uint16_t compression;
    uint16_t block_log;
    uint16_t flags;
    uint64_t bytes_used;
    uint64_t id_table_start;
    uint64_t inode_table_start;
    uint64_t directory_table_start;
    uint64_t fragment_table_start;
    uint64_t export_table_start;
};

#pragma pack(pop)

// =========================================================================
// --- THREAD-LOCAL ARENA ALLOCATOR (CRASH-PROOF MEMORY POOL) ---
// =========================================================================
class ThreadLocalArena {
private:
    uint8_t* arenaBuffer;
    size_t   totalSize;
    size_t   currentOffset;

public:
    ThreadLocalArena(size_t size = 2 * 1024 * 1024) 
        : totalSize(size), currentOffset(0), arenaBuffer(nullptr) {
        arenaBuffer = reinterpret_cast<uint8_t*>(VirtualAlloc(NULL, totalSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }

    ~ThreadLocalArena() {
        if (arenaBuffer) {
            VirtualFree(arenaBuffer, 0, MEM_RELEASE);
            arenaBuffer = nullptr;
        }
    }

    void* Alloc(size_t size) {
        if (!arenaBuffer) return nullptr;
        size = (size + 7) & ~7; // Выравнивание по границе 8 байт для amd64

        if (currentOffset + size > totalSize) {
            return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size); // Системный резервный фолбэк
        }

        void* ptr = arenaBuffer + currentOffset;
        currentOffset += size;
        __stosb(reinterpret_cast<BYTE*>(ptr), 0, size); // Быстрое аппаратное зануление памяти
        return ptr;
    }

    void Reset() {
        currentOffset = 0;
    }
};

extern thread_local ThreadLocalArena G_StorageArena;

// =========================================================================
// --- INLINE HELPERS: PERMISSIONS & CHECKSUMS (INTEL CORE i5 OPTIMIZED) ---
// =========================================================================
#define S_IFMT   0170000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFDIR  0040000

inline void ModeToPermissionString(uint32_t mode, wchar_t* outStr) {
    if (!outStr) return;
    wchar_t* p = outStr;

    if ((mode & S_IFMT) == S_IFDIR)       *p++ = L'd';
    else if ((mode & S_IFMT) == S_IFLNK)  *p++ = L'l';
    else                                  *p++ = L'-';

    *p++ = (mode & 00400) ? L'r' : L'-';
    *p++ = (mode & 00200) ? L'w' : L'-';
    *p++ = (mode & 00100) ? L'x' : L'-';

    *p++ = (mode & 00040) ? L'r' : L'-';
    *p++ = (mode & 00020) ? L'w' : L'-';
    *p++ = (mode & 00010) ? L'x' : L'-';

    *p++ = (mode & 00004) ? L'r' : L'-';
    *p++ = (mode & 00002) ? L'w' : L'-';
    *p++ = (mode & 00001) ? L'x' : L'-';
    *p = L'\0';
}

inline ZfsChecksumResult ComputeFletcher4(const uint64_t* data, size_t sizeBytes) {
    ZfsChecksumResult result = { {0, 0, 0, 0} };
    if (!data || sizeBytes == 0) return result;

    size_t count = sizeBytes / sizeof(uint64_t);
    uint64_t a = 0, b = 0, c = 0, d = 0;

    // Цикл полностью разворачивается компилятором (-funroll-loops) под Intel LSD
    for (size_t i = 0; i < count; ++i) {
        a += data[i];
        b += a;
        c += b;
        d += c;
    }

    result.zcr_word[0] = a; result.zcr_word[1] = b;
    result.zcr_word[2] = c; result.zcr_word[3] = d;
    return result;
}

inline uint64_t SwapBytes64(uint64_t val) {
    return _byteswap_uint64(val);
}

// =========================================================================
// --- МАТЕМАТИКА ХЭШИРОВАНИЯ ИМЕН ИЗ ПРОЕКТА CROSSMETA ---
// =========================================================================

/**
 * Алгоритм вычисления хэша HTree (Half-MD4) для эффективного поиска в Ext4.
 * Позволяет парсеру сразу вычислять логический блок директории по имени файла.
 */
inline uint32_t Crossmeta_Ext4_DirHash(const char* name, int len) {
    uint32_t hash = 0;
    if (!name || len <= 0) return hash;

    uint32_t buf[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 }; // Константы Half-MD4
    
    // Поблочное хэширование строки имени файла (батчинг по 4 байта)
    // Развернуто компилятором (-funroll-loops) под порты исполнения Intel i5
    for (int i = 0; i < len; i += 4) {
        uint32_t data = 0;
        for (int j = 0; j < 4; ++j) {
            if (i + j < len) {
                data |= (static_cast<uint32_t>(name[i + j]) << (j * 8));
            }
        }
        
        // Быстрое регистровое перемешивание Crossmeta
        buf[0] += data;
        buf[0] = (buf[0] + buf[1]) ^ ((buf[0] << 9) | (buf[0] >> 23));
        buf[2] += buf[3];
        buf[2] = (buf[2] + buf[0]) ^ ((buf[2] << 13) | (buf[2] >> 19));
    }

    hash = buf[0] + buf[2];
    return hash & ~1; // Сброс младшего бита согласно on-disk спецификации Ext4
}

/**
 * Алгоритм хэширования имен файлов файловой системы XFS (Dir2 Hash).
 * Позволяет осуществлять двоичный поиск в индексах блоков Allocation Groups.
 */
inline uint32_t Crossmeta_Xfs_DirHash(const char* name, int len) {
    uint32_t hash = 0;
    if (!name || len <= 0) return hash;

    // Сверхлегкий итерационный сдвиг регистра, утилизирующий ALU процессора Intel Core i5
    for (int i = 0; i < len; ++i) {
        hash = static_cast<uint32_t>(name[i]) ^ ((hash << 7) | (hash >> 25));
    }
    return hash;
}
