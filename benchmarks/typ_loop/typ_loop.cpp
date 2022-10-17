#include "typ_loop.h"
#include <stdlib.h>

void typ_loop(in_int_t m, inout_int_t x[1024])
{
  int jj = 16*32;
  for (int j=16; j<32; j++){
    for (int i=2; i<16; i++){
#pragma HLS PIPELINE
        x[jj+i] = x[jj-32+i+m] + j;
    }
    jj += 32;
  }
}

int main() {
  int x[1024];

  int m = 8;

  for (int i = 0; i < 1024; i++) {
    x[i] = rand()%16;
  }

  typ_loop(m, x);

  return 0;
}
