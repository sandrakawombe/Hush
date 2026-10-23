// Browser-facing wrapper around the C API, compiled to WebAssembly.
//
// JavaScript cannot pass arrays directly, so each engine owns one input and
// one output buffer inside WebAssembly memory. The audio worklet writes 128
// samples into the input, calls hush_wasm_process, and reads the output.
#include <cstdlib>
#include <new>

#include "hush/hush.h"

#define EXPORT(name) extern "C" __attribute__((export_name(#name)))

struct WasmEngine {
  hush_engine* h;
  float* in;
  float* out;
  size_t block;
  hush_stats stats;
};

EXPORT(hush_wasm_create)
WasmEngine* hush_wasm_create(double sample_rate, int block, double lookahead_ms, int detector) {
  if (block <= 0) return nullptr;
  hush_config c = hush_default_config();
  c.sample_rate = sample_rate;
  c.max_block = static_cast<size_t>(block);
  c.lookahead_ms = lookahead_ms;
  c.detector = detector;
  hush_engine* h = hush_create(&c);
  if (!h) return nullptr;
  auto* w = new (std::nothrow) WasmEngine{h, nullptr, nullptr, static_cast<size_t>(block), {}};
  if (w) {
    w->in = static_cast<float*>(std::calloc(static_cast<size_t>(block), sizeof(float)));
    w->out = static_cast<float*>(std::calloc(static_cast<size_t>(block), sizeof(float)));
  }
  if (!w || !w->in || !w->out) {
    if (w) {
      std::free(w->in);
      std::free(w->out);
      delete w;
    }
    hush_destroy(h);
    return nullptr;
  }
  return w;
}

EXPORT(hush_wasm_destroy)
void hush_wasm_destroy(WasmEngine* w) {
  if (!w) return;
  hush_destroy(w->h);
  std::free(w->in);
  std::free(w->out);
  delete w;
}

EXPORT(hush_wasm_input) float* hush_wasm_input(WasmEngine* w) { return w ? w->in : nullptr; }
EXPORT(hush_wasm_output) float* hush_wasm_output(WasmEngine* w) { return w ? w->out : nullptr; }

// Processes n samples from the input buffer into the output buffer.
// Returns 1 while the outgoing audio is muted, 0 when open, -1 on error.
EXPORT(hush_wasm_process)
int hush_wasm_process(WasmEngine* w, int n) {
  if (!w || n < 0 || static_cast<size_t>(n) > w->block) return -1;
  if (hush_process(w->h, w->in, w->out, static_cast<size_t>(n)) != 0) return -1;
  return hush_is_muted(w->h);
}

EXPORT(hush_wasm_set_enabled) void hush_wasm_set_enabled(WasmEngine* w, int on) { if (w) hush_set_enabled(w->h, on); }
EXPORT(hush_wasm_latency) int hush_wasm_latency(WasmEngine* w) { return w ? static_cast<int>(hush_latency_frames(w->h)) : 0; }
EXPORT(hush_wasm_reset) void hush_wasm_reset(WasmEngine* w) { if (w) hush_reset(w->h); }

// Stats as six doubles at the returned address:
// samples, mutes, leaked_samples, late_decisions, forced_unmutes, dropped_decisions
EXPORT(hush_wasm_stats)
double* hush_wasm_stats(WasmEngine* w) {
  static double out[6];
  if (!w) return out;
  const hush_stats s = hush_get_stats(w->h);
  out[0] = static_cast<double>(s.samples);
  out[1] = static_cast<double>(s.mutes);
  out[2] = static_cast<double>(s.leaked_samples);
  out[3] = static_cast<double>(s.late_decisions);
  out[4] = static_cast<double>(s.forced_unmutes);
  out[5] = static_cast<double>(s.dropped_decisions);
  return out;
}

EXPORT(hush_wasm_api_version) int hush_wasm_api_version() { return hush_api_version(); }
