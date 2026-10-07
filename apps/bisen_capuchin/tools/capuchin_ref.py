"""
capuchin_ref.py -- independent NumPy model of Capuchin's integer arithmetic,
decoded from MODEL_ARRAY. Written from a reading of upstream 76b6eb2 sources,
not from the port, so agreement with the host-compiled C is a check on BOTH.

  kernel='lea'  IS_MSP paths: conv = filter_im2col (weights <<3 to Q13, q15
                matrix multiply: exact sum >>15 floor, saturate int16, then <<2
                with int16 wrap); dense = matrix_multiply (q15 mpy, then <<5 wrap)
  kernel='cpu'  upstream non-MSP paths: filter_simple / matrix_multiply_vanilla,
                per-product fp_mul (>>10 floor, int16) with int16 accumulation
Common: channel sums / bias add are int16 wrap (fp_add), ReLU, flatten HWC,
avg-pool [PORT A1] = int32 sum / 4 truncated toward zero, argmax ties as upstream.
"""
import numpy as np

DENSE, LEAKY, CONV, MAXPOOL, FLATTEN, DROPOUT, AVGPOOL = 0, 1, 2, 3, 4, 5, 6


def w16(x):
    return ((np.asarray(x, dtype=np.int64) + 32768) % 65536) - 32768


def sat16(x):
    return np.clip(x, -32768, 32767)


def decode(arr):
    a = [int(v) for v in arr]
    assert a[0] == 0
    i, layers = 1, []
    while i < len(a):
        c = a[i]
        if c == DENSE:
            act, kr, kc, br, bc = a[i + 1:i + 6]; i += 6
            W = np.array(a[i:i + kr * kc], np.int64).reshape(kr, kc); i += kr * kc
            b = np.array(a[i:i + br * bc], np.int64); i += br * bc
            layers.append(('dense', act, W, b))
        elif c == CONV:
            act, nf, nc, fr, fc, sr, sc, flen, pad = a[i + 1:i + 10]; i += 10
            assert (sr, sc, pad) == (1, 1, 0), 'only stride 1 / valid modelled'
            W = np.array(a[i:i + flen], np.int64).reshape(nf, nc, fr, fc); i += flen
            b = np.array(a[i:i + nf], np.int64); i += nf
            layers.append(('conv', act, W, b))
        elif c in (MAXPOOL, AVGPOOL):
            pr, pc = a[i + 1:i + 3]; i += 6
            layers.append(('maxpool' if c == MAXPOOL else 'avgpool', pr, pc))
        elif c == FLATTEN:
            i += 1; layers.append(('flatten',))
        elif c == DROPOUT:
            i += 1
        else:
            raise ValueError('layer code %d' % c)
    return layers


def _act(x, act):
    if act == 2:
        return np.where(x >= 0, x, 0)
    if act == 0 or act == 4:   # linear (softmax is decoded as linear upstream)
        return x
    raise NotImplementedError('activation %d' % act)


def _patches(x, fr, fc):
    # x: [N, H, W] -> [N, OH, OW, fr*fc] row-major window (m, n) order
    N, H, W = x.shape
    OH, OW = H - fr + 1, W - fc + 1
    out = np.empty((N, OH, OW, fr * fc), np.int64)
    for m in range(fr):
        for n in range(fc):
            out[..., m * fc + n] = x[:, m:m + OH, n:n + OW]
    return out


def run(arr, q10_chw, kernel='lea'):
    """q10_chw: [N, C, H, W] int16 Q5.10. Returns (scores [N,10] int16, label [N])."""
    layers = decode(arr)
    x = np.asarray(q10_chw, np.int64)            # [N, C, H, W]
    for L in layers:
        if L[0] == 'conv':
            _, act, W, b = L
            nf, nc, fr, fc = W.shape
            P = [_patches(x[:, c], fr, fc) for c in range(nc)]
            outs = []
            for f in range(nf):
                acc = 0
                for c in range(nc):
                    wv = W[f, c].reshape(-1)
                    if kernel == 'lea':
                        s = P[c] @ w16(wv << 3)                        # exact (audited: no int32 overflow)
                        t = w16(sat16(np.floor_divide(s, 1 << 15)) << 2)
                    else:
                        t = w16((np.floor_divide(P[c] * wv, 1 << 10)).sum(-1))
                    acc = w16(acc + t)
                outs.append(_act(w16(acc + b[f]), act))
            x = np.stack(outs, 1)
        elif L[0] in ('maxpool', 'avgpool'):
            _, pr, pc = L
            N, C, H, Wd = x.shape
            v = x[:, :, :H // pr * pr, :Wd // pc * pc].reshape(N, C, H // pr, pr, Wd // pc, pc)
            if L[0] == 'maxpool':
                x = v.max((3, 5))
            else:
                s = v.sum((3, 5)); x = np.fix(s / (pr * pc)).astype(np.int64)
        elif L[0] == 'flatten':
            x = x.transpose(0, 2, 3, 1).reshape(x.shape[0], -1)       # f0[0], f1[0], ... (HWC)
        elif L[0] == 'dense':
            _, act, W, b = L
            if kernel == 'lea':
                s = x @ W.T
                y = w16(sat16(np.floor_divide(s, 1 << 15)) << 5)
            else:
                y = w16(np.floor_divide(x[:, None, :] * W[None], 1 << 10).sum(-1))
            x = _act(w16(y + b), act)
    scores = x.astype(np.int16)
    # upstream argmax: start at index 0, scan i = n-1 .. 1, replace only if strictly greater
    lab = np.zeros(len(scores), np.int64); best = scores[:, 0].astype(np.int64)
    for i in range(scores.shape[1] - 1, 0, -1):
        upd = scores[:, i] > best
        lab[upd] = i; best = np.where(upd, scores[:, i], best)
    return scores, lab


def run_batched(arr, q10_chw, kernel='lea', batch=500):
    S, Lb = [], []
    for i in range(0, len(q10_chw), batch):
        s, l = run(arr, q10_chw[i:i + batch], kernel)
        S.append(s); Lb.append(l)
    return np.concatenate(S), np.concatenate(Lb)


def model_array_from_header(path):
    import re
    t = open(path).read()
    m = re.search(r'MODEL_ARRAY\[MODEL_ARRAY_LENGTH\] = \{(.*?)\};', t, re.S)
    return np.array([int(v) for v in m.group(1).replace('\n', '').split(',') if v.strip()], np.int64)
