#!/bin/bash
#author: marisaselfranco

#number of repetitions
repetitions="20"

INIT=1
END=20

id=1

#to access the directory
cd hash-join

echo "Compilando os códigos da Query 10 e da Query 11 (versões regulares e 'paralelizadas', que simulam dois servidores)..."
#to compile Query 10, Query 10 - parallel version -, Query 11 and Query 11 - parallel version
gcc -O3 main_query10.c header.h hash_table.c murmurhash3.c hash_join.c fnv1a.c -o query10 -lm
gcc -O3 main_query10_parallel.c header.h hash_table.c murmurhash3.c hash_join.c fnv1a.c -o query10parallel -lm
gcc -O3 main_query11.c header.h hash_table.c murmurhash3.c hash_join.c fnv1a.c -o query11 -lm
gcc -O3 main_query11_parallel.c header.h hash_table.c murmurhash3.c hash_join.c fnv1a.c -o query11parallel -lm

#fist battery of tests - Query 10 (1 giga)
echo "Executando testes da Query 10 com Murmur3, customer.tbl com 150000 e orders.tbl com 1500000 entradas e hash de tamanho 16381..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query10 murmur3 customer.tbl 150000 orders.tbl 1500000 16381 $size
	done

echo "Executando testes da Query 10 com FNV1-a, customer.tbl com 150000 e orders.tbl com 1500000 entradas e hash de tamanho 16381..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query10 fnv1a customer.tbl 150000 orders.tbl 1500000 16381 $size
	done

echo "Executando testes da Query 10 (versão paralelizada) com Murmur3, customer.tbl com 150000 e orders.tbl com 1500000 entradas e hash de tamanho 8191..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query10parallel murmur3 customer.tbl 150000 orders.tbl 1500000 8191 $size
	done

echo "Executando testes da Query 10 (versão paralelizada) com FNV1-a, customer.tbl com 150000 e orders.tbl com 1500000 entradas e hash de tamanho 8191..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query10parallel fnv1a customer.tbl 150000 orders.tbl 1500000 8191 $size
	done

#second battery of tests - Query 10 (10 gigas)
echo "Executando testes da Query 10 com Murmur3, customer10.tbl com 1500000 e orders10.tbl com 15000000 entradas e hash de tamanho 262139..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query10 murmur3 customer10.tbl 1500000 orders10.tbl 15000000 262139 $size
	done

echo "Executando testes da Query 10 com FNV1-a, customer10.tbl com 1500000 e orders10.tbl com 15000000 entradas e hash de tamanho 262139..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query10 fnv1a customer10.tbl 1500000 orders10.tbl 15000000 262139 $size
	done

#echo "Executando testes da Query 10 (versão paralelizada) com Murmur3, customer10.tbl com 1500000 e orders10.tbl com 15000000 entradas e hash de tamanho 131071..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query10parallel murmur3 customer10.tbl 1500000 orders10.tbl 15000000 131071 $size
	done

#echo "Executando testes da Query 10 (versão paralelizada) com FNV1-a, customer10.tbl com 1500000 e orders10.tbl com 15000000 entradas e hash de tamanho 131071..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query10parallel fnv1a customer10.tbl 1500000 orders10.tbl 15000000 131071 $size
	done	

#third battery of tests - Query 10 (100 gigas)
echo "Executando testes da Query 10 com Murmur3, customer100.tbl com 15000000 e orders100.tbl com 150000000 entradas e hash de tamanho 2097143..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query10 murmur3 customer100.tbl 15000000 orders100.tbl 150000000 2097143 $size
	done

echo "Executando testes da Query 10 com FNV1-a, customer100.tbl com 15000000 e orders100.tbl com 150000000 entradas e hash de tamanho 2097143..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query10 fnv1a customer100.tbl 15000000 orders100.tbl 150000000 2097143 $size
	done

echo "Executando testes da Query 10 (versão paralelizada) com Murmur3, customer100.tbl com 15000000 e orders100.tbl com 150000000 entradas e hash de tamanho 1048573..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query10parallel murmur3 customer100.tbl 15000000 orders100.tbl 150000000 1048573 $size
	done

echo "Executando testes da Query 10 (versão paralelizada) com FNV1-a, customer100.tbl com 15000000 e orders100.tbl com 150000000 entradas e hash de tamanho 1048573..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query10parallel fnv1a customer100.tbl 15000000 orders100.tbl 150000000 1048573 $size
	done	

#fist battery of tests - Query 11 (1 giga)
echo "Executando testes da Query 11 com Murmur3, supplier.tbl com 10000 e partsupp.tbl com 800000 entradas e hash de tamanho 2039..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query11 murmur3 supplier.tbl 10000 partsupp.tbl 800000 2039 $size
	done

echo "Executando testes da Query 11 com FNV1-a, supplier.tbl com 10000 e partsupp.tbl com 800000 entradas e hash de tamanho 2039..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query11 fnv1a supplier.tbl 10000 partsupp.tbl 800000 2039 $size
	done

echo "Executando testes da Query 11 (versão paralelizada) com Murmur3, supplier.tbl com 10000 e partsupp.tbl com 800000 entradas e hash de tamanho 1021..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query11parallel murmur3 supplier.tbl 10000 partsupp.tbl 800000 1021 $size
	done

echo "Executando testes da Query 11 (versão paralelizada) com FNV1-a, supplier.tbl com 10000 e partsupp.tbl com 800000 entradas e hash de tamanho 1021..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query11parallel fnv1a supplier.tbl 10000 partsupp.tbl 800000 1021 $size
	done

#second battery of tests - Query 11 (10 gigas)
echo "Executando testes da Query 11 com Murmur3, supplier10.tbl com 100000 e partsupp10.tbl com 8000000 entradas e hash de tamanho 16381..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query11 murmur3 supplier10.tbl 100000 partsupp10.tbl 8000000 16381 $size
	done

echo "Executando testes da Query 11 com FNV1-a, supplier10.tbl com 100000 e partsupp10.tbl com 8000000 entradas e hash de tamanho 16381..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query11 fnv1a supplier10.tbl 100000 partsupp10.tbl 8000000 16381 $size
	done

echo "Executando testes da Query 11 (versão paralelizada) com Murmur3, supplier10.tbl com 100000 e partsupp10.tbl com 8000000 entradas e hash de tamanho 8191..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query11parallel murmur3 supplier10.tbl 100000 partsupp10.tbl 8000000 8191 $size
	done

echo "Executando testes da Query 11 (versão paralelizada) com FNV1-a, supplier10.tbl com 100000 e partsupp10.tbl com 8000000 entradas e hash de tamanho 8191..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query11parallel fnv1a supplier10.tbl 100000 partsupp10.tbl 8000000 8191 $size
	done

#third battery of tests - Query 11 (10 gigas)
echo "Executando testes da Query 11 com Murmur3, supplier100.tbl com 1000000 e partsupp100.tbl com 80000000 entradas e hash de tamanho 131071..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query11 murmur3 supplier100.tbl 1000000 partsupp100.tbl 80000000 131071 $size
	done

echo "Executando testes da Query 11 com FNV1-a, supplier100.tbl com 1000000 e partsupp100.tbl com 80000000 entradas e hash de tamanho 131071..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query11 fnv1a supplier100.tbl 1000000 partsupp100.tbl 80000000 131071 $size
	done

echo "Executando testes da Query 11 (versão paralelizada) com Murmur3, supplier100.tbl com 1000000 e partsupp100.tbl com 80000000 entradas e hash de tamanho 65521..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query11parallel murmur3 supplier100.tbl 1000000 partsupp100.tbl 80000000 65521 $size
	done

echo "Executando testes da Query 11 (versão paralelizada) com FNV1-a, supplier100.tbl com 1000000 e partsupp100.tbl com 80000000 entradas e hash de tamanho 65521..."
for ((size=INIT; size<=END; size++)) ; 
	do
		./query11parallel fnv1a supplier100.tbl 1000000 partsupp100.tbl 80000000 65521 $size
	done