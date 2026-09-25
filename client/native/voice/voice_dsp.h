// Audio pipeline of the Otera voice helper, without devices or network so it can
// run offline in tests: capture processing (echo cancellation, noise suppression
// and the voice gate), the Opus codec and the receive-side jitter buffer.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

struct SpeexEchoState_;
struct SpeexPreprocessState_;
struct OpusEncoder;
struct OpusDecoder;

namespace otera::voice {

constexpr int kRate = 48000;
// The echo canceller and the preprocessor step in 10 ms; Opus packets carry 20 ms.
constexpr int kFrame = kRate / 100;
constexpr int kPacketFrames = 2;
constexpr int kPacket = kFrame * kPacketFrames;
constexpr int kMaxOpusBytes = 400;

float levelDb(const int16_t* pcm, int count);

// Mic in, clean voice out, plus whether it is speech worth sending. Open mic
// depends on the gate never opening on what the speakers play, so the far-end
// signal has to be exactly what went to the output device in the same duplex
// period: the echo canceller learns the path from one to the other.
class CaptureProcessor {
public:
  struct Result {
    bool open;          // gate decision for this 10 ms frame
    float probability;  // speech probability from the preprocessor, 0-1
    float micDb;        // raw mic level, dBFS
    float outDb;        // level after echo cancellation and denoise, dBFS
  };

  explicit CaptureProcessor(int echoTailMs = 300, bool echoCancel = true);
  ~CaptureProcessor();
  CaptureProcessor(const CaptureProcessor&) = delete;
  CaptureProcessor& operator=(const CaptureProcessor&) = delete;

  // mic, speaker and out hold kFrame samples; speaker may be null (nothing played).
  Result process(const int16_t* mic, const int16_t* speaker, int16_t* out);

  // 0 = only clear, loud speech opens the gate; 1 = opens with almost anything.
  void setSensitivity(float value);
  float sensitivity() const { return sensitivity_; }

private:
  SpeexEchoState_* echo_ = nullptr;
  SpeexPreprocessState_* pre_ = nullptr;
  std::vector<int16_t> silence_;
  float sensitivity_ = 0.5f;
  float openAbove_ = 0.f, stayAbove_ = 0.f, minDb_ = 0.f;
  bool open_ = false;
  int hold_ = 0;
  bool echoSafe_ = false;
  int safeFrames_ = 0;
  int farHold_ = 0;
};

class Encoder {
public:
  explicit Encoder(int bitrate = 24000);
  ~Encoder();
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;
  // kPacket samples in; returns the packet size or a negative Opus error.
  int encode(const int16_t* pcm, uint8_t* out, int capacity);

private:
  OpusEncoder* enc_ = nullptr;
};

// One talker as heard by one listener. Packets arrive out of order, late or never;
// pull() is called every 20 ms by the playback clock and always produces audio
// while the talker is active, filling holes with Opus FEC or concealment.
class JitterBuffer {
public:
  enum Flags : uint8_t { kEnd = 1 };  // last packet of a talk spurt

  explicit JitterBuffer(int targetPackets = 3);
  ~JitterBuffer();
  JitterBuffer(const JitterBuffer&) = delete;
  JitterBuffer& operator=(const JitterBuffer&) = delete;

  void push(uint16_t seq, uint8_t flags, const uint8_t* data, size_t size);
  // Fills kPacket samples. False (and silence) when the talker is not active.
  bool pull(int16_t* out);
  // True while the talker is audible: what the speaking icon follows.
  bool talking() const { return state_ == State::Playing; }

  struct Stats {
    uint64_t received = 0, played = 0, recovered = 0, concealed = 0, late = 0, dropped = 0;
  };
  const Stats& stats() const { return stats_; }

private:
  enum class State { Idle, Buffering, Playing };
  struct Packet {
    uint8_t flags;
    std::vector<uint8_t> data;
  };

  void reset();

  OpusDecoder* dec_ = nullptr;
  std::map<int32_t, Packet> queue_;  // keyed by unwrapped sequence
  State state_ = State::Idle;
  int32_t next_ = 0;
  int32_t base_ = 0;       // unwrapped sequence of the last seen packet
  int32_t floor_ = INT32_MIN;  // everything below was played or given up on
  bool haveBase_ = false;
  int target_;
  int waited_ = 0;
  int missing_ = 0;
  Stats stats_;
};

// Fixed-capacity FIFO for the audio callback: no allocation once built.
class Ring {
public:
  explicit Ring(size_t capacity) : data_(capacity) {}
  size_t size() const { return size_; }
  void push(const int16_t* in, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      if (size_ == data_.size()) {  // full: drop the oldest
        head_ = (head_ + 1) % data_.size();
        --size_;
      }
      data_[(head_ + size_++) % data_.size()] = in[i];
    }
  }
  size_t pop(int16_t* out, size_t n) {
    size_t take = std::min(n, size_);
    for (size_t i = 0; i < take; ++i) out[i] = data_[(head_ + i) % data_.size()];
    head_ = (head_ + take) % data_.size();
    size_ -= take;
    return take;
  }

private:
  std::vector<int16_t> data_;
  size_t head_ = 0, size_ = 0;
};

// Sender side of loopback, selftest and serve: turns processed 10 ms frames into
// the packets that would go on the wire. Encoding never stops, so the encoder has
// context when the gate opens, and the packet just before the opening is kept as
// pre-roll: without it the first syllable is cut.
class Sender {
public:
  template <typename Send>
  void frame(const int16_t* pcm, bool open, Send&& send) {
    std::copy(pcm, pcm + kFrame, packet_.begin() + filled_ * kFrame);
    anyOpen_ = anyOpen_ || open;
    if (++filled_ < kPacketFrames) return;
    filled_ = 0;
    uint8_t buf[kMaxOpusBytes];
    int n = enc_.encode(packet_.data(), buf, sizeof(buf));
    bool open20 = anyOpen_;
    anyOpen_ = false;
    if (n <= 0) return;
    if (open20) {
      if (!sending_ && havePreroll_) send(seq_++, 0, preroll_.data(), preroll_.size());
      sending_ = true;
      send(seq_++, 0, buf, size_t(n));
    } else if (sending_) {
      sending_ = false;
      send(seq_++, JitterBuffer::kEnd, buf, size_t(n));
    }
    preroll_.assign(buf, buf + n);
    havePreroll_ = true;
  }
  bool sending() const { return sending_; }

private:
  Encoder enc_;
  std::vector<int16_t> packet_ = std::vector<int16_t>(kPacket);
  int filled_ = 0;
  bool anyOpen_ = false, sending_ = false, havePreroll_ = false;
  std::vector<uint8_t> preroll_;
  uint16_t seq_ = 0;
};

}  // namespace otera::voice
