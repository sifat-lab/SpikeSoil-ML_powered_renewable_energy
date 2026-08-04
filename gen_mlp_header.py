"""Emit the C header for the soiling MLP baseline.
Usage: python gen_mlp_header.py soiling_mlp_export.npz soiling_mlp
"""
import numpy as np, sys
npz, prefix = sys.argv[1], sys.argv[2]
z = np.load(npz)

def fmt(v):
    t = f"{float(v):.9g}"
    if "." not in t and "e" not in t and "E" not in t and "inf" not in t:
        t += ".0"
    return t + "f"

def arr(name, a):
    a = np.asarray(a, np.float32).ravel()
    return f"static const float {name}[{a.size}] = {{{','.join(fmt(v) for v in a)}}};\n"

H0, H1 = z["w0"].shape[1], z["w1"].shape[1]
up = prefix.upper()
h  = f"// AUTO-GENERATED from {npz} -- do not edit\n#ifndef {up}_H\n#define {up}_H\n\n"
h += f"#define MLP_T {int(z['T'])}\n#define MLP_NCH {int(z['NCH'])}\n"
h += f"#define MLP_NFEAT {int(z['NFEAT'])}\n#define MLP_H0 {H0}\n#define MLP_H1 {H1}\n\n"
# sklearn coefs_ are [in][out]; keep that layout so the matvec walks rows of input
h += arr("mlp_w0", z["w0"]); h += arr("mlp_b0", z["b0"])
h += arr("mlp_w1", z["w1"]); h += arr("mlp_b1", z["b1"])
h += arr("mlp_w2", z["w2"]); h += arr("mlp_b2", z["b2"])
h += arr("mlp_mu", z["mu"]); h += arr("mlp_sd", z["sd"])
h += "\n#endif\n"
open(f"{prefix}.h", "w").write(h)

gi, go = z["golden_in"], z["golden_out"]
g  = f"// golden vectors from scikit-learn (float32)\n#ifndef {up}_GOLDEN_H\n#define {up}_GOLDEN_H\n"
g += f"#define N_MLP_GOLDEN {gi.shape[0]}\n"
g += arr("mlp_golden_in", gi); g += arr("mlp_golden_out", go)
g += "\n#endif\n"
open(f"{prefix}_golden.h", "w").write(g)
n = sum(np.asarray(z[k]).size for k in ["w0","b0","w1","b1","w2","b2"])
print(f"{prefix}: {n} params, {int(z['NFEAT'])}-{H0}-{H1}-1")
