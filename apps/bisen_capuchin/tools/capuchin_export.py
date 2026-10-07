#!/usr/bin/env python3
"""
capuchin_export.py -- generate Capuchin's model header for the RUIC LeNet with
Capuchin's OWN encoder (upstream encoder.py at 76b6eb2, used unmodified).

Two headers are written from one MODEL_ARRAY:
  msp430/neural_network_parameters.h   upstream template, byte-for-byte as
                                       encoder.export_model() writes it; drop into
                                       capuchin-MCU/ for the native MSP430FR5994 run
  src/capuchin/neural_network_parameters.h
                                       Apollo4 flavour: same MODEL_ARRAY values and
                                       defines; only the MSP430 placement pragmas,
                                       the absolute-address MODEL_ARRAY_END and the
                                       IS_MSP switch are adapted (listed below)

The only extension to the encoder is AveragePooling2D (layer class 6, same
payload as MaxPooling2D's class 3), which the RUIC LeNet needs and upstream
lacks. Upstream encode() silently STOPS at the first unsupported layer, so the
unextended encoder would emit a truncated model; this tool refuses that.

Weight source (choose one):
  --keras lenet_mnist.keras    the original float model (preferred; not yet in repo)
  --ruic-header lenet_weights.h --act-range R[,R2,R3,R4] [--logit-mult m]
                               float reconstruction of the deployed int8 model;
                               see ruic_model.py for why R is a free parameter

Environment: TF_USE_LEGACY_KERAS=1 with tf_keras (Keras 2 API) and fxpmath.
"""
import argparse, hashlib, json, os, re, shutil, sys, tempfile, importlib.util

os.environ.setdefault('TF_USE_LEGACY_KERAS', '1')
os.environ.setdefault('TF_CPP_MIN_LOG_LEVEL', '3')
HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
sys.path.insert(0, HERE)

UPSTREAM_ENCODER_SHA256 = 'd831518239e58dbaaaad425c419845be38814642b2a5fe1f569233cee2adf7bc'
AVGPOOL_CODE = 6


def sha256(path_or_bytes):
    b = open(path_or_bytes, 'rb').read() if isinstance(path_or_bytes, str) else path_or_bytes
    return hashlib.sha256(b).hexdigest()


def load_upstream_encoder(path):
    sys.dont_write_bytecode = True        # keep upstream/ pristine (no __pycache__)
    if sha256(path) != UPSTREAM_ENCODER_SHA256:
        raise SystemExit('upstream encoder.py hash mismatch: %s' % path)
    spec = importlib.util.spec_from_file_location('encoder', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def make_encode_ext(enc):
    """upstream encode() with one more branch (AveragePooling2D -> class 6)."""
    from tensorflow.keras import layers as kl

    def encode_layer_ext(layer):
        code = enc.encode_layer(layer)
        if code == -1 and isinstance(layer, kl.AveragePooling2D):
            return AVGPOOL_CODE
        return code

    def encode_ext(model):
        encode_list = []
        if enc.encode_model(model) == -1:
            raise SystemExit('Capuchin supports Sequential models only')
        encode_list.append(enc.encode_model(model))
        for layer in model.layers:
            code = encode_layer_ext(layer)
            if code == -1:
                raise SystemExit('layer %s (%s) unsupported by Capuchin; upstream would truncate here'
                                 % (layer.name, type(layer).__name__))
            encode_list.append(code)
            if code == 0:
                encode_list += enc.encode_dense_data(layer)
            elif code == 2:
                encode_list += enc.encode_conv2d_data(layer)
            elif code in (3, AVGPOOL_CODE):
                encode_list += enc.encode_maxpooling2d_data(layer)   # same fields
            # 1 LeakyReLU, 4 Flatten, 5 Dropout: code only, as upstream
        return encode_list
    return encode_ext


def apollo4_header(msp_text):
    """Adapt the upstream (MSP430) header for GCC/Apollo4. Values untouched."""
    t = msp_text
    subs = [
        ('#define IS_MSP\n',
         '/* [PORT] IS_MSP selects upstream\'s LEA code paths; CAPUCHIN_KERNEL=0 builds the non-MSP paths. */\n'
         '#if !defined(CAPUCHIN_KERNEL) || (CAPUCHIN_KERNEL != 0)\n#define IS_MSP\n#endif\n'),
        (re.search(r'#define MODEL_ARRAY_END 0x[0-9a-f]+\n', t).group(0),
         '/* [PORT] upstream: absolute FRAM address (0x18000 + 2*length), compared with a pointer. */\n'
         '#define MODEL_ARRAY_END (MODEL_ARRAY + MODEL_ARRAY_LENGTH)\n'),
        ('#pragma LOCATION(MODEL_ARRAY, 0x18000)\n#pragma PERSISTENT(MODEL_ARRAY)\nstatic dtype MODEL_ARRAY[',
         '/* [PORT] upstream places MODEL_ARRAY in FRAM at 0x18000 (#pragma LOCATION/PERSISTENT).\n'
         ' * Default here: const, i.e. MRAM like the RUIC engine\'s weights; -DCAPUCHIN_MODEL_QUAL=\n'
         ' * (empty) puts it in SRAM as an ablation. */\n'
         '#ifndef CAPUCHIN_MODEL_QUAL\n#define CAPUCHIN_MODEL_QUAL const\n#endif\n'
         'static CAPUCHIN_MODEL_QUAL dtype MODEL_ARRAY['),
        ('#pragma PERSISTENT(input_buffer)\n', '/* [PORT] FRAM-persistent on MSP430; SRAM here */\n'),
        ('#pragma PERSISTENT(output_buffer)\n', ''),
    ]
    for a, b in subs:
        if t.count(a) != 1:
            raise SystemExit('unexpected upstream header layout near: %r' % a[:40])
        t = t.replace(a, b)
    banner = ('/* GENERATED by apps/bisen_capuchin/tools/capuchin_export.py -- do not edit.\n'
              ' * Apollo4 flavour of Capuchin\'s neural_network_parameters.h; MODEL_ARRAY is\n'
              ' * value-identical to msp430/neural_network_parameters.h. */\n')
    return banner + t


def model_array_values(text):
    m = re.search(r'MODEL_ARRAY\[MODEL_ARRAY_LENGTH\] = \{(.*?)\};', text, re.S)
    return [int(v) for v in m.group(1).replace('\n', '').split(',') if v.strip()]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument('--keras')
    src.add_argument('--ruic-header')
    ap.add_argument('--act-range', default=None, help='R or R1,R2,R3,R4 (with --ruic-header)')
    ap.add_argument('--logit-mult', type=float, default=1.0)
    ap.add_argument('--encoder', default=os.path.join(APP, 'upstream', 'encoder.py'))
    ap.add_argument('--out-apollo4', default=os.path.join(APP, 'src', 'capuchin', 'neural_network_parameters.h'))
    ap.add_argument('--out-msp430', default=os.path.join(APP, 'msp430', 'neural_network_parameters.h'))
    ap.add_argument('--manifest', default=os.path.join(APP, 'msp430', 'model_manifest.json'))
    a = ap.parse_args()

    import ruic_model as rm
    enc = load_upstream_encoder(a.encoder)
    manifest = {'capuchin_upstream': 'https://github.com/leleonardzhang/Capuchin@76b6eb223f1b6da27520064a8f52b9df6fa4cab5',
                'encoder_sha256': UPSTREAM_ENCODER_SHA256, 'avgpool_extension_code': AVGPOOL_CODE}
    if a.keras:
        from tensorflow import keras
        model = keras.models.load_model(a.keras)
        manifest.update(weight_source='keras', keras_path=os.path.basename(a.keras), keras_sha256=sha256(a.keras))
    else:
        if a.act_range is None:
            raise SystemExit('--act-range is required with --ruic-header')
        R = [float(v) for v in a.act_range.split(',')]
        layers, meta = rm.parse_header(a.ruic_header)
        model = rm.build_keras(rm.float_weights(layers, meta, R if len(R) > 1 else R[0], a.logit_mult))
        manifest.update(weight_source='ruic_int8_header_reconstruction', ruic_header=os.path.basename(a.ruic_header),
                        ruic_header_sha256=sha256(a.ruic_header), act_range=R, logit_mult=a.logit_mult)

    enc.encode = make_encode_ext(enc)          # export_model() calls the module-level encode()
    with tempfile.TemporaryDirectory() as td:
        cwd = os.getcwd()
        os.chdir(td)
        try:
            enc.export_model(model)
        finally:
            os.chdir(cwd)
        msp_text = open(os.path.join(td, 'neural_network_parameters.h')).read()

    a4_text = apollo4_header(msp_text)
    assert model_array_values(a4_text) == model_array_values(msp_text)
    vals = model_array_values(msp_text)
    for path, text in ((a.out_msp430, msp_text), (a.out_apollo4, a4_text)):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, 'w').write(text)
    import numpy as np
    arr = np.array(vals, dtype=np.int16)
    manifest.update(model_array_length=len(vals), model_array_sha256=sha256(arr.tobytes()),
                    msp430_header_sha256=sha256(a.out_msp430), apollo4_header_sha256=sha256(a.out_apollo4),
                    weights_saturated_q10=int(np.sum((arr == 32767) | (arr == -32768))))
    json.dump(manifest, open(a.manifest, 'w'), indent=2)
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
