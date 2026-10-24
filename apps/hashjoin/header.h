//******************* "Header" **********************
//	Implementation:	Marisa Sel Franco				*
//	HiPES - UFPR									*
//***************************************************

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include "supplier.h"
#include "partsupp.h"
#include "customer.h"
#include "orders.h"
#include "murmurhash3.h"
#include "hash_join.h"
#include "fnv1a.h"
//#include <omp.h>

#define LINES
//#define NUM_THREADS 2