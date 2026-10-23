// hush_run: run WAV files through Hush from the command line.
//
//   hush_run spans [--detector model|rules] [--lookahead-ms 100] [--block 160] [--out DIR] FILE.wav...
//       Runs the full engine (default detector: model). Prints one JSON line per
//       file with the spans the engine actually muted (seconds, on the input
//       timeline) and its stats. --out also writes the processed audio.
//
//   hush_run proposals [--detector model|rules] FILE.wav...
//       Runs only the detector and prints what it proposed and when it
//       decided. Used by the bench to check the C++ port against Python.
//
//   Either mode also accepts --list FILE with one WAV path per line.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "hush/engine.hpp"
#include "hush/model_detector.hpp"
#include "hush/rules_detector.hpp"

namespace {

struct Wav {
  std::vector<float> x;
  int rate = 0;
};

uint32_t u32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
uint16_t u16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

// 16-bit PCM WAV, any channel count (mixed to mono), scaled like the bench (x / 32767).
Wav read_wav(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), {});
  if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4))
    throw std::runtime_error(path + " is not a WAV file");
  Wav w;
  int channels = 0, bits = 0;
  for (size_t p = 12; p + 8 <= d.size();) {
    const uint32_t len = u32(&d[p + 4]);
    const unsigned char* body = &d[p + 8];
    if (p + 8 + len > d.size()) break;
    if (!std::memcmp(&d[p], "fmt ", 4)) {
      if (u16(body) != 1) throw std::runtime_error(path + ": only PCM WAV is supported");
      channels = u16(body + 2);
      w.rate = static_cast<int>(u32(body + 4));
      bits = u16(body + 14);
    } else if (!std::memcmp(&d[p], "data", 4)) {
      if (bits != 16 || channels < 1) throw std::runtime_error(path + ": only 16-bit PCM is supported");
      const size_t frames = len / (2u * static_cast<size_t>(channels));
      w.x.resize(frames);
      for (size_t i = 0; i < frames; ++i) {
        float acc = 0;
        for (int c = 0; c < channels; ++c)
          acc += static_cast<float>(static_cast<int16_t>(u16(body + 2 * (i * channels + c)))) / 32767.f;
        w.x[i] = acc / static_cast<float>(channels);
      }
    }
    p += 8 + len + (len & 1);
  }
  if (!w.rate || w.x.empty()) throw std::runtime_error(path + ": no audio found");
  return w;
}

void write_wav(const std::string& path, const std::vector<float>& x, int rate) {
  std::ofstream f(path, std::ios::binary);
  auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) f.put(static_cast<char>(v >> (8 * i) & 0xff)); };
  auto put16 = [&](uint16_t v) { f.put(static_cast<char>(v & 0xff)); f.put(static_cast<char>(v >> 8)); };
  const uint32_t bytes = static_cast<uint32_t>(x.size() * 2);
  f.write("RIFF", 4); put32(36 + bytes); f.write("WAVEfmt ", 8);
  put32(16); put16(1); put16(1); put32(static_cast<uint32_t>(rate)); put32(static_cast<uint32_t>(rate) * 2); put16(2); put16(16);
  f.write("data", 4); put32(bytes);
  for (float v : x) put16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::fmax(-1.f, std::fmin(1.f, v)) * 32767.f))));
}

std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o + "\"";
}

std::unique_ptr<hush::Detector> make_detector(const std::string& name) {
  if (name == "rules") return std::make_unique<hush::RulesDetector>();
  if (name == "model") return std::make_unique<hush::ModelDetector>();
  throw std::runtime_error("unknown detector: " + name + " (use model or rules)");
}

// Records detector proposals with the moment each was decided.
struct Recorder final : hush::MuteSink {
  struct P { uint64_t start, end, decided; int kind; };
  std::vector<P> out;
  uint64_t now = 0;
  const hush::RulesDetector* det = nullptr;  // only the rules detector guesses cough vs sneeze
  void begin_mute(uint64_t start) override {
    if (!out.empty() && out.back().end != UINT64_MAX && start <= out.back().end) {
      out.back().end = UINT64_MAX;  // merged with the previous one, as the engine would
      return;
    }
    out.push_back({start, UINT64_MAX, now, det ? det->last_kind() : 1});
  }
  void end_mute(uint64_t end) override {
    if (!out.empty() && out.back().end == UINT64_MAX) out.back().end = end;
  }
};

int run_proposals(const std::vector<std::string>& files, const std::string& detector) {
  for (const auto& path : files) {
    Wav w = read_wav(path);
    std::unique_ptr<hush::Detector> det = make_detector(detector);
    det->prepare(w.rate, 1);
    Recorder rec;
    rec.det = dynamic_cast<const hush::RulesDetector*>(det.get());
    std::vector<float> x = w.x;
    x.resize(x.size() + static_cast<size_t>(w.rate), 0.f);  // 1 s of silence lets the last event finish
    for (size_t i = 0; i < x.size(); ++i) {
      rec.now = i + 1;  // decisions made after hearing sample i
      det->process(&x[i], 1, i, rec);
    }
    const double sr = w.rate;
    std::printf("{\"file\":%s,\"mutes\":[", json_str(path).c_str());
    for (size_t k = 0; k < rec.out.size(); ++k) {
      const auto& p = rec.out[k];
      const double end = p.end == UINT64_MAX ? static_cast<double>(x.size()) : static_cast<double>(p.end);
      std::printf("%s[%.6f,%.6f,%.6f,\"%s\"]", k ? "," : "", static_cast<double>(p.start) / sr, end / sr,
                  static_cast<double>(p.decided) / sr, p.kind == 2 ? "sneeze" : "cough");
    }
    std::printf("]}\n");
  }
  return 0;
}

int run_spans(const std::vector<std::string>& files, double lookahead_ms, size_t block, const std::string& out_dir,
              const std::string& detector) {
  for (const auto& path : files) {
    Wav w = read_wav(path);
    hush::Config cfg;
    cfg.sample_rate = w.rate;
    cfg.max_block = block;
    cfg.lookahead_ms = lookahead_ms;
    hush::Engine eng(cfg, make_detector(detector));
    const size_t D = eng.latency_samples();
    std::vector<float> x = w.x;
    x.resize(x.size() + D + static_cast<size_t>(w.rate), 0.f);  // flush the delay line
    std::vector<float> y(x.size()), g(x.size());
    for (size_t i = 0; i < x.size(); i += block) {
      const size_t n = std::min(block, x.size() - i);
      eng.process(&x[i], &y[i], n, &g[i]);
    }
    // Output sample o carries input sample o - D.
    const double sr = w.rate;
    const size_t len = w.x.size();
    std::printf("{\"file\":%s,\"latency_ms\":%.3f,\"spans\":[", json_str(path).c_str(), 1000.0 * static_cast<double>(D) / sr);
    bool first = true, in = false;
    size_t a = 0;
    for (size_t s = 0; s <= len; ++s) {
      const bool m = s < len && g[s + D] < 0.5f;
      if (m && !in) { a = s; in = true; }
      if (!m && in) {
        std::printf("%s[%.6f,%.6f]", first ? "" : ",", static_cast<double>(a) / sr, static_cast<double>(s) / sr);
        first = false;
        in = false;
      }
    }
    const hush::Stats st = eng.stats();
    std::printf("],\"mutes\":%llu,\"late\":%llu,\"leaked_ms\":%.1f,\"forced\":%llu}\n",
                static_cast<unsigned long long>(st.mutes), static_cast<unsigned long long>(st.late_decisions),
                1000.0 * static_cast<double>(st.leaked_samples) / sr, static_cast<unsigned long long>(st.forced_unmutes));
    if (!out_dir.empty()) {
      std::string name = path.substr(path.find_last_of("/\\") + 1);
      std::vector<float> aligned(y.begin() + static_cast<long>(D), y.begin() + static_cast<long>(D + len));
      write_wav(out_dir + "/" + name, aligned, w.rate);
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || (std::strcmp(argv[1], "spans") && std::strcmp(argv[1], "proposals"))) {
    std::fprintf(stderr, "usage: hush_run spans|proposals [--detector model|rules] [--lookahead-ms N] [--block N] [--out DIR] [--list FILE] FILE.wav...\n");
    return 2;
  }
  const std::string mode = argv[1];
  double lookahead = 100;
  size_t block = 160;
  std::string out_dir, detector = "model";
  std::vector<std::string> files;
  try {
    for (int i = 2; i < argc; ++i) {
      const std::string a = argv[i];
      auto next = [&]() -> std::string {
        if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
        return argv[++i];
      };
      if (a == "--lookahead-ms") lookahead = std::stod(next());
      else if (a == "--block") block = std::stoul(next());
      else if (a == "--out") out_dir = next();
      else if (a == "--detector") detector = next();
      else if (a == "--list") {
        std::ifstream l(next());
        for (std::string line; std::getline(l, line);)
          if (!line.empty()) files.push_back(line);
      } else files.push_back(a);
    }
    if (files.empty()) throw std::runtime_error("no input files");
    if (block == 0) throw std::runtime_error("--block must be at least 1");
    make_detector(detector);  // validate the name before reading any files
    return mode == "spans" ? run_spans(files, lookahead, block, out_dir, detector) : run_proposals(files, detector);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "hush_run: %s\n", e.what());
    return 1;
  }
}
