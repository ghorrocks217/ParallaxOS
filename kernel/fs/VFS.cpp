
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

struct VfsFileDescriptor {
    int used;
    unsigned flags;
    unsigned position;
    unsigned size;
    char path[VFS_PATH_MAX];
};

static VfsFileDescriptor file_descriptors[VFS_MAX_FDS];

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

static int ramfs_driver_read_at(
    void *,
    const char *path,
    unsigned offset,
    char *buffer,
    unsigned size
) {
    return ramfs_read_at(path, offset, buffer, size);
}

static int ramfs_driver_write_at(
    void *,
    const char *path,
    unsigned offset,
    const char *buffer,
    unsigned size
) {
    return ramfs_write_at(path, offset, buffer, size);
}

static int ramfs_driver_truncate(
    void *,
    const char *path,
    unsigned size
) {
    return ramfs_truncate(path, size);
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
    return ramfs_stat(path, type, size);
}

static const vfs_fs_ops ramfs_operations = {
    ramfs_driver_create,
    ramfs_driver_mkdir,
    ramfs_driver_remove,
    ramfs_driver_write,
    ramfs_driver_read,
    ramfs_driver_list,
    ramfs_driver_stat,
    ramfs_driver_read_at,
    ramfs_driver_write_at,
    ramfs_driver_truncate
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

static int fat16_driver_read_at(
    void *,
    const char *path,
    unsigned offset,
    char *buffer,
    unsigned size
) {
    return fat16_read_at(path, offset, buffer, size);
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
    fat16_driver_stat,
    fat16_driver_read_at,
    0,
    0
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
    for (unsigned i = 0; i < VFS_MAX_FDS; ++i) {
        file_descriptors[i].used = 0;
        file_descriptors[i].flags = 0;
        file_descriptors[i].position = 0;
        file_descriptors[i].size = 0;
        file_descriptors[i].path[0] = '\0';
    }

    mounted_count = 0;

    ramfs_init();

    /* RAMFS is the root filesystem. */
    vfs_mount("/", &ramfs_operations, 0);

    /* Mount FAT16 when available. */
    if (fat16_is_mounted()) {
        ramfs_create("/disk", VFS_DIRECTORY);

        vfs_mount("/disk", &fat16_operations, 0);
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

int vfs_read_at(
    const char *path,
    unsigned offset,
    char *buffer,
    unsigned size
) {
    if (!buffer && size)
        return -1;

    if (offset > 0x7FFFFFFFu ||
        size > 0x7FFFFFFFu - offset)
        return -1;

    ResolvedPath resolved;

    if (!resolve_path(path, &resolved))
        return -1;

    if (!resolved.mount->ops->read_at)
        return -1;

    return resolved.mount->ops->read_at(
        resolved.mount->context,
        resolved.relative_path,
        offset,
        buffer,
        size
    );
}

int vfs_write_at(
    const char *path,
    unsigned offset,
    const char *buffer,
    unsigned size
) {
    if (!buffer && size)
        return -1;

    if (offset > 0x7FFFFFFFu ||
        size > 0x7FFFFFFFu - offset)
        return -1;

    ResolvedPath resolved;

    if (!resolve_path(path, &resolved))
        return -1;

    if (!resolved.mount->ops->write_at)
        return -1;

    return resolved.mount->ops->write_at(
        resolved.mount->context,
        resolved.relative_path,
        offset,
        buffer,
        size
    );
}

int vfs_truncate(
    const char *path,
    unsigned size
) {
    if (size > 0x7FFFFFFFu)
        return 0;

    ResolvedPath resolved;

    if (!resolve_path(path, &resolved))
        return 0;

    if (!resolved.mount->ops->truncate)
        return 0;

    return resolved.mount->ops->truncate(
        resolved.mount->context,
        resolved.relative_path,
        size
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


/* ---------------------------------------------------------
   File descriptor implementation
   --------------------------------------------------------- */

static int valid_fd(int fd) {
    return fd >= 0 &&
           fd < VFS_MAX_FDS &&
           file_descriptors[fd].used;
}

int vfs_open(const char *path, unsigned flags) {
    if (!path || path[0] != '/')
        return -1;

    if (!(flags & (VFS_O_READ | VFS_O_WRITE)))
        return -1;

    if (flags & ~(VFS_O_READ | VFS_O_WRITE |
                  VFS_O_CREATE | VFS_O_TRUNC |
                  VFS_O_APPEND))
        return -1;

    if ((flags & VFS_O_TRUNC) &&
        !(flags & VFS_O_WRITE))
        return -1;

    if ((flags & VFS_O_APPEND) &&
        !(flags & VFS_O_WRITE))
        return -1;

    if (!vfs_exists(path)) {
        if (!(flags & VFS_O_CREATE))
            return -1;

        if (!vfs_create(path))
            return -1;
    }

    unsigned type = 0;
    unsigned size = 0;

    if (!vfs_stat(path, &type, &size) || type != VFS_FILE)
        return -1;

    if (flags & VFS_O_TRUNC) {
        if (!vfs_truncate(path, 0))
            return -1;

        size = 0;
    }

    int slot = -1;

    for (unsigned i = 0; i < VFS_MAX_FDS; ++i) {
        if (!file_descriptors[i].used) {
            slot = (int)i;
            break;
        }
    }

    if (slot < 0)
        return -1;

    VfsFileDescriptor *fd = &file_descriptors[slot];

    fd->used = 0;
    fd->flags = flags;
    fd->position = 0;
    fd->size = size;

    str_copy(fd->path, path, sizeof(fd->path));

    if (flags & VFS_O_APPEND)
        fd->position = size;

    fd->used = 1;

    return slot;
}

int vfs_read_fd(int fd, char *buffer, unsigned size) {
    if (!valid_fd(fd) || (!buffer && size))
        return -1;

    VfsFileDescriptor *file = &file_descriptors[fd];

    if (!(file->flags & VFS_O_READ))
        return -1;

    if (size == 0)
        return 0;

    unsigned actual_size = 0;

    if (!vfs_stat(file->path, 0, &actual_size))
        return -1;

    file->size = actual_size;

    if (file->position >= actual_size)
        return 0;

    unsigned remaining = actual_size - file->position;

    if (size > remaining)
        size = remaining;

    int result = vfs_read_at(
        file->path,
        file->position,
        buffer,
        size
    );

    if (result > 0)
        file->position += (unsigned)result;

    return result;
}

int vfs_write_fd(
    int fd,
    const char *buffer,
    unsigned size
) {
    if (!valid_fd(fd) || (!buffer && size))
        return -1;

    VfsFileDescriptor *file = &file_descriptors[fd];

    if (!(file->flags & VFS_O_WRITE))
        return -1;

    if (size == 0)
        return 0;

    if (file->flags & VFS_O_APPEND) {
        unsigned actual_size = 0;

        if (!vfs_stat(file->path, 0, &actual_size))
            return -1;

        file->position = actual_size;
        file->size = actual_size;
    }

    if (file->position > 0x7FFFFFFFu ||
        size > 0x7FFFFFFFu - file->position)
        return -1;

    int result = vfs_write_at(
        file->path,
        file->position,
        buffer,
        size
    );

    if (result < 0)
        return -1;

    file->position += (unsigned)result;

    if (file->position > file->size)
        file->size = file->position;

    return result;
}

int vfs_seek(int fd, int offset, unsigned origin) {
    if (!valid_fd(fd))
        return -1;

    VfsFileDescriptor *file = &file_descriptors[fd];

    unsigned base;

    if (origin == VFS_SEEK_SET) {
        base = 0;
    } else if (origin == VFS_SEEK_CUR) {
        base = file->position;
    } else if (origin == VFS_SEEK_END) {
        unsigned actual_size = 0;

        if (!vfs_stat(file->path, 0, &actual_size))
            return -1;

        file->size = actual_size;
        base = actual_size;
    } else {
        return -1;
    }

    unsigned position;

    if (offset < 0) {
        unsigned magnitude =
            (unsigned)(-(offset + 1)) + 1;

        if (magnitude > base)
            return -1;

        position = base - magnitude;
    } else {
        unsigned amount = (unsigned)offset;

        if (amount > 0x7FFFFFFFu - base)
            return -1;

        position = base + amount;
    }

    file->position = position;

    return (int)position;
}

int vfs_tell(int fd) {
    if (!valid_fd(fd))
        return -1;

    return (int)file_descriptors[fd].position;
}

int vfs_close(int fd) {
    if (!valid_fd(fd))
        return 0;

    VfsFileDescriptor *file = &file_descriptors[fd];

    file->used = 0;
    file->flags = 0;
    file->position = 0;
    file->size = 0;
    file->path[0] = '\0';

    return 1;
}


int vfs_stat(
    const char *path,
    unsigned *type,
    unsigned *size
) {
    ResolvedPath resolved;

    if (!resolve_path(path, &resolved))
        return 0;

    return resolved.mount->ops->stat(
        resolved.mount->context,
        resolved.relative_path,
        type,
        size
    );
}
