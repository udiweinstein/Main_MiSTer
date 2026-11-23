// Apple IIgs core: disk image API host.  See iigs_disks_model.h.
//
// Shared by Main_MiSTer (support/a2/iigs_disks.cpp) and the Verilator sim
// (vsim/sim/).  Keep the two copies identical.
//
// DDR3 window (rtl/disk_api.sv), byte offsets:
//   $0000-$1FFF  data returned (LIST / DRIVES entries)    = CPU $FB:8000
//   $2000        the call: +0 command, +1 bank B, +2/+3 block address,
//                +$10 the parameter block (68 bytes)       = CPU $FB:A000
//   $2080        the answer: +0 A, +1 1 = error (carry), +2 X,
//                +3 flags (bit 0 = set X), +4/+5 bytes to copy back
//   $4000 REQ (FPGA->ARM)  $4008 ACK (ARM->FPGA)  $4010 GEN (FPGA->ARM)
//   $4018 HOST (ARM->FPGA, "DAPI" << 32 | GEN)  $4020 MAGIC "IIGSDAP1"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

#include "iigs_disks_model.h"

#define OFF_DATA   0x0000
#define OFF_CALL   0x2000
#define OFF_PARM   0x2010
#define OFF_ANS    0x2080
#define OFF_REQ    0x4000
#define OFF_ACK    0x4008
#define OFF_GEN    0x4010
#define OFF_HOST   0x4018
#define OFF_MAGIC  0x4020

#define MAX_ENTRIES  255
#define ENTRY_SIZE   32
#define NAME_MAX_LEN 26          // LIST names; DRIVES allows 27

enum { ERR_BADCMD = 1, ERR_NOFOLDER, ERR_BADENTRY, ERR_BADDRIVE, ERR_NOTIMAGE, ERR_PROTECTED };

static const int drive_sd[4] = { 0x51, 0x61, 0x71, 0x72 };   // slot*16 + drive

static volatile uint8_t *win;
static uint32_t last_gen;
static uint32_t handled;

typedef struct { char *name; int dir; uint64_t size; } entry;
static entry list[MAX_ENTRIES];
static int   list_num;
static char  list_dir[1024];

void dapi_attach(volatile uint8_t *w)
{
	win = w;
	last_gen = 0;
}

static uint32_t rd32(int off)
{
	return win[off] | (win[off + 1] << 8) | (win[off + 2] << 16) | ((uint32_t)win[off + 3] << 24);
}

static void wr32(int off, uint32_t v)
{
	for (int i = 0; i < 4; i++) win[off + i] = (uint8_t)(v >> (8 * i));
}

static int is_image_name(const char *name)
{
	static const char *exts[] = { ".po", ".2mg", ".2img", ".dsk", ".do", ".hdv", ".woz", ".nib", ".img", 0 };
	const char *dot = strrchr(name, '.');
	if (!dot) return 0;
	for (int i = 0; exts[i]; i++) if (!strcasecmp(dot, exts[i])) return 1;
	return 0;
}

// The slot an image belongs in, as GS40 guesses it: 6 = 5.25", 5 = 3.5",
// 7 = hard disk.  WOZ and 2MG headers first, else the file size.
static int guess_slot(const char *path, uint64_t size)
{
	uint8_t h[64];
	memset(h, 0, sizeof h);
	FILE *f = fopen(path, "rb");
	if (f)
	{
		if (fread(h, 1, sizeof h, f)) {}
		fclose(f);
	}
	if (!memcmp(h, "WOZ", 3)) return h[21] == 1 ? 6 : 5;
	if (!memcmp(h, "2IMG", 4))
	{
		uint32_t blocks = h[0x14] | (h[0x15] << 8) | (h[0x16] << 16) | ((uint32_t)h[0x17] << 24);
		uint32_t len    = h[0x1C] | (h[0x1D] << 8) | (h[0x1E] << 16) | ((uint32_t)h[0x1F] << 24);
		if (blocks) size = (uint64_t)blocks * 512;
		else if (len) size = len;
	}
	return size <= 250000 ? 6 : size <= 1700000 ? 5 : 7;
}

static int drive_with(const dapi_host *h, const char *path)
{
	for (int d = 0; d < 4; d++)
	{
		uint64_t sz;
		const char *p = h->image(h->ctx, d, &sz);
		if (p && !strcmp(p, path)) return drive_sd[d];
	}
	return 0;
}

static void put_name(int off, const char *name, int max)
{
	int len = (int)strlen(name);
	if (len > max) len = max;
	win[off] = len;
	for (int i = 0; i < max; i++)
	{
		int c = 0;
		if (i < len)
		{
			c = (uint8_t)name[i];
			if (c < 0x20 || c > 0x7e) c = '?';
		}
		win[off + 1 + i] = c;
	}
}

static unsigned kb_of(uint64_t size)
{
	uint64_t kb = (size + 1023) >> 10;
	return kb > 65535 ? 65535 : (unsigned)kb;
}

static int compare(const void *a, const void *b)
{
	const entry *ea = (const entry *)a, *eb = (const entry *)b;
	if (ea->dir != eb->dir) return eb->dir - ea->dir;   // folders first
	return strcasecmp(ea->name, eb->name);
}

static void free_list(void)
{
	for (int i = 0; i < list_num; i++) free(list[i].name);
	list_num = 0;
}

static int cmd_list(const dapi_host *h, int *len)
{
	char path[2048], sub[80];
	int max = win[OFF_PARM + 2];
	int sub_len = win[OFF_PARM + 3] & 0x3f;
	for (int i = 0; i < sub_len; i++) sub[i] = win[OFF_PARM + 4 + i] & 0x7f;
	sub[sub_len] = 0;
	if (strstr(sub, "..") || sub[0] == '/') return -ERR_NOFOLDER;   // stay in the folder

	snprintf(list_dir, sizeof list_dir, "%s/%s", h->dir, sub);
	if (sub_len && sub[sub_len - 1] != '/') strncat(list_dir, "/", sizeof list_dir - strlen(list_dir) - 1);

	free_list();
	DIR *d = opendir(list_dir);
	if (!d) return -ERR_NOFOLDER;
	struct dirent *de;
	while ((de = readdir(d)) && list_num < MAX_ENTRIES)
	{
		struct stat st;
		if (de->d_name[0] == '.') continue;
		snprintf(path, sizeof path, "%s%s", list_dir, de->d_name);
		if (stat(path, &st)) continue;
		int dir = S_ISDIR(st.st_mode);
		if (!dir && !(S_ISREG(st.st_mode) && is_image_name(de->d_name))) continue;
		list[list_num].name = strdup(de->d_name);
		list[list_num].dir = dir;
		list[list_num].size = dir ? 0 : (uint64_t)st.st_size;
		list_num++;
	}
	closedir(d);
	qsort(list, list_num, sizeof(entry), compare);

	int num = list_num < max ? list_num : max;
	for (int i = 0; i < num; i++)
	{
		int e = OFF_DATA + i * ENTRY_SIZE;
		unsigned kb = list[i].dir ? 0 : kb_of(list[i].size);
		snprintf(path, sizeof path, "%s%s", list_dir, list[i].name);
		win[e + 0] = list[i].dir ? 1 : 2;
		win[e + 1] = list[i].dir ? 0 : drive_with(h, path);
		win[e + 2] = kb & 0xff;
		win[e + 3] = kb >> 8;
		put_name(e + 4, list[i].name, NAME_MAX_LEN);
		win[e + 31] = list[i].dir ? 0 : guess_slot(path, list[i].size);
	}
	*len = num * ENTRY_SIZE;
	return num;
}

static int drive_index(int sd)
{
	for (int d = 0; d < 4; d++) if (drive_sd[d] == sd) return d;
	return -1;
}

static int cmd_mount(const dapi_host *h)
{
	char path[2048];
	int idx = win[OFF_PARM + 0], sd = win[OFF_PARM + 1], prot = win[OFF_PARM + 2];
	if (idx >= list_num || list[idx].dir) return -ERR_BADENTRY;
	snprintf(path, sizeof path, "%s%s", list_dir, list[idx].name);

	int drive = -1;
	if (sd == 0)
	{
		// the first empty drive of the image's slot, else the first one that
		// is not protected
		int slot = guess_slot(path, list[idx].size);
		for (int pass = 0; pass < 2 && drive < 0; pass++)
			for (int d = 0; d < 4; d++)
			{
				uint64_t sz;
				if ((drive_sd[d] >> 4) != slot || drive_sd[d] == prot) continue;
				if (pass == 0 && h->image(h->ctx, d, &sz)) continue;
				drive = d;
				break;
			}
		if (drive < 0) return -ERR_PROTECTED;
	}
	else
	{
		drive = drive_index(sd);
		if (drive < 0) return -ERR_BADDRIVE;
		if (sd == prot) return -ERR_PROTECTED;
	}
	printf("IIgs disk API: insert %s into s%dd%d\n", path, drive_sd[drive] >> 4, drive_sd[drive] & 15);
	if (!h->mount(h->ctx, drive, path)) return -ERR_NOTIMAGE;
	return drive_sd[drive];
}

static int cmd_eject(const dapi_host *h)
{
	int sd = win[OFF_PARM + 1];
	int drive = drive_index(sd);
	if (drive < 0) return -ERR_BADDRIVE;
	uint64_t sz;
	if (h->image(h->ctx, drive, &sz))
	{
		printf("IIgs disk API: eject s%dd%d\n", sd >> 4, sd & 15);
		h->eject(h->ctx, drive);
	}
	return sd;
}

static int cmd_drives(const dapi_host *h, int *len)
{
	for (int d = 0; d < 4; d++)
	{
		int e = OFF_DATA + d * ENTRY_SIZE;
		uint64_t size = 0;
		const char *p = h->image(h->ctx, d, &size);
		const char *name = "";
		if (p)
		{
			const char *slash = strrchr(p, '/');
			name = slash ? slash + 1 : p;
		}
		unsigned kb = p ? kb_of(size) : 0;
		win[e + 0] = drive_sd[d];
		win[e + 1] = p ? 1 : 0;
		win[e + 2] = kb & 0xff;
		win[e + 3] = kb >> 8;
		put_name(e + 4, name, NAME_MAX_LEN + 1);
	}
	*len = 4 * ENTRY_SIZE;
	return 4;
}

int dapi_poll(const dapi_host *h)
{
	if (!win) return 0;
	if (memcmp((const void *)(win + OFF_MAGIC), "IIGSDAP1", 8)) return 0;

	uint32_t gen = rd32(OFF_GEN);
	if (!gen) return 0;
	if (gen != last_gen)
	{
		// new core session: nothing of the old one is pending
		last_gen = gen;
		handled = rd32(OFF_REQ);
		free_list();
		wr32(OFF_HOST + 4, 0x49504144);   // "DAPI"
		wr32(OFF_HOST, gen);
		__sync_synchronize();
	}

	uint32_t req = rd32(OFF_REQ);
	if (req == handled) return 1;
	__sync_synchronize();

	int cmd = win[OFF_CALL], len = 0, x = -1, ret;
	switch (cmd)
	{
	case 0: ret = 'K'; x = 2; break;            // ID: API 2, B = bank
	case 1: ret = cmd_list(h, &len); break;
	case 2: ret = cmd_mount(h); break;
	case 3: ret = cmd_eject(h); break;
	case 4: ret = cmd_drives(h, &len); break;
	default: ret = -ERR_BADCMD; break;
	}
	if (ret < 0) len = 0;
	win[OFF_ANS + 0] = (uint8_t)(ret < 0 ? -ret : ret);
	win[OFF_ANS + 1] = ret < 0 ? 1 : 0;
	win[OFF_ANS + 2] = (uint8_t)x;
	win[OFF_ANS + 3] = x >= 0 ? 1 : 0;
	win[OFF_ANS + 4] = len & 0xff;
	win[OFF_ANS + 5] = len >> 8;
	__sync_synchronize();
	wr32(OFF_ACK + 4, 0);
	wr32(OFF_ACK, req);
	handled = req;
	return 1;
}
