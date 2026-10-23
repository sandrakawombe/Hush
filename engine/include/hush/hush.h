/* Hush C API. Plain C so every platform can call it: WebAssembly (Chrome
 * extension), JNI (Android), Swift (iOS/macOS), C# (Windows).
 *
 *   hush_config cfg = hush_default_config();
 *   cfg.sample_rate = 48000;
 *   hush_engine* h = hush_create(&cfg);
 *   // audio callback:
 *   hush_process(h, in, out, frames);
 *   // UI thread:
 *   int muted = hush_is_muted(h);
 *   hush_destroy(h);
 */
#ifndef HUSH_H
#define HUSH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUSH_API_VERSION 1

typedef struct hush_engine hush_engine;

typedef enum {
  HUSH_DETECTOR_NONE = 0, /* pass-through, never mutes (useful for A/B tests) */
  HUSH_DETECTOR_RULES = 1 /* rules-based cough/sneeze detector */
} hush_detector_kind;

typedef struct {
  double sample_rate;   /* Hz, 8000..192000 */
  size_t max_block;     /* largest frame count per hush_process call */
  double lookahead_ms;  /* added delay, 0..500 */
  double fade_ms;       /* 0..100 */
  double max_mute_ms;   /* safety limit, 100..10000 */
  int32_t detector;     /* a hush_detector_kind value; plain int so any caller value is safe to check */
} hush_config;

typedef struct {
  uint64_t samples, mutes, leaked_samples, late_decisions, forced_unmutes, dropped_decisions;
} hush_stats;

hush_config hush_default_config(void);

/* Returns NULL if the config is out of range or the detector is unavailable. */
hush_engine* hush_create(const hush_config* cfg);
void hush_destroy(hush_engine* h);

/* Audio thread. in and out may alias. frames <= max_block.
 * Returns 0 on success, -1 on bad arguments (out is left untouched). */
int hush_process(hush_engine* h, const float* in, float* out, size_t frames);

/* Any thread. */
void hush_set_enabled(hush_engine* h, int enabled);
int hush_is_muted(const hush_engine* h);
size_t hush_latency_frames(const hush_engine* h);
hush_stats hush_get_stats(const hush_engine* h);
void hush_reset(hush_engine* h);
int hush_api_version(void);

#ifdef __cplusplus
}
#endif
#endif /* HUSH_H */
