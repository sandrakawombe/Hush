#include "hush/engine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace hush {

namespace {
constexpr uint64_t kOpen = std::numeric_limits<uint64_t>::max();
constexpr float kLeakGain = 0.01f;  // -40 dB: above this, a muted sample still counts as heard
}  // namespace

// Fixed-size list of upcoming and current mutes, on the input timeline.
// Doubles as the MuteSink the detector writes into. Decisions are queued and
// only take effect at the next step boundary, so the result never depends on
// how the host chops audio into blocks. No allocation after construction; if
// a detector floods it, extra decisions are dropped and counted.
class Engine::Schedule final : public MuteSink {
 public:
  struct Interval {
    uint64_t start, end;
  };
  static constexpr size_t kCap = 32;

  uint64_t mutes = 0, late = 0, dropped = 0;

  void begin_mute(uint64_t start) override { queue(true, start); }
  void end_mute(uint64_t end) override { queue(false, end); }

  // Apply queued decisions. `floor` = oldest sample not yet sent; a mute
  // that should have started earlier is too late for its first part.
  void apply(uint64_t floor) {
    for (size_t k = 0; k < npending_; ++k) {
      if (pending_[k].begin) do_begin(pending_[k].at, floor);
      else do_end(pending_[k].at);
    }
    npending_ = 0;
  }

  // First mute that hasn't finished by sample `s` (fade tail included), or null.
  Interval* upcoming(uint64_t s, uint64_t tail) {
    while (count_ > 0 && at(0).end != kOpen && at(0).end + tail <= s) {
      head_ = (head_ + 1) % kCap;
      --count_;
    }
    for (size_t i = 0; i < count_; ++i)
      if (at(i).end > s) return &at(i);
    return nullptr;
  }

  void clear() { head_ = count_ = npending_ = 0; }

 private:
  struct Op {
    bool begin;
    uint64_t at;
  };

  void queue(bool begin, uint64_t at) {
    if (npending_ == pending_.size()) {
      ++dropped;
      return;
    }
    pending_[npending_++] = {begin, at};
  }

  void do_begin(uint64_t start, uint64_t floor) {
    if (start < floor) {
      start = floor;
      ++late;
    }
    if (count_ > 0) {
      Interval& last = at(count_ - 1);
      if (last.end == kOpen) return;               // already muting
      if (start <= last.end) {                     // touches the previous mute: extend it
        last.end = kOpen;
        return;
      }
      if (start < last.start) start = last.start;  // keep the list ordered
    }
    if (count_ == kCap) {
      ++dropped;
      return;
    }
    at(count_++) = {start, kOpen};
    ++mutes;
  }

  void do_end(uint64_t end) {
    if (count_ == 0) return;
    Interval& last = at(count_ - 1);
    if (last.end != kOpen) return;
    last.end = std::max(end, last.start + 1);
  }

  Interval& at(size_t i) { return buf_[(head_ + i) % kCap]; }
  std::array<Interval, kCap> buf_{};
  std::array<Op, 64> pending_{};
  size_t head_ = 0, count_ = 0, npending_ = 0;
};

namespace {
class NullDetector final : public Detector {
 public:
  const char* name() const override { return "none"; }
  void prepare(double, size_t) override {}
  void process(const float*, size_t, uint64_t, MuteSink&) override {}
  void reset() override {}
};
}  // namespace

Engine::Engine(const Config& cfg, std::unique_ptr<Detector> detector)
    : cfg_(cfg), detector_(detector ? std::move(detector) : std::make_unique<NullDetector>()),
      schedule_(std::make_unique<Schedule>()) {
  const double sr = cfg_.sample_rate;
  delay_ = static_cast<size_t>(std::lround(cfg_.lookahead_ms * sr / 1000.0));
  ring_.assign(std::max<size_t>(delay_, 1), 0.f);
  const double fade = std::max(1.0, cfg_.fade_ms * sr / 1000.0);
  const double late = std::max(1.0, cfg_.late_fade_ms * sr / 1000.0);
  fade_step_ = static_cast<float>(1.0 / fade);
  late_step_ = static_cast<float>(1.0 / late);
  max_mute_ = static_cast<uint64_t>(cfg_.max_mute_ms * sr / 1000.0);
  detector_->prepare(sr, cfg_.max_block);
}

Engine::~Engine() = default;

const char* Engine::detector_name() const { return detector_->name(); }

void Engine::reset() {
  std::fill(ring_.begin(), ring_.end(), 0.f);
  write_ = 0;
  next_in_ = 0;
  gain_ = 1.f;
  schedule_->clear();
  detector_->reset();
  muted_now_.store(false, std::memory_order_relaxed);
}

// Gain for the sample at absolute input index `s`, which is being sent now.
float Engine::gain_for(uint64_t s) {
  const uint64_t fade_len = static_cast<uint64_t>(1.0f / fade_step_);
  Schedule::Interval* iv = enabled() ? schedule_->upcoming(s, fade_len) : nullptr;

  if (iv && iv->end == kOpen && max_mute_ > 0 && s >= iv->start + max_mute_) {
    iv->end = s;  // safety: the detector never said stop
    stats_.forced.fetch_add(1, std::memory_order_relaxed);
  } else if (iv && iv->end != kOpen && max_mute_ > 0 && iv->end > iv->start + max_mute_) {
    iv->end = iv->start + max_mute_;
    stats_.forced.fetch_add(1, std::memory_order_relaxed);
  }

  if (iv && s >= iv->start && s < iv->end) {
    // Inside a mute. Normally gain is already 0 here; if the decision came
    // late, drop fast and count what still got through.
    gain_ = std::max(0.f, gain_ - late_step_);
    if (gain_ > kLeakGain) stats_.leaked.fetch_add(1, std::memory_order_relaxed);
  } else if (iv && iv->start > s && iv->start - s <= fade_len) {
    // Fading in to a mute so gain hits 0 exactly at its start.
    const float left = static_cast<float>(iv->start - s);
    gain_ = std::max(0.f, gain_ - std::max(fade_step_, gain_ / left));
  } else {
    gain_ = std::min(1.f, gain_ + fade_step_);
  }
  return gain_;
}

void Engine::process(const float* in, float* out, size_t n, float* gain_out) {
  // Work in small steps lined up to the absolute sample count, so the result
  // never depends on how the host chops the audio into blocks.
  size_t done = 0;
  while (done < n) {
    const size_t k = std::min(n - done, kStep - static_cast<size_t>(next_in_ % kStep));
    process_step(in + done, out + done, k, gain_out ? gain_out + done : nullptr);
    done += k;
  }
  stats_.samples.fetch_add(n, std::memory_order_relaxed);
  muted_now_.store(gain_ < 0.5f, std::memory_order_relaxed);
}

void Engine::process_step(const float* in, float* out, size_t n, float* gain_out) {
  const uint64_t mutes0 = schedule_->mutes, late0 = schedule_->late, drop0 = schedule_->dropped;
  // 1. At each step boundary, decisions from the previous step take effect.
  if (next_in_ % kStep == 0) schedule_->apply(next_in_ >= delay_ ? next_in_ - delay_ : 0);
  // 2. The detector hears the newest audio.
  detector_->process(in, n, next_in_, *schedule_);
  stats_.mutes.fetch_add(schedule_->mutes - mutes0, std::memory_order_relaxed);
  stats_.late.fetch_add(schedule_->late - late0, std::memory_order_relaxed);
  stats_.dropped.fetch_add(schedule_->dropped - drop0, std::memory_order_relaxed);

  // 3. Delay the audio by the look-ahead and apply the gate.
  for (size_t i = 0; i < n; ++i) {
    const float x = in[i];  // read before writing: in and out may alias
    float y;
    if (delay_ == 0) {
      y = x;
    } else {
      y = ring_[write_];
      ring_[write_] = x;
      write_ = (write_ + 1 == delay_) ? 0 : write_ + 1;
    }
    const uint64_t abs_in = next_in_ + i;
    const float g = abs_in >= delay_ ? gain_for(abs_in - delay_) : 1.f;
    out[i] = y * g;
    if (gain_out) gain_out[i] = g;
  }
  next_in_ += n;
}

Stats Engine::stats() const {
  Stats s;
  s.samples = stats_.samples.load(std::memory_order_relaxed);
  s.mutes = stats_.mutes.load(std::memory_order_relaxed);
  s.leaked_samples = stats_.leaked.load(std::memory_order_relaxed);
  s.late_decisions = stats_.late.load(std::memory_order_relaxed);
  s.forced_unmutes = stats_.forced.load(std::memory_order_relaxed);
  s.dropped_decisions = stats_.dropped.load(std::memory_order_relaxed);
  return s;
}

}  // namespace hush
