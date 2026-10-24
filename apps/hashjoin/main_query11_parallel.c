//*"SIMPLIFIED 2 SERVERS QUERY 11 TPC-H - HASH JOIN"*
//	Implementation:	Marisa Sel Franco				*
//	HiPES - UFPR									*
//***************************************************
//	Date		|	Description						*
//***************************************************
//	11/23/20	|	Start of implementation			*
//	01/28/21	|	End of implementation			*
//***************************************************

#include "header.h"

int main(int argc, char *argv[]){								// receives arguments via terminal
	long int l_tab1, l_tab2, tuples_counter = 0, tuples_counter_a = 0, tuples_counter_b = 0;;
	uint32_t i = 0, j = 0, l = 0, size;
	int hits_counter_a[66], hits_counter_b[66], k = 0;
	FILE *input_tab, *output_file;								// declare pointer to the files
	char line[1000], *token = NULL, *result = NULL, *p, buffer[25], file_name[12] = "tempos";
	htype *table_a, *table_b;
	ctype counter_a, counter_b;
	clock_t ticks[2], totalticks[2];
	double elapsedtime, time_register[15];
	uint32_t *out_partkey_a, *out_partkey_b;

	// FAZER TRATAMENTO DE ERROS PARA NÚMERO E TIPOS DE ARGUMENTOS
	totalticks[0] = ticks[0] = clock();
	l_tab1 = strtol(argv[3], &p, 10); 							// number of rows from the first table
	l_tab2 = strtol(argv[5], &p, 10);							// number of rows from the second table
	if (l_tab1 > l_tab2){ 										// if malloc returns NULL, there is not enough memory
		printf("\nErro. Você inseriu os parâmetros da forma errada.\n");
		printf("As informações da menor tabela devem ser colocadas antes dos parâmetros da maior tabela.\n");
		printf("Exemplo: ./query11parallel murmur3 supplier.tbl 10000 partsupp.tbl 800000 1021\n");
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
	s_suppkey = malloc(l_tab1 * sizeof(uint32_t));				// SF*10000 are populated
	if (s_suppkey == NULL)	{									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	s_name = (char **)malloc(l_tab1 * sizeof(char *));			// fixed text, size 25
	if (s_name == NULL){										// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	s_name[0] = (char *)malloc(sizeof(char) * 35 * l_tab1); 	// allocates the big vector 
	if (s_name[0] == NULL){										// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	for (l = 0; l < l_tab1; l++){								// rearranges the pointers  
		s_name[l] = s_name[0] + (l * 35 * sizeof(char));
	}
	s_address = (char **)malloc(l_tab1 * sizeof(char *));		// variable text, size 40
	if (s_address == NULL){										// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	s_address[0] = (char *)malloc(sizeof(char) * 50 * l_tab1); 	// allocates the big vector 
	if (s_address[0] == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	for (l = 0; l < l_tab1; l++){								// rearranges the pointers  
		s_address[l] = s_address[0] + (l * 50 * sizeof(char));
	}
	s_nationkey = malloc(l_tab1 * sizeof(int));					// 25 nations are populated
	if (s_nationkey == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	s_phone = (char **)malloc(l_tab1 * sizeof(char *));			// fixed text, size 15
	if (s_phone == NULL){										// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	s_phone[0] = (char *)malloc(sizeof(char) * 25 * l_tab1); 	// allocates the big vector 
	if (s_phone[0] == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	for (l = 0; l < l_tab1; l++){								// rearranges the pointers  
		s_phone[l] = s_phone[0] + (l * 25 * sizeof(char));
	}
	s_acctbal = malloc(l_tab1 * sizeof(float));					// decimal
	if (s_acctbal == NULL){										// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	s_comment = (char **)malloc(l_tab1 * sizeof(char *));		// variable text, size 101
	if (s_comment == NULL){										// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	s_comment[0] = (char *)malloc(sizeof(char) * 101 * l_tab1); // allocates the big vector 
	if (s_comment[0] == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	for (l = 0; l < l_tab1; l++){								// rearranges the pointers  
		s_comment[l] = s_comment[0] + (l * 101 * sizeof(char));
	}
	ps_partkey = malloc(l_tab2 * sizeof(uint32_t));				// SF*800000 are populated
	if (ps_partkey == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	ps_suppkey = malloc(l_tab2 * sizeof(uint32_t));				// SF*800000 are populated
	if (ps_suppkey == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	ps_availqty = malloc(l_tab2 * sizeof(int));					// integer
	if (ps_availqty == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	ps_supplycost = malloc(l_tab2 * sizeof(float));				// decimal
	if (ps_supplycost == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	ps_comment = (char **)malloc(l_tab2 * sizeof(char *));		// variable text, size 199
	if (ps_comment == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	ps_comment[0] = (char *)malloc(sizeof(char) * 250 * l_tab2);	// allocates the big vector 
	if (ps_comment[0] == NULL){									// if malloc returns NULL, there is not enough memory
		printf("Erro. Espaço de memória insuficiente.\n");
		exit(1);
	}
	for (l = 0; l < l_tab2; l++){								// rearranges the pointers  
		ps_comment[l] = ps_comment[0] + (l * 250 * sizeof(char));
	}
	printf("Reservou os espaços de memória para as tabelas de entrada.\n");
	ticks[1] = clock();
	elapsedtime = (ticks[1] - ticks[0]) * 1000.0 / CLOCKS_PER_SEC;
	time_register[k] = elapsedtime;
	k++;
	//TIME STAMP
	printf("Tempo decorrido para reservar os espaços de memória para as tabelas de entrada: %g ms\n\n", elapsedtime);
	
	ticks[0] = clock();	
	// STORE SUPPLIER TAB
	input_tab = fopen(argv[2], "r");							// read the file that is in the element of the argv array
	if (!input_tab){
		printf("Erro. Não foi possível ler a primeira tabela.\n");
		abort();
	}
	while (1){
		token = NULL;											// repeat until the end of the file
		result = fgets(line, 1000, input_tab);					// read a line from input_tab and store it into buffer_line
		if (feof(input_tab)){
			break;
		}
		token = strtok(result, "|");							// get the first token
		for (j = 0; j < 7; j++){								// repeat until the end of the line
			if (j == 0 && i < l_tab1)
				if (token != NULL)
					s_suppkey[i] = (uint32_t) atol(token);		// copy token to s_suppkey[i] and convert string to unsigned int 32
			token = strtok(NULL, "|");
			if (j == 0 && i < l_tab1){
				if (token != NULL)
					strcpy(s_name[i], token);					// copy token to s_name[i]
			}
			if (j == 1 && i < l_tab1){
				if (token != NULL)
					strcpy(s_address[i], token);				// copy token to s_address[i]
			}
			if (j == 2 && i < l_tab1){
				if (token != NULL)
					s_nationkey[i] = atoi(token);				// copy token to s_nationkey[i] and convert string to integer
			}
			if (j == 3 && i < l_tab1){
				if (token != NULL)
					strcpy(s_phone[i], token);					// copy token to s_phone[i]
			}
			if (j == 4 && i < l_tab1){
				if (token != NULL)
					s_acctbal[i] = atof(token);					// copy token to s_acctbal[i] and convert string to float
			}
			if (j == 5 && i < l_tab1){
				if (token != NULL)
					strcpy(s_comment[i], token);				// copy token to s_comment[i]
			}
		}
		i++;													// increment the counter of the arrays that store the columns
	}
	fclose(input_tab);											// close the file

	// STORE PARTSUP TAB
	input_tab = fopen(argv[4], "r");							// read the file that is in the element of the argv array
	if (!input_tab){
		printf("Erro. Não foi possível ler a segunda tabela.\n");
		abort();
	}
	i = 0;
	while (1)
	{
		token = NULL;											// repeat until the end of the file
		result = fgets(line, 1000, input_tab);					// read a line from input_tab and store it into buffer_line
		if (feof(input_tab))
			break;
		token = strtok(result, "|");							// get the first token
		for (j = 0; j < 5; j++){								// repeat until the end of the line
			if (j == 0 && i < l_tab2)
				if (token != NULL)
					ps_partkey[i] = (uint32_t) atol(token);		// copy token to ps_partkey[i] and convert string to unsigned int 32
			token = strtok(NULL, "|");
			if (j == 0 && i < l_tab2){
				if (token != NULL)
					ps_suppkey[i] = (uint32_t) atol(token);		// copy token to ps_suppkey[i] and convert string to unsigned int 32
			}
			if (j == 1 && i < l_tab2){
				if (token != NULL)
					ps_availqty[i] = atoi(token);				// copy token to ps_availqty[i] and convert string to integer
			}
			if (j == 2 && i < l_tab2){
				if (token != NULL)
					ps_supplycost[i] = atof(token);				// copy token to ps_supplycost[i] and convert string to long int
			}
			if (j == 3 && i < l_tab2){
				if (token != NULL)
					strcpy(ps_comment[i], token);				// copy token to ps_comment[i]
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
			// BUILD PHASE - HASH TABLE CONSTRUCTION USING THE SMALLEST TABLE (RELATION R)
			// SERVER 1 SIMULATION: deal with the first half of the data in the smallest table (relation R)
			table_a = (malloc(size * sizeof(htype)));					// allocation of the hash table_a vector
			if (!table_a){
				printf("Erro. Espaço de memória insuficiente para criação da tabela hash.\n");
				exit(1);
			}
			init_table(table_a, size);									// hash table_a initialization
			init_counter(&counter_a);									// counter_a initialization
			uint32_t key_a, index_a, match_key_a; 
			for (i = 0; i < l_tab1/2; i++){
				key_a = s_suppkey[i];									// key_a receives s_suppkey[i]
				index_a = murmurhash_index(key_a, size);				// "index_a" receives an index created using murmur3_32 hash
				store(key_a, index_a, table_a, &counter_a);				// stores c_custekey[i] in the hash table_a
			}
			printf("O 'servidor 1' terminou a fase de construção e criou a tabela hash A com Murmur3.\n");
			ticks[1] = clock();
			elapsedtime = (ticks[1] - ticks[0]) * 1000.0 / CLOCKS_PER_SEC;
			time_register[k] = elapsedtime;
			k++;
			//TIME STAMP
			printf("Tempo decorrido para terminar a fase de construção no 'servidor 1', criando a tabela hash A com Murmur3: %g ms\n\n", elapsedtime);
			
			ticks[0] = clock();	
			// SERVER 2 SIMULATION: deal with the first half of the data in the smallest table (relation R)
			table_b = (malloc(size * sizeof(htype)));					// allocation of the hash table_b vector
			if (!table_b){
				printf("Erro. Espaço de memória insuficiente para criação da tabela hash.\n");
				exit(1);
			}
			init_table(table_b, size);									// hash table_b initialization
			init_counter(&counter_b);									// counter_b initialization
			uint32_t key_b, index_b, match_key_b;
			for (i = l_tab1/2; i < l_tab1; i++){
				key_b = s_suppkey[i];									// key_b receives s_suppkey[i]
				index_b = murmurhash_index(key_b, size);				// "index_b" receives an index created using murmur3_32 hash
				store(key_b, index_b, table_b, &counter_b);				// stores c_custekey[i] in the hash table_b
			}
			printf("O 'servidor 2' terminou a fase de construção e criou a tabela hash B com Murmur3.\n");
			ticks[1] = clock();
			elapsedtime = (ticks[1] - ticks[0]) * 1000.0 / CLOCKS_PER_SEC;
			time_register[k] = elapsedtime;
			k++;
			//TIME STAMP
			printf("Tempo decorrido para terminar a fase de construção no 'servidor 2', criando a tabela hash B com Murmur3: %g ms\n\n", elapsedtime);
			
			ticks[0] = clock();	
			//printf("Tabela A:\n");	
			//print_hashtable(table_a, size);							// prints the hash table_a
			//init_hits_counter(hits_counter_a);						// hits_counter_a initialization
			//histogram(table_a, hits_counter_a, size);					// prints data to make an histogram about collisions in the hash table_a	

			//printf("Tabela B:\n");	
			//print_hashtable(table_b, size);							// prints the hash table_b
			//init_hits_counter(hits_counter_b);						// hits_counter_b initialization
			//histogram(table_b, hits_counter_b, size);					// prints data to make an histogram about collisions in the hash table_b			

			// PROBE PHASE - CHECKS MATCHES IN THE LARGEST TABLE
			// SERVER 1 SIMULATION
			out_partkey_a = malloc(l_tab2/2 * sizeof(uint32_t));		// SF*800000/2 are populated
			if (out_partkey_a == NULL){									// if malloc returns NULL, there is not enough memory
				printf("Erro. Espaço de memória insuficiente.\n");
				exit(1);
			}
			for (i = 0; i < l_tab2/2; i++){
				key_a = ps_suppkey[i];									// key_a receives ps_suppkey[i]
				index_a = murmurhash_index(key_a, size);				// "index" receives an index created using murmur3_32 hash
				match_key_a = verify(key_a, index_a, table_a);			// checks matches beetwen the largest and the smallest table keys
				if(match_key_a > 0){									// when it finds a match, it creates query 10 outputs
					out_partkey_a[i] = ps_partkey[i];					// out_partkey_a[i] receives ps_partkey[i]
					tuples_counter_a++;
				}
				else{
					match_key_a = verify(key_a, index_a, table_b);		// checks matches beetwen the largest and the smallest table keys
					if(match_key_a > 0){
						out_partkey_a[i] = ps_partkey[i];				// out_partkey_a[i] receives ps_partkey[i]
						tuples_counter_a++;
					}	
				}
			}
			printf("O 'servidor 1' terminou a fase de análise.\n");
			ticks[1] = clock();
			elapsedtime = (ticks[1] - ticks[0]) * 1000.0 / CLOCKS_PER_SEC;
			time_register[k] = elapsedtime;
			k++;
			//TIME STAMP
			printf("Tempo decorrido para o 'servidor 1' terminar a fase de análise: %g ms\n\n", elapsedtime);

			ticks[0] = clock();
			// SERVER 2 SIMULATION
			out_partkey_b = malloc(l_tab2/2 * sizeof(uint32_t));		// SF*800000/2 are populated
			if (out_partkey_b == NULL){									// if malloc returns NULL, there is not enough memory
				printf("Erro. Espaço de memória insuficiente.\n");
				exit(1);
			}
			for (i = l_tab2/2; i < l_tab2; i++){
				key_b =	ps_suppkey[i];									// key_a receives ps_suppkey[i]
				index_b = murmurhash_index(key_b, size);				// "index" receives an index created using murmur3_32 hash
				match_key_b = verify(key_b, index_b, table_b);			// checks matches beetwen the largest and the smallest table keys
				if(match_key_b > 0){									// when it finds a match, it creates query 10 outputs
					out_partkey_b[i - l_tab2/2] = ps_partkey[i];		// out_partkey_b[i - l_tab2/2] receives ps_partkey[i]
					tuples_counter_b++;						
				}
				else{
					match_key_b = verify(key_b, index_b, table_a);		// checks matches beetwen the largest and the smallest table keys
					if(match_key_b > 0){								// when it finds a match, it creates query 10 outputs
						out_partkey_b[i - l_tab2/2] = ps_partkey[i];	// out_partkey_b[i - l_tab2/2] receives ps_partkey[i]
						tuples_counter_b++;						
					}
				}
			}	
			printf("O 'servidor 2' terminou a fase de análise.\n");
			ticks[1] = clock();
			elapsedtime = (ticks[1] - ticks[0]) * 1000.0 / CLOCKS_PER_SEC;
			time_register[k] = elapsedtime;
			k++;
			//TIME STAMP
			printf("Tempo decorrido para o 'servidor 2' terminar a fase de análise: %g ms\n\n", elapsedtime);
		
			ticks[0] = clock();
			tuples_counter = tuples_counter_a + tuples_counter_b;
		}

		//FNV-1A
		else{
			// BUILD PHASE - HASH TABLE CONSTRUCTION USING THE SMALLEST TABLE	
			// SERVER 1 SIMULATION: deal with the first half of the data in the smallest table (relation R)
			table_a = (malloc(size * sizeof(htype)));					// allocation of the hash table_a vector
			if (!table_a){
				printf("Erro. Espaço de memória insuficiente para criação da tabela hash.\n");
				exit(1);
			}
			init_table(table_a, size);									// hash table_a initialization
			init_counter(&counter_a);									// counter_a initialization
			uint32_t key_a, index_a, match_key_a;
			for (i = 0; i < l_tab1/2; i++){
				key_a = s_suppkey[i];									// key_a receives s_suppkey[i]
				index_a = fnv1a_index(key_a, size);						// "index_a" receives an index created using FNV1-a hash
				store(key_a, index_a, table_a, &counter_a);				// stores c_custekey[i] in the hash table_a
			}
			printf("O 'servidor 1' criou terminou a fase de construção e criou a tabela hash A com FNV-1a.\n");
			ticks[1] = clock();
			elapsedtime = (ticks[1] - ticks[0]) * 1000.0 / CLOCKS_PER_SEC;
			time_register[k] = elapsedtime;
			k++;
			//TIME STAMP
			printf("Tempo decorrido para terminar a fase de construção no 'servidor 1', criando a tabela hash A com FNV-1a: %g ms\n\n", elapsedtime);
			
			ticks[0] = clock();	
			// SERVER 2 SIMULATION: deal with the first half of the data in the smallest table (relation R)
			table_b = (malloc(size * sizeof(htype)));					// allocation of the hash table_b vector
			if (!table_b){
				printf("Erro. Espaço de memória insuficiente para criação da tabela hash.\n");
				exit(1);
			}  
			init_table(table_b, size);									// hash table_b initialization
			init_counter(&counter_b);									// counter_b initialization
			uint32_t key_b, index_b, match_key_b;
			for (i = l_tab1/2; i < l_tab1; i++){
				key_b = s_suppkey[i];									// key_b receives s_suppkey[i]
				index_b = fnv1a_index(key_b, size);						// "index_b" receives an index created using FNV1-a hash
				store(key_b, index_b, table_b, &counter_b);				// stores c_custekey[i] in the hash table_b
			}
			printf("O 'servidor 2' criou terminou a fase de construção e criou a tabela hash B com FNV-1a.\n");
			ticks[1] = clock();
			elapsedtime = (ticks[1] - ticks[0]) * 1000.0 / CLOCKS_PER_SEC;
			time_register[k] = elapsedtime;
			k++;
			//TIME STAMP
			printf("Tempo decorrido para terminar a fase de construção no 'servidor 2', criando a tabela hash B com FNV-1a: %g ms\n\n", elapsedtime);
			
			ticks[0] = clock();	
			//printf("Tabela A:\n");	
			//print_hashtable(table_a, size);							// prints the hash table_a
			//init_hits_counter(hits_counter_a);						// hits_counter_a initialization
			//histogram(table_a, hits_counter_a, size);					// prints data to make an histogram about collisions in the hash table_a	

			//printf("Tabela B:\n");	
			//print_hashtable(table_b, size);							// prints the hash table_b
			//init_hits_counter(hits_counter_b);						// hits_counter_b initialization
			//histogram(table_b, hits_counter_b, size);					// prints data to make an histogram about collisions in the hash table_b	
			
			// PROBE PHASE - CHECKS MATCHES IN THE LARGEST TABLE
			// SERVER 1 SIMULATION
			out_partkey_a = malloc(l_tab2/2 * sizeof(uint32_t));		// SF*800000/2 are populated
			if (out_partkey_a == NULL){									// if malloc returns NULL, there is not enough memory
				printf("Erro. Espaço de memória insuficiente.\n");
				exit(1);
			}
			for (i = 0; i < l_tab2/2; i++){
				key_a = ps_suppkey[i];									// key receives ps_suppkey[i]
				index_a = fnv1a_index(key_a, size);						// "index" receives an index created using FNV1-a hash
				match_key_a = verify(key_a, index_a, table_a);			// checks matches beetwen the largest and the smallest table keys
				if(match_key_a > 0){									// when it finds a match, it creates query 10 outputs
					out_partkey_a[i] = ps_partkey[i];					// out_partkey_a[i] receives ps_partkey[i]
					tuples_counter_a++;
				}
				else{
					match_key_a = verify(key_a, index_a, table_b);		// checks matches beetwen the largest and the smallest table keys
					if(match_key_a > 0){	
						out_partkey_a[i] = ps_partkey[i];				// out_partkey_a[i] receives ps_partkey[i]
						tuples_counter_a++;
					}
				}
			}
			printf("O 'servidor 1' terminou a fase de análise.\n");
			ticks[1] = clock();
			elapsedtime = (ticks[1] - ticks[0]) * 1000.0 / CLOCKS_PER_SEC;
			time_register[k] = elapsedtime;
			k++;
			//TIME STAMP
			printf("Tempo decorrido para o 'servidor 1' terminar a fase de análise: %g ms\n\n", elapsedtime);
			
			ticks[0] = clock();
			// SERVER 2 SIMULATION
			out_partkey_b = malloc(l_tab2/2 * sizeof(uint32_t));		// SF*800000/2 are populated
			if (out_partkey_b == NULL){									// if malloc returns NULL, there is not enough memory
				printf("Erro. Espaço de memória insuficiente.\n");
				exit(1);
			}			
			for (i = l_tab2/2; i < l_tab2; i++){
				key_b = ps_suppkey[i];									// key receives ps_suppkey[i]						
				index_b = fnv1a_index(key_b, size);						// "index" receives an index created using FNV1-a hash
				match_key_b = verify(key_b, index_b, table_b);			// checks matches beetwen the largest and the smallest table keys
				if(match_key_b > 0){									// when it finds a match, it creates query 10 outputs
					out_partkey_b[i - l_tab2/2] = ps_partkey[i];		// out_partkey_b[i - l_tab2/2] receives ps_partkey[i]
					tuples_counter_b++;
				}
				else{
					match_key_b = verify(key_b, index_b, table_a);		// checks matches beetwen the largest and the smallest table keys
					if(match_key_b > 0){								// when it finds a match, it creates query 10 outputs
						out_partkey_b[i - l_tab2/2] = ps_partkey[i];	// out_partkey_b[i- l_tab2/2] receives ps_partkey[i]
						tuples_counter_b++;
					}
				}
			}
			printf("O 'servidor 2' terminou a fase de análise.\n");
			ticks[1] = clock();
			elapsedtime = (ticks[1] - ticks[0]) * 1000.0 / CLOCKS_PER_SEC;
			time_register[k] = elapsedtime;
			k++;
			//TIME STAMP
			printf("Tempo decorrido para o 'servidor 2' terminar a fase de análise: %g ms\n\n", elapsedtime);
			
			ticks[0] = clock();
			tuples_counter = tuples_counter_a + tuples_counter_b;			
		}
	}

	print_query11(out_partkey_a, l_tab2/2);						// prints query 10 output - part 1
	print_query11(out_partkey_b, l_tab2/2);						// prints query 10 output - part 1

	printf("\nA saída da 'Query 11' tem %li tuplas.\n", tuples_counter);
	ticks[1] = clock();
	elapsedtime = (ticks[1] - ticks[0]) * 1000.0 / CLOCKS_PER_SEC;
	time_register[k] = elapsedtime;
	k++;
	//TIME STAMP
	printf("Tempo decorrido para imprimir a saída da 'Query 11': %g ms\n\n", elapsedtime);

	ticks[0] = clock();
	free(s_suppkey);
	s_suppkey = NULL;
	free(s_name);
	s_name = NULL;
	free(s_address);
	s_address = NULL;
	free(s_nationkey);
	s_nationkey = NULL;
	free(s_phone);
	s_phone = NULL;
	free(s_acctbal);
	s_acctbal = NULL;
	free(s_comment);
	s_comment = NULL;
	free(ps_partkey);
	ps_partkey = NULL;
	free(ps_suppkey);
	ps_suppkey = NULL;
	free(ps_availqty);
	ps_availqty = NULL;
	free(ps_supplycost);
	ps_supplycost = NULL;
	free(ps_comment);
	ps_comment = NULL;
	free(table_a);
	table_a = NULL;
	free(table_b);
	table_b = NULL;
	free(out_partkey_a);
	out_partkey_a = NULL;
	free(out_partkey_b);
	out_partkey_b = NULL;

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
	printf("Tempo de execução da Query 11 em '2 servidores': %g ms\n\n", elapsedtime);
	for (int count = 0; count < k; count++)
		printf("%g ms\n", time_register[count]);

	output_file = fopen(file_name,"a");								// opens a file named "tempos" for writing
	fputs(argv[7], output_file);									// execution ID
	fputs(";", output_file);
	fputs("Query 11 paralelizada", output_file);					// query
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
	build phase "server 1", build phase "server 2", probe phase "server 1", probe phase "server 2", 
	printing of the output , memory release, "Query11" runtime */
	for (int count2 = 0; count2 < k; count2++){
		sprintf (buffer, "%g", time_register[count2]); 				// puts the time in the buffer string
		fputs(buffer, output_file);
		fputs(";", output_file);
	}
	fputs ("\n", output_file);
	fclose(output_file);

	return 0;
}