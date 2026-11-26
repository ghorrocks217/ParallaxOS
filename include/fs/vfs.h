
#ifndef FS_VFS_H
#define FS_VFS_H

#define VFS_NAME_MAX 32
#define VFS_PATH_MAX 128
#define VFS_FILE_MAX 512
#define VFS_MAX_MOUNTS 8

#define VFS_MAX_FDS 8

#define VFS_O_READ    0x01
#define VFS_O_WRITE   0x02
#define VFS_O_CREATE  0x04
#define VFS_O_TRUNC   0x08
#define VFS_O_APPEND  0x10

#define VFS_SEEK_SET  0
#define VFS_SEEK_CUR  1
#define VFS_SEEK_END  2

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

/* File descriptors */
int vfs_open(const char *path, unsigned flags);
int vfs_read_fd(int fd, char *buffer, unsigned size);
int vfs_write_fd(int fd, const char *buffer, unsigned size);

int vfs_seek(int fd, int offset, unsigned origin);
int vfs_tell(int fd);

int vfs_close(int fd);

#ifdef __cplusplus
}
#endif

#endif
