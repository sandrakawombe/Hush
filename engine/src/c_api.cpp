#include <cmath>
#include <memory>
#include <new>

#include "hush/engine.hpp"
#include "hush/hush.h"

struct hush_engine {
  std::unique_ptr<hush::Engine> e;
};

namespace {
bool in_range(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }
}  // namespace

extern "C" {

hush_config hush_default_config(void) {
  hush_config c;
  c.sample_rate = 16000;
  c.max_block = 4096;
  c.lookahead_ms = 100;
  c.fade_ms = 15;
  c.max_mute_ms = 2500;
  c.detector = HUSH_DETECTOR_NONE;
  return c;
}

hush_engine* hush_create(const hush_config* c) {
  if (!c || !in_range(c->sample_rate, 8000, 192000) || c->max_block == 0 || c->max_block > (1u << 20) ||
      !in_range(c->lookahead_ms, 0, 500) || !in_range(c->fade_ms, 0, 100) || !in_range(c->max_mute_ms, 100, 10000))
    return nullptr;
  std::unique_ptr<hush::Detector> det;
  switch (c->detector) {
    case HUSH_DETECTOR_NONE:
      break;
    default:
      return nullptr;  // HUSH_DETECTOR_RULES arrives in piece 3
  }
  hush::Config cfg;
  cfg.sample_rate = c->sample_rate;
  cfg.max_block = c->max_block;
  cfg.lookahead_ms = c->lookahead_ms;
  cfg.fade_ms = c->fade_ms;
  cfg.max_mute_ms = c->max_mute_ms;
  auto* h = new (std::nothrow) hush_engine;
  if (!h) return nullptr;
  try {
    h->e = std::make_unique<hush::Engine>(cfg, std::move(det));
  } catch (...) {
    delete h;
    return nullptr;
  }
  return h;
}

void hush_destroy(hush_engine* h) { delete h; }

int hush_process(hush_engine* h, const float* in, float* out, size_t frames) {
  if (!h || !in || !out || frames > h->e->config().max_block) return -1;
  h->e->process(in, out, frames);
  return 0;
}

void hush_set_enabled(hush_engine* h, int on) {
  if (h) h->e->set_enabled(on != 0);
}
int hush_is_muted(const hush_engine* h) { return h && h->e->muted_now() ? 1 : 0; }
size_t hush_latency_frames(const hush_engine* h) { return h ? h->e->latency_samples() : 0; }

hush_stats hush_get_stats(const hush_engine* h) {
  hush_stats s{};
  if (!h) return s;
  const hush::Stats t = h->e->stats();
  s.samples = t.samples;
  s.mutes = t.mutes;
  s.leaked_samples = t.leaked_samples;
  s.late_decisions = t.late_decisions;
  s.forced_unmutes = t.forced_unmutes;
  s.dropped_decisions = t.dropped_decisions;
  return s;
}

void hush_reset(hush_engine* h) {
  if (h) h->e->reset();
}
int hush_api_version(void) { return HUSH_API_VERSION; }

}  // extern "C"
