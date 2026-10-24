//******************* "HASH JOIN" *******************
//	Implementation:	Marisa Sel Franco				*
//	HiPES - UFPR									*
//***************************************************

https://www.singlestore.com/blog/scaling-distributed-joins/
https://www.cenapad.unicamp.br/servicos/treinamentos/apostilas/apostila_openmp.pdf


Note: the resources needed to evaluate network traffic in the simulation of a distributed database are still being studied and will be added to the algorithms.
-------------------------------
# Hash Join
Implementation of hash join in C, using MurmurHash3 and FNV-1a as hash functions. Two main programs have been implemented: one for "Query 10" (simplified version) and the other for "Query 11" (simplified version) from TPC Benchmark™H (TPC-H).


TCP-H data generator available at https://github.com/electrum/tpch-dbgen/archive/master.zip.

Given the nature of the operations (joins that do not return the complete data for a row, but only one or a few columns), the algorithms store the data from the input tables and the output data in a columnar manner.

Both algorithms perform two routines: 

-------------------------------
1. Buil phase:
-------------------------------
hash table construction using the smallest relation (table with the least number of entries). In this implementation, collisions are being treated with separate chaining.

-------------------------------
2. Probe phase:
-------------------------------
cycles through the largest relation (table with the largest number of entries) looking for matchs related to the query and add the join values ​​to the output table.

-------------------------------
Description of "Query 10" (simplified version) in SQL:
-------------------------------

SELECT C_CUSTKEY, C_NAME 
FROM CUSTOMER, ORDERS
WHERE C_CUSTKEY = O_CUSTKEY

Note: customer.tbl has 150000 entries and orders.tbl has 1500000 entries (in 1 GB "file size").

-------------------------------
Description of "Query 11" (simplified version) in SQL:
-------------------------------

SELECT PS_PARTKEY
FROM PARTSUPP, SUPPLIER
WHERE PS_SUPPKEY = S_SUPPKEY

Note: supplier.tbl has 10000 entries and partsupp.tbl has 800000 entries (in 1 GB "file size").

-------------------------------
"Hidden" functions
-------------------------------
There are three "hidden" (as comments) functions in both codes:

//print_hashtable(table, size);						// prints the hash table
//init_hits_counter(hits_counter);					// hits_counter initialization
//histogram(table, hits_counter, size);				// prints data to make an histogram about collisions in the hash table
-------------------------------
#About Query 10

-------------------------------
1. To compile the code:
-------------------------------
$gcc -O3 main_query10.c header.h hash_table.c murmurhash3.c hash_join.c fnv1a.c -o query10 -lm

For the parallel version, do:
gcc -O3 main_query10_parallel.c header.h hash_table.c murmurhash3.c hash_join.c fnv1a.c -o query10parallel -lm

-------------------------------
2. To execute the code:
-------------------------------
$./query10 murmur3 customer.tbl 150000 orders.tbl 1500000 16381 #id
Note: "#id" is the number of test execution.

or 

$./query10 fnv1a customer.tbl 150000 orders.tbl 1500000 16381 #id
Note: "#id" is the number of test execution.

For the parallel version, do:
$./query10parallel murmur3 customer.tbl 150000 orders.tbl 1500000 8191 #id
Note: "#id" is the number of test execution.

or

$./query10parallel fnv1a customer.tbl 150000 orders.tbl 1500000 8191 #id
Note: "#id" is the number of test execution.

The first parameter determines the type of hash function chosen ('murmur3' or 'fnv1a') to create the hash table.
The second parameter is the name of the smallest table('customer.tbl') and the third is its size (150000 entries, in 1 GB "file size").
The fourth parameter is the name of the largest table('orders.tbl') and the fifth is its size (1500000 entries, in 1 GB "file size").
The sixth and final parameter ('16381') determines the size of the hash table (number of positions). It is recommended to use a prime number (if possible, the first prime number below the expected number of entries).

The first 100008 primes:
https://primes.utm.edu/lists/small/100000.txt
-------------------------------
#About Query 11

-------------------------------
1. To compile the code:
-------------------------------
$gcc -O3 main_query11.c header.h hash_table.c murmurhash3.c hash_join.c fnv1a.c -o query11 -lm

For the parallel version, do:
$gcc -O3 main_query11_parallel.c header.h hash_table.c murmurhash3.c hash_join.c fnv1a.c -o query11parallel -lm

-------------------------------
2. To execute the code:
-------------------------------
$./query11 murmur3 supplier.tbl 10000 partsupp.tbl 800000 2039 #id
Note: "#id" is the number of test execution.

or 

$./query11 fnv1a supplier.tbl 10000 partsupp.tbl 800000 2039 #id
Note: "#id" is the number of test execution.


For the parallel version, do:
$./query11parallel_teste murmur3 supplier.tbl 10000 partsupp.tbl 800000 1021 #id
Note: "#id" is the number of test execution.

or

$./query11parallel fnv1a supplier.tbl 10000 partsupp.tbl 800000 1021 #id
Note: "#id" is the number of test execution.

The first parameter determines the type of hash function chosen ('murmur3' or 'fnv1a') to create the hash table.
The second parameter is the name of the smallest table('supplier.tbl') and the third is its size (10000 entries, in 1 GB "file size").
The fourth parameter is the name of the largest table('partsupp.tbl') and the fifth is its size (800000 entries, in 1 GB "file size").
The sixth and final parameter ('2039') determines the size of the hash table (number of positions). It is recommended to use a prime number (if possible, the first prime number below the expected number of entries).

The first 100008 primes:
https://primes.utm.edu/lists/small/100000.txt
-------------------------------
#About MumrmurHash3

MurmurHash is a non-cryptographic hash function created by Austin Appleby (https://github.com/aappleby) in 2008. It is hosted on GitHib as 'SMHasher'.'SMHasher is released under the MIT license. All MurmurHash versions are public domain software, and the author disclaims all copyright to their code. SMHasher is a test suite designed to test the distribution, collision, and performance properties of non-cryptographic hash functions - it aims to be the DieHarder of hash testing, and does a pretty good job of finding flaws with a number of popular hashes.

The SMHasher suite also includes MurmurHash3, which is the latest version in the series of MurmurHash functions - the new version is faster, more robust, and its variants can produce 32- and 128-bit hash values efficiently on both x86 and x64 platforms'- Source: https://github.com/aappleby/smhasher

In this version of MurmurHash3, all arithmetic is performed with unsigned 32-bit integers. In the case of overflow, the result is reduced modulo 2^32. It works with bitwise operations.This implementation is for little-endian CPUs.
-------------------------------
#About FNV-1a (Fowler/Noll/Vo)

'The basis of the FNV hash algorithm was taken from an idea sent as reviewer comments to the IEEE POSIX P1003.2 committee by Glenn Fowler and Phong Vo back in 1991. In a subsequent ballot round: Landon Curt Noll improved on their algorithm. Some people tried this hash and found that it worked rather well. In an EMail message to Landon, they named it the 'Fowler/Noll/Vo' or FNV hash. FNV hashes are designed to be fast while maintaining a low collision rate. The FNV speed allows one to quickly hash lots of data while maintaining a reasonable collision rate. The high dispersion of the FNV hashes makes them well suited for hashing nearly identical strings such as URLs, hostnames, filenames, text, IP addresses, etc. 

[...] FNV hash algorithms and source code have been released into the public domain. The authors of the FNV algorithmm took deliberate steps to disclose the algorithmm in a public forum soon after it was invented. More than a year passed after this public disclosure and the authors deliberatly took no steps to patent the FNV algorithm. Therefore it is safe to say that the FNV authors have no patent claims on the FNV algorithm as published'- Source: http://www.isthe.com/chongo/tech/comp/fnv/

The FNV_prime is dependent on n, the size of the hash. The prime is calculated using the formula 2^(n−8)+2^8
32-bit magic FNV-1a prime = 16777619

The offset_basis for FNV-1 is dependent on n, the size of the hash.
32 bit offset_basis = 2166136261

FNV1-A is designed for hash sizes that are a power of 2. It makes a bit mask for bit values of hash size that are power of 2
i.e., (u_int32_t)0xffffff

------------------------------
1. Note: Changing the FNV hash size - xor-folding
------------------------------
FNV1-A's authors recommend, "if you need an x-bit hash where x is not a power of 2, that you compute the FNV hash that is just larger than x-bits and xor-fold the result down to x-bits". So, I did the implementation following this advice.
