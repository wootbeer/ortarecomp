/*
 * An address in the program the code is compiled for.
 *
 * src/hle/ is compiled once for each program on the disc: Orta, and pd1
 * (the original Panzer Dragoon), whose build force-includes
 * src/pd1/orta_hooks.h, which defines PDO_PROGRAM_PD1. Hooks are written
 * with Orta's addresses and that header maps them; data addresses are
 * written as PROG_VA(Orta's, pd1's), both checked by tools/program_map.py.
 * 0 for pd1: Orta's own game code, which pd1 does not have (a caller
 * check, say); such a test then never matches.
 */
#ifndef PDO_PROGRAM_VA_H
#define PDO_PROGRAM_VA_H

#ifdef PDO_PROGRAM_PD1
#define PROG_VA(orta, pd1) (pd1)
#else
#define PROG_VA(orta, pd1) (orta)
#endif

#endif
