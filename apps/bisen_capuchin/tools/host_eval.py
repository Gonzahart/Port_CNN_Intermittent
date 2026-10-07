"""Run host builds of Capuchin (and the RUIC engine) over MNIST; shared helpers."""
import os, subprocess, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
def run_capuchin(binary, q10):
    p = subprocess.run([binary], input=np.ascontiguousarray(q10, dtype='<i2').tobytes(), capture_output=True)
    rows = [list(map(int, l.split())) for l in p.stdout.decode().splitlines() if l.strip()]
    a = np.array(rows, dtype=np.int64)
    return a[:, :10], a[:, 10], p.stderr.decode()
def run_ruic(binary, i8):
    p = subprocess.run([binary], input=np.ascontiguousarray(i8, dtype=np.int8).tobytes(), capture_output=True)
    a = np.array([list(map(int, l.split())) for l in p.stdout.decode().splitlines() if l.strip()], dtype=np.int64)
    return a[:, :10], a[:, 10]
