//***************** "Orders" **********************
//	Implementation:	Marisa Sel Franco				*
//	HiPES - UFPR									*
//***************************************************

#ifndef _ORDERS_H_
#define _ORDERS_H_

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int *o_shippriority;
uint32_t *o_orderkey, *o_custkey; 
//long int *o_orderkey, *o_custkey; 
float *o_totalprice;
// CHECAR SE DEVO ARMAZENAR A DATA COMO CHAR. 
// FORMATO 1996-01-02
char **o_orderstatus, **o_orderdate, **o_orderpriority, **o_clerk, **o_comment;

#endif