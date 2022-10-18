#include "dist_param.h"
#include <stdlib.h>

void dist_param(in_int_t m, inout_float_t A[200]) {
#pragma HLS RESOURCE core=RAM_S2P_BRAM latency=2 variable=A 
  for (int i = 0; i < 100; i++) {
#pragma HLS loop_flatten off
#pragma HLS PIPELINE
    A[i + m] = A[i] + 0.5f;
  }
}

int main() {
  float A[200];

  int m = 8;

  for (int i = 0; i < 200; i++) {
    A[i] = rand() % 100;
  }

  dist_param(m, A);

  return 0;
}
