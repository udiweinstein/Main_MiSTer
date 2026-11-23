// Apple IIgs core: disk image API host (the Disks desk accessory, YANC).
//
// Shared by Main_MiSTer (support/a2/iigs_disks.cpp) and the Verilator sim
// (vsim/sim_main.cpp --disk-api), like the Uthernet II model.  The FPGA
// half is rtl/disk_api.sv + roms/dapi_helper.s in the Apple-IIgs core; the
// two meet in a DDR3 window (see the layout in disk_api.sv).
//
// The calls are the GS40 disk API (GS40 src/diskapi.c): ID, LIST, MOUNT,
// EJECT, DRIVES.  The MiSTer core has 4 drives, so DRIVES returns 4 entries:
//   drive 0 = S5D1 (3.5"), 1 = S6D1 (5.25"), 2 = S7D1, 3 = S7D2 (hard disks)

#ifndef IIGS_DISKS_MODEL_H
#define IIGS_DISKS_MODEL_H

#include <stdint.h>

#define DAPI_DDR_BASE  0x1FF20000u   // ARM physical address of the window
#define DAPI_WIN_SIZE  0x8000u       // bytes to map (the layout uses $0000-$4027)

typedef struct dapi_host
{
	void       *ctx;
	const char *dir;                 // disk images folder (full path, no '/' at the end)
	// insert path into drive 0-3; return 1 if it worked
	int        (*mount)(void *ctx, int drive, const char *path);
	void       (*eject)(void *ctx, int drive);
	// the image in drive 0-3: full path and size in bytes, or NULL if empty
	const char *(*image)(void *ctx, int drive, uint64_t *size);
} dapi_host;

// The DDR3 window, mapped (DAPI_WIN_SIZE bytes at DAPI_DDR_BASE)
void dapi_attach(volatile uint8_t *win);

// Call often.  Answers a waiting call.  Returns 1 while the core's API is up
// (it has written its MAGIC and GEN).
int dapi_poll(const dapi_host *h);

#endif
