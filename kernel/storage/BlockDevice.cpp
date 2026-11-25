#include "storage/block_device.h"
#include "drivers/ata.h"

static int ata_block_read(
    unsigned device,
    unsigned lba,
    unsigned count,
    void *buffer
) {
    return ata_read_sectors(device, lba, count, buffer);
}

static int ata_block_write(
    unsigned device,
    unsigned lba,
    unsigned count,
    const void *buffer
) {
    return ata_write_sectors(device, lba, count, buffer);
}

extern "C" int block_device_get(
    unsigned index,
    block_device *out
) {
    if (!out)
        return 0;

    ata_device_info info;

    if (!ata_get_device(index, &info))
        return 0;

    out->id = index;
    out->sector_count = info.sectors;
    out->read = ata_block_read;
    out->write = ata_block_write;

    return 1;
}
