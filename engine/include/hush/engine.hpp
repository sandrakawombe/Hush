// Hush engine: look-ahead delay + detector slot + mute gate.
//
//   mic ──► detector ──► decisions ─┐
//    │                              ▼
//    └──► delay line (look-ahead) ─► gate ─► call
//
// The outgoing audio is held for `lookahead_ms`, so when the detector decides
// "that was a cough, starting 60 ms ago", the gate can still silence it.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "hush/detector.hpp"

namespace hush {

struct Config {
  double sample_rate = 16000;
  size_t max_block = 4096;      // largest block process() will ever be given
  double lookahead_ms = 100;    // delay added to outgoing audio
  double fade_ms = 15;          // ramp length into and out of a mute
  double late_fade_ms = 1;      // ramp used when a decision arrives too late for a full fade
  double max_mute_ms = 2500;    // safety: never stay muted longer than this
};

struct Stats {
  uint64_t samples = 0;         // samples processed
  uint64_t mutes = 0;           // mutes started
  uint64_t leaked_samples = 0;  // samples inside a mute that still went out above -40 dB gain
  uint64_t late_decisions = 0;  // mutes whose start was already sent when decided
  uint64_t forced_unmutes = 0;  // mutes ended by the max_mute_ms safety
  uint64_t dropped_decisions = 0;  // decisions ignored because the schedule was full
};

class Engine {
 public:
  Engine(const Config& cfg, std::unique_ptr<Detector> detector);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Audio thread. `in` and `out` may be the same buffer. n <= max_block.
  // Never allocates, locks or blocks.
  void process(const float* in, float* out, size_t n) { process(in, out, n, nullptr); }
  // Same, and also writes the gain applied to each output sample (1 = open,
  // 0 = muted) into gain_out, for tools and tests. gain_out may be null.
  void process(const float* in, float* out, size_t n, float* gain_out);

  // Any thread. When disabled, audio passes through delayed but never muted
  // (so turning Hush on and off never makes the audio jump).
  void set_enabled(bool on) { enabled_.store(on, std::memory_order_relaxed); }
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
  // Any thread. True while the outgoing audio is muted (for a UI indicator).
  bool muted_now() const { return muted_now_.load(std::memory_order_relaxed); }
  // Any thread. A consistent-enough snapshot for display and telemetry.
  Stats stats() const;

  size_t latency_samples() const { return delay_; }
  const Config& config() const { return cfg_; }
  const char* detector_name() const;
  void reset();  // new call: clear audio, schedule, detector history (not stats)

 private:
  class Schedule;
  static constexpr size_t kStep = 32;  // detector/gate step, aligned to the sample count
  void process_step(const float* in, float* out, size_t n, float* gain_out);
  float gain_for(uint64_t s);

  Config cfg_;
  std::unique_ptr<Detector> detector_;
  std::unique_ptr<Schedule> schedule_;
  std::vector<float> ring_;  // look-ahead delay line
  size_t delay_ = 0, write_ = 0;
  uint64_t next_in_ = 0;     // absolute index of the next input sample
  float gain_ = 1.f, fade_step_ = 0, late_step_ = 0;
  uint64_t max_mute_ = 0;
  std::atomic<bool> enabled_{true};
  std::atomic<bool> muted_now_{false};
  struct AtomicStats {
    std::atomic<uint64_t> samples{0}, mutes{0}, leaked{0}, late{0}, forced{0}, dropped{0};
  } stats_;
};

}  // namespace hush
