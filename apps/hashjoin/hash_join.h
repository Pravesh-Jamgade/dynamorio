//***************** "Hash join **********************
//	Implementation:	Marisa Sel Franco				*
//	HiPES - UFPR									*
//***************************************************

#ifndef __HASHJOIN__
#define __HASHJOIN__

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include "hash_table.h"

uint32_t *out_custkey, *out_partkey;
char **out_name;

uint32_t verify(uint32_t key, uint32_t index, htype *table);	
void init_array(uint32_t *out_custkey, long int l_tab2);
void print_query10(uint32_t *out_custkey, char **out_name, long int l_tab2);
void print_query11(uint32_t *out_partkey, long int l_tab2);

#endif