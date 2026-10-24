//******************* "FNV-1A" **********************
//	Implementation:	Marisa Sel Franco				*
//	HiPES - UFPR									*
//***************************************************

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>
#include "fnv1a.h"

//****************************************************
//	1. FNV-1a function for 32-bit integer (uint32_t)
//****************************************************

uint32_t fnv1a_32(uint32_t *key, size_t len, uint32_t seed){
	uint8_t byte[4];
	uint32_t hash = seed;

	// converting/ breaking up an integer into four bytes, using bit mask
	byte[0] = (*key & 0xff);
	byte[1] = ((*key >> 8) & 0xff);
	byte[2] = ((*key >> 16) & 0xff);
	byte[3] = ((*key >> 24) & 0xff);

	// FNV-1a hash each byte of the number
	for(int i = 3; i >= 0; i--){
		hash ^= (uint32_t)byte[i]; 				// xor the bottom with the current byte
		hash *= FNV_32A_PRIME;					// multiply by the 32 bit FNV magic prime mod 2^32
	}
	return hash;
}

//****************************************************
//	2. It checks if the number is a power of 2
//****************************************************
int is_power2(uint32_t size){
	int flag = 0;								// assumes it is not a power of 2
	uint8_t firstbyte = (size - 1) & 0xff;		// extract n-1 first byte

	if (size == 0)
		return flag;
	if (ceil(log2(size)) == floor(log2(size))){ // the number is a power of 2 if log2 is an integer
		flag = 1;
		return flag;
	}
	return flag;
}

//****************************************************
//	3. It finds total bit in given number 
//****************************************************
unsigned int count_bits(uint32_t size){

	unsigned int count;

	count = (int)floor(log2(size));				// log function in base 2 and take only integer part 
	return count;
	/*
	unsigned int count = 0;

	while (size){
		count++;
		size >>= 1;
	}
	return count;
*/
}

//****************************************************
//	4. It adapts the hash size when it's a power of 2
//****************************************************
uint32_t adapt_hash(uint32_t size, uint32_t hash){
	unsigned int nbits;

	nbits = count_bits(size);
	//printf("O tamanho da tabela (%u) tem %u bits. \n", size, nbits);
	// to produce a x bit FNV-1 hash in C
	if (nbits >= 16){							// if 16 <= x <= 32 bits (maximum for this implementation)
		hash = (hash >> nbits) ^ (hash & BIT_MASK(nbits));	// xor-fold fold a 32 bit FNV-1a hash
		return hash;
	}
 	// the above process assumes that you are using an FNV-1a hash  
	// that, at most, twice as large as the x-bits that you need
	// for x < 16, there is no 16 bit (or less) FNV-1 hash to use
	else {										// if x < 16 bits
		hash = (((hash >> nbits) ^ hash) & BIT_MASK(nbits));
		return hash;
	}
}

//**************************************************************************
//	5. Function that creates the hash value and the index using FNV-1a
//**************************************************************************
uint32_t fnv1a_index(uint32_t key, uint32_t size){
	uint32_t hash, index;
	size_t len;
	int flag;

	len = sizeof(key);
	hash = fnv1a_32(&key, len, FNV_32A_INIT); 	// "hash" receives the return of fnv1a_32
	
	// fnv-1a is designed for hash sizes that are a power of 2
	flag = is_power2(size);						// so, this function checks if the hash size is a power of 2
	if (flag == 1)								// the hash size is a power of 2
		index = adapt_hash (size, hash);
	else										// the hash size isn't a power of 2
		index = hash % size;
	return index;
}