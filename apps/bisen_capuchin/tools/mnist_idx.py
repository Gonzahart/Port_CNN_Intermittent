"""Minimal reader for the original MNIST IDX files (yann.lecun.com format).
Official MD5s: t10k-images 9fb629c4189551a2d022fa330f9573f3,
t10k-labels ec29112dd5afa0611ce80d1b7f02629c,
train-images f68b3c2dcbeaaa9fbdd348bbdeb94873, train-labels d53e105ee54ea40749a09fcbcd1e9432."""
import gzip, hashlib, os
import numpy as np
MD5 = {'t10k-images-idx3-ubyte.gz': '9fb629c4189551a2d022fa330f9573f3',
       't10k-labels-idx1-ubyte.gz': 'ec29112dd5afa0611ce80d1b7f02629c',
       'train-images-idx3-ubyte.gz': 'f68b3c2dcbeaaa9fbdd348bbdeb94873',
       'train-labels-idx1-ubyte.gz': 'd53e105ee54ea40749a09fcbcd1e9432'}
def _read(d, f):
    raw = open(os.path.join(d, f), 'rb').read()
    if hashlib.md5(raw).hexdigest() != MD5[f]:
        raise ValueError('MNIST file %s does not match the official MD5' % f)
    return gzip.decompress(raw)
def load(d, split='test'):
    p = 't10k' if split == 'test' else 'train'
    x = np.frombuffer(_read(d, p + '-images-idx3-ubyte.gz'), np.uint8, offset=16).reshape(-1, 28, 28)
    y = np.frombuffer(_read(d, p + '-labels-idx1-ubyte.gz'), np.uint8, offset=8)
    return x, y
