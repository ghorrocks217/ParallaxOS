
#include "fs/vfs.h"
#include "fs/ramfs.h"
#include "fs/fat16.h"

/* Maximum number of simultaneously mounted filesystems. */
struct VfsMount {
    int used;
    char path[VFS_PATH_MAX];

    const vfs_fs_ops *ops;
    void *context;
};

static VfsMount mounts[VFS_MAX_MOUNTS];
static unsigned mounted_count = 0;

/* ---------------------------------------------------------
   String helpers
   --------------------------------------------------------- */

static unsigned str_length(const char *s) {
    unsigned length = 0;

    if (!s)
        return 0;

    while (s[length])
        ++length;

    return length;
}

static int str_equal(const char *a, const char *b) {
    if (!a || !b)
        return 0;

    while (*a && *b) {
        if (*a != *b)
            return 0;

        ++a;
        ++b;
    }

    return *a == *b;
}

static void str_copy(
    char *destination,
    const char *source,
    unsigned capacity
) {
    if (!destination || !capacity)
        return;

    unsigned i = 0;

    while (source && source[i] && i < capacity - 1) {
        destination[i] = source[i];
        ++i;
    }

    destination[i] = '\0';
}

static int valid_mount_path(const char *path) {
    if (!path || path[0] != '/')
        return 0;

    unsigned length = str_length(path);

    if (!length || length >= VFS_PATH_MAX)
        return 0;

    if (length > 1 && path[length - 1] == '/')
        return 0;

    for (unsigned i = 0; i < length; ++i) {
        if (path[i] == '/' && path[i + 1] == '/')
            return 0;
    }

    return 1;
}

/* ---------------------------------------------------------
   Filesystem driver adapters
   --------------------------------------------------------- */

/* RAMFS */

static int ramfs_driver_create(void *, const char *path) {
    return ramfs_create(path, VFS_FILE);
}

static int ramfs_driver_mkdir(void *, const char *path) {
    return ramfs_create(path, VFS_DIRECTORY);
}

static int ramfs_driver_remove(void *, const char *path) {
    return ramfs_remove(path);
}

static int ramfs_driver_write(
    void *,
    const char *path,
    const char *data,
    unsigned size
) {
    return ramfs_write(path, data, size);
}

static int ramfs_driver_read(
    void *,
    const char *path,
    char *buffer,
    unsigned capacity
) {
    return ramfs_read(path, buffer, capacity);
}

static int ramfs_driver_list(
    void *,
    const char *path,
    struct vfs_dirent *entries,
    unsigned capacity
) {
    return ramfs_list(path, entries, capacity);
}

static int ramfs_driver_stat(
    void *,
    const char *path,
    unsigned *type,
    unsigned *size
) {
    if (!ramfs_exists(path))
        return 0;

    if (type) {
        *type = ramfs_is_directory(path)
            ? VFS_DIRECTORY
            : VFS_FILE;
    }

    if (size)
        *size = 0;

    return 1;
}

static const vfs_fs_ops ramfs_operations = {
    ramfs_driver_create,
    ramfs_driver_mkdir,
    ramfs_driver_remove,
    ramfs_driver_write,
    ramfs_driver_read,
    ramfs_driver_list,
    ramfs_driver_stat
};

/* FAT16 */

static int fat16_driver_create(void *, const char *path) {
    return fat16_create(path);
}

static int fat16_driver_mkdir(void *, const char *path) {
    return fat16_mkdir(path);
}

static int fat16_driver_remove(void *, const char *path) {
    return fat16_remove(path);
}

static int fat16_driver_write(
    void *,
    const char *path,
    const char *data,
    unsigned size
) {
    return fat16_write(path, data, size);
}

static int fat16_driver_read(
    void *,
    const char *path,
    char *buffer,
    unsigned capacity
) {
    return fat16_read(path, buffer, capacity);
}

static int fat16_driver_list(
    void *,
    const char *path,
    struct vfs_dirent *entries,
    unsigned capacity
) {
    return fat16_list(path, entries, capacity);
}

static int fat16_driver_stat(
    void *,
    const char *path,
    unsigned *type,
    unsigned *size
) {
    return fat16_stat(path, type, size);
}

static const vfs_fs_ops fat16_operations = {
    fat16_driver_create,
    fat16_driver_mkdir,
    fat16_driver_remove,
    fat16_driver_write,
    fat16_driver_read,
    fat16_driver_list,
    fat16_driver_stat
};

/* ---------------------------------------------------------
   Mount management
   --------------------------------------------------------- */

int vfs_mount(
    const char *mount_path,
    const struct vfs_fs_ops *ops,
    void *context
) {
    if (!valid_mount_path(mount_path) || !ops)
        return 0;

    /* Every mounted filesystem needs a stat operation. */
    if (!ops->stat)
        return 0;

    /* Reject duplicate mount points. */
    for (unsigned i = 0; i < VFS_MAX_MOUNTS; ++i) {
        if (mounts[i].used &&
            str_equal(mounts[i].path, mount_path))
            return 0;
    }

    /* Find an unused mount slot. */
    for (unsigned i = 0; i < VFS_MAX_MOUNTS; ++i) {
        if (mounts[i].used)
            continue;

        mounts[i].used = 1;
        str_copy(
            mounts[i].path,
            mount_path,
            VFS_PATH_MAX
        );

        mounts[i].ops = ops;
        mounts[i].context = context;

        ++mounted_count;
        return 1;
    }

    return 0;
}

int vfs_unmount(const char *mount_path) {
    if (!mount_path)
        return 0;

    /* Keep the root filesystem mounted. */
    if (str_equal(mount_path, "/"))
        return 0;

    for (unsigned i = 0; i < VFS_MAX_MOUNTS; ++i) {
        if (!mounts[i].used ||
            !str_equal(mounts[i].path, mount_path))
            continue;

        mounts[i].used = 0;
        mounts[i].path[0] = '\0';
        mounts[i].ops = 0;
        mounts[i].context = 0;

        if (mounted_count)
            --mounted_count;

        return 1;
    }

    return 0;
}

unsigned vfs_mount_count(void) {
    return mounted_count;
}

/* ---------------------------------------------------------
   Path resolution
   --------------------------------------------------------- */

struct ResolvedPath {
    VfsMount *mount;
    const char *relative_path;
};

/*
 * Select the longest matching mount prefix.
 *
 * Example:
 *   /             -> RAMFS
 *   /disk         -> FAT16
 *   /disk/file    -> FAT16
 *
 * /disk2 does not match /disk.
 */
static int resolve_path(
    const char *path,
    ResolvedPath *result
) {
    if (!path || path[0] != '/' || !result)
        return 0;

    unsigned path_length = str_length(path);
    int best = -1;
    unsigned best_length = 0;

    for (unsigned i = 0; i < VFS_MAX_MOUNTS; ++i) {
        if (!mounts[i].used)
            continue;

        unsigned mount_length =
            str_length(mounts[i].path);

        if (mount_length > path_length)
            continue;

        int matches = 1;

        for (unsigned j = 0; j < mount_length; ++j) {
            if (path[j] != mounts[i].path[j]) {
                matches = 0;
                break;
            }
        }

        if (!matches)
            continue;

        /* Root matches every absolute path. */
        if (mount_length != 1 &&
            path_length != mount_length &&
            path[mount_length] != '/') {
            continue;
        }

        if (mount_length > best_length) {
            best = (int)i;
            best_length = mount_length;
        }
    }

    if (best < 0)
        return 0;

    result->mount = &mounts[best];

    if (best_length == 1) {
        result->relative_path = path;
    } else if (path_length == best_length) {
        result->relative_path = "/";
    } else {
        result->relative_path = path + best_length;
    }

    return 1;
}

/* ---------------------------------------------------------
   VFS initialization
   --------------------------------------------------------- */

void vfs_init(void) {
    for (unsigned i = 0; i < VFS_MAX_MOUNTS; ++i) {
        mounts[i].used = 0;
        mounts[i].path[0] = '\0';
        mounts[i].ops = 0;
        mounts[i].context = 0;
    }

    mounted_count = 0;

    ramfs_init();

    /* RAMFS is the root filesystem. */
    vfs_mount("/", &ramfs_operations, 0);

    /*
     * Create the mount-point directory before mounting FAT16.
     * This preserves the directory entry visible in RAMFS.
     */
    if (fat16_is_mounted()) {
        ramfs_create("/disk", VFS_DIRECTORY);

        vfs_mount(
            "/disk",
            &fat16_operations,
            0
        );
    }
}

/* ---------------------------------------------------------
   Filesystem operations
   --------------------------------------------------------- */

int vfs_create(const char *path) {
    ResolvedPath resolved;

    if (!resolve_path(path, &resolved))
        return 0;

    if (!resolved.mount->ops->create)
        return 0;

    return resolved.mount->ops->create(
        resolved.mount->context,
        resolved.relative_path
    );
}

int vfs_mkdir(const char *path) {
    ResolvedPath resolved;

    if (!resolve_path(path, &resolved))
        return 0;

    if (!resolved.mount->ops->mkdir)
        return 0;

    return resolved.mount->ops->mkdir(
        resolved.mount->context,
        resolved.relative_path
    );
}

int vfs_remove(const char *path) {
    ResolvedPath resolved;

    if (!resolve_path(path, &resolved))
        return 0;

    if (!resolved.mount->ops->remove)
        return 0;

    return resolved.mount->ops->remove(
        resolved.mount->context,
        resolved.relative_path
    );
}

int vfs_write(
    const char *path,
    const char *data,
    unsigned size
) {
    ResolvedPath resolved;

    if (!resolve_path(path, &resolved))
        return 0;

    if (!resolved.mount->ops->write)
        return 0;

    return resolved.mount->ops->write(
        resolved.mount->context,
        resolved.relative_path,
        data,
        size
    );
}

int vfs_read(
    const char *path,
    char *buffer,
    unsigned capacity
) {
    ResolvedPath resolved;

    if (!resolve_path(path, &resolved))
        return -1;

    if (!resolved.mount->ops->read)
        return -1;

    return resolved.mount->ops->read(
        resolved.mount->context,
        resolved.relative_path,
        buffer,
        capacity
    );
}

int vfs_list(
    const char *path,
    struct vfs_dirent *entries,
    unsigned capacity
) {
    ResolvedPath resolved;

    if (!resolve_path(path, &resolved))
        return -1;

    if (!resolved.mount->ops->list)
        return -1;

    return resolved.mount->ops->list(
        resolved.mount->context,
        resolved.relative_path,
        entries,
        capacity
    );
}

int vfs_exists(const char *path) {
    ResolvedPath resolved;

    if (!resolve_path(path, &resolved))
        return 0;

    return resolved.mount->ops->stat(
        resolved.mount->context,
        resolved.relative_path,
        0,
        0
    );
}

int vfs_is_directory(const char *path) {
    ResolvedPath resolved;
    unsigned type = 0;

    if (!resolve_path(path, &resolved))
        return 0;

    if (!resolved.mount->ops->stat(
            resolved.mount->context,
            resolved.relative_path,
            &type,
            0))
        return 0;

    return type == VFS_DIRECTORY;
}
