//*************** "Murmurhash 3" ********************
//	Implementation:	Marisa Sel Franco				*
//	HiPES - UFPR									*
//***************************************************

/* Note: In this version, all arithmetic is performed 
with unsigned 32-bit integers. In the case of overflow, 
the result is reduced modulo 2^32. 

It works with bitwise operations.This implementation 
is for little-endian CPUs.

MurmurHash3 for 32-bit  has the lowest throughput, 
but also the lowest latency. If you're making a
hash table that usually has small keys, this is 
probably the one you want to use on 32-bit machines. 
It has a 32-bit output.

Input: key, len, seed. */

#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include <math.h>

//***************************************************
//	1. MurmurHash3 scramble function for 32-bit, x86
//***************************************************
/* This function encode next 4 byte chunk of 'key':
the remaining bytes*/
static inline uint32_t murmur_32_scramble(uint32_t k){
	uint32_t c1 = 0xcc9e2d51;
	uint32_t c2 = 0x1b873593;

	k *= c1;								// 1st multiplication
	k = (k << 15) | (k >> 17);				// rotation
	k *= c2;								// 2nd multiplication
	return k;
}

//***************************************************
//	2. MurmurHash3 function for 32-bit, x86
//***************************************************
//uint32_t murmur3_32(long int* key, size_t len, uint32_t seed){
uint32_t murmur3_32(uint32_t* key, size_t len, uint32_t seed){
	
	uint32_t hash = seed;
	uint32_t k;

	// Read in groups of 4 (fourByteChunk of the key)
	for (size_t i = len >> 2; i; i--){
	
		k = *((uint32_t*)key);				// k = fourByteChunk of the key
		key += sizeof(uint32_t);
		hash ^= murmur_32_scramble(k);		// hash XOR remaining bytes
		hash = (hash << 13) | (hash >> 19); // rotation
		hash = hash * 5 + 0xe6546b64;		// (hash multiplication with 5) + other constant
	}

	// Read the remaing bytes in key, the rest
	k = 0;
	for (size_t i = len & 3; i; i--) {
		k <<= 8;
		k |= key[i - 1];
	}

	/* A swap is *not* necessary here because the preceeding 
	loop already places the low bytes in the low places 
	according to whatever endianness we use. Swaps only apply 
	when the memory is copied in a chunk. */
	hash ^= murmur_32_scramble(k);			// hash XOR remaining bytes 
	hash ^= len;							// hash XOR len
	hash ^= hash >> 16;						// hash XOR (hash >> 16)
	hash *= 0x85ebca6b;						// hash multiplication with 0x85ebca6b
	hash ^= hash >> 13;						// hash XOR (hash >> 13)
	hash *= 0xc2b2ae35;						// hash multiplication with 0xc2b2ae35
	hash ^= hash >> 16;						// hash XOR (hash >> 16)
	return hash;
}

//**************************************************************************
//	3. Function that creates the hash value and the index using Murmurhash3
//**************************************************************************
uint32_t murmurhash_index(uint32_t key, uint32_t size){
	uint32_t seed = 42, hash, index;
	size_t len;

	len = sizeof(key);
	hash = murmur3_32(&key, len, seed);		// "hash" receives the return of murmur3_32
	index = hash % size;
	return index;
}