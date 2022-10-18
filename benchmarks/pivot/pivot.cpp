#include "pivot.h"
#include <stdlib.h>

void pivot(in_int_t k, in_int_t n, inout_float_t A[40000]) {
#pragma HLS RESOURCE core=RAM_S2P_BRAM latency=2 variable=A 
  int i, j;
  int kk = 200 * k;
  int ii = 200 * (k + 1);
  for (i = k + 1; i <= n; i++) {
    for (j = k + 1; j <= n; j++) {
#pragma HLS loop_flatten off
#pragma HLS PIPELINE
      A[ii + j] = A[ii + j] - A[kk + j] * A[ii + k];
    }
    ii += 200;
  }
}

int main() {
  float A[40000];
  int n = 180;
  int k = 100;

  for (int i = 0; i < 40000; i++) {
    A[i] = rand() % 100;
  }

  pivot(k, n, A);

  return 0;
}
