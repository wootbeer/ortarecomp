/*
 * Orta's dispatch entry points under its own names, so that another program
 * (pd1, gen-pd1/pd1_names.h) can link beside it. CMake force-includes this
 * into Orta's generated code and its sources in src/guest/; the unprefixed
 * names, which the kernel layer calls, are src/guest/program.c's.
 * Keep in step with DISPATCH_NAMES in tools/recomp.py.
 */
#pragma once
#define recomp_call_all orta_recomp_call_all
#define recomp_dispatch_flat_bytes orta_recomp_dispatch_flat_bytes
#define recomp_dispatch_init orta_recomp_dispatch_init
#define recomp_get_count orta_recomp_get_count
#define recomp_lookup orta_recomp_lookup
#define recomp_lookup_manual orta_recomp_lookup_manual
