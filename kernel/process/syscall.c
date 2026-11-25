#include "process/syscall.h"

extern void kprintf(const char *format, ...);

extern int copy_user_string(
    char *destination,
    unsigned source,
    unsigned capacity
);

int syscall_handler(
    unsigned int number,
    unsigned int arg1,
    unsigned int arg2
) {
    (void)arg2;

    switch (number) {
        case SYS_WRITE: {
            char buffer[256];

            if (!copy_user_string(buffer, arg1, sizeof(buffer))) {
                kprintf("[syscall] Invalid user pointer\n");
                return -1;
            }

            kprintf("%s", buffer);
            return 0;
        }

        case SYS_YIELD:
            kprintf("[syscall] SYS_YIELD is unavailable until user scheduling is implemented\n");
            return -1;

        case SYS_EXIT:
            kprintf("[syscall] SYS_EXIT is not implemented yet\n");
            return -1;

        default:
            kprintf("[syscall] Unknown syscall: %u\n", number);
            return -1;
    }
}
