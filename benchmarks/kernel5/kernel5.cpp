#include "kernel5.h"
#include <stdlib.h>

void kernel5(inout_float_t x[1001], inout_float_t y[1001],
             inout_float_t z[1001], in_int_t n) {
#pragma HLS RESOURCE core=RAM_S2P_BRAM latency=2 variable=x
#pragma HLS RESOURCE core=RAM_S2P_BRAM latency=2 variable=y 
#pragma HLS RESOURCE core=RAM_S2P_BRAM latency=2 variable=z 
  int l, i;
  for (l = 1; l <= 500; l++) {
    for (i = 1; i < n; i++) {
#pragma HLS PIPELINE
#pragma HLS loop_flatten off
      x[i] = z[i] * (y[i] - x[i - 1]);
    }
  }
}

int main() {
  float x[1001];
  float y[1001];
  float z[1001];
  int n = 5;

  srand(9);

  for (int i = 0; i < 1001; i++) {
    x[i] = rand();
  }

  for (int i = 0; i < 1001; i++) {
    y[i] = rand();
  }

  for (int i = 0; i < 1001; i++) {
    z[i] = rand();
  }

  kernel5(x, y, z, n);

  return 0;
}
