#ifndef FS_VFS_H
#define FS_VFS_H

#define VFS_NAME_MAX 32
#define VFS_PATH_MAX 128
#define VFS_FILE_MAX 512

enum vfs_node_type {
    VFS_FILE = 1,
    VFS_DIRECTORY = 2
};

struct vfs_dirent {
    char name[VFS_NAME_MAX];
    unsigned type;
    unsigned size;
};

#ifdef __cplusplus
extern "C" {
#endif

void vfs_init(void);

int vfs_create(const char *path);
int vfs_mkdir(const char *path);
int vfs_remove(const char *path);

int vfs_write(const char *path, const char *data, unsigned size);
int vfs_read(const char *path, char *buffer, unsigned capacity);

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
