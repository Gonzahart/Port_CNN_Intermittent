#!/usr/bin/env python3
"""
compare_accuracy.py -- held-out (MNIST t10k, 10,000 images) accuracy and
agreement of the RUIC engine and the Capuchin port, from host builds of the
same sources the board runs. Writes results/accuracy_10k.json.

Tier B by construction (TASKS V8/C3): same topology and the same deployed
weights, different arithmetic (int8 per-channel TFLite-style vs Capuchin Q5.10
int16). Reported: accuracy, label agreement, McNemar exact test on the
discordant pairs, and Capuchin's numeric audit counters.
"""
import argparse, json, os, sys, subprocess
from math import comb
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import numpy as np
import ruic_model as rm, mnist_idx, host_eval as he


def mcnemar_exact(b, c):
    n = b + c
    if n == 0:
        return 1.0
    k = min(b, c)
    p = sum(comb(n, i) for i in range(0, k + 1)) / 2 ** n
    return min(1.0, 2 * p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mnist-dir', required=True)
    ap.add_argument('--ruic-header', required=True)
    ap.add_argument('--out', default=os.path.join(os.path.dirname(HERE), 'results', 'accuracy_10k.json'))
    a = ap.parse_args()
    X, Y = mnist_idx.load(a.mnist_dir, 'test'); P = rm.mnist_pad32(X)
    rs, rl = he.run_ruic(os.path.join(HERE, 'host', 'host_ruic_os'), rm.ruic_input_int8(P).reshape(len(P), -1))
    q = rm.capuchin_input_q10(P).reshape(len(P), -1)
    c1s, c1l, audit = he.run_capuchin(os.path.join(HERE, 'host', 'host_capuchin_k1_audit'), q)
    c0s, c0l, _ = he.run_capuchin(os.path.join(HERE, 'host', 'host_capuchin_k0'), q)
    os.environ.setdefault('TF_USE_LEGACY_KERAS', '1'); os.environ.setdefault('TF_CPP_MIN_LOG_LEVEL', '3')
    man = json.load(open(os.path.join(os.path.dirname(HERE), 'msp430', 'model_manifest.json')))
    layers, meta = rm.parse_header(a.ruic_header)
    R = man.get('act_range', [4.0]); R = R if len(R) > 1 else R[0]
    fm = rm.build_keras(rm.float_weights(layers, meta, R, man.get('logit_mult', 1.0)))
    fl = fm.predict((P.astype(np.float32) / 255.0)[..., None], verbose=0, batch_size=1000).argmax(1)

    def acc(l): return float((l == Y).mean())
    out = {'n': int(len(Y)), 'dataset': 'MNIST t10k (official IDX, MD5-checked), 28x28 centred in 32x32',
           'model_manifest': man,
           'accuracy': {'ruic_int8_engine_r2a_OS': acc(rl), 'capuchin_lea_path_q5_10': acc(c1l),
                        'capuchin_cpu_path_q5_10': acc(c0l), 'float_reconstruction': acc(fl)},
           'ruic_top_score_ties': int((np.sort(rs, 1)[:, -1] == np.sort(rs, 1)[:, -2]).sum()),
           'capuchin_numeric_audit': audit.strip()}
    for name, l in (('capuchin_lea', c1l), ('capuchin_cpu', c0l), ('float', fl)):
        b = int(((rl == Y) & (l != Y)).sum()); c = int(((rl != Y) & (l == Y)).sum())
        out['vs_ruic_' + name] = {'label_agreement': float((l == rl).mean()), 'ruic_right_other_wrong': b,
                                  'ruic_wrong_other_right': c, 'mcnemar_exact_p': mcnemar_exact(b, c)}
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    json.dump(out, open(a.out, 'w'), indent=2)
    print(json.dumps({k: out[k] for k in out if k != 'model_manifest'}, indent=2))


if __name__ == '__main__':
    main()
