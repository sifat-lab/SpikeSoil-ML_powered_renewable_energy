/* bench_main.cpp -- SpikeSoil Phase D on-device benchmark (ESP32-S3)
 *
 * Serial commands (115200):
 *   v  verify against PyTorch golden vectors
 *   l  latency benchmark
 *   m  memory report
 *   a  ENERGY: 30 s of continuous inference   (measure mean rail current)
 *   b  ENERGY: 30 s idle at the same clock    (baseline to subtract)
 *
 * Energy method: a single INA219 sample is far slower than one inference, so
 * per-inference energy is never measured directly. Instead run mode 'a' and
 * mode 'b' for the same wall-clock duration and take the difference:
 *
 *     E_inf = (I_busy - I_idle) * V_rail * 30 s / n_inferences
 *
 * The subtraction removes the regulator, flash and idle-core draw, which
 * otherwise dominate and would make the ANN and SNN look identical.
 *
 * MEASUREMENT DISCIPLINE -- all of this goes in the paper's methods:
 *   - WiFi and BT off (radio draw is ~100x the compute we are measuring)
 *   - CPU pinned to 240 MHz, no light sleep, no DFS
 *   - same board, same USB supply, same ambient temperature for both modes
 *   - GPIO_MARKER goes high during inference so a scope can confirm the duty
 *     cycle really is ~100% in mode 'a'
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_timer.h>
#include <esp_pm.h>
extern "C" {
  #include "lif_snn.h"
}
#include "golden.h"
#include "soiling_snn.h"

#define GPIO_MARKER  21   /* HIGH during event-driven inference */
#define GPIO_MARKER2 47   /* HIGH during dense inference */
#define GPIO_AUTORUN 14   /* tie to GND to run the energy sequence on boot */
#define ENERGY_SECONDS 30

static float in_buf[SNN_T * SNN_NF];

static void verify() {
  float worst = 0;
  Serial.println(F("\n idx        device        pytorch       abs err"));
  for (int n = 0; n < N_GOLDEN; ++n) {
    const float *x = &golden_in[n * SNN_T * SNN_NF];
    snn_stats_t st;
    float y = snn_infer(x, &st);
    float e = fabsf(y - golden_out[n]);
    if (e > worst) worst = e;
    Serial.printf("%4d  %12.8f  %12.8f  %12.3e\n", n, y, golden_out[n], e);
  }
  Serial.printf("worst = %.3e  -> %s\n", worst,
                worst < 1e-4f ? "PASS" : "FAIL (kernel does not match training)");
}

static void latency() {
  snn_stats_t sp, dn;
  volatile float sink = 0;
  unsigned long mac_sum = 0;
  float rate_sum = 0;

  /* An event-driven net's cost depends on its INPUT. Benchmarking one window
   * measures that window, not the model. Rotate through all of them. */
  for (int n = 0; n < N_GOLDEN; ++n) {
    snn_infer(&golden_in[n * SNN_T * SNN_NF], &sp);
    Serial.printf("  window %d: %lu MACs  rate %.4f\n", n, sp.macs, sp.rate);
    mac_sum += sp.macs; rate_sum += sp.rate;
  }

  for (int i = 0; i < 50; ++i)
    sink += snn_infer(&golden_in[(i % N_GOLDEN) * SNN_T * SNN_NF], &sp);

  const int N = 2000;
  int64_t t0 = esp_timer_get_time();
  for (int i = 0; i < N; ++i)
    sink += snn_infer(&golden_in[(i % N_GOLDEN) * SNN_T * SNN_NF], &sp);
  int64_t t1 = esp_timer_get_time();
  for (int i = 0; i < N; ++i)
    sink += snn_infer_dense(&golden_in[(i % N_GOLDEN) * SNN_T * SNN_NF], &dn);
  int64_t t2 = esp_timer_get_time();
  for (int i = 0; i < N; ++i)
    sink += snn_infer_lif_only(&golden_in[(i % N_GOLDEN) * SNN_T * SNN_NF], NULL);
  int64_t t3 = esp_timer_get_time();

  Serial.printf("\nevent-driven : %8.2f us/inf   %.0f MACs (mean)  rate %.4f\n",
                (double)(t1 - t0) / N, (double)mac_sum / N_GOLDEN,
                rate_sum / N_GOLDEN);
  Serial.printf("dense        : %8.2f us/inf   %lu MACs\n",
                (double)(t2 - t1) / N, dn.macs);
  Serial.printf("MAC reduction: %.1f%%   time reduction: %.1f%%\n",
                100.0 * (1.0 - ((double)mac_sum / N_GOLDEN) / dn.macs),
                100.0 * (1.0 - (double)(t1 - t0) / (t2 - t1)));
  double t_lif = (double)(t3 - t2) / N;
  double t_ev  = (double)(t1 - t0) / N;
  Serial.printf("LIF dynamics only : %8.2f us/inf  (no synapses at all)\n", t_lif);
  Serial.printf("  -> neuron floor is %.1f%% of the event-driven latency\n",
                100.0 * t_lif / t_ev);
  Serial.printf("  -> synaptic part  : %8.2f us  (this is what sparsity shrinks)\n",
                t_ev - t_lif);
  Serial.println(F("Report MAC reduction AND time reduction -- they differ because"));
  Serial.println(F("the neuron floor does not shrink with sparsity."));
  (void)sink;
}

static void memory_report() {
  size_t w = sizeof(snn_f1_w) + sizeof(snn_f1_b) + sizeof(snn_f2_wT)
           + sizeof(snn_f2_b) + sizeof(snn_out_w) + sizeof(snn_out_b)
           + sizeof(snn_mu) + sizeof(snn_sd);
  Serial.printf("\nweights in flash : %u B (%u params, float32)\n",
                (unsigned)w, (unsigned)(w / 4));
  Serial.printf("input buffer     : %u B\n", (unsigned)sizeof(in_buf));
  Serial.printf("scratch (stack)  : ~%u B  (4 x SNN_H floats + locals)\n",
                (unsigned)(4 * SNN_H * sizeof(float)));
  Serial.printf("free heap        : %u B\n", (unsigned)ESP.getFreeHeap());
  Serial.printf("min free heap    : %u B\n", (unsigned)ESP.getMinFreeHeap());
  Serial.printf("stack high-water : %u B\n",
                (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

/* mode: 0 = idle, 1 = event-driven, 2 = dense.
 * Dense needs its own measured energy -- assuming it draws the same power as
 * the sparse kernel would beg the question the benchmark exists to answer. */
static void energy_run(int mode) {
  const char *name = mode == 0 ? "IDLE (no inference)"
                   : mode == 1 ? "BUSY (event-driven)"
                               : "BUSY (dense)";
  Serial.printf("\n%s for %d s\n", name, ENERGY_SECONDS);
  delay(3000);                       // time to start the external logger

  snn_stats_t st;
  volatile float sink = 0;
  unsigned long n = 0;
  int64_t t_end = esp_timer_get_time() + (int64_t)ENERGY_SECONDS * 1000000;

  digitalWrite(GPIO_MARKER,  mode == 1 ? HIGH : LOW);
  digitalWrite(GPIO_MARKER2, mode == 2 ? HIGH : LOW);
  while (esp_timer_get_time() < t_end) {
    const float *x = &golden_in[(n % N_GOLDEN) * SNN_T * SNN_NF];
    if      (mode == 1) { sink += snn_infer(x, &st);       n++; }
    else if (mode == 2) { sink += snn_infer_dense(x, &st); n++; }
  }
  digitalWrite(GPIO_MARKER,  LOW);
  digitalWrite(GPIO_MARKER2, LOW);

  Serial.printf("done. inferences = %lu  (%.0f /s, %.2f us each)\n",
                n, (double)n / ENERGY_SECONDS,
                n ? (double)ENERGY_SECONDS * 1e6 / n : 0.0);
  (void)sink;
}

/* Headless sequence for the energy rig: no USB needed on this board.
 * The marker pin tells the Arduino which bucket each INA219 sample belongs to,
 * so the two modes are measured under identical conditions minutes apart. */
static void auto_sequence() {
  Serial.println(F("\nauto: repeating idle / event-driven / idle / dense until reset"));
  for (int cycle = 1; ; ++cycle) {
    Serial.printf("--- cycle %d ---\n", cycle);
    energy_run(0);
    energy_run(1);
    energy_run(0);
    energy_run(2);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  WiFi.mode(WIFI_OFF);               // radio off: it dwarfs everything measured here
  btStop();
  setCpuFrequencyMhz(240);

  pinMode(GPIO_AUTORUN, INPUT_PULLUP);
  pinMode(GPIO_MARKER, OUTPUT);
  pinMode(GPIO_MARKER2, OUTPUT);
  digitalWrite(GPIO_MARKER, LOW);
  digitalWrite(GPIO_MARKER2, LOW);

  Serial.println(F("\nSpikeSoil Phase D benchmark"));
  Serial.printf("CPU %d MHz | SNN %d-%d-%d, T=%d\n",
                getCpuFrequencyMhz(), SNN_NF, SNN_H, SNN_H, SNN_T);
  Serial.println(F("commands: v verify | l latency | m memory | a sparse | d dense | b idle"));
  /* Run the energy sequence unconditionally. Requiring a jumper meant a
   * missing wire looked exactly like a broken measurement -- BUSY stays empty
   * either way, and there is no USB attached to ask the board why. */
  verify();

  /* Measure latency here too. auto_sequence() below never returns, so loop()
   * is unreachable and the 'l' command can no longer be typed -- every number
   * the board can report has to be produced before the sequence starts. */
  latency();

  Serial.println(F("\nstarting energy sequence in 5 s (reset the board to stop)"));
  delay(5000);
  auto_sequence();
}

void loop() {
  if (!Serial.available()) return;
  switch (Serial.read()) {
    case 'v': verify();            break;
    case 'l': latency();           break;
    case 'm': memory_report();     break;
    case 'a': energy_run(1);       break;
    case 'b': energy_run(0);       break;
    case 'd': energy_run(2);       break;
    case 's': auto_sequence();     break;
    default: break;
  }
}