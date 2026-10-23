#include "hush/rules_detector.hpp"

#include <algorithm>
#include <cmath>

namespace hush {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

void RulesDetector::prepare(double sample_rate, size_t) {
  sr_ = sample_rate;
  hop_ = static_cast<size_t>(std::lround(0.010 * sr_));
  win_ = static_cast<size_t>(std::lround(0.032 * sr_));  // 512 at 16 kHz, 1536 at 48 kHz
  nfft_ = 1;
  while (nfft_ < win_) nfft_ <<= 1;
  hi_bin_ = static_cast<size_t>(2000.0 / (sr_ / static_cast<double>(nfft_)));

  ring_.assign(win_, 0.f);
  window_.resize(win_);
  for (size_t n = 0; n < win_; ++n)  // same as numpy.hanning
    window_[n] = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(n) / static_cast<double>(win_ - 1));
  fft_.assign(nfft_, {0, 0});
  tw_.resize(nfft_ / 2);
  for (size_t k = 0; k < nfft_ / 2; ++k)
    tw_[k] = std::polar(1.0, -2 * kPi * static_cast<double>(k) / static_cast<double>(nfft_));
  rev_.resize(nfft_);
  size_t bits = 0;
  while ((size_t{1} << bits) < nfft_) ++bits;
  for (size_t i = 0; i < nfft_; ++i) {
    size_t r = 0;
    for (size_t b = 0; b < bits; ++b)
      if (i & (size_t{1} << b)) r |= size_t{1} << (bits - 1 - b);
    rev_[i] = r;
  }
  reset();
}

void RulesDetector::reset() {
  std::fill(ring_.begin(), ring_.end(), 0.f);
  hist_.fill({});
  ring_pos_ = 0;
  seen_ = frames_ = 0;
  since_hop_ = 0;
  eval_ = 15;
  speech_level_ = -40;
  scanning_ = false;
  hit_ = scan_ = 0;
  hit_floor_ = 0;
  have_last_ = false;
  last_end_ = 0;
  last_kind_ = 0;
}

void RulesDetector::analyze_frame() {
  double energy = 0;
  for (size_t n = 0; n < nfft_; ++n) fft_[n] = {0.0, 0.0};
  for (size_t n = 0; n < win_; ++n) {
    const double x = ring_[(ring_pos_ + n) % win_];  // oldest first
    energy += x * x;
    fft_[rev_[n]] = {x * window_[n], 0.0};
  }
  for (size_t len = 2; len <= nfft_; len <<= 1) {
    const size_t half = len / 2, step = nfft_ / len;
    for (size_t i = 0; i < nfft_; i += len)
      for (size_t k = 0; k < half; ++k) {
        const std::complex<double> t = fft_[i + k + half] * tw_[k * step];
        fft_[i + k + half] = fft_[i + k] - t;
        fft_[i + k] += t;
      }
  }
  double log_sum = 0, sum = 0, hi = 0;
  const size_t lo = 2, top = nfft_ / 2;  // bins 2 .. nfft/2-1, as in the Python reference
  for (size_t k = lo; k < top; ++k) {
    const double pw = std::norm(fft_[k]) + 1e-18;
    log_sum += std::log(pw);
    sum += pw;
    if (k >= hi_bin_) hi += pw;
  }
  const double m = static_cast<double>(top - lo);
  Frame& fr = hist_[frames_ % kHist];
  fr.db = 10 * std::log10(energy / static_cast<double>(win_) + 1e-12);
  fr.flat = std::exp(log_sum / m) / (sum / m);
  fr.hi = hi / sum;
  ++frames_;
}

void RulesDetector::step(MuteSink& sink) {
  const uint64_t confirm = static_cast<uint64_t>(std::max(1, p_.confirm_frames));
  for (;;) {
    if (frames_ == 0) return;
    const uint64_t latest = frames_ - 1;

    if (scanning_) {
      // Follow the event until its level falls back near the floor.
      while (scan_ <= latest && f(scan_).db > hit_floor_ + 6) ++scan_;
      if (scan_ > latest) return;  // still loud: wait for more audio
      const uint64_t hold = static_cast<uint64_t>(std::lround(p_.hold_s * sr_));
      const uint64_t tail = static_cast<uint64_t>(std::lround(p_.tail_s * sr_));
      const uint64_t end = std::max(frame_start(scan_), frame_start(hit_) + hold) + tail;
      sink.end_mute(end);
      have_last_ = true;
      last_end_ = end;
      scanning_ = false;
      eval_ = std::max(hit_ + 1, scan_);
      continue;
    }

    if (eval_ + confirm - 1 > latest) return;  // need more frames of evidence
    const uint64_t i = eval_;
    double floor = 0;
    for (uint64_t k = i - 15; k < i - 2; ++k) floor += f(k).db;
    floor /= 13;
    const Frame& fi = f(i);
    if (fi.db > -45 && fi.flat < 0.15) speech_level_ = 0.97 * speech_level_ + 0.03 * fi.db;

    if (fi.db - floor >= p_.onset_db && fi.db >= speech_level_ + p_.loud_db) {
      uint64_t noisy = 0;
      for (uint64_t k = i; k < i + confirm; ++k) noisy += (f(k).flat > p_.flat || f(k).hi > p_.hi_ratio) ? 1 : 0;
      if (noisy + 1 >= confirm) {
        const uint64_t pre = static_cast<uint64_t>(std::lround(p_.pre_roll_s * sr_));
        uint64_t start = frame_start(i) > pre ? frame_start(i) - pre : 0;
        const uint64_t merge = static_cast<uint64_t>(std::lround(p_.merge_s * sr_));
        if (have_last_ && start <= last_end_ + merge) start = std::min(start, last_end_);
        // Label it before reporting it. Sneezes usually have a breathy
        // inhale 150-500 ms before the burst.
        uint64_t inhale = 0;
        for (uint64_t k = (i > 50 ? i - 50 : 0); k + 3 < i; ++k)
          inhale += (f(k).flat > 0.3 && f(k).db > -42) ? 1 : 0;
        last_kind_ = inhale >= 12 ? 2 : 1;
        sink.begin_mute(start);
        scanning_ = true;
        hit_ = scan_ = i;
        hit_floor_ = floor;
        continue;
      }
    }
    ++eval_;
  }
}

void RulesDetector::process(const float* in, size_t n, uint64_t, MuteSink& sink) {
  for (size_t s = 0; s < n; ++s) {
    ring_[ring_pos_] = in[s];
    ring_pos_ = (ring_pos_ + 1 == win_) ? 0 : ring_pos_ + 1;
    ++seen_;
    if (seen_ == frames_ * hop_ + win_) {  // a new full window is ready
      analyze_frame();
      step(sink);
    }
  }
}

}  // namespace hush
