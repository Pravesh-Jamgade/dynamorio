//****"SIMPLIFIED QUERY 10 TPC-H - HASH JOIN" *******
//	Implementation:	Marisa Sel Franco				*
//	HiPES - UFPR									*
//***************************************************
//	Date		|	Description						*
//***************************************************
//	08/11/20	|	Start of implementation			*
//	01/28/21	|	End of implementation			*
//***************************************************

#include "header.h"

int main(int argc, char *argv[]){								// receives arguments via terminal
	long int l_tab1, l_tab2, tuples_counter = 0;
	uint32_t i = 0, j = 0, l = 0, size;
	int hits_counter[66], k = 0;
	FILE *input_tab, *output_file;								// declare pointer to the files
	char line[1000], *token = NULL, *result = NULL, *p, buffer[25], file_name[12] = "tempos"; 
	htype *table;
	ctype counter;
	clock_t ticks[2], totalticks[2];
	double elapsedtime, time_register[15];

	// FAZER TRATAMENTO DE ERROS PARA NÚMERO E TIPOS DE ARGUMENTOS
	totalticks[0] = ticks[0] = clock();
	l_tab1 = strtol(argv[3], &p, 10);							// number of rows from the first table
	l_tab2 = strtol(argv[5], &p, 10);							// number of rows from the second table
	if (l_tab1 > l_tab2){ 										// if malloc returns NULL, there is not enough memory
		printf("\nErro. Você inseriu os parâmetros da forma errada.\n");
		printf("As informações da menor tabela devem ser colocadas antes dos parâmetros da maior tabela.\n");
		printf("Exemplo: ./query10 murmur3 customer.tbl 150000 orders.tbl 1500000 16381\n");
		printf("\nO programa será encerrado.\n");
		exit(1);
	}
	size = strtol(argv[6], &p, 10);								// hash table's size
	ticks[1] = clock();
	elapsedtime = (ticks[1] - ticks[0]) * 1000.0 / CLOCKS_PER_SEC;
	time_register[k] = elapsedtime;
	k++;
	//TIME STAMP
	printf("Tempo decorrido para armazenar parâmetros passados: %g ms\n\n", elapsedtime);
	
	ticks[0] = clock();
	// ALLOCATION OF MEMORY SPACES
	c_custkey = malloc(l_tab1 * sizeof(uint32_t));				// SF*150000 are populated
	if (c_custkey == NULL){										// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	c_name = (char **)malloc(l_tab1 * sizeof(char *));			// fixed text, size 25
	if (c_name == NULL){										// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	c_name[0] = (char *)malloc(sizeof(char) * 35 * l_tab1); 	// allocates the big vector 
	if (c_name[0] == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	for (l = 0; l < l_tab1; l++){								// rearranges the pointers  
		c_name[l] = c_name[0] + (l * 35 * sizeof(char));
	}	
	c_address = (char **)malloc(l_tab1 * sizeof(char *));		// variable text, size 40
	if (c_address == NULL){										// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	c_address[0] = (char *)malloc(sizeof(char) * 50 * l_tab1); 	// allocates the big vector 
	if (c_address[0] == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	for (l = 0; l < l_tab1; l++){								// rearranges the pointers  
		c_address[l] = c_address[0] + (l * 50 * sizeof(char));
	}
	c_nationkey = malloc(l_tab1 * sizeof(int));					// 25 nations are populated
	if (c_nationkey == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	c_phone = (char **)malloc(l_tab1 * sizeof(char *));			// fixed text, size 15
	if (c_phone == NULL){										// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	c_phone[0] = (char *)malloc(sizeof(char) * 25 * l_tab1); 	// allocates the big vector 
	if (c_phone[0] == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	for (l = 0; l < l_tab1; l++){								// rearranges the pointers  
		c_phone[l] = c_phone[0] + (l * 25 * sizeof(char));
	}
	c_acctbal = malloc(l_tab1 * sizeof(float));					// decimal
	if (c_acctbal == NULL){										// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	c_mktsegment = (char **)malloc(l_tab1 * sizeof(char *));	// fixed text, size 10
	if (c_mktsegment == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	c_mktsegment[0] = (char *)malloc(sizeof(char) * 20 * l_tab1); 	// allocates the big vector 
	if (c_mktsegment[0] == NULL){								// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	for (l = 0; l < l_tab1; l++){								// rearranges the pointers  
		c_mktsegment[l] = c_mktsegment[0] + (l * 20 * sizeof(char));
	}
	c_comment = (char **)malloc(l_tab1 * sizeof(char *));		// variable text, size 117
	if (c_comment == NULL){										// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	c_comment[0] = (char *)malloc(sizeof(char) * 117 * l_tab1); // allocates the big vector 
	if (c_comment[0] == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	for (l = 0; l < l_tab1; l++){								// rearranges the pointers  
		c_comment[l] = c_comment[0] + (l * 117 * sizeof(char));
	}
	o_orderkey = malloc(l_tab2 * sizeof(uint32_t));				// SF*1.500.000 are sparsely populated
	if (o_orderkey == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	o_custkey = malloc(l_tab2 * sizeof(uint32_t));				// SF*1.500.000 are populated
	if (o_custkey == NULL){										// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	o_orderstatus = malloc(l_tab2 * sizeof(char *));			// fixed text, size 1
	if (o_orderstatus == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	o_orderstatus[0] = (char *)malloc(sizeof(char) * 2 * l_tab2);	// allocates the big vector 
	if (o_orderstatus[0] == NULL){								// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	for (l = 0; l < l_tab2; l++){								// rearranges the pointers  
		o_orderstatus[l] = o_orderstatus[0] + (l * 2 * sizeof(char));
	}
	o_totalprice = malloc(l_tab2 * sizeof(float));				// decimal
	if (o_totalprice == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	// CHECAR SE É O MAIS ADEQUADO GUARDAR A DATA COMO UMA STRING
	o_orderdate = (char **)malloc(l_tab2 * sizeof(char *));		// fixed text, size 10
	if (o_orderdate == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	o_orderdate[0] = (char *)malloc(sizeof(char) * 20 * l_tab2);	// allocates the big vector 
	if (o_orderdate[0] == NULL){								// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	for (l = 0; l < l_tab2; l++){								// rearranges the pointers  
		o_orderdate[l] = o_orderdate[0] + (l * 20 * sizeof(char));
	}
	o_orderpriority = (char **)malloc(l_tab2 * sizeof(char *)); // fixed text, size 15
	if (o_orderpriority == NULL){								// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	o_orderpriority[0] = (char *)malloc(sizeof(char) * 20 * l_tab2);	// allocates the big vector 
	if (o_orderpriority[0] == NULL){							// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	for (l = 0; l < l_tab2; l++){								// rearranges the pointers  
		o_orderpriority[l] = o_orderpriority[0] + (l * 20 * sizeof(char));
	}
	o_clerk = (char **)malloc(l_tab2 * sizeof(char *));			// fixed text, size 15
	if (o_clerk == NULL){										// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	o_clerk[0] = (char *)malloc(sizeof(char) * 25 * l_tab2);	// allocates the big vector 
	if (o_clerk[0] == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	for (l = 0; l < l_tab2; l++){								// rearranges the pointers  
		o_clerk[l] = o_clerk[0] + (l * 25 * sizeof(char));
	}
	o_shippriority = malloc(l_tab2 * sizeof(int));				// integer
	if (o_shippriority == NULL){								// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	o_comment = (char **)malloc(l_tab2 * sizeof(char *));		// variable text, size 79
	if (o_comment == NULL){										// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	o_comment[0] = (char *)malloc(sizeof(char) * 79 * l_tab2);	// allocates the big vector 
	if (o_comment[0] == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	for (l = 0; l < l_tab2; l++){								// rearranges the pointers  
		o_comment[l] = o_comment[0] + (l * 79 * sizeof(char));
	}	
	printf("Reservou os espaços de memória para as tabelas de entrada.\n");
	ticks[1] = clock();
	elapsedtime = (ticks[1] - ticks[0]) * 1000.0 / CLOCKS_PER_SEC;
	time_register[k] = elapsedtime;
	k++;
	//TIME STAMP
	printf("Tempo decorrido para reservar os espaços de memória para as tabelas de entrada: %g ms\n\n", elapsedtime);
	
	ticks[0] = clock();
	
	// STORE CUSTOMER TAB
	input_tab = fopen(argv[2], "r");							// read the file that is in the element of the argv array
	if (!input_tab){
		printf("Erro. Não foi possível ler a primeira tabela.\n");
		abort();
	}
	while (1){
		token = NULL;											// repeat until the end of the file
		result = fgets(line, 1000, input_tab);					// read a line from input_tab and store it into buffer_line
		if (feof(input_tab))
			break;
		token = strtok(result, "|");							// get the first token
		for (j = 0; j < 8; j++){								// repeat until the end of the line
			if (j == 0 && i < l_tab1)
				if (token != NULL)
					c_custkey[i] = (uint32_t) atol(token);		// copy token to c_custkey[i] and convert string to unsigned int 32
			token = strtok(NULL, "|");
			if (j == 0 && i < l_tab1){
				if (token != NULL)
					strcpy(c_name[i], token);					// copy token to c_name[i]
			}
			if (j == 1 && i < l_tab1){
				if (token != NULL)
					strcpy(c_address[i], token);				// copy token to c_address[i]
			}
			if (j == 2 && i < l_tab1){
				if (token != NULL)
					c_nationkey[i] = atoi(token);				// copy token to c_nationkey[i] and convert string to integer
			}
			if (j == 3 && i < l_tab1){
				if (token != NULL)
					strcpy(c_phone[i], token);					// copy token to c_phone[i]
			}
			if (j == 4 && i < l_tab1){
				if (token != NULL)
					c_acctbal[i] = atof(token); 				// copy token to c_acctbal[i] and convert string to float
			}
			if (j == 5 && i < l_tab1){
				if (token != NULL)
					strcpy(c_mktsegment[i], token);				// copy token to c_mktsegment[i]
			}			
			if (j == 6 && i < l_tab1){
				if (token != NULL)
					strcpy(c_comment[i], token);				// copy token to c_comment[i]
			}
		}
		i++;													// increment the counter of the arrays that store the columns
	}
	fclose(input_tab);											// close the file

	// STORE ORDER TAB
	input_tab = fopen(argv[4], "r");							// read the file that is in the element of the argv array
	if (!input_tab){
		printf("Erro. Não foi possível ler a segunda tabela.\n");
		abort();
	}
	i = 0;														// reset counter i
	while (1){
		token = NULL;											// repeat until the end of the file
		result = fgets(line, 1000, input_tab);					// read a line from input_tab and store it into buffer_line
		if (feof(input_tab))
			break;					 				
		token = strtok(result, "|");							// get the first token
		for (j = 0; j < 9; j++){								// repeat until the end of the line
			if (j == 0 && i < l_tab2)
				if (token != NULL)
					o_orderkey[i] = (uint32_t) atol(token);		// copy token to o_orderkey[i] and convert string to unsigned int 32
			token = strtok(NULL, "|");
			if (j == 0 && i < l_tab2){
				if (token != NULL)
					o_custkey[i] = (uint32_t) atol(token);		// copy token to o_custkey[i] and convert string to unsigned int 32
			}
			if (j == 1 && i < l_tab2){
				if (token != NULL)
					strcpy(o_orderstatus[i], token);			// copy token to o_orderstatus[i]
			}
			if (j == 2 && i < l_tab2){
				if (token != NULL)
					o_totalprice[i] = atol(token);				// copy token to o_totalprice[i] and convert string to long int
			}
			if (j == 3 && i < l_tab2){
				if (token != NULL)
					strcpy(o_orderdate[i], token);				// copy token to o_orderdate[i]
			}
			if (j == 4 && i < l_tab2){
				if (token != NULL)
					strcpy(o_orderpriority[i], token);			// copy token to o_orderpriority[i]
			}
			if (j == 5 && i < l_tab2){
				if (token != NULL)
					strcpy(o_clerk[i], token);					// copy token to o_clerk[i]
			}
			if (j == 6 && i < l_tab2){
				if (token != NULL)
					o_shippriority[i] = atoi(token);			// copy token to o_shippriority[i] and convert string to integer
			}
			if (j == 7 && i < l_tab2){
				if (token != NULL)
					strcpy(o_comment[i], token);				// copy token to o_comment[i]
			}
		}			
		i++;													// increment the counter of the arrays that store the columns
	}
	fclose(input_tab);
	printf("Armazenou os dados das tabelas de entrada.\n");
	ticks[1] = clock();
	elapsedtime = (ticks[1] - ticks[0]) * 1000.0 / CLOCKS_PER_SEC;
	time_register[k] = elapsedtime;
	k++;
	//TIME STAMP
	printf("Tempo decorrido para armazenar os dados das tabelas de entrada: %g ms\n\n", elapsedtime);
	
	ticks[0] = clock();	
	// HASH JOIN
	// INVALID HASH FUNCTION PARAMETER
	if ((strcmp(argv[1], "murmur3") != 0) && (strcmp(argv[1], "fnv1a") != 0)){	
		printf("Opção de função hash inválida. O programa será encerrado.\n");
		exit(1);
	}
	else{
		// MURMURHASH3		
		if (strcmp(argv[1], "murmur3") == 0){
			// BUILD PHASE - HASH TABLE CONSTRUCTION USING THE SMALLEST TABLE
			table = (malloc(size * sizeof(htype)));				// allocation of the hash table vector
			if (!table){
				printf("Erro. Espaço de memória insuficiente para criação da tabela hash.\n");
				exit(1);
			} 
			init_table(table, size);							// hash table initialization
			init_counter(&counter);	
			uint32_t key, index, match_key;
			for (i = 0; i < l_tab1; i++){
				key = c_custkey[i];								// key receives c_custekey[i]
				index = murmurhash_index(key, size);			// "index" receives an index created using murmur3_32 hash
				store(key, index, table, &counter);				// stores c_custekey[i] in the hash table
			}
			printf("Terminou a fase de construção, na qual criou a tabela hash com Murmur3.\n");

			ticks[1] = clock();
			elapsedtime = (ticks[1] - ticks[0]) * 1000.0 / CLOCKS_PER_SEC;
			time_register[k] = elapsedtime;
			k++;
			//TIME STAMP
			printf("Tempo decorrido para realizar a fase de construção, na qual criou a tabela hash com Murmur3: %g ms\n\n", elapsedtime);
	
			//print_hashtable(table, size);						// prints the hash table
			//init_hits_counter(hits_counter);					// hits_counter initialization
			//histogram(table, hits_counter, size);				// prints data to make an histogram about collisions in the hash table	
			
			ticks[0] = clock();	
			// PROBE PHASE - CHECKS MATCHES IN THE LARGEST TABLE
			out_custkey = malloc(l_tab2 * sizeof(uint32_t));			// SF*1500000 are populated
			if (out_custkey == NULL){									// if malloc returns NULL, there is not enough memory
				printf("Erro. Espaço de memória insuficiente.\n");
				exit(1);
			}
			init_array(out_custkey, l_tab2);							// out_custkey initialization: -1 in all positions
			out_name = (char **)malloc(l_tab2 * sizeof(char *));		// fixed text, size 25
			if (out_name == NULL){										// if malloc returns NULL, there is not enough memory
				printf("Erro. Espaço de memória insuficiente.\n");
				exit(1);
			}
			/*for (i = 0; i < l_tab2; i++){
				out_name[i] = (char *)malloc(sizeof(char) * 35);
				if (out_name[i] == NULL){						// if malloc returns NULL, there is not enough memory
					printf("Erro. Espaço de memória insuficiente.\n");
					exit(1);
				}
			}*/
			out_name[0] = (char *)malloc(sizeof(char) * 35 * l_tab2); 	// allocates the big vector 
			if (out_name[0] == NULL){									// if malloc returns NULL, there is not enough memory
				printf("Erro. Espaço de memória insuficiente.\n");
				exit(1);
			}
			for (i = 0; i < l_tab2; i++){								// rearranges the pointers  
				out_name[i] = out_name[0] + (i * 35 * sizeof(char));
			}	
			for (i = 0; i < l_tab2; i++){
				key = o_custkey[i];								// key receives o_custekey[i]
				index = murmurhash_index(key, size);			// "index" receives an index created using murmur3_32 hash
				match_key = verify(key, index, table);			// checks matches beetwen the largest and the smallest table keys
				if (match_key > 0){								// when it finds a match, it creates query 10 outputs
					out_custkey[i] = match_key;					// out_custkey[i] receives match_key
					memcpy(out_name[i], c_name[match_key - 1], strlen(c_name[match_key - 1])+1);	// copy c_name[match_key - 1] to out_name[i]
					tuples_counter++;
				}
			}
		}
		//FNV-1A
		else{
			// BUILD PHASE - HASH TABLE CONSTRUCTION USING THE SMALLEST TABLE
			table = (malloc(size * sizeof(htype)));				// allocation of the hash table vector
			if (!table){
				printf("Erro. Espaço de memória insuficiente para criação da tabela hash.\n");
				exit(1);
			} 
			init_table(table, size);							// hash table initialization
			init_counter(&counter);	
			uint32_t key, index, match_key;
			for (i = 0; i < l_tab1; i++){
				key = c_custkey[i];								// key receives o_custekey[i]
				index = fnv1a_index(key, size);					// "index" receives an index created using FNV1-A hash
				store(key, index, table, &counter);				// stores c_custekey[i] in the hash table
			}
			printf("Terminou a fase de construção, na qual criou a tabela hash com FNV1-a.\n");
			ticks[1] = clock();
			elapsedtime = (ticks[1] - ticks[0]) * 1000.0 / CLOCKS_PER_SEC;
			time_register[k] = elapsedtime;
			k++;
			//TIME STAMP
			printf("Tempo decorrido para realizar a fase de construção, na qual criou a tabela hash com FNV1-a: %g ms\n\n", elapsedtime);
	
			//print_hashtable(table, size);						// prints the hash table
			//init_hits_counter(hits_counter);					// hits_counter initialization
			//histogram(table, hits_counter, size);				// prints data to make an histogram about collisions in the hash table

			ticks[0] = clock();			
			// PROBE PHASE - CHECKS MATCHES IN THE LARGEST TABLE
			out_custkey = malloc(l_tab2 * sizeof(uint32_t));			// SF*1500000 are populated
			if (out_custkey == NULL){									// if malloc returns NULL, there is not enough memory
				printf("Erro. Espaço de memória insuficiente.\n");
				exit(1);
			}
			init_array(out_custkey, l_tab2);							// out_custkey initialization: -1 in all positions
			out_name = (char **)malloc(l_tab2 * sizeof(char *));		// fixed text, size 25
			if (out_name == NULL){										// if malloc returns NULL, there is not enough memory
				printf("Erro. Espaço de memória insuficiente.\n");
				exit(1);
			}
			out_name[0] = (char *)malloc(sizeof(char) * 35 * l_tab2); 	// allocates the big vector 
			if (out_name[0] == NULL){									// if malloc returns NULL, there is not enough memory
				printf("Erro. Espaço de memória insuficiente.\n");
				exit(1);
			}
			for (i = 0; i < l_tab2; i++){								// rearranges the pointers  
				out_name[i] = out_name[0] + (i * 35 * sizeof(char));
			}	
			for (i = 0; i < l_tab2; i++){
				key = o_custkey[i];								// key receives o_custekey[i]
				index = fnv1a_index(key, size);					// "index" receives an index created using murmur3_32 hash
				match_key = verify(key, index, table);			// checks matches beetwen the largest and the smallest table keys
				if (match_key > 0){								// when it finds a match, it creates query 10 outputs
					out_custkey[i] = match_key;					// out_custkey[i] receives match_key
					if (out_name[i] == NULL){					// if malloc returns NULL, there is not enough memory
						printf("Erro. Espaço de memória insuficiente.\n");
						exit(1);
					}
					memcpy(out_name[i], c_name[match_key - 1], strlen(c_name[match_key - 1])+1);	// copy c_name[match_key - 1] to out_name[i]
					tuples_counter++;
				}
			}
		}
	}
	printf("Terminou a fase de análise.\n");
	ticks[1] = clock();
	elapsedtime = (ticks[1] - ticks[0]) * 1000.0 / CLOCKS_PER_SEC;
	time_register[k] = elapsedtime;
	k++;
	//TIME STAMP
	printf("Tempo decorrido para realizar a fase de análise: %g ms\n\n", elapsedtime);

	ticks[0] = clock();
	print_query10(out_custkey, out_name, l_tab2);				// prints query 10 output

	printf("\nA saída da 'Query 10' tem %li tuplas.\n", tuples_counter);
	ticks[1] = clock();
	elapsedtime = (ticks[1] - ticks[0]) * 1000.0 / CLOCKS_PER_SEC;
	time_register[k] = elapsedtime;
	k++;
	//TIME STAMP
	printf("Tempo decorrido para imprimir a saída da 'Query 10': %g ms\n\n", elapsedtime);

	ticks[0] = clock();
	free(c_custkey);
	c_custkey = NULL;
	free(c_name);
	c_name = NULL;
	free(c_address);
	c_address = NULL;
	free(c_nationkey);
	c_nationkey = NULL;
	free(c_phone);
	c_phone = NULL;
	free(c_acctbal);
	c_acctbal = NULL;
	free(c_mktsegment);
	c_mktsegment = NULL;
	free(c_comment);
	c_comment = NULL;
	free(o_orderkey);
	o_orderkey = NULL;
	free(o_custkey);
	o_custkey = NULL;
	free(o_orderstatus);
	o_orderstatus = NULL;
	free(o_totalprice);
	o_totalprice = NULL;
	free(o_clerk);
	o_clerk = NULL;
	free(o_shippriority);
	o_shippriority = NULL;
	free(o_comment);
	o_comment = NULL;
	free(table);
	table = NULL;
	free(out_custkey);
	out_custkey = NULL;
	free(out_name);
	out_name = NULL;

	ticks[1] = clock();
	elapsedtime = (ticks[1] - ticks[0]) * 1000.0 / CLOCKS_PER_SEC;
	time_register[k] = elapsedtime;
	k++;
	//TIME STAMP
	printf("Tempo decorrido para liberar os espaços de memória: %g ms\n\n", elapsedtime);
	
	
	totalticks[1] = clock();
	elapsedtime = (totalticks[1] - totalticks[0]) * 1000.0 / CLOCKS_PER_SEC;
	time_register[k] = elapsedtime;
	k++;
	//TIME STAMP
	printf("Tempo de execução da 'Query 10': %g ms\n\n", elapsedtime);
	for (int count = 0; count < k; count++)
		printf("%g ms\n", time_register[count]);

	output_file = fopen(file_name,"a");								// opens a file named "tempos" for writing
	fputs(argv[7], output_file);									// execution ID
	fputs(";", output_file);
	fputs("Query 10", output_file);									// query
	fputs(";", output_file);
	sprintf (buffer, "%li", tuples_counter); 
	fputs(buffer, output_file);										// number of tuples from the 'Query 10' output
	fputs(";", output_file);
	fputs(argv[1], output_file);									// hash function type
	fputs(";", output_file); 
	fputs(argv[3], output_file);									// size of the smallest relation
	fputs(";", output_file);
	fputs(argv[5], output_file);									// size of the largest relation
	fputs(";", output_file);
	fputs(argv[6], output_file);									// size of the hash table
	fputs(";", output_file);

	/* Displayed times: parameter storage, allocation of memory spaces for input data, storage of input data, 
	build phase, probe phase, printing of the output , memory release, "Query10" runtime */
	for (int count2 = 0; count2 < k; count2++){
		sprintf(buffer, "%g", time_register[count2]); 				// puts the time in the buffer string
		fputs(buffer, output_file);
		fputs(";", output_file);
	}
	fputs ("\n", output_file);
	fclose(output_file);

	return 0;
}