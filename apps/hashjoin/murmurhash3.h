//*************** "Murmurhash 3" ********************
//	Implementation:	Marisa Sel Franco				*
//	HiPES - UFPR									*
//***************************************************

#ifndef __MURMURHASH3__
#define __MURMURHASH3__

#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include <math.h>

uint32_t murmur3_32(uint32_t* key, size_t len, uint32_t seed);
uint32_t murmurhash_index(uint32_t key, uint32_t size);

#endif