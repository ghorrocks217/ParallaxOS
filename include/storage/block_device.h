#ifndef PARALLAX_BLOCK_DEVICE_H
#define PARALLAX_BLOCK_DEVICE_H

typedef struct block_device {
    unsigned id;
    unsigned sector_count;

    int (*read)(
        unsigned device,
        unsigned lba,
        unsigned count,
        void *buffer
    );

    int (*write)(
        unsigned device,
        unsigned lba,
        unsigned count,
        const void *buffer
    );
} block_device;

#ifdef __cplusplus
extern "C" {
#endif

int block_device_get(unsigned index, block_device *out);

#ifdef __cplusplus
}
#endif

#endif
