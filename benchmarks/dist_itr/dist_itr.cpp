#include "dist_itr.h"
#include <stdlib.h>
// Data set size

void dist_itr(inout_float_t A[200]) {
#pragma HLS RESOURCE core=RAM_S2P_BRAM latency=2 variable=A 
  for (int i = 0; i < 100; i++) {
#pragma HLS PIPELINE II = 14
    A[2 * i] = A[i] + 0.5f;
  }
}

int main() {
  float A[200];

  srand(9);

  for (int i = 0; i < 200; i++) {
    A[i] = 100;
  }

  dist_itr(A);

  return 0;
}
