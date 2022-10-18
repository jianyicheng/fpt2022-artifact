

#include "kernel4.h"
#include <stdlib.h>

void kernel4(inout_int_t x[1001], inout_int_t y[1001], in_int_t n) {
#pragma HLS RESOURCE core=RAM_S2P_BRAM latency=2 variable=x
#pragma HLS RESOURCE core=RAM_S2P_BRAM latency=2 variable=y 
  int l, k, j, lw, temp;
  int m = (1001 - 7) / 2;
  for (l = 1; l <= 64; l++) {
    for (k = 6; k < 1001; k = k + m) {
      lw = k - 6;
      temp = x[k - 1];
      for (j = 4; j < n; j = j + 5) {
#pragma HLS PIPELINE
#pragma HLS loop_flatten off
        temp -= x[lw] * y[j];
        lw++;
      }
      x[k - 1] = y[4] * temp;
    }
  }
}

int main() {
  int x[1001];
  int y[1001];
  int n = 32;

  srand(9);

  for (int i = 0; i < 1001; i++) {
    x[i] = rand();
  }

  for (int i = 0; i < 1001; i++) {
    y[i] = rand();
  }

  kernel4(x, y, n);

  return 0;
}
