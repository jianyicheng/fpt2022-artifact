#include "kernel6.h"
#include <stdlib.h>

void kernel6(inout_float_t w[1001], inout_float_t b[4096], in_int_t n) {
#pragma HLS RESOURCE core=RAM_S2P_BRAM latency=2 variable=w
#pragma HLS RESOURCE core=RAM_S2P_BRAM latency=2 variable=b 
  int kk;
  for (int l = 1; l <= 16; l++) {
    for (int i = 1; i < n; i++) {
      kk = 0;
      for (int k = 0; k < i; k++) {
#pragma HLS loop_flatten off
#pragma HLS PIPELINE
        w[i] = b[kk + i] * w[i - k - 1] + w[i];
        kk += 6;
      }
    }
  }
}

int main() {
  float w[1001];
  float b[4096];
  int n = 10;

  srand(9);

  for (int i = 0; i < 1001; i++) {
    w[i] = rand();
  }

  for (int i = 0; i < 4096; i++) {
    b[i] = rand();
  }

  kernel6(w, b, n);

  return 0;
}
