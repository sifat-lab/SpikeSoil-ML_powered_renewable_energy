<h1 align="center">SpikeSoil</h1>

<p align="center">
  <b>A solar PV edge node that detects panel soiling and forecasts its own output — on fewer than 5,000 parameters.</b>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Espressif-ESP32--S3-E7352C?style=flat-square&logo=espressif&logoColor=white" alt="ESP32-S3">
  <img src="https://img.shields.io/badge/PlatformIO-FF7F00?style=flat-square&logo=platformio&logoColor=white" alt="PlatformIO">
  <img src="https://img.shields.io/badge/PyTorch-EE4C2C?style=flat-square&logo=pytorch&logoColor=white" alt="PyTorch">
  <img src="https://img.shields.io/badge/snnTorch-6E44FF?style=flat-square" alt="snnTorch">
  <img src="https://img.shields.io/badge/status-active-success?style=flat-square" alt="Status">
</p>

---

## What this is

Dust and dirt on a solar panel can cost double-digit percentages of its output, and in most
installations nobody notices until someone climbs up to look. SpikeSoil is an attempt to catch
it from the electrical signature instead: a sensor node that logs a panel's real behaviour,
learns what a clean panel looks like under given irradiance and temperature, and flags the gap.

The same node also forecasts its own output 15 to 60 minutes ahead, which is what makes load
scheduling possible.

The constraint that shapes everything here: **the model has to run on the microcontroller that
collects the data.** No gateway, no cloud, no server. That rules out most of the forecasting
literature and is the reason spiking neural networks are worth the trouble — an SNN that fires
on 13% of timesteps does roughly a tenth of the arithmetic of its dense equivalent.

---

## Hardware

| Component | Purpose | Notes |
|---|---|---|
| ESP32-S3 (N16R8 clone) | MCU, Wi-Fi, logging | 16 MB flash; **PSRAM non-functional** on this clone despite the label |
| 2× INA219 | Panel current & voltage | I²C, addresses `0x40` / `0x41` — one per panel |
| BH1750 | Irradiance proxy | I²C |
| DS18B20 | Panel temperature | 1-Wire, GPIO 15 |
| microSD module | Local CSV storage | SPI over FSPI — SCK 4, MISO 5, MOSI 6, CS 7 |
| 2× 3 W PV panel | Clean / soiled pair | Vmp 7 V, Imp 428 mA, Voc 8.4 V |
| 2× 16 Ω 10 W resistor | Resistive load | |

Two identical panels run side by side: one kept clean as the reference, one allowed to soil.
The differential between them is the ground truth the detector is trained against.

> **Two things that cost me days, recorded here so they don't cost you any:**
>
> - Run the SD module from **3.3 V, not 5 V.** On this board the 5 V rail produced
>   intermittent write failures that looked like card corruption.
> - **Breadboard contact resistance is not negligible at these currents.** A loose rail
>   contact silently corrupted about three days of logging before I traced it. The circuit
>   is being migrated to a soldered board for exactly this reason.

---

## Firmware

PlatformIO project under `firmware/`.

- Connects to Wi-Fi and syncs time over NTP, so every row carries a real timestamp
- Samples all four sensors on a 5-second cadence
- Appends one CSV row per sample with **sync-per-write plus retry logic** — a pulled power
  cable costs one row, not the whole file
- Continues logging through individual sensor dropout rather than halting

```bash
cd firmware
pio run -t upload
pio device monitor
```

Log format:

```
timestamp,v_panel_a,i_panel_a,v_panel_b,i_panel_b,lux,temp_c
```

---

## Forecasting models

Trained on the [DKASC](http://dkasolarcentre.com.au/) public PV dataset at 5-minute
resolution, with a 12-step input window (one hour of history).

| Model | Params | MAE @15 min | @30 min | @60 min |
|---|---|---|---|---|
| Persistence (baseline) | — | 0.111 kW | 0.156 kW | 0.241 kW |
| **GRU** | 4,035 | **0.058 kW** | **0.063 kW** | **0.073 kW** |
| **SNN** — snnTorch Leaky + integrator readout | 4,867 | 0.066 kW | 0.070 kW | 0.076 kW |

**Reading these numbers honestly:** persistence is a weak baseline on a 60-minute horizon —
beating it by 70% there is less impressive than it sounds, because cloud cover makes
"whatever it was an hour ago" a poor guess. The 15-minute column is the harder test, and
both models roughly halve the error there.

The interesting result is the comparison between the two. The SNN gives up about 12% of the
GRU's accuracy at 15 minutes and essentially matches it by 60 minutes, while operating at a
**13% firing rate (87% spike sparsity)**. Energy per inference on real hardware has not been
measured yet — that's on the roadmap below, and until it's done the efficiency claim is an
argument from operation count, not a measurement.

Notebook: `ml/spikesoil_ml_01_dataset.ipynb`

---

## Repository structure

```
firmware/     PlatformIO project — ESP32-S3 data logger
ml/           Training notebooks, model checkpoints, normalisation stats
data/         Logged CSVs from field runs
docs/         Wiring diagrams, bench photos, notes
PROJECT_STATE.md   Current status and open threads
```

---

## Status & roadmap

- [x] End-to-end firmware with reliable SD logging
- [x] Full sensor suite verified, first rooftop sun test (~336 mA peaks)
- [x] GRU and SNN forecasters trained and benchmarked on DKASC
- [ ] Migration from breadboard to soldered board
- [ ] Multi-day clean-panel baseline, both panels
- [ ] Controlled soiling experiment with the differential ground truth
- [ ] On-device SNN inference on ESP32-S3, with **measured** energy per inference

Current work is the board migration — the forecasting side is ahead of the hardware, and the
soiling detector can't be trained until there's a clean multi-day baseline to compare against.

---

## Author

**Md. Tahsinul Sifat** — EEE undergraduate, Daffodil International University, Dhaka
[LinkedIn](https://www.linkedin.com/in/md-tahsinul-sifat-959269407) · [GitHub](https://github.com/sifat-lab)

Built as a Renewable Energy course engineering project; continuing as independent research.
