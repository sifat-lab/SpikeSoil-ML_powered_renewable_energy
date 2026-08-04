# Day 1–2 integration — build changes, deviations, open issues

State: kernel interface confirmed against `src/bench/lif_snn.h` and
`src/bench/soiling_golden.h`; channel order and units confirmed against
`ml_05_soiling_snn-1.ipynb` and the `features` array in `soiling_snn.npz`.

## Files

| file | where | change |
|---|---|---|
| `inference.h`, `inference.cpp` | `src/` | new |
| `main.cpp`, `sensors.cpp` | `src/` | replace |
| `sensors.h`, `logger.*` | `src/` | unchanged |
| `INTEGRATION_NOTES.md` | repo root | this file |
| ~~`soiling_norm.h`~~ | — | **not needed, delete if created** |

`lif_snn.c` and `soiling_snn.h` are used unmodified.

## Why there is no normalisation in the firmware

`snn_infer(const float *x_raw, ...)` takes raw physical units. `golden_in`
opens with `3.92, 245.7, 58292.75, 31.0, 4.21` — volts, milliamps, lux, degC,
and `1000 × 245.7 / 58292.75 = 4.215`. The kernel applies `mu`/`sd` internally
from `soiling_snn.h`. Normalising in the firmware would apply it twice.

Consequence worth stating in the report: because the golden input is raw, the
boot self-test covers channel order, the milliamp scaling, the internal
normalisation **and** the kernel arithmetic in one check. The three silent
failure modes flagged at the start of integration are closed by construction.

## platformio.ini

`esp32s3` currently excludes all of `src/bench/`, which is where the kernel
lives — the link would fail on `snn_infer`. Bring back the one file:

```ini
[env:esp32s3]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200

; bench/ stays out (bench_main.cpp has its own setup()/loop() and would
; collide with main.cpp), but the kernel itself comes back in.
build_src_filter = +<*> -<bench/> +<bench/lif_snn.c>

build_flags =
    -Isrc/bench
    -DSNN_MODEL_HEADER=\"soiling_snn.h\"
    -DSNN_GOLDEN_HEADER=\"soiling_golden.h\"

lib_deps =
    adafruit/Adafruit INA219 @ ^1.2.3
    claws/BH1750 @ ^1.3.0
    paulstoffregen/OneWire @ ^2.3.7
    milesburton/DallasTemperature @ ^3.11.0
    greiman/SdFat
```

`bench` and `bench_fc` need no change; they extend `esp32s3` but override both
`build_src_filter` and `build_flags`.

`lif_snn.h` has no `__cplusplus` guard. `inference.cpp` wraps the include in
`extern "C"`, so it links — but adding the guard to the header is the durable
fix, and `bench_main.cpp` will need it too if it is ever compiled as C++.

## Deviations from training — state these in the report

1. **Window cadence.** Training: 12 timesteps × 25 s = 300 s. Node: 12 × 1 s =
   12 s. The network sees twelve values either way and dust does not change
   within a window, but a transient (a passing cloud, a hand) now occupies a
   much larger fraction of a window than it ever did in training. This is the
   assumption most worth a sentence of justification.
2. **Sub-sampling.** Each timestep is the mean of four readings at 4 Hz.
   Training timesteps were 25 s means of the 5 s log — roughly five readings
   averaged — and `mu`/`sd` describe that smoothed distribution. Single
   instantaneous readings would be noisier than anything the model was
   normalised against.
3. **Temperature.** DS18B20 moved to 11-bit asynchronous conversion (0.125 °C,
   375 ms) because the 12-bit blocking call takes 750 ms. `tB` is up to one
   sub-sample stale. Negligible against a panel's thermal time constant.
4. **Display smoothing.** The screen shows an EMA (α = 0.4, ≈5 s settling);
   quoted numbers come from the raw per-window value.

## Open issue: which seed is flashed

The export cell calls `fit_eval(tr, te)` with the default `seed=0`. The 10-seed
table in `ml_05_soiling_snn-1.ipynb` reports LOSO-31 as 0.053 ± 0.021 with a
best seed of 0.024 and a worst of 0.142 — so the deployed weights are one
arbitrary draw from a distribution spanning roughly 6×.

The mean ± sd belongs in the paper, as the notebook says. But the number that
describes *the node on the roof* is seed 0's own MAE, and it is not currently
written down anywhere. Two things worth doing:

- record seed 0's LOSO-31 and LOSO-28 MAE explicitly, and say in the report
  that the deployed model is that seed rather than the fold mean;
- if seed 0 turns out to sit near the bad end, re-export from the median seed
  and note the selection rule. Selecting by test MAE is a form of test-set
  leakage, so if it is done it must be stated plainly rather than quietly.

Either way this does not block the demo — it blocks claiming a specific
accuracy for the demonstrated device.

## Verification order

1. `pio run -e esp32s3` — build must succeed. If `snn_infer` is undefined, the
   `build_src_filter` line is the cause.
2. Boot serial. Five `golden[i]` lines, all `err` around 1e-7, `self-test PASS`.
   Anything else: stop, do not flash to the roof.
3. Indoors: `insufficient irradiance`. This is correct behaviour and is worth
   showing the examiner.
4. Outdoors: `loss=…` rising when shaded or dusted, falling when wiped.
5. Cross-check: run one recorded CSV through the Python model and through
   replay mode, diff the traces.

The `lat=` field is free and worth keeping: it is exactly what `LAT_EVENT_US`
needs in `rail_logger.ino`, where a stale hardcoded value produces a precisely
wrong µJ with no error.

## Housekeeping confirmed

`soiling_snn.npz` and `soiling_snn (1).npz` are byte-identical across every
array — weights, biases, `mu`, `sd`, `features`, `T`, `beta`. Delete the copy.
`ml_05_soiling_snn-1.ipynb` is the one to keep: same export cell, but 10 seeds
and the corrected results table.

## Out of scope here

Web server, SoftAP, payback arithmetic (day 3–4), replay mode (day 5). The loss
estimate is deliberately not logged to SD: adding a column would break the CSV
schema the `ml/` pipeline parses. If it is wanted, write a separate
`infer_YYYYMMDD.csv`.
