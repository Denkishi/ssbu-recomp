/* Host runtime interface for the fully lifted game. */
#ifndef RECOMP_HOSTRT_H
#define RECOMP_HOSTRT_H

#include <stdio.h>
#include <stdarg.h>
#include "runtime.h"

#define GUEST_BASE 0x7100000000ULL
#define IMPORT_BASE 0x7300000000ULL      /* placeholder addresses for symbols of other modules */
#define IMPORT_SLOT 256ULL

typedef void (*ImportFn)(Ctx *c);
typedef struct { const char *name; ImportFn fn; } ImportDef;

#define HOSTFN_BASE 0x7400000000ULL      /* placeholder addresses for host functions handed out at run time */

uint64_t hostfn_address(const char *name, ImportFn fn);
const char *hostfn_current_name(void);
unsigned long long hostfn_current_calls(void);
void print_hostfn_summary(FILE *out);
void report_and_exit(int code);
void runtime_init(const char *elf, const char *trace_path);
void *import_data(const char *name);
/* Make an imported data symbol resolve to host memory (call before runtime_init). */
void import_set_address(const char *name, void *addr);
typedef struct { const char *name; const unsigned char *bytes; unsigned size; } SdkData;
extern const SdkData sdk_data[];
extern const int sdk_data_count;
void register_imports(const ImportDef *defs, int count);
ImportFn import_lookup(const char *name);    /* registered handler of an imported function, or NULL */
void call_guest(Ctx *c, uint64_t addr);
uint64_t guest_function_of(uint64_t host_rip);
void print_import_summary(FILE *out);
void trace_printf(const char *fmt, ...);

/* Game builds (hostrt.c, docs/VERSIONS.md). 13.0.4 is the reference: every game address written in the runtime is a
 * 13.0.4 address, and GA() gives the address of the same thing in the build that runs (identity on 13.0.4). The layout
 * of main (segments, relocation and symbol tables, init array) is read from main.elf at load, not written down. */
enum { GAME_13_0_4, GAME_13_0_5 };
extern int g_game_build;
typedef struct {
    uint64_t text_size, ro_start, data_start, file_image;         /* offsets from GUEST_BASE; file_image = bytes stored in the ELF */
    uint64_t rela, relasz, jmprel, pltrelsz, symtab, strtab, init_array, init_arraysz;   /* from the dynamic section */
} MainLayout;
extern MainLayout g_main;
uint64_t ga(uint64_t addr_13_0_4);
#define GA(a) ga(a)

#endif
