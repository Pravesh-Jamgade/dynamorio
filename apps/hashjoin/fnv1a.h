//******************* "FNV-1A" **********************
//	Implementation:	Marisa Sel Franco				*
//	HiPES - UFPR									*
//***************************************************
#ifndef __FNV1A__
#define __FNV1A__

#include <sys/types.h>
// the FNV_prime is dependent on n, the size of the hash
// the prime is calculated using the formula 2^(n−8)+2^8
#define FNV_32A_PRIME ((uint32_t)0x01000193)		// 32-bit magic FNV-1a prime = 16777619

// the offset_basis for FNV-1 is dependent on n, the size of the hash
#define FNV_32A_INIT ((uint32_t)0x811c9dc5)			// 32 bit offset_basis = 2166136261

// for bit values of hash size that are power of 2
#define BIT_MASK(x) (((u_int32_t) 1 << (x)) - 1)	// i.e., (u_int32_t)0xffffff

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>

uint32_t fnv1a_32(uint32_t *key, size_t len, uint32_t seed);
int is_power2(uint32_t size);
unsigned int count_bits(uint32_t size);
uint32_t adapt_hash(uint32_t size, uint32_t hash);
uint32_t fnv1a_index(uint32_t key, uint32_t size);

#endif