
#ifndef FS_VFS_H
#define FS_VFS_H

#define VFS_NAME_MAX 32
#define VFS_PATH_MAX 128
#define VFS_FILE_MAX 512
#define VFS_MAX_MOUNTS 8

enum vfs_node_type {
    VFS_FILE = 1,
    VFS_DIRECTORY = 2
};

struct vfs_dirent {
    char name[VFS_NAME_MAX];
    unsigned type;
    unsigned size;
};

/*
 * Filesystem driver interface.
 *
 * Paths passed to these functions are relative to the
 * filesystem's mount point, beginning with '/'.
 */
struct vfs_fs_ops {
    int (*create)(void *context, const char *path);
    int (*mkdir)(void *context, const char *path);
    int (*remove)(void *context, const char *path);

    int (*write)(
        void *context,
        const char *path,
        const char *data,
        unsigned size
    );

    int (*read)(
        void *context,
        const char *path,
        char *buffer,
        unsigned capacity
    );

    int (*list)(
        void *context,
        const char *path,
        struct vfs_dirent *entries,
        unsigned capacity
    );

    int (*stat)(
        void *context,
        const char *path,
        unsigned *type,
        unsigned *size
    );
};

#ifdef __cplusplus
extern "C" {
#endif

void vfs_init(void);

/* Mount management */
int vfs_mount(
    const char *mount_path,
    const struct vfs_fs_ops *ops,
    void *context
);

int vfs_unmount(const char *mount_path);
unsigned vfs_mount_count(void);

/* Filesystem operations */
int vfs_create(const char *path);
int vfs_mkdir(const char *path);
int vfs_remove(const char *path);

int vfs_write(
    const char *path,
    const char *data,
    unsigned size
);

int vfs_read(
    const char *path,
    char *buffer,
    unsigned capacity
);

int vfs_list(
    const char *path,
    struct vfs_dirent *entries,
    unsigned capacity
);

int vfs_exists(const char *path);
int vfs_is_directory(const char *path);

#ifdef __cplusplus
}
#endif

#endif
