#include "fs/vfs.h"
#include "fs/ramfs.h"

void vfs_init(void) {
    ramfs_init();
}

int vfs_create(const char *path) {
    return ramfs_create(path, VFS_FILE);
}

int vfs_mkdir(const char *path) {
    return ramfs_create(path, VFS_DIRECTORY);
}

int vfs_remove(const char *path) {
    return ramfs_remove(path);
}

int vfs_write(
    const char *path,
    const char *data,
    unsigned size
) {
    return ramfs_write(path, data, size);
}

int vfs_read(
    const char *path,
    char *buffer,
    unsigned capacity
) {
    return ramfs_read(path, buffer, capacity);
}

int vfs_list(
    const char *path,
    struct vfs_dirent *entries,
    unsigned capacity
) {
    return ramfs_list(path, entries, capacity);
}

int vfs_exists(const char *path) {
    return ramfs_exists(path);
}

int vfs_is_directory(const char *path) {
    return ramfs_is_directory(path);
}
