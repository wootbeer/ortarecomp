#ifndef PDO_PROGRAM_H
#define PDO_PROGRAM_H

#include <stdbool.h>

/* The programs on the Orta disc that this build can run. Each is
 * recompiled on its own (tools/recomp.py --program) into its own folder and
 * links in under its own names (gen/orta_names.h, gen-pd1/pd1_names.h). */
typedef enum pdo_program {
    PDO_PROGRAM_ORTA,   /* default.xbe: Panzer Dragoon Orta */
    PDO_PROGRAM_PD1,    /* pd1.xbe: Panzer Dragoon, from Pandora's Box */
    PDO_PROGRAM_COUNT
} pdo_program;

/* "orta", "pd1": --program and the logs. */
const char *pdo_program_name(pdo_program p);
/* Parse a name; false if it is none of them. */
bool pdo_program_from_name(const char *name, pdo_program *out);
/* Its XBE in the game files: "default.xbe", "pd1.xbe". */
const char *pdo_program_xbe(pdo_program p);
/* Image hash of the XBE it was generated from, or NULL when this build does
 * not have it (gen-pd1/ was not generated). */
const char *pdo_program_built_from(pdo_program p);

/* The program the guest runs: recomp_lookup and recomp_lookup_manual, which
 * the kernel layer calls (thread starts, APCs, callbacks), go to its tables.
 * Set before pdo_guest_init. */
void pdo_program_select(pdo_program p);
pdo_program pdo_program_active(void);

#endif
