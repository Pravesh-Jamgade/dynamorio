//***************** "Hash join **********************
//	Implementation:	Marisa Sel Franco				*
//	HiPES - UFPR									*
//***************************************************

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include "hash_table.h"

//********************************************************************
//	1. Checks matches beetwen the largest and the smallest table keys
//********************************************************************
uint32_t verify(uint32_t key, uint32_t index, htype *table){
	htype *aux;
	aux = &table[index];

	if (table[index].key != 0) {
		for (int i = 0; i < table[index].hits; i++){
			if (aux->key == key){						// a match was found
				return key;
			}			
			aux = aux->next;
		}
	}
	return 0;											// it was a collision without match or the position is empty
}
	/*if (table[index].key == 0)						// position is empty
		return 0;
	else{
		for (int i = 0; i < table[index].hits; i++){
			if (aux->key == key){						// a match was found
				aux->match = 1;
				return key;
			}
			aux = aux->next;
		}
		return 0;										// it was a collision without match
	}
}*/

//*********************************************
//	2. Vector initialization function
//*********************************************
void init_array(uint32_t *out_custkey, long int l_tab2){
	uint32_t i = 0;

	for (i = 0; i < l_tab2; i++)
		out_custkey[i] = 0;								// this value cannot be a key, so it signals an empty index
	return;
}

//*********************************************
//	3. Print Query 10 results
//*********************************************
void print_query10(uint32_t *out_custkey, char **out_name, long int l_tab2){
	uint32_t i = 0;

	for (i = 0; i < l_tab2; i++){
		if (out_custkey[i] > 0){
			printf("\t%u \t| \t%s \n", out_custkey[i], out_name[i]);
		}
	}
	return;
}

//*********************************************
//	4. Print Query 11 results
//*********************************************
void print_query11(uint32_t *out_partkey, long int l_tab2){
	uint32_t i = 0;

	for (i = 0; i < l_tab2; i++){
		if (out_partkey[i] > 0){
			printf("\t%u \t|\n", out_partkey[i]);
		}
	}
	return;
}