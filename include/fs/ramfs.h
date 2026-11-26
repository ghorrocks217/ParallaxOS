#ifndef FS_RAMFS_H
#define FS_RAMFS_H

#include "fs/vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

void ramfs_init(void);

int ramfs_create(const char *path, unsigned type);
int ramfs_remove(const char *path);

int ramfs_write(
    const char *path,
    const char *data,
    unsigned size
);

int ramfs_read(
    const char *path,
    char *buffer,
    unsigned capacity
);

int ramfs_read_at(
    const char *path,
    unsigned offset,
    char *buffer,
    unsigned size
);

int ramfs_write_at(
    const char *path,
    unsigned offset,
    const char *buffer,
    unsigned size
);

int ramfs_list(
    const char *path,
    struct vfs_dirent *entries,
    unsigned capacity
);

int ramfs_truncate(
    const char *path,
    unsigned size
);

int ramfs_stat(
    const char *path,
    unsigned *type,
    unsigned *size
);

int ramfs_exists(const char *path);
int ramfs_is_directory(const char *path);

#ifdef __cplusplus
}
#endif

#endif
