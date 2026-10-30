/*
 * 虚拟 U 盘：只读 FAT16，只有一个 index.htm。
 *
 * 几何参数（必须自洽，主机按 BPB 自己算）：
 *   扇区 512 B，1 扇区/簇，保留 1 扇区，2 份 FAT，根目录 16 项（1 扇区）
 *   数据区 4096 簇  ->  总扇区 = 1 + 2*17 + 1 + 4096 = 4132（约 2 MiB）
 *
 * 4096 >= 4085，所以主机会把它判成 FAT16（再少就变成 FAT12，目录项格式不同）。
 * 17 个 FAT 扇区能装 4352 个表项，覆盖 4098 个簇（0、1 是保留项）。
 *
 * 这里不做通用文件系统：数据区从簇 2 开始、只有一个连续文件，所以
 * 「扇区 -> 文件偏移」是一次减法，FAT 表按簇号现算，不需要缓存。
 */

#include "boot_disk.h"

#include <string.h>

#define DISK_SECTORS_PER_CLUSTER 1u
#define DISK_RESERVED_SECTORS    1u
#define DISK_FAT_COUNT           2u
#define DISK_ROOT_ENTRIES        16u
#define DISK_DATA_CLUSTERS       4096u

#define DISK_FAT_SECTORS \
    (((DISK_DATA_CLUSTERS + 2u) * 2u + BOOT_DISK_SECTOR_SIZE - 1u) / BOOT_DISK_SECTOR_SIZE)
#define DISK_ROOT_SECTORS (DISK_ROOT_ENTRIES * 32u / BOOT_DISK_SECTOR_SIZE)

#define DISK_FAT_START  (DISK_RESERVED_SECTORS)
#define DISK_ROOT_START (DISK_FAT_START + DISK_FAT_COUNT * DISK_FAT_SECTORS)
#define DISK_DATA_START (DISK_ROOT_START + DISK_ROOT_SECTORS)
#define DISK_TOTAL_SECTORS \
    (DISK_DATA_START + DISK_DATA_CLUSTERS * DISK_SECTORS_PER_CLUSTER)

#define DISK_INDEX_URL "https://github.com/TheBadZhang/uDap"

static const char s_index_htm[] =
    "<!doctype html><meta charset=utf-8><title>uDap</title>"
    "<script>location.replace('" DISK_INDEX_URL "')</script>"
    "<noscript><a href='" DISK_INDEX_URL "'>" DISK_INDEX_URL "</a></noscript>";

#define DISK_INDEX_SIZE ((uint32_t)(sizeof(s_index_htm) - 1u))
#define DISK_INDEX_CLUSTERS ((DISK_INDEX_SIZE + BOOT_DISK_SECTOR_SIZE - 1u) / BOOT_DISK_SECTOR_SIZE)
#define DISK_INDEX_FIRST_CLUSTER 2u
#define DISK_INDEX_LAST_CLUSTER  (DISK_INDEX_FIRST_CLUSTER + DISK_INDEX_CLUSTERS - 1u)

/* 2024-01-01 00:00:00，FAT 目录项里随便一个有意义的日期 */
#define DISK_DOS_DATE (((2024u - 1980u) << 9u) | (1u << 5u) | 1u)

typedef struct __attribute__((packed)) {
    uint8_t jump[3];
    uint8_t oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entries;
    uint16_t total_sectors16;
    uint8_t media;
    uint16_t fat_sectors16;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors32;
    uint8_t drive;
    uint8_t reserved;
    uint8_t boot_signature;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t fs_type[8];
} disk_bpb_t;

typedef struct __attribute__((packed)) {
    char name[11];
    uint8_t attribute;
    uint8_t nt_reserved;
    uint8_t create_tenths;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_high;
    uint16_t update_time;
    uint16_t update_date;
    uint16_t cluster_low;
    uint32_t size;
} disk_entry_t;

_Static_assert(sizeof(disk_bpb_t) == 62, "BPB must be 62 bytes");
_Static_assert(sizeof(disk_entry_t) == 32, "directory entry must be 32 bytes");
_Static_assert(DISK_TOTAL_SECTORS <= 0xFFFFu, "volume must fit the 16-bit sector count");
_Static_assert(DISK_DATA_CLUSTERS >= 4085u, "fewer than 4085 clusters makes hosts treat the volume as FAT12");

static const disk_bpb_t s_bpb = {
    .jump = {0xEB, 0x3C, 0x90},
    .oem = {'U', 'D', 'A', 'P', ' ', ' ', ' ', ' '},
    .bytes_per_sector = BOOT_DISK_SECTOR_SIZE,
    .sectors_per_cluster = DISK_SECTORS_PER_CLUSTER,
    .reserved_sectors = DISK_RESERVED_SECTORS,
    .fat_count = DISK_FAT_COUNT,
    .root_entries = DISK_ROOT_ENTRIES,
    .total_sectors16 = DISK_TOTAL_SECTORS,
    .media = 0xF8,
    .fat_sectors16 = DISK_FAT_SECTORS,
    .sectors_per_track = 1,
    .heads = 1,
    .hidden_sectors = 0,
    .total_sectors32 = 0,
    .drive = 0x80,
    .reserved = 0,
    .boot_signature = 0x29,
    .volume_id = 0x75446170u, /* "uDap" */
    .volume_label = {'U', 'D', 'A', 'P', ' ', ' ', ' ', ' ', ' ', ' ', ' '},
    .fs_type = {'F', 'A', 'T', '1', '6', ' ', ' ', ' '},
};

void boot_disk_init(void) {
    /* 无状态：所有内容都是从常量现算的 */
}

uint16_t boot_disk_sector_size(void) {
    return BOOT_DISK_SECTOR_SIZE;
}

uint32_t boot_disk_sector_count(void) {
    return DISK_TOTAL_SECTORS;
}

/* FAT16 表项。只有 index.htm 一条簇链（簇 2..LAST），其余簇都空闲。 */
static uint16_t disk_fat_value(uint32_t cluster) {
    if (cluster == 0u) {
        return 0xFFF8u; /* 低字节必须等于媒体描述符 0xF8 */
    }
    if (cluster == 1u) {
        return 0xFFFFu;
    }
    if (cluster < DISK_INDEX_LAST_CLUSTER) {
        return (uint16_t)(cluster + 1u);
    }
    if (cluster == DISK_INDEX_LAST_CLUSTER) {
        return 0xFFFFu; /* 簇链结束 */
    }
    return 0u; /* 空闲 */
}

static void disk_read_fat(uint32_t sector, uint8_t *buffer) {
    uint32_t relative = (sector - DISK_FAT_START) % DISK_FAT_SECTORS;
    uint32_t first_cluster = relative * (BOOT_DISK_SECTOR_SIZE / 2u);

    for (uint32_t i = 0u; i < BOOT_DISK_SECTOR_SIZE / 2u; ++i) {
        uint16_t value = disk_fat_value(first_cluster + i);

        buffer[i * 2u] = (uint8_t)value;
        buffer[i * 2u + 1u] = (uint8_t)(value >> 8);
    }
}

static void disk_entry_fill(disk_entry_t *entry, const char *name, uint8_t attribute,
                            uint16_t cluster, uint32_t size) {
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->name, name, sizeof(entry->name));
    entry->attribute = attribute;
    entry->create_date = DISK_DOS_DATE;
    entry->access_date = DISK_DOS_DATE;
    entry->update_date = DISK_DOS_DATE;
    entry->cluster_low = cluster;
    entry->size = size;
}

static void disk_read_root(uint8_t *buffer) {
    disk_entry_t label;
    disk_entry_t file;

    /* 卷标（属性 0x08），主机资源管理器里显示成盘名 */
    disk_entry_fill(&label, "UDAP       ", 0x08, 0u, 0u);

    /* index.htm：名字按 FAT 规范存大写，NTReserved 的 0x08/0x10 告诉主机
     * 「基本名/扩展名都是小写」，Windows/Linux/macOS 都会显示成 index.htm。 */
    disk_entry_fill(&file, "INDEX   HTM", 0x20, DISK_INDEX_FIRST_CLUSTER, DISK_INDEX_SIZE);
    file.nt_reserved = 0x18;

    memcpy(buffer, &label, sizeof(label));
    memcpy(buffer + sizeof(label), &file, sizeof(file));
}

static void disk_read_data(uint32_t sector, uint8_t *buffer) {
    uint32_t offset = (sector - DISK_DATA_START) * BOOT_DISK_SECTOR_SIZE;
    uint32_t remain;

    if (offset >= DISK_INDEX_SIZE) {
        return;
    }

    remain = DISK_INDEX_SIZE - offset;
    if (remain > BOOT_DISK_SECTOR_SIZE) {
        remain = BOOT_DISK_SECTOR_SIZE;
    }
    memcpy(buffer, s_index_htm + offset, remain);
}

int boot_disk_read_sector(uint32_t start_sector, uint8_t *buffer, uint32_t sector_count) {
    while (sector_count-- != 0u) {
        memset(buffer, 0, BOOT_DISK_SECTOR_SIZE);

        if (start_sector == 0u) {
            memcpy(buffer, &s_bpb, sizeof(s_bpb));
            buffer[510] = 0x55;
            buffer[511] = 0xAA;
        } else if (start_sector < DISK_ROOT_START) {
            disk_read_fat(start_sector, buffer);
        } else if (start_sector < DISK_DATA_START) {
            disk_read_root(buffer);
        } else if (start_sector < DISK_TOTAL_SECTORS) {
            disk_read_data(start_sector, buffer);
        }

        buffer += BOOT_DISK_SECTOR_SIZE;
        start_sector++;
    }

    return 0;
}
