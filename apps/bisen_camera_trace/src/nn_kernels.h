// nn_kernels.h -- kernel registry, internal to the engine.
//
// Not public API; callers include nn_engine.h. Exists so the walker can
// dispatch without seeing how a kernel is written, and so adding an op only
// touches nn_kernels.c.
#ifndef NN_KERNELS_H
#define NN_KERNELS_H

#include <stdint.h>
#include "nn_engine.h"

// Every kernel has this signature: consume units [u0, u0+n) using only the
// input buffer, the accumulator band and the descriptor. That is what makes it
// stoppable -- nothing carries over except what lives in those buffers, and all
// of them are inside nn_ctx_t where a checkpoint can reach them.
//
// A unit is one input pixel for convolution and pooling (input-stationary), one
// output neuron for dense. `acc` holds the part-accumulated output rows; it is
// meaningless between layers and is re-opened from the bias as each output row
// is first touched.
typedef void (*nn_kernel_fn)(const nn_layer_t *L, const int8_t *in,
                             int8_t *out, int32_t *acc, int u0, int n);

// Indexed by the op ids in nn_engine.h.
extern const nn_kernel_fn nn_kernels[NN_OP_COUNT];

#endif  // NN_KERNELS_H
