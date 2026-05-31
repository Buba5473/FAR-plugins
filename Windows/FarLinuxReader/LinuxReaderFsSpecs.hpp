// =========================================================================
#pragma once
#include <stdint.h>

#pragma pack(push, 1)

// =========================================================================
// --- СТРУКТУРЫ И КОНСТАНТЫ BTRFS (АКТУАЛЬНОСТЬ: МАЙ 2026) ---
// =========================================================================
#define BTRFS_SIGNATURE "_BHRfS_M"
#define BTRFS_SUPER_INFO_OFFSET 0x10000

#define BTRFS_ROOT_TREE_OBJECTID        1
#define BTRFS_CHUNK_TREE_OBJECTID       3
#define BTRFS_BLOCK_GROUP_TREE_OBJECTID 11 

#define BTRFS_INODE_ITEM_KEY      1
#define BTRFS_ROOT_ITEM_KEY       132
#define BTRFS_DIR_ITEM_KEY        168
#define BTRFS_DIR_INDEX_KEY       169
#define BTRFS_CHUNK_ITEM_KEY      228

#define BTRFS_FT_REG_FILE         1
#define BTRFS_FT_DIR              2
#define BTRFS_FT_SYMLNK           7

#define BTRFS_BLOCK_GROUP_DATA     (1ULL << 0)
#define BTRFS_BLOCK_GROUP_SYSTEM   (1ULL << 1)
#define BTRFS_BLOCK_GROUP_METADATA (1ULL << 2)
#define BTRFS_BLOCK_GROUP_RAID0    (1ULL << 3)
#define BTRFS_BLOCK_GROUP_RAID1    (1ULL << 4)
#define BTRFS_BLOCK_GROUP_RAID10   (1ULL << 6)
#define BTRFS_BLOCK_GROUP_RAID1C3  (1ULL << 7)
#define BTRFS_BLOCK_GROUP_RAID1C4  (1ULL << 8)

#define BTRFS_EXTENT_DATA_KEY     108
#define BTRFS_FILE_EXTENT_INLINE  0
#define BTRFS_FILE_EXTENT_REG     1
#define BTRFS_FILE_EXTENT_PREALLOC 2

struct btrfs_disk_key {
    uint64_t objectid;
    uint8_t  type;
    uint64_t offset;
};

struct btrfs_header {
    uint8_t  csum[32];
    uint8_t  fsid[16];
    uint64_t bytenr;
    uint64_t flags;
    uint8_t  chunk_tree_uuid[16];
    uint64_t generation;
    uint64_t owner;
    uint32_t nritems;
    uint8_t  level;
};

struct btrfs_key_ptr {
    struct btrfs_disk_key key;
    uint64_t blockptr;
    uint64_t generation;
};

struct btrfs_item {
    struct btrfs_disk_key key;
    uint32_t offset;
    uint32_t size;
};

struct btrfs_dir_item {
    struct btrfs_disk_key location;
    uint64_t transid;
    uint16_t data_len;
    uint16_t name_len;
    uint8_t  type;
};

struct btrfs_stripe {
    uint64_t devid;
    uint64_t offset;
    uint8_t  dev_uuid[16];
};

struct btrfs_chunk {
    uint64_t length;
    uint64_t owner;
    uint64_t stripe_len;
    uint64_t type;
    uint32_t io_align;
    uint32_t io_width;
    uint32_t sector_size;
    uint16_t num_stripes;
    uint16_t sub_stripes;
    struct btrfs_stripe stripes; 
};

struct btrfs_dev_item {
    uint64_t devid;
    uint64_t total_bytes;
    uint64_t bytes_used;
    uint32_t io_align;
    uint32_t io_width;
    uint32_t sector_size;
    uint64_t type;
    uint64_t generation;
    uint64_t start_offset;
    uint32_t dev_group;
    uint8_t  uuid[16];
    uint8_t  fsid[16];
};

struct btrfs_super_block {
    uint8_t  csum[32];
    uint8_t  fsid[16];
    uint64_t bytenr;
    uint64_t flags;
    uint8_t  magic[8];
    uint64_t generation;
    uint64_t root;
    uint64_t chunk_root;
    uint64_t log_root;
    uint64_t log_root_transid;
    uint64_t total_bytes;
    uint64_t bytes_used;
    uint64_t root_dir_objectid;
    uint64_t num_devices;
    uint32_t sectorsize;
    uint32_t nodesize;
    uint32_t leafsize;
    uint32_t stripesize;
    uint32_t sys_chunk_array_size;
    uint64_t chunk_root_generation;
    uint64_t compat_flags;
    uint64_t compat_ro_flags;
    uint64_t incompat_flags;
    uint16_t csum_type;
    struct btrfs_dev_item dev_item;
    uint8_t  label[256];
    uint64_t cache_generation;
    uint64_t uuid_tree_generation;
    uint8_t  metadata_uuid[16];
    uint64_t reserved[28];
};

struct btrfs_file_extent_item {
    uint64_t generation;
    uint64_t ram_bytes;
    uint8_t  compression;
    uint8_t  encryption;
    uint16_t other_encoding;
    uint8_t  type;
    uint64_t disk_bytenr;
    uint64_t disk_num_bytes;
    uint64_t offset;
    uint64_t num_bytes;
};

struct BtrfsChunkMapEntry {
    uint64_t LogicalStart;
    uint64_t Length;
    uint64_t Type;
    uint16_t NumStripes;
    uint64_t PhysicalOffset;
};

struct BtrfsSubvolumeEntry {
    uint64_t Id;
    uint64_t RootVaddr;
    wchar_t  Name[256];
};

// =========================================================================
// --- СТРУКТУРЫ И КОНСТАНТЫ EXT4 (64BIT & FAST COMMITS READY) ---
// =========================================================================
#define EXT4_SUPER_MAGIC   0xEF53
#define EXT4_FEATURE_INCOMPAT_EXTENTS      0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT        0x0080
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
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
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
    char     s_mount_opts[64];
    uint32_t s_usr_quota_inum;
    uint32_t s_grp_quota_inum;
    uint32_t s_overhead_blocks;
    uint32_t s_backup_bgs[2];
    uint8_t  s_encrypt_algos[4];
    uint8_t  s_encrypt_pw_salt[16];
    uint32_t s_lpf_ino;
    uint32_t s_prj_quota_inum;
    uint32_t s_checksum_seed;
    uint32_t s_reserved[98];
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
    uint8_t  i_block[60];
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
// --- СТРУКТУРЫ И КОНСТАНТЫ XFS (V5 BIGTIME COMPATIBLE) ---
#define XFS_SB_MAGIC 0x58465342

#define XFS_SB_FEAT_INCOMPAT_FTYPE      (1ULL << 0)
#define XFS_SB_FEAT_INCOMPAT_SPINODES   (1ULL << 1)
#define XFS_SB_FEAT_INCOMPAT_META_UUID  (1ULL << 2)
#define XFS_SB_FEAT_INCOMPAT_BIGTIME    (1ULL << 3)
#define XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR (1ULL << 4)
#define XFS_SB_FEAT_INCOMPAT_NREXT64    (1ULL << 5)

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
// --- СТРУКТУРЫ И КОНСТАНТЫ OPENZFS (FLETCHER4 VERIFICATION) ---
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
// --- СТРУКТУРЫ И КОНСТАНТЫ SAMSUNG F2FS (FLASH-FRIENDLY LFS) ---
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
// --- ДОПОЛНИТЕЛЬНЫЕ ФУНДАМЕНТАЛЬНЫЕ ФС UNIX / APPLE (МАЙ 2026 SPECS) ---
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
