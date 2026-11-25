#include "drivers/ata.h"

#define PRIMARY_IO    0x1F0
#define PRIMARY_CTL   0x3F6
#define SECONDARY_IO  0x170
#define SECONDARY_CTL 0x376

#define ATA_READ_PIO       0x20
#define ATA_WRITE_PIO      0x30
#define ATA_CACHE_FLUSH    0xE7
#define ATA_LBA28_MAX      0x0FFFFFFF

#define ATA_DATA      0
#define ATA_ERROR     1
#define ATA_SECCOUNT  2
#define ATA_LBA0      3
#define ATA_LBA1      4
#define ATA_LBA2      5
#define ATA_HDDEVSEL  6
#define ATA_COMMAND   7
#define ATA_STATUS    7

#define ATA_IDENTIFY  0xEC

#define ATA_BSY 0x80
#define ATA_DRQ 0x08
#define ATA_ERR 0x01
#define ATA_DF  0x20

static ata_device_info devices[ATA_DEVICE_COUNT];

static inline void outw(unsigned short port, unsigned short value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outb(unsigned short port, unsigned char value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned char inb(unsigned short port) {
    unsigned char value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline unsigned short inw(unsigned short port) {
    unsigned short value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void delay_400ns(unsigned short control) {
    for (int i = 0; i < 4; ++i)
        (void)inb(control);
}

static int wait_ready(unsigned short io) {
    for (unsigned i = 0; i < 1000000; ++i) {
        unsigned char status = inb(io + ATA_STATUS);

        if (status == 0 || status == 0xFF)
            return 0;

        if (!(status & ATA_BSY))
            return 1;
    }

    return 0;
}

static int wait_drq(unsigned short io) {
    for (unsigned i = 0; i < 1000000; ++i) {
        unsigned char status = inb(io + ATA_STATUS);

        if (status == 0 || status == 0xFF)
            return 0;

        if (status & (ATA_ERR | ATA_DF))
            return 0;

        if (!(status & ATA_BSY) && (status & ATA_DRQ))
            return 1;
    }

    return 0;
}

static void identify_device(
    unsigned index,
    unsigned short io,
    unsigned short control,
    unsigned drive
) {
    ata_device_info *dev = &devices[index];

    dev->present = 0;
    dev->io_base = io;
    dev->control_base = control;
    dev->drive = drive;

    outb(control, 0x02);

    outb(io + ATA_HDDEVSEL, 0xA0 | (drive << 4));
    delay_400ns(control);

    outb(io + ATA_SECCOUNT, 0);
    outb(io + ATA_LBA0, 0);
    outb(io + ATA_LBA1, 0);
    outb(io + ATA_LBA2, 0);

    outb(io + ATA_COMMAND, ATA_IDENTIFY);

    unsigned char status = inb(io + ATA_STATUS);

    if (status == 0 || status == 0xFF)
        return;

    if (!wait_ready(io))
        return;

    /* Nonzero signature means this isn't an ATA disk. */
    if (inb(io + ATA_LBA1) != 0 ||
        inb(io + ATA_LBA2) != 0)
        return;

    if (!wait_drq(io))
        return;

    unsigned short data[256];

    for (unsigned i = 0; i < 256; ++i)
        data[i] = inw(io + ATA_DATA);

    unsigned sectors =
        (unsigned)data[60] |
        ((unsigned)data[61] << 16);

    if (!sectors)
        return;

    dev->sectors = sectors;
    dev->present = 1;

    for (unsigned i = 0; i < 20; ++i) {
        dev->model[i * 2] =
            (char)(data[27 + i] >> 8);

        dev->model[i * 2 + 1] =
            (char)(data[27 + i] & 0xFF);
    }

    dev->model[40] = '\0';
}

void ata_init(void) {
    for (unsigned i = 0; i < ATA_DEVICE_COUNT; ++i) {
        devices[i].present = 0;
        devices[i].sectors = 0;
        devices[i].model[0] = '\0';
    }

    identify_device(0, PRIMARY_IO, PRIMARY_CTL, 0);
    identify_device(1, PRIMARY_IO, PRIMARY_CTL, 1);
    identify_device(2, SECONDARY_IO, SECONDARY_CTL, 0);
    identify_device(3, SECONDARY_IO, SECONDARY_CTL, 1);
}

unsigned ata_device_count(void) {
    unsigned count = 0;

    for (unsigned i = 0; i < ATA_DEVICE_COUNT; ++i)
        if (devices[i].present)
            ++count;

    return count;
}

int ata_get_device(unsigned index, ata_device_info *out) {
    if (index >= ATA_DEVICE_COUNT || !out ||
        !devices[index].present)
        return 0;

    *out = devices[index];
    return 1;
}

static int ata_wait_not_busy(unsigned short io) {
    for (unsigned i = 0; i < 1000000; ++i) {
        unsigned char status = inb(io + ATA_STATUS);

        if (status == 0 || status == 0xFF)
            return 0;

        if (status & (ATA_ERR | ATA_DF))
            return 0;

        if (!(status & ATA_BSY))
            return 1;
    }

    return 0;
}

static int ata_wait_data(unsigned short io) {
    for (unsigned i = 0; i < 1000000; ++i) {
        unsigned char status = inb(io + ATA_STATUS);

        if (status == 0 || status == 0xFF)
            return 0;

        if (status & (ATA_ERR | ATA_DF))
            return 0;

        if (!(status & ATA_BSY) && (status & ATA_DRQ))
            return 1;
    }

    return 0;
}

static int ata_validate_request(
    unsigned device,
    unsigned lba,
    unsigned count,
    ata_device_info **out
) {
    if (device >= ATA_DEVICE_COUNT || !count || count > 255)
        return 0;

    ata_device_info *dev = &devices[device];

    if (!dev->present)
        return 0;

    if (lba > ATA_LBA28_MAX)
        return 0;

    if (count - 1 > ATA_LBA28_MAX - lba)
        return 0;

    if (lba >= dev->sectors || count > dev->sectors - lba)
        return 0;

    *out = dev;
    return 1;
}

int ata_read_sectors(
    unsigned device,
    unsigned lba,
    unsigned count,
    void *buffer
) {
    ata_device_info *dev;

    if (!buffer ||
        !ata_validate_request(device, lba, count, &dev))
        return 0;

    unsigned short io = (unsigned short)dev->io_base;

    if (!ata_wait_not_busy(io))
        return 0;

    outb(io + ATA_HDDEVSEL,
         0xE0 | (dev->drive << 4) | ((lba >> 24) & 0x0F));

    delay_400ns((unsigned short)dev->control_base);

    outb(io + ATA_SECCOUNT, (unsigned char)count);
    outb(io + ATA_LBA0, (unsigned char)lba);
    outb(io + ATA_LBA1, (unsigned char)(lba >> 8));
    outb(io + ATA_LBA2, (unsigned char)(lba >> 16));
    outb(io + ATA_COMMAND, ATA_READ_PIO);

    unsigned short *destination = (unsigned short *)buffer;

    for (unsigned sector = 0; sector < count; ++sector) {
        if (!ata_wait_data(io))
            return 0;

        for (unsigned word = 0; word < 256; ++word)
            *destination++ = inw(io + ATA_DATA);
    }

    return ata_wait_not_busy(io);
}

int ata_write_sectors(
    unsigned device,
    unsigned lba,
    unsigned count,
    const void *buffer
) {
    ata_device_info *dev;

    if (!buffer ||
        !ata_validate_request(device, lba, count, &dev))
        return 0;

    unsigned short io = (unsigned short)dev->io_base;

    if (!ata_wait_not_busy(io))
        return 0;

    outb(io + ATA_HDDEVSEL,
         0xE0 | (dev->drive << 4) | ((lba >> 24) & 0x0F));

    delay_400ns((unsigned short)dev->control_base);

    outb(io + ATA_SECCOUNT, (unsigned char)count);
    outb(io + ATA_LBA0, (unsigned char)lba);
    outb(io + ATA_LBA1, (unsigned char)(lba >> 8));
    outb(io + ATA_LBA2, (unsigned char)(lba >> 16));
    outb(io + ATA_COMMAND, ATA_WRITE_PIO);

    const unsigned short *source =
        (const unsigned short *)buffer;

    for (unsigned sector = 0; sector < count; ++sector) {
        if (!ata_wait_data(io))
            return 0;

        for (unsigned word = 0; word < 256; ++word)
            outw(io + ATA_DATA, *source++);
    }

    if (!ata_wait_not_busy(io))
        return 0;

    outb(io + ATA_COMMAND, ATA_CACHE_FLUSH);

    return ata_wait_not_busy(io);
}
