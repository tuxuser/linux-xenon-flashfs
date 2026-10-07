/* sha1.h - tiny portable SHA-1 (userspace and kernel module share it) */
#ifndef _FLASHFS_SHA1_H
#define _FLASHFS_SHA1_H

#include "flashfs_priv.h"

void flashfs_sha1(const void *data, size_t len, uint8_t out[20]);

#endif
