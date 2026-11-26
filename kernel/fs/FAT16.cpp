#include "fs/fat16.h"
#include "storage/block_device.h"

#define FAT16_SECTOR_SIZE 512
#define FAT16_ENTRY_SIZE  32

#define FAT16_ATTR_DIRECTORY 0x10
#define FAT16_ATTR_VOLUME    0x08
#define FAT16_ATTR_LONG_NAME 0x0F

#define FAT16_CLUSTER_EOC    0xFFF8
#define FAT16_CLUSTER_BAD    0xFFF7
#define FAT16_CLUSTER_MIN    2

struct Fat16State {
    int mounted;
    block_device device;

    unsigned total_sectors;
    unsigned sectors_per_cluster;
    unsigned reserved_sectors;
    unsigned fat_count;
    unsigned sectors_per_fat;
    unsigned root_entries;

    unsigned fat_start;
    unsigned root_start;
    unsigned root_sectors;
    unsigned data_start;
    unsigned cluster_count;
};

static Fat16State fs;
static unsigned char sector_buffer[FAT16_SECTOR_SIZE];

static unsigned read16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned read32(const unsigned char *p) {
    return read16(p) | (read16(p + 2) << 16);
}

static unsigned min_unsigned(unsigned a, unsigned b) {
    return a < b ? a : b;
}

static int power_of_two(unsigned value) {
    return value && !(value & (value - 1));
}

static unsigned string_length(const char *s) {
    unsigned n = 0;

    if (!s)
        return 0;

    while (s[n])
        ++n;

    return n;
}

static int char_equal_ci(char a, char b) {
    if (a >= 'a' && a <= 'z')
        a -= 32;

    if (b >= 'a' && b <= 'z')
        b -= 32;

    return a == b;
}

static int string_equal_ci(const char *a, const char *b) {
    if (!a || !b)
        return 0;

    while (*a && *b) {
        if (!char_equal_ci(*a, *b))
            return 0;

        ++a;
        ++b;
    }

    return *a == *b;
}

static int read_sector(unsigned lba, void *buffer) {
    if (!fs.mounted)
        return 0;

    if (lba >= fs.device.sector_count)
        return 0;

    return fs.device.read(fs.device.id, lba, 1, buffer);
}

static int write_sector(unsigned lba, const void *buffer) {
    if (!fs.mounted || !buffer)
        return 0;

    if (lba >= fs.device.sector_count)
        return 0;

    return fs.device.write(fs.device.id, lba, 1, buffer);
}

static unsigned cluster_to_sector(unsigned cluster) {
    return fs.data_start +
        (cluster - FAT16_CLUSTER_MIN) * fs.sectors_per_cluster;
}

static int valid_cluster(unsigned cluster) {
    return cluster >= FAT16_CLUSTER_MIN &&
           cluster < fs.cluster_count + FAT16_CLUSTER_MIN;
}

static int next_cluster(unsigned cluster, unsigned *next) {
    if (!next || !valid_cluster(cluster))
        return 0;

    unsigned fat_offset = cluster * 2;
    unsigned fat_sector = fs.fat_start +
                          fat_offset / FAT16_SECTOR_SIZE;
    unsigned entry_offset = fat_offset % FAT16_SECTOR_SIZE;

    if (!read_sector(fat_sector, sector_buffer))
        return 0;

    unsigned value = read16(sector_buffer + entry_offset);

    if (value >= FAT16_CLUSTER_EOC)
        return 2;

    if (value == FAT16_CLUSTER_BAD ||
        value < FAT16_CLUSTER_MIN ||
        !valid_cluster(value))
        return 0;

    *next = value;
    return 1;
}


static int read_fat_entry(unsigned cluster, unsigned *value) {
    if (!value || !valid_cluster(cluster))
        return 0;

    unsigned offset = cluster * 2;
    unsigned lba = fs.fat_start + offset / FAT16_SECTOR_SIZE;
    unsigned entry_offset = offset % FAT16_SECTOR_SIZE;

    if (!read_sector(lba, sector_buffer))
        return 0;

    *value = read16(sector_buffer + entry_offset);
    return 1;
}

static int write_fat_entry(unsigned cluster, unsigned value) {
    if (!valid_cluster(cluster))
        return 0;

    unsigned offset = cluster * 2;
    unsigned relative_sector = offset / FAT16_SECTOR_SIZE;
    unsigned entry_offset = offset % FAT16_SECTOR_SIZE;

    for (unsigned copy = 0; copy < fs.fat_count; ++copy) {
        unsigned lba =
            fs.fat_start +
            copy * fs.sectors_per_fat +
            relative_sector;

        if (!read_sector(lba, sector_buffer))
            return 0;

        sector_buffer[entry_offset] =
            (unsigned char)value;

        sector_buffer[entry_offset + 1] =
            (unsigned char)(value >> 8);

        if (!write_sector(lba, sector_buffer))
            return 0;
    }

    return 1;
}

static int zero_cluster(unsigned cluster) {
    if (!valid_cluster(cluster))
        return 0;

    unsigned first_sector = cluster_to_sector(cluster);

    for (unsigned i = 0; i < FAT16_SECTOR_SIZE; ++i)
        sector_buffer[i] = 0;

    for (unsigned i = 0; i < fs.sectors_per_cluster; ++i) {
        if (!write_sector(first_sector + i, sector_buffer))
            return 0;
    }

    return 1;
}


static int allocate_cluster(unsigned *result) {
    if (!fs.mounted || !result)
        return 0;

    for (unsigned cluster = FAT16_CLUSTER_MIN;
         cluster < fs.cluster_count + FAT16_CLUSTER_MIN;
         ++cluster) {

        unsigned value;

        if (!read_fat_entry(cluster, &value))
            return 0;

        /* A zero FAT entry represents an unused cluster. */
        if (value != 0)
            continue;

        /* Clear old contents before making the cluster available. */
        if (!zero_cluster(cluster))
            return 0;

        /* Mark this cluster as the end of a new chain. */
        if (!write_fat_entry(cluster, 0xFFFF)) {
            /* Best-effort rollback if a FAT copy failed. */
            write_fat_entry(cluster, 0);
            return 0;
        }

        *result = cluster;
        return 1;
    }

    /* No free clusters remain. */
    return 0;
}

/*
 * Convert a FAT 8.3 directory entry into a printable name.
 * Long filename entries are deliberately skipped for now.
 */
static void format_name(
    const unsigned char *entry,
    char *output,
    unsigned capacity
) {
    unsigned pos = 0;

    for (unsigned i = 0; i < 8 && entry[i] != ' '; ++i) {
        if (pos + 1 >= capacity)
            break;

        output[pos++] = (char)entry[i];
    }

    int has_extension = entry[8] != ' ';

    if (has_extension && pos + 1 < capacity)
        output[pos++] = '.';

    if (has_extension) {
        for (unsigned i = 8; i < 11 && entry[i] != ' '; ++i) {
            if (pos + 1 >= capacity)
                break;

            output[pos++] = (char)entry[i];
        }
    }

    output[pos] = '\0';
}

/*
 * Convert a requested name into the on-disk 8.3 representation.
 * Names longer than 8.3 or containing unsupported characters fail.
 */
static int make_short_name(
    const char *input,
    unsigned char output[11]
) {
    if (!input || !*input)
        return 0;

    for (unsigned i = 0; i < 11; ++i)
        output[i] = ' ';

    unsigned base = 0;
    unsigned extension = 0;
    int after_dot = 0;

    for (unsigned i = 0; input[i]; ++i) {
        char c = input[i];

        if (c == '.') {
            if (after_dot || base == 0)
                return 0;

            after_dot = 1;
            continue;
        }

        if (c == '/' || c == '\\' || c == ' ')
            return 0;

        if (c >= 'a' && c <= 'z')
            c -= 32;

        if (!((c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '$' ||
              c == '~' || c == '!'))
            return 0;

        if (!after_dot) {
            if (base >= 8)
                return 0;

            output[base++] = (unsigned char)c;
        } else {
            if (extension >= 3)
                return 0;

            output[8 + extension++] = (unsigned char)c;
        }
    }

    return base != 0;
}

/*
 * Read a directory entry by index.
 * root_cluster == 0 means the fixed-size FAT16 root directory.
 */
static int read_directory_entry(
    unsigned root_cluster,
    unsigned index,
    unsigned char output[FAT16_ENTRY_SIZE],
    unsigned *entry_lba,
    unsigned *entry_offset
) {
    if (root_cluster == 0) {
        if (index >= fs.root_entries)
            return 0;

        unsigned lba = fs.root_start +
                       (index * FAT16_ENTRY_SIZE) /
                       FAT16_SECTOR_SIZE;

        unsigned offset =
            (index * FAT16_ENTRY_SIZE) % FAT16_SECTOR_SIZE;

        if (entry_lba)
            *entry_lba = lba;

        if (entry_offset)
            *entry_offset = offset;

        if (!read_sector(lba, sector_buffer))
            return -1;

        for (unsigned i = 0; i < FAT16_ENTRY_SIZE; ++i)
            output[i] = sector_buffer[offset + i];

        return 1;
    }

    unsigned entries_per_cluster =
        fs.sectors_per_cluster *
        (FAT16_SECTOR_SIZE / FAT16_ENTRY_SIZE);

    unsigned cluster_steps = index / entries_per_cluster;
    unsigned entry_in_cluster = index % entries_per_cluster;

    unsigned cluster = root_cluster;

    for (unsigned i = 0; i < cluster_steps; ++i) {
        unsigned next;
        int result = next_cluster(cluster, &next);

        if (result != 1)
            return 0;

        cluster = next;
    }

    unsigned sector_in_cluster =
        entry_in_cluster /
        (FAT16_SECTOR_SIZE / FAT16_ENTRY_SIZE);

    unsigned entry_in_sector =
        entry_in_cluster %
        (FAT16_SECTOR_SIZE / FAT16_ENTRY_SIZE);

    unsigned lba = cluster_to_sector(cluster) +
                   sector_in_cluster;

    if (!read_sector(lba, sector_buffer))
        return -1;

    unsigned offset = entry_in_sector * FAT16_ENTRY_SIZE;

    if (entry_lba)
        *entry_lba = lba;

    if (entry_offset)
        *entry_offset = offset;

    for (unsigned i = 0; i < FAT16_ENTRY_SIZE; ++i)
        output[i] = sector_buffer[offset + i];

    return 1;
}

static int find_entry(
    unsigned directory_cluster,
    const unsigned char name[11],
    unsigned char output[FAT16_ENTRY_SIZE],
    unsigned *entry_lba,
    unsigned *entry_offset
) {
    unsigned max_entries;

    if (directory_cluster == 0) {
        max_entries = fs.root_entries;
    } else {
        max_entries = fs.cluster_count *
            fs.sectors_per_cluster *
            (FAT16_SECTOR_SIZE / FAT16_ENTRY_SIZE);
    }

    for (unsigned i = 0; i < max_entries; ++i) {
        unsigned char entry[FAT16_ENTRY_SIZE];

        unsigned current_lba;
        unsigned current_offset;

        int result = read_directory_entry(
            directory_cluster,
            i,
            entry,
            &current_lba,
            &current_offset
        );

        if (result <= 0)
            return 0;

        if (entry[0] == 0x00)
            return 0;

        if (entry[0] == 0xE5)
            continue;

        if (entry[11] == FAT16_ATTR_LONG_NAME)
            continue;

        if (entry[11] & FAT16_ATTR_VOLUME)
            continue;

        int matches = 1;

        for (unsigned j = 0; j < 11; ++j) {
            if (entry[j] != name[j]) {
                matches = 0;
                break;
            }
        }

        if (matches) {
            for (unsigned j = 0; j < FAT16_ENTRY_SIZE; ++j)
                output[j] = entry[j];

            if (entry_lba)
                *entry_lba = current_lba;

            if (entry_offset)
                *entry_offset = current_offset;

            return 1;
        }
    }

    return 0;
}

/*
 * Resolve a directory path to its first cluster.
 * Cluster zero is reserved here as the fixed FAT16 root directory.
 */
static int resolve_directory(
    const char *path,
    unsigned *result_cluster
) {
    if (!path || !result_cluster || path[0] != '/')
        return 0;

    if (string_length(path) >= VFS_PATH_MAX)
        return 0;

    unsigned current = 0;
    unsigned pos = 1;

    while (path[pos]) {
        while (path[pos] == '/')
            ++pos;

        if (!path[pos])
            break;

        char component[13];
        unsigned length = 0;

        while (path[pos] && path[pos] != '/') {
            if (length >= 12)
                return 0;

            component[length++] = path[pos++];
        }

        component[length] = '\0';

        if (string_equal_ci(component, "."))
            continue;

        if (string_equal_ci(component, "..")) {
            /* Parent traversal is handled conservatively. */
            return 0;
        }

        unsigned char short_name[11];
        unsigned char entry[FAT16_ENTRY_SIZE];

        if (!make_short_name(component, short_name))
            return 0;

        if (!find_entry(current, short_name, entry, 0, 0))
            return 0;

        if (!(entry[11] & FAT16_ATTR_DIRECTORY))
            return 0;

        current = read16(entry + 26);

        if (!valid_cluster(current))
            return 0;
    }

    *result_cluster = current;
    return 1;
}

static int resolve_file(
    const char *path,
    unsigned *parent_cluster,
    unsigned char output[FAT16_ENTRY_SIZE],
    unsigned *entry_lba,
    unsigned *entry_offset
) {
    if (!path || path[0] != '/')
        return 0;

    unsigned length = string_length(path);

    if (length < 2 || length >= VFS_PATH_MAX || path[length - 1] == '/')
        return 0;

    unsigned split = length;

    while (split > 0 && path[split - 1] != '/')
        --split;

    if (split == 0 || split >= length)
        return 0;

    char parent_path[VFS_PATH_MAX];
    unsigned parent_length = split;

    if (parent_length == 1) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        for (unsigned i = 0; i < parent_length - 1; ++i)
            parent_path[i] = path[i];

        parent_path[parent_length - 1] = '\0';
    }

    char filename[13];
    unsigned filename_length = length - split;

    if (filename_length > 12)
        return 0;

    for (unsigned i = 0; i < filename_length; ++i)
        filename[i] = path[split + i];

    filename[filename_length] = '\0';

    unsigned char short_name[11];

    if (!make_short_name(filename, short_name))
        return 0;

    if (!resolve_directory(parent_path, parent_cluster))
        return 0;

    return find_entry(
        *parent_cluster,
        short_name,
        output,
        entry_lba,
        entry_offset
    );
}

extern "C" int fat16_mount(unsigned device_index) {
    fs.mounted = 0;

    if (!block_device_get(device_index, &fs.device))
        return 0;

    if (fs.device.sector_count == 0)
        return 0;

    unsigned char boot[512];

    if (!fs.device.read(
            fs.device.id, 0, 1, boot))
        return 0;

    if (boot[510] != 0x55 || boot[511] != 0xAA)
        return 0;

    unsigned bytes_per_sector = read16(boot + 11);
    unsigned sectors_per_cluster = boot[13];
    unsigned reserved = read16(boot + 14);
    unsigned fats = boot[16];
    unsigned root_entries = read16(boot + 17);
    unsigned total16 = read16(boot + 19);
    unsigned fat_size = read16(boot + 22);
    unsigned total32 = read32(boot + 32);

    unsigned total = total16 ? total16 : total32;

    if (bytes_per_sector != FAT16_SECTOR_SIZE ||
        !power_of_two(sectors_per_cluster) ||
        sectors_per_cluster > 128 ||
        reserved == 0 ||
        fats == 0 ||
        fats > 2 ||
        root_entries == 0 ||
        fat_size == 0 ||
        total == 0 ||
        total > fs.device.sector_count)
        return 0;

    unsigned root_sectors =
        (root_entries * FAT16_ENTRY_SIZE +
         FAT16_SECTOR_SIZE - 1) /
        FAT16_SECTOR_SIZE;

    unsigned fat_start = reserved;
    unsigned root_start = reserved + fats * fat_size;
    unsigned data_start = root_start + root_sectors;

    if (data_start >= total)
        return 0;

    unsigned data_sectors = total - data_start;
    unsigned cluster_count =
        data_sectors / sectors_per_cluster;

    /* FAT16 has 4085 through 65524 data clusters. */
    if (cluster_count < 4085 || cluster_count > 65524)
        return 0;

    if ((cluster_count + 2) * 2 >
        fat_size * FAT16_SECTOR_SIZE)
        return 0;

    fs.device.id = device_index;
    fs.total_sectors = total;
    fs.sectors_per_cluster = sectors_per_cluster;
    fs.reserved_sectors = reserved;
    fs.fat_count = fats;
    fs.sectors_per_fat = fat_size;
    fs.root_entries = root_entries;
    fs.fat_start = fat_start;
    fs.root_start = root_start;
    fs.root_sectors = root_sectors;
    fs.data_start = data_start;
    fs.cluster_count = cluster_count;
    fs.mounted = 1;

    return 1;
}

extern "C" int fat16_is_mounted(void) {
    return fs.mounted;
}

extern "C" int fat16_get_info(struct fat16_info *info) {
    if (!fs.mounted || !info)
        return 0;

    info->device = fs.device.id;
    info->total_sectors = fs.total_sectors;
    info->sectors_per_cluster = fs.sectors_per_cluster;
    info->reserved_sectors = fs.reserved_sectors;
    info->fat_count = fs.fat_count;
    info->sectors_per_fat = fs.sectors_per_fat;
    info->root_entries = fs.root_entries;
    info->root_directory_sectors = fs.root_sectors;
    info->data_start_sector = fs.data_start;
    info->cluster_count = fs.cluster_count;

    return 1;
}

extern "C" int fat16_list(
    const char *path,
    struct vfs_dirent *entries,
    unsigned capacity
) {
    if (!fs.mounted || !path || path[0] != '/')
        return -1;

    unsigned directory_cluster;

    if (!resolve_directory(path, &directory_cluster))
        return -1;

    unsigned max_entries;

    if (directory_cluster == 0) {
        max_entries = fs.root_entries;
    } else {
        max_entries = fs.cluster_count *
            fs.sectors_per_cluster *
            (FAT16_SECTOR_SIZE / FAT16_ENTRY_SIZE);
    }

    unsigned count = 0;

    for (unsigned i = 0; i < max_entries; ++i) {
        unsigned char entry[FAT16_ENTRY_SIZE];

        int result = read_directory_entry(
            directory_cluster,
            i,
            entry,
            0,
            0
        );

        if (result <= 0)
            break;

        if (entry[0] == 0x00)
            break;

        if (entry[0] == 0xE5 ||
            entry[11] == FAT16_ATTR_LONG_NAME ||
            (entry[11] & FAT16_ATTR_VOLUME))
            continue;

        if (count < capacity && entries) {
            format_name(
                entry,
                entries[count].name,
                VFS_NAME_MAX
            );

            entries[count].type =
                (entry[11] & FAT16_ATTR_DIRECTORY)
                ? VFS_DIRECTORY : VFS_FILE;

            entries[count].size = read32(entry + 28);
        }

        ++count;
    }

    return (int)count;
}

extern "C" int fat16_read(
    const char *path,
    char *buffer,
    unsigned capacity
) {
    if (!fs.mounted || !path || !buffer)
        return -1;

    unsigned parent_cluster;
    unsigned char entry[FAT16_ENTRY_SIZE];

    if (!resolve_file(path, &parent_cluster, entry, 0, 0))
        return -1;

    if (entry[11] & FAT16_ATTR_DIRECTORY)
        return -1;

    unsigned file_size = read32(entry + 28);

    if (file_size > capacity)
        return -1;

    if (file_size == 0)
        return 0;

    unsigned cluster =
        ((unsigned)read16(entry + 20) << 16) |
        read16(entry + 26);

    if (!valid_cluster(cluster))
        return -1;

    unsigned copied = 0;
    unsigned steps = 0;

    while (copied < file_size &&
           steps++ < fs.cluster_count) {
        unsigned first_sector = cluster_to_sector(cluster);

        for (unsigned s = 0;
             s < fs.sectors_per_cluster && copied < file_size;
             ++s) {
            if (!read_sector(first_sector + s, sector_buffer))
                return -1;

            unsigned amount = min_unsigned(
                FAT16_SECTOR_SIZE,
                file_size - copied
            );

            for (unsigned i = 0; i < amount; ++i)
                buffer[copied + i] = (char)sector_buffer[i];

            copied += amount;
        }

        if (copied >= file_size)
            break;

        unsigned next;
        int result = next_cluster(cluster, &next);

        if (result != 1)
            return -1;

        cluster = next;
    }

    return copied == file_size ? (int)copied : -1;
}

extern "C" int fat16_stat(
    const char *path,
    unsigned *type,
    unsigned *size
) {
    if (!fs.mounted || !path || path[0] != '/')
        return 0;

    /* The FAT16 root is always a directory. */
    if (string_equal_ci(path, "/")) {
        if (type)
            *type = VFS_DIRECTORY;

        if (size)
            *size = 0;

        return 1;
    }

    unsigned directory_cluster;

    if (resolve_directory(path, &directory_cluster)) {
        if (type)
            *type = VFS_DIRECTORY;

        if (size)
            *size = 0;

        return 1;
    }

    unsigned parent_cluster;
    unsigned char entry[FAT16_ENTRY_SIZE];

    if (!resolve_file(path, &parent_cluster, entry, 0, 0))
        return 0;

    if (type) {
        *type = (entry[11] & FAT16_ATTR_DIRECTORY)
            ? VFS_DIRECTORY : VFS_FILE;
    }

    if (size)
        *size = read32(entry + 28);

    return 1;
}

extern "C" int fat16_write(
    const char *path,
    const char *data,
    unsigned size
) {
    if (!fs.mounted || !path || (!data && size))
        return 0;

    unsigned parent_cluster;
    unsigned entry_lba;
    unsigned entry_offset;

    unsigned char entry[FAT16_ENTRY_SIZE];

    if (!resolve_file(
            path,
            &parent_cluster,
            entry,
            &entry_lba,
            &entry_offset))
        return 0;

    /* Do not write directories or read-only files. */
    if (entry[11] & (0x01 | FAT16_ATTR_DIRECTORY))
        return 0;

    unsigned first_cluster =
        ((unsigned)read16(entry + 20) << 16) |
        read16(entry + 26);

    unsigned cluster_bytes =
        fs.sectors_per_cluster * FAT16_SECTOR_SIZE;

    unsigned needed_clusters =
        size / cluster_bytes +
        (size % cluster_bytes != 0);

    if (needed_clusters > fs.cluster_count)
        return 0;
    /*
     * This first implementation only overwrites files using
     * their existing cluster chain. It does not allocate clusters.
     */

    if (needed_clusters && !valid_cluster(first_cluster))
        return 0;

    /* Verify that the existing chain is large enough. */
    unsigned cluster = first_cluster;
    unsigned available_clusters = 0;

    if (needed_clusters) {
        if (!valid_cluster(cluster))
            return 0;

        available_clusters = 1;

        while (available_clusters < needed_clusters) {
            unsigned next;

            if (next_cluster(cluster, &next) != 1)
                return 0;

            cluster = next;
            ++available_clusters;
        }
    }

    /*
     * Write file contents, one sector at a time.
     * The final sector is zero-padded.
     */
    cluster = first_cluster;
    unsigned written = 0;

    for (unsigned c = 0; c < needed_clusters; ++c) {
        unsigned first_sector = cluster_to_sector(cluster);

        for (unsigned s = 0;
             s < fs.sectors_per_cluster && written < size;
             ++s) {

            unsigned amount = min_unsigned(
                FAT16_SECTOR_SIZE,
                size - written
            );

            for (unsigned i = 0; i < FAT16_SECTOR_SIZE; ++i) {
                if (i < amount)
                    sector_buffer[i] = (unsigned char)data[written + i];
                else
                    sector_buffer[i] = 0;
            }

            if (!write_sector(first_sector + s, sector_buffer))
                return 0;

            written += amount;
        }

        if (written >= size)
            break;

        unsigned next;

        if (next_cluster(cluster, &next) != 1)
            return 0;

        cluster = next;
    }

    if (written != size)
        return 0;

    /* Update the file size in its directory entry. */
    if (!read_sector(entry_lba, sector_buffer))
        return 0;

    sector_buffer[entry_offset + 28] =
        (unsigned char)(size);

    sector_buffer[entry_offset + 29] =
        (unsigned char)(size >> 8);

    sector_buffer[entry_offset + 30] =
        (unsigned char)(size >> 16);

    sector_buffer[entry_offset + 31] =
        (unsigned char)(size >> 24);

    if (!write_sector(entry_lba, sector_buffer))
        return 0;

    return 1;
}
