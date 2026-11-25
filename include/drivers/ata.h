#ifndef PARALLAX_ATA_H
#define PARALLAX_ATA_H

#define ATA_DEVICE_COUNT 4
#define ATA_MODEL_LENGTH 41

typedef struct {
    int present;
    unsigned sectors;
    char model[ATA_MODEL_LENGTH];
    unsigned io_base;
    unsigned control_base;
    unsigned drive;
} ata_device_info;

void ata_init(void);
unsigned ata_device_count(void);
int ata_get_device(unsigned index, ata_device_info *out);

#endif
