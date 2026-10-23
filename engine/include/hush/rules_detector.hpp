// Rules detector: the first thing to plug into the detector slot.
// Streaming C++ port of bench/hush_bench/detectors/rules.py; the bench
// checks that both make the same decisions.
//
// A frame starts a cough/sneeze when it jumps well above the recent floor, is
// louder than the talker's usual voice, and is noisy (flat spectrum or lots
// of high-frequency energy) rather than tonal for several frames in a row.
#pragma once
#include <array>
#include <complex>
#include <vector>

#include "hush/detector.hpp"

namespace hush {

struct RulesParams {
  double onset_db = 14.0;    // jump above the trailing 150 ms floor
  double loud_db = 4.0;      // above the running speech level
  double flat = 0.22;        // spectral flatness that counts as noisy
  double hi_ratio = 0.35;    // share of energy above 2 kHz that counts as noisy
  int confirm_frames = 4;    // frames of evidence before deciding
  double hold_s = 0.45;      // minimum mute length
  double pre_roll_s = 0.03;  // start the mute this long before the onset
  double tail_s = 0.05;      // stay muted this long after it fades
  double merge_s = 0.10;     // join mutes closer than this
};

class RulesDetector final : public Detector {
 public:
  explicit RulesDetector(RulesParams p = {}) : p_(p) {}
  const char* name() const override { return "rules"; }
  void prepare(double sample_rate, size_t max_block) override;
  void process(const float* in, size_t n, uint64_t first_sample, MuteSink& sink) override;
  void reset() override;

  // Last decision's guess, for UIs and logs: 0 none, 1 cough, 2 sneeze.
  int last_kind() const { return last_kind_; }

 private:
  struct Frame {
    double db, flat, hi;
  };
  static constexpr size_t kHist = 128;  // frames of history kept (1.28 s)

  void analyze_frame();      // features of the newest full window -> history
  void step(MuteSink& sink);  // advance the decision state machine
  const Frame& f(uint64_t k) const { return hist_[k % kHist]; }
  uint64_t frame_start(uint64_t k) const { return k * hop_; }

  RulesParams p_;
  double sr_ = 16000;
  size_t win_ = 512, nfft_ = 512, hop_ = 160, hi_bin_ = 0;  // window 32 ms, FFT padded to a power of 2
  std::vector<float> ring_;                  // last win_ samples
  std::vector<double> window_;               // Hann
  std::vector<std::complex<double>> fft_, tw_;
  std::vector<size_t> rev_;
  std::array<Frame, kHist> hist_{};
  size_t ring_pos_ = 0;
  uint64_t seen_ = 0;        // samples received
  uint64_t frames_ = 0;      // frames analyzed
  size_t since_hop_ = 0;

  // decision state
  uint64_t eval_ = 15;       // next frame to evaluate as a possible onset
  double speech_level_ = -40;
  bool scanning_ = false;    // inside an event, looking for where it fades
  uint64_t hit_ = 0, scan_ = 0;
  double hit_floor_ = 0;
  bool have_last_ = false;
  uint64_t last_end_ = 0;
  int last_kind_ = 0;
};

}  // namespace hush
