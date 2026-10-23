#include "hush/model_detector.hpp"

#include <algorithm>
#include <cmath>

namespace hush {

namespace {
#include "model_weights.inc"

constexpr double kPi = 3.14159265358979323846;
constexpr int kMels = HUSH_MODEL_MELS;
constexpr int kK = HUSH_MODEL_K;
constexpr double kFLo = 60.0, kFHi = 7600.0;

double hz_to_mel(double f) { return 2595.0 * std::log10(1.0 + f / 700.0); }
double mel_to_hz(double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); }
}  // namespace

ModelParams ModelParams::tuned() {
  ModelParams p;
  p.on_th = HUSH_TUNED_ON_TH;
  p.on_frames = HUSH_TUNED_ON_FRAMES;
  p.off_th = HUSH_TUNED_OFF_TH;
  p.off_frames = HUSH_TUNED_OFF_FRAMES;
  p.lead_s = HUSH_TUNED_LEAD_S;
  p.tail_s = HUSH_TUNED_TAIL_S;
  p.min_hold_s = HUSH_TUNED_MIN_HOLD_S;
  p.merge_s = HUSH_TUNED_MERGE_S;
  return p;
}

const char* ModelDetector::name() const { return HUSH_MODEL_NAME; }

void ModelDetector::prepare(double sample_rate, size_t) {
  sr_ = sample_rate;
  // Same geometry as bench/hush_bench/features.py: 32 ms window, 10 ms hop.
  win_ = static_cast<size_t>(std::lround(0.032 * sr_));
  hop_ = static_cast<size_t>(std::lround(0.010 * sr_));
  nfft_ = 1;
  while (nfft_ < win_) nfft_ <<= 1;

  ring_.assign(win_, 0.f);
  window_.resize(win_);
  double wsum = 0;
  for (size_t n = 0; n < win_; ++n) {
    window_[n] = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(n) / static_cast<double>(win_ - 1));
    wsum += window_[n];
  }
  wsum2_ = wsum * wsum;
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
  power_.assign(nfft_ / 2 + 1, 0.0);

  // Triangular mel filters with unit peak (HTK mel scale), stored sparsely.
  const double mlo = hz_to_mel(kFLo), mhi = hz_to_mel(kFHi);
  std::vector<double> edges(kMels + 2);
  for (int i = 0; i < kMels + 2; ++i) edges[i] = mel_to_hz(mlo + (mhi - mlo) * i / (kMels + 1));
  bands_.assign(kMels, {});
  for (int m = 0; m < kMels; ++m) {
    const double lo = edges[m], mid = edges[m + 1], hi = edges[m + 2];
    Band& band = bands_[m];
    bool started = false;
    for (size_t k = 0; k <= nfft_ / 2; ++k) {
      const double f = static_cast<double>(k) * sr_ / static_cast<double>(nfft_);
      const double v = std::max(0.0, std::min((f - lo) / (mid - lo), (hi - f) / (hi - mid)));
      if (v > 0 && !started) {
        band.first = k;
        started = true;
      }
      if (started) {
        if (v <= 0) break;
        band.w.push_back(v);
      }
    }
  }
  feat_.assign(kMels, 0.f);

  layers_.assign(HUSH_MODEL_LAYERS, {});
  int c_in = kMels;
  for (int l = 0; l < HUSH_MODEL_LAYERS; ++l) {
    Layer& L = layers_[l];
    L.c_in = c_in;
    L.c_out = kModelChannels[l];
    L.dil = kModelDils[l];
    L.span = (kK - 1) * L.dil + 1;
    L.w = kModelW[l];
    L.b = kModelB[l];
    L.hist.assign(static_cast<size_t>(L.span * L.c_in), 0.f);
    L.out.assign(static_cast<size_t>(L.c_out), 0.f);
    c_in = L.c_out;
  }
  lead_ = static_cast<uint64_t>(std::llround(p_.lead_s * sr_));
  tail_ = static_cast<uint64_t>(std::llround(p_.tail_s * sr_));
  hold_ = static_cast<uint64_t>(std::llround(p_.min_hold_s * sr_));
  merge_ = static_cast<uint64_t>(std::llround(p_.merge_s * sr_));
  reset();
}

void ModelDetector::reset() {
  std::fill(ring_.begin(), ring_.end(), 0.f);
  ring_pos_ = 0;
  seen_ = frames_ = 0;
  last_p_ = -1;
  for (Layer& L : layers_) {
    std::fill(L.hist.begin(), L.hist.end(), 0.f);
    L.pos = L.filled = 0;
  }
  active_ = have_last_ = false;
  run_ = below_ = 0;
  cur_start_ = last_end_ = 0;
}

bool ModelDetector::run_layers(const float* x) {
  const float* in = x;
  for (Layer& L : layers_) {
    // push the newest input vector into this layer's history
    std::copy(in, in + L.c_in, L.hist.begin() + static_cast<long>(L.pos * L.c_in));
    L.pos = (L.pos + 1) % L.span;
    if (L.filled < L.span) ++L.filled;
    if (L.filled < L.span) return false;  // not enough history yet
    // newest vector sits at pos-1; kernel tap j reads (K-1-j)*dil frames back
    for (int o = 0; o < L.c_out; ++o) {
      double acc = L.b[o];
      const float* wo = L.w + static_cast<long>(o) * L.c_in * kK;
      for (int j = 0; j < kK; ++j) {
        const int back = (kK - 1 - j) * L.dil;
        const int idx = ((L.pos - 1 - back) % L.span + L.span) % L.span;
        const float* v = L.hist.data() + static_cast<long>(idx) * L.c_in;
        for (int c = 0; c < L.c_in; ++c) acc += static_cast<double>(wo[c * kK + j]) * v[c];
      }
      L.out[o] = static_cast<float>(std::max(0.0, acc));
    }
    in = L.out.data();
  }
  return true;
}

void ModelDetector::analyze_frame(MuteSink& sink) {
  for (size_t n = 0; n < nfft_; ++n) fft_[n] = {0.0, 0.0};
  for (size_t n = 0; n < win_; ++n) fft_[rev_[n]] = {ring_[(ring_pos_ + n) % win_] * window_[n], 0.0};
  for (size_t len = 2; len <= nfft_; len <<= 1) {
    const size_t half = len / 2, step = nfft_ / len;
    for (size_t i = 0; i < nfft_; i += len)
      for (size_t k = 0; k < half; ++k) {
        const std::complex<double> t = fft_[i + k + half] * tw_[k * step];
        fft_[i + k + half] = fft_[i + k] - t;
        fft_[i + k] += t;
      }
  }
  for (size_t k = 0; k <= nfft_ / 2; ++k) power_[k] = std::norm(fft_[k]) / wsum2_;
  for (int m = 0; m < kMels; ++m) {
    double e = 0;
    const Band& b = bands_[m];
    for (size_t i = 0; i < b.w.size(); ++i) e += b.w[i] * power_[b.first + i];
    // as in Python: log10 in double, stored as float32, then normalized in float32
    const float lf = static_cast<float>(std::log10(e + 1e-10));
    feat_[m] = (lf - kModelMean[m]) / kModelStd[m];
  }
  float p = 0.f;
  if (run_layers(feat_.data())) {
    double z = kModelBo[0];
    const std::vector<float>& h = layers_.back().out;
    for (size_t c = 0; c < h.size(); ++c) z += static_cast<double>(kModelWo[c]) * h[c];
    p = static_cast<float>(1.0 / (1.0 + std::exp(-z)));
  }
  last_p_ = p;
  decide(p, sink);
  ++frames_;
}

void ModelDetector::decide(float p, MuteSink& sink) {
  const uint64_t k = frames_;
  if (!active_) {
    run_ = p >= p_.on_th ? run_ + 1 : 0;
    if (run_ >= p_.on_frames) {
      const uint64_t k0 = k - static_cast<uint64_t>(p_.on_frames) + 1;
      const uint64_t edge = k0 * hop_ + win_;
      uint64_t start = edge > lead_ ? edge - lead_ : 0;
      if (have_last_ && start <= last_end_ + merge_) start = std::min(start, last_end_);
      sink.begin_mute(start);
      cur_start_ = start;
      active_ = true;
      below_ = run_ = 0;
    }
  } else {
    below_ = p < p_.off_th ? below_ + 1 : 0;
    if (below_ >= p_.off_frames) {
      const uint64_t last_on = k - static_cast<uint64_t>(p_.off_frames);
      const uint64_t end = std::max(last_on * hop_ + win_ + tail_, cur_start_ + hold_);
      sink.end_mute(end);
      last_end_ = end;
      have_last_ = true;
      active_ = false;
    }
  }
}

void ModelDetector::process(const float* in, size_t n, uint64_t, MuteSink& sink) {
  for (size_t s = 0; s < n; ++s) {
    ring_[ring_pos_] = in[s];
    ring_pos_ = (ring_pos_ + 1 == win_) ? 0 : ring_pos_ + 1;
    ++seen_;
    if (seen_ == frames_ * hop_ + win_) analyze_frame(sink);
  }
}

}  // namespace hush
