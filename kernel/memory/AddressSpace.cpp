#include "memory/address_space.h"

extern "C" {
    unsigned paging_kernel_directory(void);
    int paging_create_directory(unsigned *directory);
    void paging_destroy_directory(unsigned directory);
    void paging_switch_directory(unsigned directory);

    int paging_map_user(
        unsigned directory,
        unsigned virtual_address,
        unsigned physical_address,
        unsigned flags
    );

    int paging_unmap_user(
        unsigned directory,
        unsigned virtual_address
    );

    unsigned paging_translate_user(
        unsigned directory,
        unsigned virtual_address
    );
}

class AddressSpaceManager {
public:
    int create(address_space *space) {
        if (!space)
            return 0;

        space->directory = 0;

        return paging_create_directory(&space->directory);
    }

    void destroy(address_space *space) {
        if (!space || !space->directory)
            return;

        paging_switch_directory(paging_kernel_directory());

        paging_destroy_directory(space->directory);
        space->directory = 0;
    }

    int map(
        address_space *space,
        unsigned virtual_address,
        unsigned physical_address,
        unsigned flags
    ) {
        if (!space || !space->directory)
            return 0;

        if ((virtual_address & 0xFFF) ||
            (physical_address & 0xFFF))
            return 0;

        return paging_map_user(
            space->directory,
            virtual_address,
            physical_address,
            flags
        );
    }

    int unmap(address_space *space, unsigned virtual_address) {
        if (!space || !space->directory)
            return 0;

        return paging_unmap_user(
            space->directory,
            virtual_address
        );
    }

    unsigned translate(address_space *space, unsigned virtual_address) {
        if (!space || !space->directory)
            return 0;

        return paging_translate_user(
            space->directory,
            virtual_address
        );
    }

    void activate(address_space *space) {
        if (!space || !space->directory)
            return;

        paging_switch_directory(space->directory);
    }
};

static AddressSpaceManager manager;

extern "C" int address_space_create(address_space *space) {
    return manager.create(space);
}

extern "C" void address_space_destroy(address_space *space) {
    manager.destroy(space);
}

extern "C" int address_space_map_user(
    address_space *space,
    unsigned virtual_address,
    unsigned physical_address,
    unsigned flags
) {
    return manager.map(space, virtual_address, physical_address, flags);
}

extern "C" int address_space_unmap_user(
    address_space *space,
    unsigned virtual_address
) {
    return manager.unmap(space, virtual_address);
}

extern "C" unsigned address_space_translate_user(
    address_space *space,
    unsigned virtual_address
) {
    return manager.translate(space, virtual_address);
}

extern "C" void address_space_activate(address_space *space) {
    manager.activate(space);
}

extern "C" unsigned address_space_kernel_directory(void) {
    return paging_kernel_directory();
}
