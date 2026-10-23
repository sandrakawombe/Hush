// Engine tests. No external test framework: build and run `hush_tests`.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <new>
#include <random>
#include <string>
#include <vector>

#include "hush/engine.hpp"
#include "hush/hush.h"

// ---- allocation counter: proves process() never touches the heap ----------
static std::atomic<bool> g_count_allocs{false};
static std::atomic<long> g_allocs{0};
#ifndef HUSH_SANITIZE  // sanitizers install their own allocator; count only in normal builds
void* operator new(std::size_t n) {
  if (g_count_allocs.load()) g_allocs.fetch_add(1);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

// ---- tiny test harness -----------------------------------------------------
static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_fail;                                                              \
      std::printf("    FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                        \
  } while (0)
static std::vector<std::pair<const char*, std::function<void()>>>& tests() {
  static std::vector<std::pair<const char*, std::function<void()>>> t;
  return t;
}
#define TEST(name)                                                   \
  static void name();                                                \
  static const bool reg_##name = (tests().push_back({#name, name}), true); \
  static void name()

using namespace hush;
constexpr double SR = 16000;
static uint64_t ms(double v) { return static_cast<uint64_t>(v * SR / 1000); }

// A detector that replays a script: at sample `decide`, report a mute.
struct Scripted final : Detector {
  struct Ev { uint64_t decide, start, end; };
  std::vector<Ev> evs;
  explicit Scripted(std::vector<Ev> e) : evs(std::move(e)) {}
  const char* name() const override { return "scripted"; }
  void prepare(double, size_t) override {}
  void reset() override {}
  void process(const float*, size_t n, uint64_t first, MuteSink& sink) override {
    for (const Ev& e : evs) {
      if (e.decide >= first && e.decide < first + n) sink.begin_mute(e.start);
      if (e.end != UINT64_MAX && e.end >= first && e.end < first + n) sink.end_mute(e.end);
    }
  }
};

static std::vector<float> noise(size_t n, unsigned seed = 1) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(-0.5f, 0.5f);
  std::vector<float> x(n);
  for (auto& v : x) v = u(rng);
  return x;
}

static std::vector<float> run(Engine& e, const std::vector<float>& x, size_t block) {
  std::vector<float> y(x.size());
  for (size_t i = 0; i < x.size(); i += block) {
    size_t n = std::min(block, x.size() - i);
    e.process(x.data() + i, y.data() + i, n);
  }
  return y;
}

static Config cfg(double lookahead_ms = 100) {
  Config c;
  c.sample_rate = SR;
  c.lookahead_ms = lookahead_ms;
  return c;
}

// ---------------------------------------------------------------------------

TEST(passes_audio_through_delayed_by_lookahead) {
  Engine e(cfg(100), nullptr);
  auto x = noise(SR * 2);
  auto y = run(e, x, 256);
  const size_t D = e.latency_samples();
  CHECK(D == ms(100));
  bool exact = true;
  for (size_t i = 0; i < D; ++i) exact &= (y[i] == 0.f);
  for (size_t i = D; i < x.size(); ++i) exact &= (y[i] == x[i - D]);
  CHECK(exact);
}

TEST(mutes_exactly_the_scheduled_span_with_no_leak) {
  const uint64_t S = ms(1000), E = ms(1400);
  Engine e(cfg(100), std::make_unique<Scripted>(std::vector<Scripted::Ev>{{S + ms(60), S, E}}));
  auto x = noise(SR * 3);
  auto y = run(e, x, 160);
  const size_t D = e.latency_samples();
  double inside = 0, before = 0, after = 0;
  for (uint64_t s = S; s < E; ++s) inside += std::fabs(y[s + D]);
  for (uint64_t s = S - ms(100); s < S - ms(20); ++s) before += std::fabs(y[s + D] - x[s]);
  for (uint64_t s = E + ms(20); s < E + ms(300); ++s) after += std::fabs(y[s + D] - x[s]);
  CHECK(inside == 0.0);   // fully silent for the whole mute
  CHECK(before == 0.0);   // untouched until the fade-in window
  CHECK(after == 0.0);    // automatically back to full level after the fade-out
  auto st = e.stats();
  CHECK(st.mutes == 1 && st.leaked_samples == 0 && st.late_decisions == 0);
}

TEST(fade_is_smooth_no_clicks) {
  const uint64_t S = ms(1000), E = ms(1400);
  Engine e(cfg(100), std::make_unique<Scripted>(std::vector<Scripted::Ev>{{S + ms(60), S, E}}));
  std::vector<float> ones(SR * 2, 1.f);  // constant input exposes the gain curve
  auto y = run(e, ones, 160);
  const size_t D = e.latency_samples();
  float max_step = 0;
  for (size_t i = D + 1; i < y.size(); ++i) max_step = std::max(max_step, std::fabs(y[i] - y[i - 1]));
  CHECK(max_step <= 1.0f / ms(15) + 1e-4f);  // never jumps more than one fade step per sample
  CHECK(y[S + D] == 0.f && y[S + D - ms(15)] > 0.99f);
}

TEST(late_decision_is_counted_and_cut_fast) {
  const uint64_t S = ms(1000), E = ms(1400);
  // only 20 ms of look-ahead, but the detector needs 60 ms to decide
  Engine e(cfg(20), std::make_unique<Scripted>(std::vector<Scripted::Ev>{{S + ms(60), S, E}}));
  auto x = noise(SR * 3);
  auto y = run(e, x, 160);
  const size_t D = e.latency_samples();
  auto st = e.stats();
  CHECK(st.late_decisions == 1);
  CHECK(st.leaked_samples > 0 && st.leaked_samples <= ms(60) + ms(160 / 16.0));
  double tail = 0;  // silent shortly after the decision lands
  for (uint64_t s = S + ms(80) + ms(10); s < E; ++s) tail += std::fabs(y[s + D]);
  CHECK(tail == 0.0);
}

TEST(output_does_not_depend_on_block_size) {
  std::vector<Scripted::Ev> script{{ms(700), ms(650), ms(900)}, {ms(2100), ms(2050), ms(2600)}};
  auto x = noise(SR * 4, 7);
  Engine a(cfg(), std::make_unique<Scripted>(script));
  auto ya = run(a, x, 1);
  Engine b(cfg(), std::make_unique<Scripted>(script));
  std::vector<float> yb(x.size());
  std::mt19937 rng(3);
  for (size_t i = 0; i < x.size();) {
    size_t n = std::min<size_t>(1 + rng() % 4096, x.size() - i);
    b.process(x.data() + i, yb.data() + i, n);
    i += n;
  }
  CHECK(ya == yb);
}

TEST(in_place_matches_separate_buffers) {
  std::vector<Scripted::Ev> script{{ms(700), ms(650), ms(900)}};
  auto x = noise(SR * 2, 9);
  Engine a(cfg(), std::make_unique<Scripted>(script));
  auto ya = run(a, x, 480);
  Engine b(cfg(), std::make_unique<Scripted>(script));
  auto z = x;
  for (size_t i = 0; i < z.size(); i += 480) b.process(z.data() + i, z.data() + i, std::min<size_t>(480, z.size() - i));
  CHECK(ya == z);
}

TEST(never_stays_muted_if_detector_forgets_to_unmute) {
  Engine e(cfg(), std::make_unique<Scripted>(std::vector<Scripted::Ev>{{ms(1000), ms(950), UINT64_MAX}}));
  std::vector<float> ones(SR * 6, 1.f);
  auto y = run(e, ones, 256);
  const size_t D = e.latency_samples();
  CHECK(y[ms(950) + ms(1000) + D] == 0.f);           // muted during
  CHECK(y[ms(950) + ms(2500) + ms(100) + D] == 1.f);  // back after the 2.5 s safety
  CHECK(e.stats().forced_unmutes == 1);
}

TEST(disabled_never_mutes_but_keeps_same_delay) {
  Engine e(cfg(), std::make_unique<Scripted>(std::vector<Scripted::Ev>{{ms(700), ms(650), ms(900)}}));
  e.set_enabled(false);
  auto x = noise(SR * 2);
  auto y = run(e, x, 256);
  const size_t D = e.latency_samples();
  bool same = true;
  for (size_t i = D; i < x.size(); ++i) same &= (y[i] == x[i - D]);
  CHECK(same);
}

TEST(flooded_schedule_drops_instead_of_allocating) {
  struct Spammer final : Detector {
    const char* name() const override { return "spam"; }
    void prepare(double, size_t) override {}
    void reset() override {}
    void process(const float*, size_t n, uint64_t first, MuteSink& s) override {
      for (uint64_t k = 0; k < 200; ++k) {
        s.begin_mute(first + n + 1000 + k * 10);
        s.end_mute(first + n + 1000 + k * 10 + 2);
      }
    }
  };
  Engine e(cfg(), std::make_unique<Spammer>());
  auto x = noise(SR);
  std::vector<float> y(x.size());
  g_allocs = 0;
  g_count_allocs = true;
  for (size_t i = 0; i < x.size(); i += 256) e.process(x.data() + i, y.data() + i, std::min<size_t>(256, x.size() - i));
  g_count_allocs = false;
  CHECK(g_allocs == 0);
  CHECK(e.stats().dropped_decisions > 0);
}

TEST(processing_never_allocates) {
  std::vector<Scripted::Ev> script;
  for (uint64_t k = 0; k < 50; ++k) script.push_back({ms(300 + k * 200), ms(260 + k * 200), ms(400 + k * 200)});
  Engine e(cfg(), std::make_unique<Scripted>(script));
  auto x = noise(SR * 12);
  std::vector<float> y(x.size());
  g_allocs = 0;
  g_count_allocs = true;
  for (size_t i = 0; i < x.size(); i += 128) e.process(x.data() + i, y.data() + i, std::min<size_t>(128, x.size() - i));
  (void)e.muted_now();
  (void)e.stats();
  g_count_allocs = false;
  CHECK(g_allocs == 0);
  CHECK(e.stats().mutes == 50);
}

TEST(muted_indicator_follows_the_gate) {
  const uint64_t S = ms(1000), E = ms(1400);
  Engine e(cfg(), std::make_unique<Scripted>(std::vector<Scripted::Ev>{{S + ms(60), S, E}}));
  std::vector<float> buf(160);
  bool seen_muted = false;
  for (uint64_t pos = 0; pos < ms(2500); pos += 160) {
    e.process(buf.data(), buf.data(), 160);
    if (pos > S + ms(150) && pos < E) seen_muted |= e.muted_now();
  }
  CHECK(seen_muted);
  CHECK(!e.muted_now());
}

TEST(reset_starts_a_fresh_call) {
  Engine e(cfg(), std::make_unique<Scripted>(std::vector<Scripted::Ev>{{ms(300), ms(250), UINT64_MAX}}));
  auto x = noise(SR);
  run(e, x, 256);
  e.reset();
  std::vector<float> ones(ms(200), 1.f), y(ones.size());
  e.process(ones.data(), y.data(), ones.size());
  CHECK(y[e.latency_samples()] == 1.f);  // no leftover mute, no leftover audio
  CHECK(y[0] == 0.f);
}

TEST(c_api_validates_and_runs) {
  hush_config c = hush_default_config();
  CHECK(hush_api_version() == HUSH_API_VERSION);
  hush_config bad = c;
  bad.sample_rate = 100;
  CHECK(hush_create(&bad) == nullptr);
  bad = c;
  bad.lookahead_ms = -1;
  CHECK(hush_create(&bad) == nullptr);
  bad = c;
  bad.max_mute_ms = NAN;
  CHECK(hush_create(&bad) == nullptr);
  bad = c;
  bad.detector = HUSH_DETECTOR_RULES;
  CHECK(hush_create(&bad) == nullptr);  // not until piece 3
  CHECK(hush_create(nullptr) == nullptr);

  c.sample_rate = 48000;
  c.max_block = 480;
  hush_engine* h = hush_create(&c);
  CHECK(h != nullptr);
  CHECK(hush_latency_frames(h) == 4800);
  std::vector<float> in(480, 0.25f), out(480);
  CHECK(hush_process(h, in.data(), out.data(), 481) == -1);
  CHECK(hush_process(nullptr, in.data(), out.data(), 480) == -1);
  for (int i = 0; i < 20; ++i) CHECK(hush_process(h, in.data(), out.data(), 480) == 0);
  CHECK(out[479] == 0.25f);
  CHECK(hush_get_stats(h).samples == 480 * 20);
  CHECK(hush_is_muted(h) == 0);
  hush_destroy(h);
  hush_destroy(nullptr);  // safe
}

TEST(speed) {
  // One hour of 48 kHz audio through the engine, 10 ms blocks.
  Config c = cfg();
  c.sample_rate = 48000;
  std::vector<Scripted::Ev> script;
  for (uint64_t k = 0; k < 3000; ++k) script.push_back({(k + 1) * 57600, (k + 1) * 57600 - 2400, (k + 1) * 57600 + 9600});
  Engine e(c, std::make_unique<Scripted>(script));
  auto x = noise(480);
  std::vector<float> y(480);
  auto t0 = std::chrono::steady_clock::now();
  for (int b = 0; b < 360000; ++b) e.process(x.data(), y.data(), 480);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("    1 h of 48 kHz audio in %.3f s  (%.0fx faster than real time)\n", secs, 3600 / secs);
#if !defined(HUSH_SANITIZE) && defined(NDEBUG)
  CHECK(secs < 36);  // generous: under 1% of one core (release builds only)
#endif
}

int main() {
  for (auto& [name, fn] : tests()) {
    int before = g_fail;
    fn();
    std::printf("%s %s\n", g_fail == before ? "PASS" : "FAIL", name);
  }
  std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
