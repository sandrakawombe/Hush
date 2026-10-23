// The detector slot. Anything that can spot a cough or sneeze plugs in here:
// the rules detector today, a trained model later. The engine never needs to
// change when the detector does.
#pragma once
#include <cstddef>
#include <cstdint>

namespace hush {

// Where a detector reports its decisions. Positions are absolute sample
// indices on the INPUT timeline (sample 0 = first sample the engine saw).
class MuteSink {
 public:
  virtual ~MuteSink() = default;
  // "Mute from `start` on." `start` may be in the past: the engine's
  // look-ahead buffer is what makes muting the past possible. Anything older
  // than the buffer has already been sent and is counted as leaked.
  virtual void begin_mute(uint64_t start) = 0;
  // "Unmute at `end`." Closes the open mute.
  virtual void end_mute(uint64_t end) = 0;
};

class Detector {
 public:
  virtual ~Detector() = default;
  virtual const char* name() const = 0;
  // Called once before audio starts. May allocate.
  virtual void prepare(double sample_rate, size_t max_block) = 0;
  // Called on the audio thread for every block, in order, with the absolute
  // index of in[0]. MUST NOT allocate, lock or block.
  virtual void process(const float* in, size_t n, uint64_t first_sample, MuteSink& sink) = 0;
  // Forget all history (new call).
  virtual void reset() = 0;
};

}  // namespace hush
