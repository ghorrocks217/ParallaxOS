#ifndef MEMORY_ADDRESS_SPACE_H
#define MEMORY_ADDRESS_SPACE_H

struct address_space {
    unsigned directory;
};

#ifdef __cplusplus
extern "C" {
#endif

int address_space_create(address_space *space);
void address_space_destroy(address_space *space);

int address_space_map_user(
    address_space *space,
    unsigned virtual_address,
    unsigned physical_address,
    unsigned flags
);

int address_space_unmap_user(
    address_space *space,
    unsigned virtual_address
);

unsigned address_space_translate_user(
    address_space *space,
    unsigned virtual_address
);

void address_space_activate(address_space *space);
unsigned address_space_kernel_directory(void);

#ifdef __cplusplus
}
#endif

#endif
