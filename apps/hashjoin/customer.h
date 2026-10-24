//***************** "Customer" **********************
//	Implementation:	Marisa Sel Franco				*
//	HiPES - UFPR									*
//***************************************************

#ifndef _CUSTOMER_H_
#define _CUSTOMER_H_

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int *c_nationkey;
//long int *c_custkey;
uint32_t *c_custkey;
float *c_acctbal;
char **c_name, **c_address, **c_phone, **c_mktsegment, **c_comment;

#endif