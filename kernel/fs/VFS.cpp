
#include "fs/vfs.h"
#include "fs/ramfs.h"
#include "fs/fat16.h"

/*
 * FAT16 is mounted at /disk.
 *
 * /disk           -> FAT16 root
 * /disk/FILE.TXT  -> FAT16 /FILE.TXT
 *
 * Everything else remains in RAMFS.
 */

static int is_disk_path(const char *path) {
    static const char mount[] = "/disk";

    if (!path)
        return 0;

    for (unsigned i = 0; i < 5; ++i) {
        if (!path[i] || path[i] != mount[i])
            return 0;
    }

    return path[5] == '\0' || path[5] == '/';
}

static const char *disk_relative_path(const char *path) {
    if (!is_disk_path(path))
        return 0;

    if (path[5] == '\0')
        return "/";

    return path + 5;
}

void vfs_init(void) {
    ramfs_init();

    /*
     * Create the mount-point directory in RAMFS.
     * FAT16 itself remains stored on the physical disk.
     */
    if (fat16_is_mounted())
        ramfs_create("/disk", VFS_DIRECTORY);
}

int vfs_create(const char *path) {
    if (is_disk_path(path)) {
        const char *relative = disk_relative_path(path);

        return fat16_create(relative);
    }

    return ramfs_create(path, VFS_FILE);
}

int vfs_mkdir(const char *path) {
    if (is_disk_path(path))
        return 0;

    return ramfs_create(path, VFS_DIRECTORY);
}

int vfs_remove(const char *path) {
    if (is_disk_path(path)) {
        const char *relative = disk_relative_path(path);
        return fat16_remove(relative);
    }

    return ramfs_remove(path);
}

int vfs_write(
    const char *path,
    const char *data,
    unsigned size
) {
    if (is_disk_path(path)) {
        const char *relative = disk_relative_path(path);

        return fat16_write(relative, data, size);
    }

    return ramfs_write(path, data, size);
}

int vfs_read(
    const char *path,
    char *buffer,
    unsigned capacity
) {
    if (is_disk_path(path)) {
        const char *relative = disk_relative_path(path);

        return fat16_read(relative, buffer, capacity);
    }

    return ramfs_read(path, buffer, capacity);
}

int vfs_list(
    const char *path,
    struct vfs_dirent *entries,
    unsigned capacity
) {
    if (is_disk_path(path)) {
        const char *relative = disk_relative_path(path);

        return fat16_list(relative, entries, capacity);
    }

    return ramfs_list(path, entries, capacity);
}

int vfs_exists(const char *path) {
    if (is_disk_path(path)) {
        const char *relative = disk_relative_path(path);

        return fat16_stat(relative, 0, 0);
    }

    return ramfs_exists(path);
}

int vfs_is_directory(const char *path) {
    if (is_disk_path(path)) {
        const char *relative = disk_relative_path(path);
        unsigned type = 0;

        if (!fat16_stat(relative, &type, 0))
            return 0;

        return type == VFS_DIRECTORY;
    }

    return ramfs_is_directory(path);
}
