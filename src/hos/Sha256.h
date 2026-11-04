/* SHA-256 (Sha256.c). */
#ifndef HOS_SHA256_H
#define HOS_SHA256_H

#include <stddef.h>
#include <stdint.h>

void hos_sha256(const void* data, size_t size, uint8_t out[32]);

#endif
