#include "adi_int.h"
#include <stdlib.h>

void adi_int(inout_float_t du1[101], inout_float_t du2[101], inout_float_t du3[101], inout_float_t u1[1010], inout_float_t u2[1010], inout_float_t u3[1010], in_int_t n)
{
    int l, kx, ky, nl1, nl2;
    for ( l=1 ; l<=80 ; l++ ) {
        nl1 = 0;
        nl2 = 1;
        for ( kx=1 ; kx<3 ; kx++ ){
            for ( ky=1 ; ky<n ; ky++ ) {
#pragma HLS PIPELINE
               du1[ky] = u1[(ky+1)*5+kx] - u1[(ky-1)*5+kx];
               du2[ky] = u2[(ky+1)*5+kx] - u2[(ky-1)*5+kx];
               du3[ky] = u3[(ky+1)*5+kx] - u3[(ky-1)*5+kx];
               u1[505+ky*5+kx]=
                  u1[ky*5+kx]+8.0f*du1[ky]+8.0f*du2[ky]+8.0f*du3[ky] +
                     (u1[ky*5+kx+1]-2.0f*u1[ky*5+kx]+u1[ky*5+kx-1]);
               u2[505+ky*5+kx]=
                  u2[ky*5+kx]+8.0f*du1[ky]+8.0f*du2[ky]+8.0f*du3[ky] +
                     (u2[ky*5+kx+1]-2.0f*u2[ky*5+kx]+u2[ky*5+kx-1]);
               u3[505+ky*5+kx]=
                  u3[ky*5+kx]+8.0f*du1[ky]+8.0f*du2[ky]+8.0f*du3[ky] +
                     (u3[ky*5+kx+1]-2.0f*u3[ky*5+kx]+u3[ky*5+kx-1]);
            }
        }
    }
}

int main() {

  float du1[101];
  float du2[101];
  float du3[101];

  float u1[1010];
  float u2[1010];
  float u3[1010];

  int n = 8;

  srand(9);

  for (int i = 0; i < 101; i++) {
    du1[i] = rand();
    du2[i] = rand();
    du3[i] = rand();
  }

  for (int i = 0; i < 1010; i++) {
    u1[i] = rand();
    u2[i] = rand();
    u3[i] = rand();
  }

  adi_int(du1, du2, du3, u1, u2, u3, n);

  return 0;
}
