#include "jacobi_2d_imper.h"
#include <stdlib.h>

void jacobi_2d_imper(in_int_t m, inout_float_t A[9216])
{
  int i, j;

  int ii = 32*64;
  int mi = m*64;
  for (i = 32; i < 64 - 1; i++){
    for (j = 32; j < 64 - 1; j++){
#pragma HLS PIPELINE
      A[ii+j] = 0.2f * (A[ii+j] + A[ii+j-m] + A[ii+m+j] + A[ii+mi+j] + A[ii-mi+j]);
    }
    ii += 64;
  }

}

int main() {
  float A[9216];

  int n = 0;
  int m = rand()%32;

  for (int i = 0; i < 9216; i++) {
    A[i] = rand()%32;
  }

  jacobi_2d_imper(m, A);

  return 0;
}
