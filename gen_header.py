"""Turn an exported SNN .npz into C headers.

Usage:  python gen_header.py <model.npz> <prefix>
        python gen_header.py soiling_snn_export.npz  soiling
        python gen_header.py forecast_snn_export.npz forecast

Writes <prefix>_snn.h (weights + geometry) and <prefix>_golden.h (test vectors).
Both models share one kernel; everything that differs between them -- feature
count, hidden width, output count, readout leak -- is a #define here.
"""
import numpy as np, sys

npz, prefix = sys.argv[1], sys.argv[2]
z = np.load(npz)

def g(k, default=None):
    return z[k] if k in z.files else default

NF, H, T = int(z["NF"]), int(z["H"]), int(z["T"])
NOUT     = int(g("NOUT", 1))
beta     = float(z["beta"])
beta_out = float(g("beta_out", 1.0))      # soiling used a pure accumulator

def fmt(v):
    """C float literal. '1f' is an integer with a bad suffix and will not
    compile; '1.0f' is a float. Every scalar and array element goes through
    here -- the first version only guarded arrays and broke on beta_out=1."""
    t = f"{float(v):.9g}"
    if "." not in t and "e" not in t and "E" not in t and "inf" not in t:
        t += ".0"
    return t + "f"

def arr(name, a):
    a = np.asarray(a, np.float32).ravel()
    return f"static const float {name}[{a.size}] = {{{','.join(fmt(v) for v in a)}}};\n"

up = prefix.upper()
h  = f"// AUTO-GENERATED from {npz} -- do not edit\n#ifndef {up}_SNN_H\n#define {up}_SNN_H\n\n"
h += f"#define SNN_NF {NF}\n#define SNN_H  {H}\n#define SNN_T  {T}\n#define SNN_NOUT {NOUT}\n"
h += f"#define SNN_BETA {fmt(beta)}\n#define SNN_BETA_OUT {fmt(beta_out)}\n#define SNN_THRESH 1.0f\n"
h += f"#define SNN_Y_MU  {fmt(g('y_mu', 0.0))}\n"
h += f"#define SNN_Y_SIG {fmt(g('y_sig', 1.0))}\n\n"
h += arr("snn_f1_w", z["f1_w"])            # [H][NF]
h += arr("snn_f1_b", z["f1_b"])
h += arr("snn_f2_wT", np.asarray(z["f2_w"]).T)   # [H_in][H_out]: column per source neuron
h += arr("snn_f2_b", z["f2_b"])
h += arr("snn_out_wT", np.asarray(z["out_w"]).T) # [H][NOUT]: row per source neuron
h += arr("snn_out_b", z["out_b"])
h += arr("snn_mu", z["mu"])
h += arr("snn_sd", z["sd"])
h += "\n#endif\n"
open(f"{prefix}_snn.h", "w").write(h)

gi, go = z["golden_in"], z["golden_out"]
gg  = f"// golden vectors from PyTorch (float32)\n#ifndef {up}_GOLDEN_H\n#define {up}_GOLDEN_H\n"
gg += f"#define N_GOLDEN {gi.shape[0]}\n"
gg += arr("golden_in", gi)                 # [N][T][NF] RAW, un-normalised
gg += arr("golden_out", go)                # [N][NOUT]
gg += "\n#endif\n"
open(f"{prefix}_golden.h", "w").write(gg)

n = sum(np.asarray(z[k]).size for k in ["f1_w","f1_b","f2_w","f2_b","out_w","out_b"])
print(f"{prefix}: {n} params, {NF}-{H}-{H}-{NOUT}, T={T}, "
      f"beta={beta}, beta_out={beta_out}")
