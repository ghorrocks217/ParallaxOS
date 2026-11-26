
#include "fs/ramfs.h"

#define RAMFS_MAX_NODES 64
#define RAMFS_INITIAL_CAPACITY 64

extern "C" void *kmalloc(unsigned size);
extern "C" void kfree(void *pointer);

struct RamNode {
    unsigned used;
    unsigned type;
    unsigned size;
    unsigned capacity;

    char path[VFS_PATH_MAX];
    char *data;
};

static RamNode nodes[RAMFS_MAX_NODES];

static unsigned str_length(const char *s) {
    unsigned n = 0;

    if (!s)
        return 0;

    while (s[n])
        ++n;

    return n;
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
    char *dst,
    const char *src,
    unsigned capacity
) {
    if (!dst || !capacity)
        return;

    unsigned i = 0;

    while (src && src[i] && i < capacity - 1) {
        dst[i] = src[i];
        ++i;
    }

    dst[i] = '\0';
}

static int path_valid(const char *path) {
    if (!path || path[0] != '/')
        return 0;

    unsigned len = str_length(path);

    if (!len || len >= VFS_PATH_MAX)
        return 0;

    if (len > 1 && path[len - 1] == '/')
        return 0;

    for (unsigned i = 0; i < len; ++i) {
        if (path[i] == '/' && path[i + 1] == '/')
            return 0;
    }

    return 1;
}

static int find_node(const char *path) {
    for (unsigned i = 0; i < RAMFS_MAX_NODES; ++i) {
        if (nodes[i].used &&
            str_equal(nodes[i].path, path))
            return (int)i;
    }

    return -1;
}

static int find_free_node(void) {
    for (unsigned i = 0; i < RAMFS_MAX_NODES; ++i) {
        if (!nodes[i].used)
            return (int)i;
    }

    return -1;
}

static int parent_exists(const char *path) {
    if (str_equal(path, "/"))
        return 1;

    char parent[VFS_PATH_MAX];
    str_copy(parent, path, sizeof(parent));

    unsigned len = str_length(parent);

    while (len > 1 && parent[len - 1] != '/')
        --len;

    if (len > 1)
        parent[len - 1] = '\0';
    else
        parent[1] = '\0';

    int index = find_node(parent);

    return index >= 0 &&
           nodes[index].type == VFS_DIRECTORY;
}

static int is_child_of(
    const char *parent,
    const char *child
) {
    if (!parent || !child)
        return 0;

    unsigned len = str_length(parent);

    if (!len)
        return 0;

    if (!str_equal(parent, "/")) {
        for (unsigned i = 0; i < len; ++i) {
            if (parent[i] != child[i])
                return 0;
        }

        return child[len] == '/';
    }

    return child[0] == '/' && child[1] != '\0';
}

/*
 * Ensure a file has enough allocated storage.
 *
 * The old allocation is retained if allocation fails,
 * so callers never lose existing file contents.
 */
static int ensure_capacity(
    RamNode *node,
    unsigned required
) {
    if (!node)
        return 0;

    if (required <= node->capacity)
        return 1;

    unsigned new_capacity =
        node->capacity
            ? node->capacity
            : RAMFS_INITIAL_CAPACITY;

    while (new_capacity < required) {
        if (new_capacity > 0x7FFFFFFFu / 2) {
            new_capacity = required;
            break;
        }

        new_capacity *= 2;
    }

    if (new_capacity < required)
        return 0;

    char *new_data =
        (char *)kmalloc(new_capacity);

    if (!new_data)
        return 0;

    for (unsigned i = 0; i < node->size; ++i)
        new_data[i] = node->data[i];

    for (unsigned i = node->size; i < new_capacity; ++i)
        new_data[i] = 0;

    if (node->data)
        kfree(node->data);

    node->data = new_data;
    node->capacity = new_capacity;

    return 1;
}

void ramfs_init(void) {
    for (unsigned i = 0; i < RAMFS_MAX_NODES; ++i) {
        if (nodes[i].data)
            kfree(nodes[i].data);

        nodes[i].used = 0;
        nodes[i].type = 0;
        nodes[i].size = 0;
        nodes[i].capacity = 0;
        nodes[i].data = 0;
        nodes[i].path[0] = '\0';
    }

    nodes[0].used = 1;
    nodes[0].type = VFS_DIRECTORY;
    nodes[0].size = 0;
    nodes[0].capacity = 0;
    nodes[0].data = 0;

    str_copy(nodes[0].path, "/", VFS_PATH_MAX);
}

int ramfs_create(const char *path, unsigned type) {
    if (!path_valid(path))
        return 0;

    if (type != VFS_FILE &&
        type != VFS_DIRECTORY)
        return 0;

    if (find_node(path) >= 0)
        return 0;

    if (!parent_exists(path))
        return 0;

    int index = find_free_node();

    if (index < 0)
        return 0;

    RamNode *node = &nodes[index];

    node->used = 1;
    node->type = type;
    node->size = 0;
    node->capacity = 0;
    node->data = 0;

    str_copy(node->path, path, sizeof(node->path));

    return 1;
}

int ramfs_remove(const char *path) {
    if (!path_valid(path) ||
        str_equal(path, "/"))
        return 0;

    int index = find_node(path);

    if (index < 0)
        return 0;

    RamNode *node = &nodes[index];

    if (node->type == VFS_DIRECTORY) {
        for (unsigned i = 0; i < RAMFS_MAX_NODES; ++i) {
            if (nodes[i].used &&
                is_child_of(path, nodes[i].path))
                return 0;
        }
    }

    if (node->data)
        kfree(node->data);

    node->data = 0;
    node->capacity = 0;
    node->size = 0;
    node->type = 0;
    node->used = 0;
    node->path[0] = '\0';

    return 1;
}

/*
 * Replace the complete contents of a file.
 */
int ramfs_write(
    const char *path,
    const char *data,
    unsigned size
) {
    if (!path || (!data && size))
        return 0;

    int index = find_node(path);

    if (index < 0 ||
        nodes[index].type != VFS_FILE)
        return 0;

    RamNode *node = &nodes[index];

    if (size && !ensure_capacity(node, size))
        return 0;

    for (unsigned i = 0; i < size; ++i)
        node->data[i] = data[i];

    node->size = size;

    return 1;
}

/*
 * Write bytes at an arbitrary file offset.
 *
 * Gaps between the previous EOF and the write offset
 * are filled with zeroes.
 */
int ramfs_write_at(
    const char *path,
    unsigned offset,
    const char *buffer,
    unsigned size
) {
    if (!path || (!buffer && size))
        return -1;

    if (size == 0)
        return 0;

    if (offset > 0x7FFFFFFFu ||
        size > 0x7FFFFFFFu - offset)
        return -1;

    int index = find_node(path);

    if (index < 0 ||
        nodes[index].type != VFS_FILE)
        return -1;

    RamNode *node = &nodes[index];
    unsigned end = offset + size;

    if (!ensure_capacity(node, end))
        return -1;

    if (offset > node->size) {
        for (unsigned i = node->size; i < offset; ++i)
            node->data[i] = 0;
    }

    for (unsigned i = 0; i < size; ++i)
        node->data[offset + i] = buffer[i];

    if (end > node->size)
        node->size = end;

    return (int)size;
}

/*
 * Resize a file.
 *
 * Extending a file zero-fills the newly visible bytes.
 * Shrinking changes its logical size while retaining
 * allocated capacity for possible future reuse.
 */
int ramfs_truncate(
    const char *path,
    unsigned size
) {
    if (!path || size > 0x7FFFFFFFu)
        return 0;

    int index = find_node(path);

    if (index < 0 ||
        nodes[index].type != VFS_FILE)
        return 0;

    RamNode *node = &nodes[index];

    if (size > node->size) {
        if (!ensure_capacity(node, size))
            return 0;

        for (unsigned i = node->size; i < size; ++i)
            node->data[i] = 0;
    }

    node->size = size;

    return 1;
}

int ramfs_read(
    const char *path,
    char *buffer,
    unsigned capacity
) {
    if (!path || (!buffer && capacity))
        return -1;

    int index = find_node(path);

    if (index < 0 ||
        nodes[index].type != VFS_FILE)
        return -1;

    RamNode *node = &nodes[index];

    if (node->size > capacity)
        return -1;

    for (unsigned i = 0; i < node->size; ++i)
        buffer[i] = node->data[i];

    return (int)node->size;
}

int ramfs_read_at(
    const char *path,
    unsigned offset,
    char *buffer,
    unsigned size
) {
    if (!path || (!buffer && size))
        return -1;

    int index = find_node(path);

    if (index < 0 ||
        nodes[index].type != VFS_FILE)
        return -1;

    RamNode *node = &nodes[index];

    if (offset >= node->size || size == 0)
        return 0;

    unsigned remaining = node->size - offset;

    if (size > remaining)
        size = remaining;

    for (unsigned i = 0; i < size; ++i)
        buffer[i] = node->data[offset + i];

    return (int)size;
}

int ramfs_list(
    const char *path,
    struct vfs_dirent *entries,
    unsigned capacity
) {
    int directory = find_node(path);

    if (directory < 0 ||
        nodes[directory].type != VFS_DIRECTORY)
        return -1;

    unsigned count = 0;
    unsigned parent_len = str_length(path);

    for (unsigned i = 0; i < RAMFS_MAX_NODES; ++i) {
        if (!nodes[i].used ||
            (int)i == directory)
            continue;

        if (!is_child_of(path, nodes[i].path))
            continue;

        const char *name;

        if (str_equal(path, "/"))
            name = nodes[i].path + 1;
        else
            name = nodes[i].path + parent_len + 1;

        int nested = 0;

        for (unsigned j = 0; name[j]; ++j) {
            if (name[j] == '/') {
                nested = 1;
                break;
            }
        }

        if (nested)
            continue;

        if (count < capacity && entries) {
            str_copy(
                entries[count].name,
                name,
                VFS_NAME_MAX
            );

            entries[count].type = nodes[i].type;
            entries[count].size = nodes[i].size;
        }

        ++count;
    }

    return (int)count;
}

int ramfs_exists(const char *path) {
    return find_node(path) >= 0;
}

int ramfs_is_directory(const char *path) {
    int index = find_node(path);

    return index >= 0 &&
           nodes[index].type == VFS_DIRECTORY;
}

int ramfs_stat(
    const char *path,
    unsigned *type,
    unsigned *size
) {
    if (!path)
        return 0;

    int index = find_node(path);

    if (index < 0)
        return 0;

    if (type)
        *type = nodes[index].type;

    if (size)
        *size = nodes[index].size;

    return 1;
}
