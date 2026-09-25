#include "voice_dsp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <opus.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

namespace otera::voice {

float levelDb(const int16_t* pcm, int count) {
  double sum = 0;
  for (int i = 0; i < count; ++i) sum += double(pcm[i]) * pcm[i];
  double rms = std::sqrt(sum / std::max(count, 1)) / 32768.0;
  return rms > 1e-5 ? float(20 * std::log10(rms)) : -100.f;
}

// --------------------------------------------------------------------------
// Capture
// --------------------------------------------------------------------------

// How long the gate stays open after the last frame that looked like speech. Short
// pauses between words must not cut the stream, or the listener hears it choppy.
constexpr int kHoldFrames = 40;  // 400 ms
// Level of the speaker signal above which the other side counts as talking, and
// how long its echo can keep arriving after that (device latency plus the room).
constexpr float kFarActiveDb = -55.f;
constexpr int kFarHoldFrames = 25;  // 250 ms
// Clear far-end frames with nothing left after processing needed before trusting
// the canceller: half a second of the other player's voice.
constexpr float kFarLoudDb = -40.f;
constexpr int kSafeFrames = 50;
// Mic minus processed level above which a frame is taken as mostly echo.
constexpr float kEchoRemovedDb = 12.f;

CaptureProcessor::CaptureProcessor(int echoTailMs, bool echoCancel) : silence_(kFrame, 0) {
  if (echoCancel) {
    echo_ = speex_echo_state_init(kFrame, kRate * echoTailMs / 1000);
    int rate = kRate;
    speex_echo_ctl(echo_, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
  }
  pre_ = speex_preprocess_state_init(kFrame, kRate);
  int on = 1;
  speex_preprocess_ctl(pre_, SPEEX_PREPROCESS_SET_DENOISE, &on);
  // SPEEX_PREPROCESS_SET_VAD is not needed: the speech probability is computed
  // anyway, and the flag only swaps the return value for Speex's own (weaker) VAD.
  // With the echo state attached the preprocessor also removes the residual echo
  // the adaptive filter leaves, which is what keeps the gate shut while the
  // speakers play someone else's voice.
  if (echo_) speex_preprocess_ctl(pre_, SPEEX_PREPROCESS_SET_ECHO_STATE, echo_);
  setSensitivity(sensitivity_);
}

CaptureProcessor::~CaptureProcessor() {
  if (pre_) speex_preprocess_state_destroy(pre_);
  if (echo_) speex_echo_state_destroy(echo_);
}

void CaptureProcessor::setSensitivity(float value) {
  sensitivity_ = std::clamp(value, 0.f, 1.f);
  // Hysteresis as in Mumble's signal-to-noise mode: a high bar to open and a lower
  // one to stay open, so the tail of a word does not flap the gate.
  openAbove_ = 0.95f - 0.35f * sensitivity_;
  stayAbove_ = openAbove_ - 0.25f;
  minDb_ = -45.f - 15.f * sensitivity_;
}

CaptureProcessor::Result CaptureProcessor::process(const int16_t* mic, const int16_t* speaker, int16_t* out) {
  Result r{};
  r.micDb = levelDb(mic, kFrame);
  if (echo_)
    speex_echo_cancellation(echo_, mic, speaker ? speaker : silence_.data(), out);
  else
    std::memcpy(out, mic, kFrame * sizeof(int16_t));
  speex_preprocess_run(pre_, out);
  int prob = 0;
  speex_preprocess_ctl(pre_, SPEEX_PREPROCESS_GET_PROB, &prob);
  r.probability = prob / 100.f;
  r.outDb = levelDb(out, kFrame);

  bool loud = r.outDb > minDb_;
  bool speech = loud && (r.probability >= openAbove_ || (open_ && r.probability >= stayAbove_));

  // The preprocessor alone lets bits of the other player's voice through: at the
  // onsets of their words, and for the first seconds while the canceller learns
  // the room. With an open mic that is the other player hearing themselves.
  // The echo arrives after the sound leaves the speakers and rings on after it
  // stops, so the far end counts as active for a while after its last frame.
  float farDb = speaker ? levelDb(speaker, kFrame) : -100.f;
  if (farDb > kFarActiveDb) farHold_ = kFarHoldFrames;
  bool farActive = farHold_ > 0;
  if (farHold_ > 0 && farDb <= kFarActiveDb) --farHold_;
  if (echo_ && farActive) {
    if (!echoSafe_) {
      // Until the far end has played clearly for a while with nothing left after
      // processing that could open the gate (a converged canceller, or
      // headphones), far-end talk keeps the gate shut: half duplex for those first
      // seconds. Anything loud left over restarts the count.
      if (farDb > kFarLoudDb) safeFrames_ = loud ? 0 : safeFrames_ + 1;
      if (safeFrames_ >= kSafeFrames) echoSafe_ = true;
      speech = false;
    } else if (r.micDb - r.outDb > kEchoRemovedDb) {
      // The canceller took out most of what the mic heard: it was echo. Near-end
      // speech over it (double talk) keeps most of its energy and passes.
      speech = false;
    }
  }
  if (speech) {
    open_ = true;
    hold_ = kHoldFrames;
  } else if (hold_ > 0) {
    --hold_;
  } else {
    open_ = false;
  }
  r.open = open_;
  return r;
}

// --------------------------------------------------------------------------
// Codec
// --------------------------------------------------------------------------

Encoder::Encoder(int bitrate) {
  int error = 0;
  enc_ = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &error);
  if (error != OPUS_OK) throw std::runtime_error(opus_strerror(error));
  opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate));
  opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(8));
  // In-band FEC: each packet carries a coarse copy of the previous one, so a
  // single lost packet is rebuilt from the next instead of guessed.
  opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(1));
  opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(10));
}

Encoder::~Encoder() {
  if (enc_) opus_encoder_destroy(enc_);
}

int Encoder::encode(const int16_t* pcm, uint8_t* out, int capacity) {
  return opus_encode(enc_, pcm, kPacket, out, capacity);
}

// --------------------------------------------------------------------------
// Jitter buffer
// --------------------------------------------------------------------------

// Nothing arrived for this long: the talker left without an end packet (it got
// lost, or they disconnected). Concealment would only make noise from here on.
constexpr int kGiveUpPackets = 10;  // 200 ms
// A backlog this far past the target means the network delivered a burst after a
// stall. Playing it all would leave the talker permanently late, so skip ahead.
constexpr int kMaxBacklog = 12;

JitterBuffer::JitterBuffer(int targetPackets) : target_(std::max(1, targetPackets)) {
  int error = 0;
  dec_ = opus_decoder_create(kRate, 1, &error);
  if (error != OPUS_OK) throw std::runtime_error(opus_strerror(error));
}

JitterBuffer::~JitterBuffer() {
  if (dec_) opus_decoder_destroy(dec_);
}

void JitterBuffer::reset() {
  if (state_ == State::Playing) floor_ = next_;
  queue_.clear();
  state_ = State::Idle;
  waited_ = missing_ = 0;
}

void JitterBuffer::push(uint16_t seq, uint8_t flags, const uint8_t* data, size_t size) {
  // 16-bit sequence numbers wrap every 22 minutes of talking; unwrap them
  // relative to the last one seen.
  int32_t unwrapped = haveBase_ ? base_ + int16_t(uint16_t(seq - uint16_t(base_))) : seq;
  if (!haveBase_ || unwrapped > base_) base_ = unwrapped;
  haveBase_ = true;

  ++stats_.received;
  // Already played, or from a spurt that already ended: a straggler.
  if (unwrapped < floor_ || (state_ == State::Playing && unwrapped < next_)) {
    ++stats_.late;
    return;
  }
  if (state_ != State::Idle && unwrapped - next_ > 50) reset();  // a new spurt far ahead
  if (state_ == State::Idle) {
    state_ = State::Buffering;
    next_ = unwrapped;
    waited_ = 0;
  } else if (state_ == State::Buffering && unwrapped < next_) {
    next_ = unwrapped;  // the pre-roll packet can arrive after the first one
  }
  queue_.emplace(unwrapped, Packet{flags, std::vector<uint8_t>(data, data + size)});
}

bool JitterBuffer::pull(int16_t* out) {
  if (state_ == State::Idle) {
    std::fill(out, out + kPacket, int16_t(0));
    return false;
  }
  if (state_ == State::Buffering) {
    // Start once the target is buffered, or after waiting that long anyway: a
    // two-packet spurt must still be heard.
    if (int(queue_.size()) < target_ && ++waited_ < target_) {
      std::fill(out, out + kPacket, int16_t(0));
      return false;
    }
    state_ = State::Playing;
    next_ = queue_.empty() ? next_ : queue_.begin()->first;
  }

  while (int(queue_.size()) > kMaxBacklog) {
    queue_.erase(queue_.begin());
    ++stats_.dropped;
    next_ = queue_.begin()->first;
  }

  bool end = false;
  auto it = queue_.find(next_);
  if (it != queue_.end()) {
    int n = opus_decode(dec_, it->second.data.data(), int(it->second.data.size()), out, kPacket, 0);
    if (n != kPacket) std::fill(out, out + kPacket, int16_t(0));
    end = it->second.flags & kEnd;
    queue_.erase(it);
    missing_ = 0;
    ++stats_.played;
  } else {
    auto after = queue_.find(next_ + 1);
    int n = after != queue_.end()
                ? opus_decode(dec_, after->second.data.data(), int(after->second.data.size()), out, kPacket, 1)
                : opus_decode(dec_, nullptr, 0, out, kPacket, 0);
    if (n != kPacket) std::fill(out, out + kPacket, int16_t(0));
    ++(after != queue_.end() ? stats_.recovered : stats_.concealed);
    ++missing_;
  }
  queue_.erase(queue_.begin(), queue_.lower_bound(next_ + 1));
  ++next_;

  if (end || (missing_ >= kGiveUpPackets && queue_.empty())) {
    reset();
    if (end) opus_decoder_ctl(dec_, OPUS_RESET_STATE);
  }
  return true;
}

}  // namespace otera::voice
