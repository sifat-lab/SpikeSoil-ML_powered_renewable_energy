"""Emit C headers for the GRU baseline.
Usage: python gen_gru_header.py forecast_gru_export.npz forecast_gru
"""
import numpy as np, sys
npz, prefix = sys.argv[1], sys.argv[2]
z = np.load(npz)
def fmt(v):
    t = f"{float(v):.9g}"
    if "." not in t and "e" not in t and "E" not in t and "inf" not in t: t += ".0"
    return t + "f"
def arr(n, a):
    a = np.asarray(a, np.float32).ravel()
    return f"static const float {n}[{a.size}] = {{{','.join(fmt(v) for v in a)}}};\n"
NF,H,T,NOUT = int(z["NF"]),int(z["H"]),int(z["T"]),int(z["NOUT"])
up = prefix.upper()
h  = f"// AUTO-GENERATED from {npz} -- do not edit\n#ifndef {up}_H\n#define {up}_H\n\n"
h += f"#define GRU_NF {NF}\n#define GRU_H {H}\n#define GRU_T {T}\n#define GRU_NOUT {NOUT}\n"
h += f"#define GRU_Y_MU  {fmt(z['y_mu'])}\n#define GRU_Y_SIG {fmt(z['y_sig'])}\n\n"
h += arr("gru_w_ih", z["w_ih"]);  h += arr("gru_b_ih", z["b_ih"])
h += arr("gru_w_hh", z["w_hh"]);  h += arr("gru_b_hh", z["b_hh"])
h += arr("gru_out_w", z["out_w"]); h += arr("gru_out_b", z["out_b"])
h += arr("gru_mu", z["mu"]);       h += arr("gru_sd", z["sd"])
h += "\n#endif\n"
open(f"{prefix}.h","w").write(h)
gi, go = z["golden_in"], z["golden_out"]
g  = f"#ifndef {up}_GOLDEN_H\n#define {up}_GOLDEN_H\n#define N_GRU_GOLDEN {gi.shape[0]}\n"
g += arr("gru_golden_in", gi); g += arr("gru_golden_out", go); g += "\n#endif\n"
open(f"{prefix}_golden.h","w").write(g)
n = sum(np.asarray(z[k]).size for k in ["w_ih","b_ih","w_hh","b_hh","out_w","out_b"])
print(f"{prefix}: {n} params, GRU({NF},{H}) + Linear({H},{NOUT}), T={T}")
