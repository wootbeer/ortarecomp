#ifndef PDO_GUEST_H
#define PDO_GUEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Map the Xbox address space from the XBE, set up the kernel layer and the
 * function dispatch table. Does not run any game code. */
bool pdo_guest_init(const void *xbe_data, size_t xbe_size,
                    const char *game_dir, const char *save_dir);
/* The game files' folder given to pdo_guest_init. */
const char *pdo_guest_game_dir(void);

/* Run the title from its entry point. Returns when the title exits. */
void pdo_guest_run(uint32_t entry);

/* The guest CPU's registers live per host thread, and pdo_guest_init sets
 * them up on the thread that calls it. To run the title on another thread
 * (made after pdo_guest_init: the Xbox address space has to be mapped at
 * its fixed addresses before a big thread stack can land on them), save
 * them there and load them on the new thread before pdo_guest_run. The
 * guest CPU lock, which the setup gives the calling thread, goes with
 * them. */
typedef struct pdo_guest_cpu {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi, fs_base;
} pdo_guest_cpu;
void pdo_guest_cpu_save(pdo_guest_cpu *c);
void pdo_guest_cpu_load(const pdo_guest_cpu *c);

void pdo_guest_shutdown(void);

/* Write every game file the title opens, once each, to list_path. */
void pdo_guest_log_opened_files(const char *list_path);

/* Image hash of the XBE the code was generated from (from pdo_build_info.h),
 * as lower-case hex. */
const char *pdo_guest_built_from(void);

#endif
