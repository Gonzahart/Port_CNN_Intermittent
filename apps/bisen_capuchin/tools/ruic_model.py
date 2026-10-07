"""
ruic_model.py -- read the deployed RUIC int8 LeNet header (lenet_weights.h) and
rebuild an equivalent float Keras model for Capuchin's encoder.

Why a reconstruction: the float source model (lenet_mnist.keras) and its
quantizer are not in the repository. The header holds int8 weights, int32
biases and per-channel requantization (mult, shift) = M = s_in * s_w / s_out, but
not s_w and the hidden activation scales s_out separately. For a network made of
conv/dense + ReLU + average pooling (all positively homogeneous) the hidden
scales are a free per-layer positive factor: every choice yields a float network
whose logits are the same up to one positive constant, i.e. the SAME classifier
in exact arithmetic. The choice only matters to a fixed-point consumer such as
Capuchin's Q5.10, so it is exposed as `act_range` (the real value of int8 code
127 for each hidden activation) and selected on TRAINING data by
capuchin_scale_sweep.py. Input scale (1/255) and output scale (NN_OUT_SCALE) are
taken from the header as they are.

Prefer the original float model when available: capuchin_export.py --keras.
"""
import re
import numpy as np

LAYERS = [  # (index in header, op) for the RUIC LeNet; checked against the table
    (0, 'conv'), (1, 'pool'), (2, 'conv'), (3, 'pool'), (4, 'fc'), (5, 'fc'), (6, 'fc')]


def _arr(text, name, dtype):
    m = re.search(r'static const \w+ %s\[(\d+)\]\s*=\s*\{(.*?)\};' % re.escape(name), text, re.S)
    if not m:
        raise KeyError(name)
    vals = np.array([int(v) for v in m.group(2).replace('\n', ' ').split(',') if v.strip()], dtype=np.int64)
    assert len(vals) == int(m.group(1)), name
    return vals.astype(dtype)


def _define(text, name, cast=float):
    m = re.search(r'#define\s+%s\s+\(?([-0-9.eEfx]+)\)?' % name, text)
    return cast(m.group(1).rstrip('f'))


def parse_header(path):
    text = open(path).read()
    # descriptor rows: { NN_OP_x, in_buf, out_buf, units, in_h, in_w, in_c, out_h, out_w, out_c, k, acc_drop, in_zp, out_zp, act_min, ...}
    rows = re.findall(r'\{\s*NN_OP_(\w+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),', text)
    layers = []
    for i, r in enumerate(rows):
        op = r[0].lower()
        d = dict(op=op, in_h=int(r[4]), in_w=int(r[5]), in_c=int(r[6]), out_h=int(r[7]), out_w=int(r[8]),
                 out_c=int(r[9]), k=int(r[10]), in_zp=int(r[12]), out_zp=int(r[13]), act_min=int(r[14]))
        if op in ('conv', 'fc'):
            d['w'] = _arr(text, 'layer%d_w' % i, np.int8)
            d['b'] = _arr(text, 'layer%d_b' % i, np.int32)
            d['mult'] = _arr(text, 'layer%d_mult' % i, np.int64)
            d['shift'] = _arr(text, 'layer%d_shift' % i, np.int64)
            d['M'] = d['mult'].astype(np.float64) * np.power(2.0, d['shift'].astype(np.float64) - 31.0)
        layers.append(d)
    assert [l['op'] for l in layers] == [op for _, op in LAYERS], [l['op'] for l in layers]
    meta = dict(in_scale=_define(text, 'NN_IN_SCALE'), in_zp=_define(text, 'NN_IN_ZP', int),
                out_scale=_define(text, 'NN_OUT_SCALE'), out_zp=_define(text, 'NN_OUT_ZP', int))
    return layers, meta


def float_weights(layers, meta, act_range, logit_mult=1.0):
    """Return Keras-layout float weights [(kernel, bias), ...] for the 5 weighted
    layers. act_range: scalar or 4-sequence (conv1/pool1, conv2/pool2, fc1, fc2):
    real value represented by int8 code 127 of that hidden activation.
    logit_mult: positive factor on the output scale (argmax-invariant; 1.0 keeps
    the header's NN_OUT_SCALE)."""
    r = np.broadcast_to(np.asarray(act_range, dtype=np.float64), (4,))
    s_hidden = r / 255.0                       # zp = -128 -> codes span 255 steps
    s_in = [meta['in_scale'], s_hidden[0], s_hidden[1], s_hidden[2], s_hidden[3]]
    s_out = [s_hidden[0], s_hidden[1], s_hidden[2], s_hidden[3], meta['out_scale'] * float(logit_mult)]
    out = []
    wl = [l for l in layers if l['op'] in ('conv', 'fc')]
    for j, L in enumerate(wl):
        M = L['M']
        s_w = M * s_out[j] / s_in[j]          # per output channel
        bias = L['b'].astype(np.float64) * s_in[j] * s_w
        if L['op'] == 'conv':
            k, ic, oc = L['k'], L['in_c'], L['out_c']
            w = L['w'].astype(np.float64).reshape(oc, k, k, ic) * s_w[:, None, None, None]
            kernel = w.transpose(1, 2, 3, 0)  # [ky,kx,ic,oc] (Keras HWIO)
        else:
            n_out, n_in = L['out_c'], L['in_c']
            w = L['w'].astype(np.float64).reshape(n_out, n_in) * s_w[:, None]
            kernel = w.T                       # [in,out]; flatten order HWC = Keras channels_last
        out.append((kernel.astype(np.float32), bias.astype(np.float32)))
    return out


def build_keras(weights=None):
    """The RUIC LeNet as a Keras Sequential model (tf_keras / Keras 2 API, which
    Capuchin's encoder.py requires: it reads layer.input_shape)."""
    from tensorflow.keras import layers, models, Input
    m = models.Sequential([
        Input(shape=(32, 32, 1)),
        layers.Conv2D(6, (5, 5), activation='relu', padding='valid'),
        layers.AveragePooling2D((2, 2)),
        layers.Conv2D(16, (5, 5), activation='relu', padding='valid'),
        layers.AveragePooling2D((2, 2)),
        layers.Flatten(),
        layers.Dense(120, activation='relu'),
        layers.Dense(84, activation='relu'),
        layers.Dense(10, activation='linear'),
    ])
    if weights is not None:
        wl = [l for l in m.layers if l.get_weights()]
        assert len(wl) == len(weights)
        for l, (k, b) in zip(wl, weights):
            l.set_weights([k, b])
    return m


def mnist_pad32(images_u8):
    """RUIC MNIST convention, confirmed by reproducing the engine's held-out
    accuracy: 28x28 digits centred in 32x32 with a 2-pixel zero border."""
    P = np.zeros((len(images_u8), 32, 32), np.uint8)
    P[:, 2:30, 2:30] = images_u8
    return P


def ruic_input_int8(pix_u8):
    """preprocess.c quantization of x = p/255: lroundf(x/NN_IN_SCALE) + NN_IN_ZP.
    Exactly p - 128 for integer pixels (verified bit-exact in float32)."""
    return (pix_u8.astype(np.int16) - 128).astype(np.int8)


def capuchin_input_q10(pix_u8):
    """Capuchin's documented input encoding (examples/MNIST notebook):
    Fxp(x, signed, 16, 10) of x = float32(p)/255, CHW. Fxp truncates toward zero
    and saturates (verified against fxpmath 0.4.0 and 0.4.10)."""
    x = pix_u8.astype(np.float32) / np.float32(255.0)
    return np.clip(np.trunc(x.astype(np.float64) * 1024.0), -32768, 32767).astype(np.int16)
