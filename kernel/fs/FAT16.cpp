#include "fs/fat16.h"
#include "storage/block_device.h"

#define FAT16_SECTOR_SIZE 512
#define FAT16_ENTRY_SIZE  32

#define FAT16_ATTR_DIRECTORY 0x10
#define FAT16_ATTR_VOLUME    0x08
#define FAT16_ATTR_LONG_NAME 0x0F
#define FAT16_ATTR_READ_ONLY 0x01

#define FAT16_CLUSTER_EOC    0xFFF8
#define FAT16_CLUSTER_BAD    0xFFF7
#define FAT16_CLUSTER_MIN    2

struct Fat16State {
    int mounted;
    block_device device;

    unsigned partition_start;
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

#define FAT16_CACHE_SIZE 4

struct Fat16CacheEntry {
    int valid;
    unsigned lba;
    unsigned age;
    unsigned char data[FAT16_SECTOR_SIZE];
};

static Fat16CacheEntry fat_cache[FAT16_CACHE_SIZE];
static unsigned fat_cache_age = 0;

static int read_sector(unsigned lba, void *buffer);
static int write_sector(unsigned lba, const void *buffer);

static void fat_cache_invalidate(void) {
    for (unsigned i = 0; i < FAT16_CACHE_SIZE; ++i) {
        fat_cache[i].valid = 0;
        fat_cache[i].age = 0;
    }

    fat_cache_age = 0;
}

static int fat_cache_read(unsigned lba, unsigned char *buffer) {
    if (!buffer)
        return 0;

    /* Check for a cached sector. */
    for (unsigned i = 0; i < FAT16_CACHE_SIZE; ++i) {
        if (fat_cache[i].valid && fat_cache[i].lba == lba) {
            for (unsigned j = 0; j < FAT16_SECTOR_SIZE; ++j)
                buffer[j] = fat_cache[i].data[j];

            fat_cache[i].age = ++fat_cache_age;
            return 1;
        }
    }

    /* Find an invalid entry or the least recently used entry. */
    unsigned slot = 0;

    for (unsigned i = 0; i < FAT16_CACHE_SIZE; ++i) {
        if (!fat_cache[i].valid) {
            slot = i;
            break;
        }

        if (fat_cache[i].age < fat_cache[slot].age)
            slot = i;
    }

    if (!read_sector(lba, fat_cache[slot].data))
        return 0;

    fat_cache[slot].valid = 1;
    fat_cache[slot].lba = lba;
    fat_cache[slot].age = ++fat_cache_age;

    for (unsigned j = 0; j < FAT16_SECTOR_SIZE; ++j)
        buffer[j] = fat_cache[slot].data[j];

    return 1;
}

static void fat_cache_invalidate_sector(unsigned lba) {
    for (unsigned i = 0; i < FAT16_CACHE_SIZE; ++i) {
        if (fat_cache[i].valid && fat_cache[i].lba == lba)
            fat_cache[i].valid = 0;
    }
}

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
    if (!fs.mounted || !buffer)
        return 0;

    if (lba >= fs.total_sectors)
        return 0;

    if (fs.partition_start > fs.device.sector_count ||
        lba >= fs.device.sector_count - fs.partition_start)
        return 0;

    return fs.device.read(
        fs.device.id,
        fs.partition_start + lba,
        1,
        buffer
    );
}

static int write_sector(unsigned lba, const void *buffer) {
    if (!fs.mounted || !buffer)
        return 0;

    if (lba >= fs.total_sectors)
        return 0;

    if (fs.partition_start > fs.device.sector_count ||
        lba >= fs.device.sector_count - fs.partition_start)
        return 0;

    return fs.device.write(
        fs.device.id,
        fs.partition_start + lba,
        1,
        (void *)buffer
    );
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

    if (!fat_cache_read(fat_sector, sector_buffer))
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

/*
 * Validate a FAT16 cluster chain without modifying it.
 *
 * Returns:
 *   0  = valid chain ending normally
 *   1  = cyclic chain
 *  -1  = invalid cluster, unreadable FAT, or impossible chain
 *
 * Floyd's tortoise-and-hare algorithm detects cycles using
 * constant memory, without allocating a cluster bitmap.
 */
static int validate_cluster_chain(unsigned first_cluster) {
    if (!valid_cluster(first_cluster))
        return -1;

    unsigned slow = first_cluster;
    unsigned fast = first_cluster;

    for (unsigned steps = 0; steps < fs.cluster_count; ++steps) {
        unsigned next_slow;
        int result = next_cluster(slow, &next_slow);

        if (result == 2)
            return 0;

        if (result != 1)
            return -1;

        slow = next_slow;

        unsigned next_fast;
        int fast_result = next_cluster(fast, &next_fast);

        if (fast_result == 2)
            return 0;

        if (fast_result != 1)
            return -1;

        fast = next_fast;

        fast_result = next_cluster(fast, &next_fast);

        if (fast_result == 2)
            return 0;

        if (fast_result != 1)
            return -1;

        fast = next_fast;

        if (slow == fast)
            return 1;
    }

    return -1;
}

static int read_fat_entry(unsigned cluster, unsigned *value) {
    if (!value || !valid_cluster(cluster))
        return 0;

    unsigned offset = cluster * 2;
    unsigned lba = fs.fat_start + offset / FAT16_SECTOR_SIZE;
    unsigned entry_offset = offset % FAT16_SECTOR_SIZE;

    if (!fat_cache_read(lba, sector_buffer))
        return 0;

    *value = read16(sector_buffer + entry_offset);
    return 1;
}

static int write_fat_entry(unsigned cluster, unsigned value) {
    if (!fs.mounted || !valid_cluster(cluster))
        return 0;

    if (fs.fat_count == 0 || fs.fat_count > 2)
        return 0;

    unsigned offset = cluster * 2;
    unsigned relative_sector = offset / FAT16_SECTOR_SIZE;
    unsigned entry_offset = offset % FAT16_SECTOR_SIZE;

    /*
     * Preserve the original sector from every FAT copy
     * before modifying any of them.
     */
    static unsigned char original[2][FAT16_SECTOR_SIZE];

    unsigned lbas[2];

    for (unsigned copy = 0; copy < fs.fat_count; ++copy) {
        lbas[copy] =
            fs.fat_start +
            copy * fs.sectors_per_fat +
            relative_sector;

        if (!read_sector(lbas[copy], original[copy])) {
            for (unsigned i = 0; i <= copy; ++i)
                fat_cache_invalidate_sector(lbas[i]);

            return 0;
        }
    }

    /*
     * Update every FAT copy.
     */
    unsigned written = 0;

    for (unsigned copy = 0; copy < fs.fat_count; ++copy) {
        for (unsigned i = 0; i < FAT16_SECTOR_SIZE; ++i)
            sector_buffer[i] = original[copy][i];

        sector_buffer[entry_offset] =
            (unsigned char)value;

        sector_buffer[entry_offset + 1] =
            (unsigned char)(value >> 8);

        if (!write_sector(lbas[copy], sector_buffer)) {
            /*
             * The failed write may have partially reached
             * the device, so attempt to restore it too.
             */
            for (unsigned i = 0; i <= copy; ++i) {
                if (!write_sector(lbas[i], original[i])) {
                    // Rollback failure means disk state is uncertain.
                }

                fat_cache_invalidate_sector(lbas[i]);
            }

            return 0;
        }

        ++written;
    }

    /*
     * Invalidate cached sectors after successful writes.
     */
    for (unsigned copy = 0; copy < written; ++copy)
        fat_cache_invalidate_sector(lbas[copy]);

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

static int free_cluster_chain(unsigned first_cluster) {
    if (!valid_cluster(first_cluster))
        return 0;

    if (validate_cluster_chain(first_cluster) != 0)
        return 0;

    unsigned cluster = first_cluster;

    for (unsigned steps = 0; steps < fs.cluster_count; ++steps) {
        unsigned next = 0;
        int result = next_cluster(cluster, &next);

        if (result == 2)
            return write_fat_entry(cluster, 0);

        if (result != 1)
            return 0;

        if (!write_fat_entry(cluster, 0))
            return 0;

        cluster = next;
    }

    return 0;
}


static int shrink_cluster_chain(
    unsigned first_cluster,
    unsigned keep_clusters
) {
    if (!valid_cluster(first_cluster))
        return 0;

    if (validate_cluster_chain(first_cluster) != 0)
        return 0;

    /* Keep zero clusters: release the entire chain. */
    if (keep_clusters == 0)
        return free_cluster_chain(first_cluster);

    unsigned cluster = first_cluster;

    /* Find the final cluster that must remain allocated. */
    for (unsigned i = 1; i < keep_clusters; ++i) {
        unsigned next;
        int result = next_cluster(cluster, &next);

        if (result != 1)
            return 0;

        cluster = next;
    }

    /* Find the first cluster that should be released. */
    unsigned next;
    int result = next_cluster(cluster, &next);

    if (result == 2)
        return 1;

    if (result != 1)
        return 0;

    /* Detach the unused tail from the file. */
    if (!write_fat_entry(cluster, 0xFFFF))
        return 0;

    /* Release the detached clusters. */
    return free_cluster_chain(next);
}

static int link_clusters(unsigned first, unsigned second) {
    if (!valid_cluster(first) || !valid_cluster(second))
        return 0;

    return write_fat_entry(first, second);
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

    if (!valid_cluster(cluster))
        return -1;

    for (unsigned i = 0; i < cluster_steps; ++i) {
        unsigned next;
        int result = next_cluster(cluster, &next);

        /* A valid end-of-chain means the directory has ended. */
        if (result == 2)
            return 0;

        /* Invalid or unreadable FAT entries indicate corruption. */
        if (result != 1)
            return -1;

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

/*
 * Find an unused directory entry.
 *
 * If a subdirectory has no free entries, allocate another
 * cluster and append it to the directory's cluster chain.
 *
 * The FAT16 root directory has a fixed size and cannot grow.
 */
static int find_directory_slot(
    unsigned directory_cluster,
    unsigned *entry_lba,
    unsigned *entry_offset
) {
    if (!entry_lba || !entry_offset)
        return 0;

    unsigned entries_per_cluster =
        fs.sectors_per_cluster *
        (FAT16_SECTOR_SIZE / FAT16_ENTRY_SIZE);

    unsigned max_entries;

    if (directory_cluster == 0) {
        max_entries = fs.root_entries;
    } else {
        max_entries = fs.cluster_count * entries_per_cluster;
    }

    /* Search for an existing free or deleted entry. */
    for (unsigned i = 0; i < max_entries; ++i) {
        unsigned char entry[FAT16_ENTRY_SIZE];
        unsigned lba;
        unsigned offset;

        int result = read_directory_entry(
            directory_cluster,
            i,
            entry,
            &lba,
            &offset
        );

        if (result < 0)
            return 0;

        if (result == 0)
            break;

        if (entry[0] == 0x00 || entry[0] == 0xE5) {
            *entry_lba = lba;
            *entry_offset = offset;
            return 1;
        }
    }

    /* The fixed-size root directory cannot be expanded. */
    if (directory_cluster == 0)
        return 0;

    /* Find the final cluster in this directory. */
    unsigned last_cluster = directory_cluster;

    for (unsigned i = 0; i < fs.cluster_count; ++i) {
        unsigned next;
        int result = next_cluster(last_cluster, &next);

        if (result == 2)
            break;

        if (result != 1)
            return 0;

        last_cluster = next;

        if (i == fs.cluster_count - 1)
            return 0;
    }

    /* Allocate a new cluster for additional entries. */
    unsigned new_cluster;

    if (!allocate_cluster(&new_cluster))
        return 0;

    /* Append it to the directory's cluster chain. */
    if (!link_clusters(last_cluster, new_cluster)) {
        write_fat_entry(new_cluster, 0);
        return 0;
    }

    *entry_lba = cluster_to_sector(new_cluster);
    *entry_offset = 0;

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
        if (validate_cluster_chain(directory_cluster) != 0)
            return -1;

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

        if (result < 0)
            return -1;

        if (result == 0)
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

        /* Current directory: do nothing. */
        if (string_equal_ci(component, "."))
            continue;

        /* Parent of root is still root. */
        if (string_equal_ci(component, "..")) {
            if (current == 0)
                continue;

            unsigned char parent_entry[FAT16_ENTRY_SIZE];

            /*
             * The second directory entry is "..".
             * It stores the parent directory's cluster.
             */
            if (read_directory_entry(
                    current,
                    1,
                    parent_entry,
                    0,
                    0) != 1)
                return 0;

            /* Verify that this really is a ".." entry. */
            if (parent_entry[0] != '.' ||
                parent_entry[1] != '.' ||
                parent_entry[2] != ' ')
                return 0;

            if (!(parent_entry[11] & FAT16_ATTR_DIRECTORY))
                return 0;

            unsigned parent =
                ((unsigned)read16(parent_entry + 20) << 16) |
                read16(parent_entry + 26);

            /* Cluster zero represents the FAT16 root directory. */
            if (parent != 0 && !valid_cluster(parent))
                return 0;

            current = parent;
            continue;
        }

        unsigned char short_name[11];
        unsigned char entry[FAT16_ENTRY_SIZE];

        if (!make_short_name(component, short_name))
            return 0;

        if (!find_entry(current, short_name, entry, 0, 0))
            return 0;

        if (find_entry(current, short_name, entry, 0, 0) != 1)
            return 0;

        current =
            ((unsigned)read16(entry + 20) << 16) |
            read16(entry + 26);

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

    int result = find_entry(
        *parent_cluster,
        short_name,
        output,
        entry_lba,
        entry_offset
    );

    return result == 1;
}

/*
 * Check whether a directory can be modified.
 * The FAT16 root directory is always considered writable.
 */
static int directory_is_writable(const char *path) {
    if (!path || path[0] != '/')
        return 0;

    if (string_equal_ci(path, "/"))
        return 1;

    unsigned parent_cluster;
    unsigned char entry[FAT16_ENTRY_SIZE];

    if (!resolve_file(
            path,
            &parent_cluster,
            entry,
            0,
            0))
        return 0;

    if (!(entry[11] & FAT16_ATTR_DIRECTORY))
        return 0;

    if (entry[11] & FAT16_ATTR_READ_ONLY)
        return 0;

    return 1;
}

static int is_fat16_boot_sector(const unsigned char *boot) {
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

    return bytes_per_sector == FAT16_SECTOR_SIZE &&
           power_of_two(sectors_per_cluster) &&
           sectors_per_cluster <= 128 &&
           reserved != 0 &&
           fats >= 1 && fats <= 2 &&
           root_entries != 0 &&
           fat_size != 0 &&
           total != 0;
}

int fat16_mount(unsigned device_index) {
    fs.mounted = 0;
    fat_cache_invalidate();

    if (!block_device_get(device_index, &fs.device))
        return 0;

    if (fs.device.sector_count == 0)
        return 0;

    unsigned char boot[FAT16_SECTOR_SIZE];
    unsigned partition_start = 0;
    unsigned partition_sectors = fs.device.sector_count;

    /* Read sector zero: either a superfloppy BPB or an MBR. */
    if (!fs.device.read(fs.device.id, 0, 1, boot))
        return 0;

    if (!is_fat16_boot_sector(boot)) {
        /* Sector zero is not a valid FAT16 boot sector. Try its MBR. */
        if (boot[510] != 0x55 || boot[511] != 0xAA)
            return 0;

        int found = 0;

        for (unsigned i = 0; i < 4; ++i) {
            unsigned char *entry = boot + 446 + i * 16;

            unsigned type = entry[4];
            unsigned start = read32(entry + 8);
            unsigned count = read32(entry + 12);

            /* FAT16 partition types: small, standard, and LBA. */
            if (type != 0x04 && type != 0x06 && type != 0x0E)
                continue;

            if (start == 0 || count == 0)
                continue;

            if (start >= fs.device.sector_count ||
                count > fs.device.sector_count - start)
                continue;

            unsigned char candidate[FAT16_SECTOR_SIZE];

            if (!fs.device.read(
                    fs.device.id, start, 1, candidate))
                continue;

            if (!is_fat16_boot_sector(candidate))
                continue;

            partition_start = start;
            partition_sectors = count;

            for (unsigned j = 0; j < FAT16_SECTOR_SIZE; ++j)
                boot[j] = candidate[j];

            found = 1;
            break;
        }

        if (!found)
            return 0;
    }

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
        total > partition_sectors)
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
    fs.partition_start = partition_start;
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
    info->partition_start_sector = fs.partition_start;
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

extern "C" int fat16_check_mirrors(
    unsigned *mismatched_sectors
) {
    if (!fs.mounted || !mismatched_sectors)
        return -1;

    *mismatched_sectors = 0;

    // A single FAT has no mirror to compare.
    if (fs.fat_count < 2)
        return 1;

    static unsigned char primary[FAT16_SECTOR_SIZE];
    static unsigned char mirror[FAT16_SECTOR_SIZE];

    for (unsigned sector = 0;
         sector < fs.sectors_per_fat;
         ++sector) {

        unsigned primary_lba =
            fs.fat_start + sector;

        unsigned mirror_lba =
            fs.fat_start + fs.sectors_per_fat + sector;

        if (!read_sector(primary_lba, primary))
            return -1;

        if (!read_sector(mirror_lba, mirror))
            return -1;

        int matches = 1;

        for (unsigned i = 0;
             i < FAT16_SECTOR_SIZE;
             ++i) {

            if (primary[i] != mirror[i]) {
                matches = 0;
                break;
            }
        }

        if (!matches)
            ++(*mismatched_sectors);
    }

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

        if (result < 0)
            return -1;

        if (result == 0)
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

    if (validate_cluster_chain(cluster) != 0)
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

extern "C" int fat16_create(const char *path) {
    if (!fs.mounted || !path || path[0] != '/')
        return 0;

    unsigned length = string_length(path);

    if (length < 2 || length >= VFS_PATH_MAX ||
        path[length - 1] == '/')
        return 0;

    /*
     * Split the path into parent directory and filename.
     */
    unsigned split = length;

    while (split > 0 && path[split - 1] != '/')
        --split;

    if (split == 0 || split >= length)
        return 0;

    char parent_path[VFS_PATH_MAX];

    if (split == 1) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        for (unsigned i = 0; i < split - 1; ++i)
            parent_path[i] = path[i];

        parent_path[split - 1] = '\0';
    }

    char filename[13];
    unsigned filename_length = length - split;

    if (filename_length > 12)
        return 0;

    for (unsigned i = 0; i < filename_length; ++i)
        filename[i] = path[split + i];

    filename[filename_length] = '\0';

    /*
     * Convert the filename to FAT 8.3 format.
     */
    unsigned char short_name[11];

    if (!make_short_name(filename, short_name))
        return 0;

    /*
     * Resolve the parent directory.
     */
    unsigned directory_cluster;

    if (!resolve_directory(parent_path, &directory_cluster))
        return 0;

    if (!directory_is_writable(parent_path))
        return 0;

    /*
     * Reject duplicate filenames.
     */
    unsigned char existing[FAT16_ENTRY_SIZE];

    int existing_result = find_entry(
        directory_cluster,
        short_name,
        existing,
        0,
        0
    );

    if (existing_result != 0)
        return 0;

    /*
     * Find an unused or deleted directory entry.
     */
    unsigned entry_lba;
    unsigned entry_offset;

    if (!find_directory_slot(
            directory_cluster,
            &entry_lba,
            &entry_offset))
        return 0;

    /* Read the sector containing the directory entry. */
    if (!read_sector(entry_lba, sector_buffer))
        return 0;

    /* Initialize the new FAT directory entry. */
    for (unsigned j = 0; j < FAT16_ENTRY_SIZE; ++j)
        sector_buffer[entry_offset + j] = 0;

    for (unsigned j = 0; j < 11; ++j)
        sector_buffer[entry_offset + j] = short_name[j];

    sector_buffer[entry_offset + 11] = 0x20;

    if (!write_sector(entry_lba, sector_buffer))
        return 0;

    return 1;
}


extern "C" int fat16_mkdir(const char *path) {
    if (!fs.mounted || !path || path[0] != '/')
        return 0;

    unsigned length = string_length(path);

    if (length < 2 || length >= VFS_PATH_MAX ||
        path[length - 1] == '/')
        return 0;

    /* Split parent path and directory name. */
    unsigned split = length;

    while (split > 0 && path[split - 1] != '/')
        --split;

    if (split == 0 || split >= length)
        return 0;

    char parent_path[VFS_PATH_MAX];

    if (split == 1) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        for (unsigned i = 0; i < split - 1; ++i)
            parent_path[i] = path[i];

        parent_path[split - 1] = '\0';
    }

    char name[13];
    unsigned name_length = length - split;

    if (name_length > 12)
        return 0;

    for (unsigned i = 0; i < name_length; ++i)
        name[i] = path[split + i];

    name[name_length] = '\0';

    unsigned char short_name[11];

    if (!make_short_name(name, short_name))
        return 0;

    /* Resolve parent directory. */
    unsigned parent_cluster;

    if (!resolve_directory(parent_path, &parent_cluster))
        return 0;

    if (!directory_is_writable(parent_path))
        return 0;

    /* Reject duplicate names. */
    unsigned char existing[FAT16_ENTRY_SIZE];

    int existing_result = find_entry(
        parent_cluster,
        short_name,
        existing,
        0,
        0
    );

    if (existing_result != 0)
        return 0;

    /* Find a free parent directory entry. */
    unsigned entry_lba;
    unsigned entry_offset;

    if (!find_directory_slot(
            parent_cluster,
            &entry_lba,
            &entry_offset))
        return 0;
    /* Allocate and clear a cluster for the new directory. */
    unsigned new_cluster;

    if (!allocate_cluster(&new_cluster))
        return 0;

    /*
     * Initialize the new directory's first sector.
     * Entry 0: .
     * Entry 1: ..
     */
    for (unsigned i = 0; i < FAT16_SECTOR_SIZE; ++i)
        sector_buffer[i] = 0;

    /* "." entry */
    sector_buffer[0] = '.';

    for (unsigned i = 1; i < 11; ++i)
        sector_buffer[i] = ' ';

    sector_buffer[11] = FAT16_ATTR_DIRECTORY;

    sector_buffer[26] = (unsigned char)new_cluster;
    sector_buffer[27] = (unsigned char)(new_cluster >> 8);

    /* ".." entry */
    sector_buffer[32] = '.';
    sector_buffer[33] = '.';

    for (unsigned i = 34; i < 43; ++i)
        sector_buffer[i] = ' ';

    sector_buffer[43] = FAT16_ATTR_DIRECTORY;

    sector_buffer[58] = (unsigned char)parent_cluster;
    sector_buffer[59] = (unsigned char)(parent_cluster >> 8);

    if (!write_sector(cluster_to_sector(new_cluster), sector_buffer)) {
        write_fat_entry(new_cluster, 0);
        return 0;
    }

    /* Create the directory entry in its parent. */
    if (!read_sector(entry_lba, sector_buffer)) {
        write_fat_entry(new_cluster, 0);
        return 0;
    }

    for (unsigned i = 0; i < FAT16_ENTRY_SIZE; ++i)
        sector_buffer[entry_offset + i] = 0;

    for (unsigned i = 0; i < 11; ++i)
        sector_buffer[entry_offset + i] = short_name[i];

    sector_buffer[entry_offset + 11] = FAT16_ATTR_DIRECTORY;

    sector_buffer[entry_offset + 20] = 0;
    sector_buffer[entry_offset + 21] = 0;

    sector_buffer[entry_offset + 26] =
        (unsigned char)new_cluster;

    sector_buffer[entry_offset + 27] =
        (unsigned char)(new_cluster >> 8);

    if (!write_sector(entry_lba, sector_buffer)) {
        write_fat_entry(new_cluster, 0);
        return 0;
    }

    return 1;
}

extern "C" int fat16_remove(const char *path) {
    if (!fs.mounted || !path)
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

    unsigned attributes = entry[11];

    /* Never remove read-only entries or volume labels. */
    if (entry[11] & (FAT16_ATTR_READ_ONLY | FAT16_ATTR_VOLUME))
        return 0;

    unsigned first_cluster =
        ((unsigned)read16(entry + 20) << 16) |
        read16(entry + 26);

    if (first_cluster != 0 &&
        validate_cluster_chain(first_cluster) != 0)
        return 0;

    if (attributes & FAT16_ATTR_DIRECTORY) {
        /* Directories must have a valid data cluster. */
        if (!valid_cluster(first_cluster))
            return 0;

        unsigned entries_per_cluster =
            fs.sectors_per_cluster *
            (FAT16_SECTOR_SIZE / FAT16_ENTRY_SIZE);

        unsigned max_entries =
            fs.cluster_count * entries_per_cluster;

        /* Verify that the directory is empty. */
        for (unsigned i = 0; i < max_entries; ++i) {
            unsigned char child[FAT16_ENTRY_SIZE];

            int result = read_directory_entry(
                first_cluster,
                i,
                child,
                0,
                0
            );

            if (result < 0)
                return 0;

            if (result == 0)
                break;

            if (child[0] == 0x00)
                break;

            if (child[0] == 0xE5)
                continue;

            if (child[11] == FAT16_ATTR_LONG_NAME)
                continue;

            if (child[11] & FAT16_ATTR_VOLUME)
                continue;

            /* Ignore the mandatory "." and ".." entries. */
            int is_dot =
                child[0] == '.' &&
                child[1] == ' ';

            int is_dotdot =
                child[0] == '.' &&
                child[1] == '.' &&
                child[2] == ' ';

            if (is_dot || is_dotdot)
                continue;

            /* A real entry means the directory isn't empty. */
            return 0;
        }
    } else {
        /* Files may have no cluster, but nonzero clusters must be valid. */
        if (first_cluster != 0 && !valid_cluster(first_cluster))
            return 0;
    }

    /*
     * Mark the parent directory entry deleted first.
     */
    if (!read_sector(entry_lba, sector_buffer))
        return 0;

    sector_buffer[entry_offset] = 0xE5;

    if (!write_sector(entry_lba, sector_buffer))
        return 0;

    /* Empty files have no cluster chain to release. */
    if (first_cluster == 0)
        return 1;

    return free_cluster_chain(first_cluster);
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

    if (entry[11] & (FAT16_ATTR_READ_ONLY | FAT16_ATTR_DIRECTORY))
        return 0;

    unsigned first_cluster =
        ((unsigned)read16(entry + 20) << 16) |
        read16(entry + 26);

    unsigned original_first_cluster = first_cluster;

    unsigned cluster_bytes =
        fs.sectors_per_cluster * FAT16_SECTOR_SIZE;

    unsigned needed_clusters =
        size / cluster_bytes +
        (size % cluster_bytes != 0);

    if (needed_clusters > fs.cluster_count)
        return 0;

    /*
     * Count the existing cluster chain.
     */
    unsigned existing_clusters = 0;
    unsigned last_existing = 0;

    if (valid_cluster(first_cluster)) {
        unsigned cluster = first_cluster;

        while (existing_clusters < fs.cluster_count) {
            ++existing_clusters;
            last_existing = cluster;

            unsigned next;
            int result = next_cluster(cluster, &next);

            if (result == 2)
                break;

            if (result != 1)
                return 0;

            cluster = next;
        }

        if (existing_clusters == fs.cluster_count) {
            unsigned next;

            if (next_cluster(last_existing, &next) == 1)
                return 0;
        }
    } else if (first_cluster != 0) {
        return 0;
    }

    /*
     * Allocate the additional clusters as a separate chain.
     */
    unsigned additional_needed = 0;

    if (needed_clusters > existing_clusters)
        additional_needed = needed_clusters - existing_clusters;

    unsigned new_chain = 0;
    unsigned new_chain_tail = 0;

    for (unsigned i = 0; i < additional_needed; ++i) {
        unsigned allocated;

        if (!allocate_cluster(&allocated)) {
            if (new_chain)
                free_cluster_chain(new_chain);

            return 0;
        }

        if (!new_chain) {
            new_chain = allocated;
            new_chain_tail = allocated;
        } else {
            if (!link_clusters(new_chain_tail, allocated)) {
                write_fat_entry(allocated, 0);
                free_cluster_chain(new_chain);
                return 0;
            }

            new_chain_tail = allocated;
        }
    }

    /*
     * Attach the new chain to the existing file.
     */
    if (new_chain) {
        if (existing_clusters) {
            if (!link_clusters(last_existing, new_chain)) {
                free_cluster_chain(new_chain);
                return 0;
            }
        } else {
            first_cluster = new_chain;
        }
    }

    /*
     * Write file data.
     */
    unsigned written = 0;
    unsigned cluster = first_cluster;

    for (unsigned c = 0; c < needed_clusters; ++c) {
        if (!valid_cluster(cluster))
            return 0;

        unsigned first_sector = cluster_to_sector(cluster);

        for (unsigned s = 0;
             s < fs.sectors_per_cluster && written < size;
             ++s) {

            unsigned amount = min_unsigned(
                FAT16_SECTOR_SIZE,
                size - written
            );

            for (unsigned i = 0; i < FAT16_SECTOR_SIZE; ++i) {
                sector_buffer[i] = i < amount
                    ? (unsigned char)data[written + i]
                    : 0;
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

    /*
     * Update the directory entry.
     */
    if (!read_sector(entry_lba, sector_buffer))
        return 0;

    /*
     * Zero-length files must not retain an allocated cluster chain.
     */
    if (size == 0)
        first_cluster = 0;

    sector_buffer[entry_offset + 20] =
        (unsigned char)(first_cluster >> 16);

    sector_buffer[entry_offset + 21] =
        (unsigned char)(first_cluster >> 24);

    sector_buffer[entry_offset + 26] =
        (unsigned char)first_cluster;

    sector_buffer[entry_offset + 27] =
        (unsigned char)(first_cluster >> 8);

    sector_buffer[entry_offset + 28] =
        (unsigned char)size;

    sector_buffer[entry_offset + 29] =
        (unsigned char)(size >> 8);

    sector_buffer[entry_offset + 30] =
        (unsigned char)(size >> 16);

    sector_buffer[entry_offset + 31] =
        (unsigned char)(size >> 24);

    if (!write_sector(entry_lba, sector_buffer))
        return 0;

    /*
     * Reclaim clusters that are no longer needed.
     *
     * Directory metadata is updated first. If reclamation fails,
     * the file remains valid, although disk space may be leaked.
     */
    if (size == 0) {
        if (valid_cluster(original_first_cluster))
            return free_cluster_chain(original_first_cluster);

        return 1;
    }

    if (needed_clusters < existing_clusters) {
        return shrink_cluster_chain(
            first_cluster,
            needed_clusters
        );
    }

    return 1;
}
