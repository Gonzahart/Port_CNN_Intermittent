#!/usr/bin/env python3
"""
capuchin_scale_sweep.py -- choose the free activation scale of the int8->float
reconstruction (ruic_model.py) in Capuchin's favour, on TRAINING data only.

Rule (pre-declared, baseline-favourable): evaluate Capuchin's exact integer
arithmetic (capuchin_ref.py, LEA path) on the first --n MNIST training images
for each candidate act_range R, pick the R with the highest training accuracy
(ties -> the smallest R: more fractional resolution), and only then report the
held-out test accuracy of that one choice. The test set never influences R.
Latency and energy do not depend on R (Capuchin has no data-dependent control
flow other than ReLU/argmax), so R affects only the accuracy column.
"""
import argparse, json, os, sys
os.environ.setdefault('TF_USE_LEGACY_KERAS', '1'); os.environ.setdefault('TF_CPP_MIN_LOG_LEVEL', '3')
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import numpy as np
import ruic_model as rm, capuchin_ref as cr, mnist_idx
import capuchin_export as ce


def encode_array(layers, meta, R, logit_mult=1.0):
    enc = ce.load_upstream_encoder(os.path.join(os.path.dirname(HERE), 'upstream', 'encoder.py'))
    model = rm.build_keras(rm.float_weights(layers, meta, R, logit_mult))
    return np.array(ce.make_encode_ext(enc)(model), np.int64)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ruic-header', required=True)
    ap.add_argument('--mnist-dir', required=True)
    ap.add_argument('--n', type=int, default=10000)
    ap.add_argument('--grid', default='0.5,1,2,3,4,6,8,12,16,24,32')
    ap.add_argument('--out', default=None)
    a = ap.parse_args()
    layers, meta = rm.parse_header(a.ruic_header)
    Xtr, Ytr = mnist_idx.load(a.mnist_dir, 'train'); Xte, Yte = mnist_idx.load(a.mnist_dir, 'test')
    qtr = rm.capuchin_input_q10(rm.mnist_pad32(Xtr[:a.n]))[:, None]
    qte = rm.capuchin_input_q10(rm.mnist_pad32(Xte))[:, None]
    rows = []
    xf = (rm.mnist_pad32(Xtr[:a.n]).astype(np.float32) / 255.0)[..., None]
    for R in [float(v) for v in a.grid.split(',')]:
        arr = encode_array(layers, meta, R)
        _, l = cr.run_batched(arr, qtr, 'lea')
        acc = float((l == Ytr[:a.n]).mean())
        # float reconstruction on the same images: should not depend on R (homogeneity)
        facc = float((rm.build_keras(rm.float_weights(layers, meta, R)).predict(xf, verbose=0, batch_size=1000)
                      .argmax(1) == Ytr[:a.n]).mean())
        rows.append({'act_range': R, 'train_acc': acc, 'float_train_acc': facc})
        print('R=%-5g train_acc=%.4f float_train_acc=%.4f' % (R, acc, facc), flush=True)
    best = max(rows, key=lambda r: (r['train_acc'], -r['act_range']))
    arr = encode_array(layers, meta, best['act_range'])
    _, lt = cr.run_batched(arr, qte, 'lea')
    best['test_acc'] = float((lt == Yte).mean())
    res = {'rule': 'argmax train_acc over grid, ties -> smallest R; test evaluated once',
           'n_train': a.n, 'grid': rows, 'selected': best}
    print(json.dumps(res['selected']))
    if a.out:
        json.dump(res, open(a.out, 'w'), indent=2)


if __name__ == '__main__':
    main()
