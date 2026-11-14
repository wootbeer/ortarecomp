/*
 * Which recompiled program runs: Orta (default.xbe) or the original Panzer
 * Dragoon (pd1.xbe, from Pandora's Box).
 *
 * Each program's generated code and its own sources are compiled with its
 * names header force-included, so their dispatch is orta_recomp_lookup and
 * pd1_recomp_lookup (and pd1's functions pd1_sub_XXXXXXXX). The kernel layer
 * and the host's shared code call the unprefixed names; they are defined
 * here and go to the program selected.
 */

#include "guest/program.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef void (*recomp_func_t)(void);

extern recomp_func_t orta_recomp_lookup(uint32_t xbox_va);
extern recomp_func_t orta_recomp_lookup_manual(uint32_t xbox_va);
extern int orta_recomp_dispatch_init(void);
extern size_t orta_recomp_dispatch_flat_bytes(void);
extern const char *orta_built_from(void);
extern void orta_pdo_dump_guest_waits(void);
#ifdef PDO_HAVE_PD1
extern void pd1_pdo_dump_guest_waits(void);
extern recomp_func_t pd1_recomp_lookup(uint32_t xbox_va);
extern recomp_func_t pd1_recomp_lookup_manual(uint32_t xbox_va);
extern int pd1_recomp_dispatch_init(void);
extern size_t pd1_recomp_dispatch_flat_bytes(void);
extern const char *pd1_built_from(void);
#endif

static pdo_program active = PDO_PROGRAM_ORTA;

static const char *const names[PDO_PROGRAM_COUNT] = { "orta", "pd1" };
static const char *const xbes[PDO_PROGRAM_COUNT] = { "default.xbe", "pd1.xbe" };

const char *pdo_program_name(pdo_program p) { return names[p]; }
const char *pdo_program_xbe(pdo_program p) { return xbes[p]; }

bool pdo_program_from_name(const char *name, pdo_program *out)
{
    int i;

    for (i = 0; i < PDO_PROGRAM_COUNT; i++) {
        if (strcmp(name, names[i]) == 0) {
            *out = (pdo_program)i;
            return true;
        }
    }
    return false;
}

const char *pdo_program_built_from(pdo_program p)
{
    switch (p) {
    case PDO_PROGRAM_ORTA:
        return orta_built_from();
#ifdef PDO_HAVE_PD1
    case PDO_PROGRAM_PD1:
        return pd1_built_from();
#endif
    default:
        return NULL;
    }
}

void pdo_program_select(pdo_program p) { active = p; }
pdo_program pdo_program_active(void) { return active; }

recomp_func_t recomp_lookup(uint32_t xbox_va)
{
#ifdef PDO_HAVE_PD1
    if (active == PDO_PROGRAM_PD1)
        return pd1_recomp_lookup(xbox_va);
#endif
    return orta_recomp_lookup(xbox_va);
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
#ifdef PDO_HAVE_PD1
    if (active == PDO_PROGRAM_PD1)
        return pd1_recomp_lookup_manual(xbox_va);
#endif
    return orta_recomp_lookup_manual(xbox_va);
}

int recomp_dispatch_init(void)
{
#ifdef PDO_HAVE_PD1
    if (active == PDO_PROGRAM_PD1)
        return pd1_recomp_dispatch_init();
#endif
    return orta_recomp_dispatch_init();
}

size_t recomp_dispatch_flat_bytes(void)
{
#ifdef PDO_HAVE_PD1
    if (active == PDO_PROGRAM_PD1)
        return pd1_recomp_dispatch_flat_bytes();
#endif
    return orta_recomp_dispatch_flat_bytes();
}

/* F11 and hangs: the guest threads' waits (src/hle/wait_trace.c). */
void pdo_dump_guest_waits(void)
{
#ifdef PDO_HAVE_PD1
    if (active == PDO_PROGRAM_PD1) {
        pd1_pdo_dump_guest_waits();
        return;
    }
#endif
    orta_pdo_dump_guest_waits();
}
