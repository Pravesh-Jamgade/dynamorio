//***************** "Hash table *********************
//	Implementation:	Marisa Sel Franco				*
//	HiPES - UFPR									*
//***************************************************

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include "hash_table.h"

//*********************************************
//	1. Hash table initialization function
//*********************************************
//void init_table(htype *table, long int size){
void init_table(htype *table, uint32_t size){
	register uint32_t i;
	for (i = 0; i < size; i++){
		table[i].key = 0;								// this value cannot be created, so it signals an empty index
		table[i].hits = 0;								// it signals that the index was never accessed
		//table[i].match = 0;							// it signals that the key doesn't have a correspondence
		table[i].next = NULL;							// end of hash sequence
	}
	return;
}

//*********************************************
//	2. Counter initialization function
//*********************************************
void init_counter(ctype *counter){
	counter->collision = 0;
	counter->number_repetition = 0;
	return;
}

//*********************************************
//	3. Store value in hash table function
//*********************************************
void store(uint32_t key, uint32_t index, htype *table, ctype *counter){
	htype *node;

	if (table[index].key == 0){						// position is empty
		table[index].key = key;						// key is stored
		table[index].hits++;
		return;
	}
	// there's a collision or a pseudo collision (repeated pseudo number)
	// if there's a pseudo collision in the hash table vector
	if (table[index].key == key){					// a pseudo collision was found
		counter->number_repetition++;
		return;
	}
	// else, a collision list is created or a value is added in the collision list
	else{
		node = (malloc(sizeof(htype)));				
		if (!node){
			printf("Erro. Espaço de memória insuficiente para criação da tabela de colisão.\n");
			return;
		} 
		node->key = key;
		htype *old, *start;
		start = &table[index];
		//printf("Houve uma colisão no index %u\n. Chaves guardadas:", index); 
		// it finds the end of the list
		while (start){
			old = start;
			if (start->key == key){
				counter->number_repetition++;
				return;
			}
			start = start->next;
		}
		// union with the new node
		old->next = node;
		node->next = NULL;
		counter->collision++;							// increment of the collision counter
		table[index].hits++;
		return;
	}
}

//*********************************************
//	4. Hits counter initialization function
//*********************************************
void init_hits_counter(int *hits_counter){
	register int i;

	for (i = 0; i < 66; i++)
		hits_counter[i] = 0;
	return;
}

//*********************************************
//	5. Histogram generator function
//*********************************************
void histogram(htype *table, int *hits_counter, uint32_t size){
	register uint32_t i;

	printf("NÚMERO DE ACESSOS DE CADA POSIÇÃO DA TABELA, EM ORDEM: \n");
	for (i = 0; i < size; i++){
		if (table[i].hits <= 64){
			hits_counter[table[i].hits]++;
			printf("%d \n", table[i].hits);
		}
		else{
			hits_counter[66]++;
			printf("%d \n", table[i].hits);
		}
	}
	for (i = 0; i < 65; i++)
		printf("Nº de acessos: %d. Quantidade de posições: %d \n", i, hits_counter[i]);
	printf("Nº de acessos: > 64. Quantidade de posições: %d \n", hits_counter[65]);

	for (i = 0; i < 65; i++)
		printf("%d \n", hits_counter[i]);
	printf("%d \n", hits_counter[65]);
	
	return;
}


//*********************************************
//	6. Print hash table function
//*********************************************
void print_hashtable(htype *table, uint32_t size){
	register uint32_t i;
	htype *aux;
	
	for (i = 0; i < size; i++){
		printf("\nÍNDICE: %u |\t CHAVES:", i);
		aux = &table[i];
		for (int j = 0; j < table[i].hits; j++){
			printf(" %u |", aux->key); 
			aux = aux->next;
		}
		printf("\n");
	}
	return;
}