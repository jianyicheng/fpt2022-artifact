#include "Tri_Sp_Slv.h"
#include <stdlib.h>


void Tri_Sp_Slv(in_int_t lb,
		       in_int_t ub,
		       inout_float_t x[100],
		       inout_float_t L[10000])
{
  int i;
  int ii = lb*100;
  for(i = lb; i<=ub ; i++){
#pragma HLS PIPELINE
    x[i] = x[i] - L[ii+8]*x[8];
    ii += 100;
  }
}

int main() {
  float x[100];
  float L[10000];

  srand(9);

  for (int i = 0; i < 100; i++) {
    x[i] = rand()%100;
  }

  for (int i = 0; i < 10000; i++) {
    L[i] = rand()%10;
  }

  int ub = 99;
  int lb = 1;

  Tri_Sp_Slv(lb, ub, x, L);

  return 0;
}
