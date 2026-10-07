// Host runner for the RUIC engine (engine-r2a sources of the OS app, unmodified).
// stdin: N x 1024 int8 inputs. stdout: 10 int8 scores + argmax per line.
#include <stdio.h>
#include <stdint.h>
#include "nn_engine.h"
static nn_ctx_t ctx;
int main(void){
  int8_t in[NN_IN_LEN];
  while (fread(in,1,NN_IN_LEN,stdin)==NN_IN_LEN){
    int cls = nn_run(&ctx, in);
    const int8_t *s = nn_scores(&ctx);
    for(int i=0;i<NN_OUT_LEN;i++) printf("%d ", s[i]);
    printf("%d\n", cls);
  }
  return 0;
}
