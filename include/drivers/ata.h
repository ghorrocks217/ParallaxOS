#ifndef PARALLAX_ATA_H
#define PARALLAX_ATA_H

#define ATA_DEVICE_COUNT 4
#define ATA_MODEL_LENGTH 41
#define ATA_SECTOR_SIZE 512

typedef struct {
    int present;
    unsigned sectors;
    char model[ATA_MODEL_LENGTH];
    unsigned io_base;
    unsigned control_base;
    unsigned drive;
} ata_device_info;

#ifdef __cplusplus
extern "C" {
#endif

void ata_init(void);
unsigned ata_device_count(void);
int ata_get_device(unsigned index, ata_device_info *out);

int ata_read_sectors(
    unsigned device,
    unsigned lba,
    unsigned count,
    void *buffer
);

int ata_write_sectors(
    unsigned device,
    unsigned lba,
    unsigned count,
    const void *buffer
);

#ifdef __cplusplus
}
#endif

#endif
