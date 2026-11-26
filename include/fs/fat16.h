#ifndef PARALLAX_FAT16_H
#define PARALLAX_FAT16_H

#include "fs/vfs.h"

struct fat16_info {
    unsigned device;
    unsigned total_sectors;
    unsigned sectors_per_cluster;
    unsigned reserved_sectors;
    unsigned fat_count;
    unsigned sectors_per_fat;
    unsigned root_entries;
    unsigned root_directory_sectors;
    unsigned data_start_sector;
    unsigned cluster_count;
};

#ifdef __cplusplus
extern "C" {
#endif

int fat16_mount(unsigned device);
int fat16_is_mounted(void);
int fat16_get_info(struct fat16_info *info);

int fat16_list(
    const char *path,
    struct vfs_dirent *entries,
    unsigned capacity
);

int fat16_stat(
    const char *path,
    unsigned *type,
    unsigned *size
);

int fat16_read(
    const char *path,
    char *buffer,
    unsigned capacity
);

int fat16_create(const char *path);
int fat16_mkdir(const char *path);
int fat16_remove(const char *path);

int fat16_write(
    const char *path,
    const char *data,
    unsigned size
);

#ifdef __cplusplus
}
#endif

#endif
