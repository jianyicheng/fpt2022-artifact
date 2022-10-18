#include "dist_itr_param.h"
#include <stdlib.h>
// Data set size

void dist_itr_param(inout_float_t A[30000]) {
#pragma HLS RESOURCE core=RAM_S2P_BRAM latency=2 variable=A 
  int i, j;

  int ii = 0;
  int mi = 800;
  for (i = 0; i < 100; i++) {
    for (j = 0; j < 2; j++) {
#pragma HLS loop_flatten off
#pragma HLS PIPELINE
      A[2 * ii + mi + j] = A[ii + j] + 0.5f;
    }
    ii += 100;
  }
}

int main() {
  float A[30000];
  srand(9);

  for (int i = 0; i < 30000; i++) {
    A[i] = rand() % 100;
  }

  dist_itr_param(A);

  return 0;
}
