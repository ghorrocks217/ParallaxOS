#include "process/process.h"

class ProcessManager {
public:
    void initialize() {
        for (unsigned i = 0; i < CAPACITY; ++i) {
            records[i].pid = 0;
            records[i].state = PROCESS_FREE;
            records[i].entry = 0;
            records[i].user_stack = 0;
            records[i].page_directory = 0;
            records[i].exit_code = 0;
            records[i].name[0] = '\0';
        }

        next_pid = 1;
        initialized = 1;
    }

    int create(const char *name, unsigned entry,
               unsigned stack, unsigned directory) {
        if (!initialized || !name || !*name || !entry || !stack)
            return -1;

        for (unsigned i = 0; i < CAPACITY; ++i) {
            if (records[i].state != PROCESS_FREE &&
                records[i].state != PROCESS_EXITED)
                continue;

            process_info *p = &records[i];

            p->pid = next_pid++;
            if (!next_pid)
                next_pid = 1;

            p->state = PROCESS_READY;
            p->entry = entry;
            p->user_stack = stack;
            p->page_directory = directory;
            p->exit_code = 0;

            unsigned n = 0;
            while (name[n] && n < PROCESS_NAME_MAX - 1) {
                p->name[n] = name[n];
                ++n;
            }

            p->name[n] = '\0';
            return (int)p->pid;
        }

        return -1;
    }

    int set_state(unsigned pid, unsigned state) {
        if (!initialized ||
            state < PROCESS_CREATED ||
            state > PROCESS_EXITED)
            return 0;

        process_info *p = find(pid);

        if (!p)
            return 0;

        p->state = state;
        return 1;
    }

    int exit_process(unsigned pid, int code) {
        process_info *p = find(pid);

        if (!p)
            return 0;

        p->exit_code = code;
        p->state = PROCESS_EXITED;
        return 1;
    }

    unsigned count() const {
        unsigned n = 0;

        for (unsigned i = 0; i < CAPACITY; ++i) {
            if (records[i].state != PROCESS_FREE)
                ++n;
        }

        return n;
    }

    int get_info(unsigned index, process_info *out) const {
        if (!out || index >= CAPACITY ||
            records[index].state == PROCESS_FREE)
            return 0;

        const process_info *p = &records[index];

        out->pid = p->pid;
        out->state = p->state;
        out->entry = p->entry;
        out->user_stack = p->user_stack;
        out->page_directory = p->page_directory;
        out->exit_code = p->exit_code;

        for (unsigned i = 0; i < PROCESS_NAME_MAX; ++i)
            out->name[i] = p->name[i];

        return 1;
    }

private:
    static const unsigned CAPACITY = 16;

    process_info records[CAPACITY];
    unsigned next_pid;
    int initialized;

    process_info *find(unsigned pid) {
        for (unsigned i = 0; i < CAPACITY; ++i) {
            if (records[i].state != PROCESS_FREE &&
                records[i].pid == pid)
                return &records[i];
        }

        return 0;
    }
};

static ProcessManager manager;

extern "C" void process_manager_init(void) {
    manager.initialize();
}

extern "C" int process_create(
    const char *name,
    unsigned entry,
    unsigned stack,
    unsigned directory
) {
    return manager.create(name, entry, stack, directory);
}

extern "C" int process_set_state(unsigned pid, unsigned state) {
    return manager.set_state(pid, state);
}

extern "C" int process_exit(unsigned pid, int code) {
    return manager.exit_process(pid, code);
}

extern "C" unsigned process_count(void) {
    return manager.count();
}

extern "C" int process_get_info(
    unsigned index,
    process_info *out
) {
    return manager.get_info(index, out);
}
