//***************** "Partsupp" **********************
//	Implementation:	Marisa Sel Franco				*
//	HiPES - UFPR									*
//***************************************************

#ifndef _PARTSUPP_H_
#define _PARTSUPP_H_

#include <stdio.h>
#include <stdlib.h>

int *ps_availqty;
uint32_t *ps_partkey, *ps_suppkey;
float *ps_supplycost;
char **ps_comment;

#endif