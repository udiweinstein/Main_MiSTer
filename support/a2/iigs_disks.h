#ifndef IIGS_DISKS_H
#define IIGS_DISKS_H

// Apple IIgs core: disk image API for the Disks desk accessory and YANC.
// Call every pass of the user_io poll loop (with the Uthernet II poll).
void iigs_disks_poll(void);

#endif
