/* Mapping of 9P2000.L Rlerror codes (Linux errno numbers) to QNX errno. */
#ifndef FS9P_ERRNO_MAP_H
#define FS9P_ERRNO_MAP_H

#include <stdint.h>

/* Returns the QNX errno for a Linux errno number; unknown codes give EIO. */
int fs9p_errno_from_linux(uint32_t e);

#endif /* FS9P_ERRNO_MAP_H */
