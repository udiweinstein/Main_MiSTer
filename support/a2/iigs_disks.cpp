// Apple IIgs core: disk image API (GS40's), Main side.
//
// The core's built-in Disks desk accessory (Ctrl-OA-Esc > Disks) and YANC
// list the disk images on the SD card and insert them into the IIgs drives
// through this API. The FPGA half is rtl/disk_api.sv + roms/dapi_helper.s
// (bank $FB, loaded as boot2.rom); the calls are answered by the shared
// model iigs_disks_model.cpp (the same file the Verilator sim builds),
// through a DDR3 window at 0x1FF20000. Without boot2.rom the core never
// writes the window and this does nothing.
//
// Images come from games/Apple-IIgs (with its sub-folders). The 4 drives
// are the core's SD slots: S5D1 3.5" = S2, S6D1 5.25" = S3, S7D1 = S0,
// S7D2 = S1; inserting goes through user_io_file_mount like the OSD.

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

#include "../../user_io.h"
#include "../../file_io.h"
#include "../../shmem.h"
#include "iigs_disks.h"
#include "iigs_disks_model.h"

static const int drive_slot[4] = { 2, 3, 0, 1 };   // SD slot of drive 0-3

static volatile uint8_t *win;
static char images_dir[1024];
static char mounted[4][1024];        // full path of what the API inserted

static int cb_mount(void *, int drive, const char *path)
{
	int slot = drive_slot[drive];
	user_io_file_mount(path, slot);
	fileTYPE *f = get_image(slot);
	if (!f->size)
	{
		mounted[drive][0] = 0;
		return 0;
	}
	snprintf(mounted[drive], sizeof mounted[drive], "%s", path);
	return 1;
}

static void cb_eject(void *, int drive)
{
	user_io_file_mount("", drive_slot[drive]);
	mounted[drive][0] = 0;
}

// The image in a drive: the full path if the API inserted it, else (inserted
// from the OSD) its file name, which is all Main keeps.
static const char *cb_image(void *, int drive, uint64_t *size)
{
	fileTYPE *f = get_image(drive_slot[drive]);
	if (!f->size)
	{
		mounted[drive][0] = 0;
		return NULL;
	}
	*size = f->size;
	const char *slash = strrchr(mounted[drive], '/');
	if (mounted[drive][0] && !strcmp(slash ? slash + 1 : mounted[drive], f->name))
		return mounted[drive];
	return f->name;
}

static dapi_host host = { NULL, images_dir, cb_mount, cb_eject, cb_image };

void iigs_disks_poll(void)
{
	if (!win)
	{
		win = (volatile uint8_t *)shmem_map(DAPI_DDR_BASE, DAPI_WIN_SIZE);
		if (!win) return;
		dapi_attach(win);
	}
	snprintf(images_dir, sizeof images_dir, "%s", getFullPath(HomeDir(NULL)));
	dapi_poll(&host);
}
