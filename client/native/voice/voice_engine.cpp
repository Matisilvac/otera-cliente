#include "voice_engine.h"

#include "miniaudio.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace otera::voice {

std::vector<int16_t> loadWav(const std::string& path, int channels) {
  ma_decoder_config config = ma_decoder_config_init(ma_format_s16, ma_uint32(channels), kRate);
  ma_decoder decoder;
  if (ma_decoder_init_file(path.c_str(), &config, &decoder) != MA_SUCCESS)
    throw std::runtime_error("no se pudo leer " + path);
  std::vector<int16_t> pcm;
  std::vector<int16_t> buf(4096 * size_t(channels));
  ma_uint64 read = 0;
  while (ma_decoder_read_pcm_frames(&decoder, buf.data(), 4096, &read) == MA_SUCCESS && read > 0)
    pcm.insert(pcm.end(), buf.begin(), buf.begin() + ptrdiff_t(read * ma_uint64(channels)));
  ma_decoder_uninit(&decoder);
  if (pcm.empty()) throw std::runtime_error(path + " no tiene audio");
  return pcm;
}

void saveWav(const std::string& path, const std::vector<int16_t>& pcm, int channels) {
  ma_encoder_config config = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, ma_uint32(channels), kRate);
  ma_encoder encoder;
  if (ma_encoder_init_file(path.c_str(), &config, &encoder) != MA_SUCCESS)
    throw std::runtime_error("no se pudo escribir " + path);
  ma_encoder_write_pcm_frames(&encoder, pcm.data(), pcm.size() / size_t(channels), nullptr);
  ma_encoder_uninit(&encoder);
}

struct Engine::Device {
  ma_context context;
  ma_device device;
  bool contextReady = false, deviceReady = false;
};

namespace {

// A talker that has not said anything for this long frees its decoder.
constexpr int kForgetPackets = 30 * 50;
// Positional mix (Engine::mix).
constexpr float kFarGain = 0.5f, kMaxPan = 0.6f;

void deviceCallback(ma_device* device, void* output, const void* input, ma_uint32 frames) {
  static_cast<Engine*>(device->pUserData)
      ->callback(static_cast<int16_t*>(output), static_cast<const int16_t*>(input), frames);
}

bool openContext(ma_context& context, bool null) {
  if (null) {
    ma_backend backends[] = {ma_backend_null};
    return ma_context_init(backends, 1, nullptr, &context) == MA_SUCCESS;
  }
  return ma_context_init(nullptr, 0, nullptr, &context) == MA_SUCCESS;
}

}  // namespace

Engine::Engine(EngineOptions options, Send send)
    : options_(std::move(options)), send_(std::move(send)), device_(std::make_unique<Device>()),
      processor_(std::make_unique<CaptureProcessor>()), sender_(std::make_unique<Sender>()) {
  if (options_.nullAudio && !options_.micFile.empty()) micFile_ = loadWav(options_.micFile);
}

Engine::~Engine() {
  close();
  if (options_.nullAudio && !options_.recordFile.empty()) saveWav(options_.recordFile, recorded_, 2);
}

void Engine::close() {
  if (device_->deviceReady) ma_device_uninit(&device_->device);
  if (device_->contextReady) ma_context_uninit(&device_->context);
  device_->deviceReady = device_->contextReady = false;
}

void Engine::setSensitivity(float value) {
  // Only between reopens of the device the processor is replaced; the audio thread
  // reads the gate thresholds as plain floats, a torn read costs one frame.
  processor_->setSensitivity(value);
}

bool Engine::configure(bool capture, const std::string& mic, const std::string& out, std::string& error) {
  float sensitivity = processor_->sensitivity();
  close();
  capture_ = capture;
  // A new device is a new echo path: the canceller starts learning again.
  processor_ = std::make_unique<CaptureProcessor>();
  processor_->setSensitivity(sensitivity);
  micIn_ = Ring(kRate);
  reference_ = Ring(kRate);
  playOut_ = Ring(2 * kRate);
  zeroFrames_ = 0;

  if (!openContext(device_->context, options_.nullAudio)) {
    error = "no se pudo abrir el sistema de audio";
    return false;
  }
  device_->contextReady = true;
  ma_device_info *playback = nullptr, *captureInfo = nullptr;
  ma_uint32 playbackCount = 0, captureCount = 0;
  ma_context_get_devices(&device_->context, &playback, &playbackCount, &captureInfo, &captureCount);

  ma_device_config config = ma_device_config_init(capture ? ma_device_type_duplex : ma_device_type_playback);
  config.sampleRate = kRate;
  config.capture.format = ma_format_s16;
  config.capture.channels = 1;
  config.playback.format = ma_format_s16;
  config.playback.channels = 2;
  config.periodSizeInFrames = kFrame;
  config.performanceProfile = ma_performance_profile_low_latency;
  config.dataCallback = deviceCallback;
  config.pUserData = this;
  for (ma_uint32 i = 0; i < captureCount && !mic.empty(); ++i)
    if (mic == captureInfo[i].name) config.capture.pDeviceID = &captureInfo[i].id;
  for (ma_uint32 i = 0; i < playbackCount && !out.empty(); ++i)
    if (out == playback[i].name) config.playback.pDeviceID = &playback[i].id;

  if (ma_device_init(&device_->context, &config, &device_->device) != MA_SUCCESS) {
    error = capture ? "no se pudo abrir el microfono o la salida" : "no se pudo abrir la salida de audio";
    close();
    return false;
  }
  device_->deviceReady = true;
  if (ma_device_start(&device_->device) != MA_SUCCESS) {
    error = "no se pudo arrancar el audio";
    close();
    return false;
  }
  return true;
}

void Engine::onVoice(uint32_t creature, uint16_t seq, uint8_t flags, const uint8_t* opus, size_t size) {
  std::lock_guard<std::mutex> lock(talkersMutex_);
  auto blocked = blocked_.find(creature);
  if (blocked != blocked_.end() && blocked->second) return;
  talkers_[creature].jitter->push(seq, flags, opus, size);
}

void Engine::setPositions(std::unordered_map<uint32_t, std::pair<float, float>> positions) {
  std::lock_guard<std::mutex> lock(talkersMutex_);
  positions_ = std::move(positions);
}

void Engine::setBlocked(uint32_t creature, bool blocked) {
  std::lock_guard<std::mutex> lock(talkersMutex_);
  blocked_[creature] = blocked;
  if (blocked) talkers_.erase(creature);
}

Engine::Status Engine::status() {
  Status s;
  s.micDb = capture_ ? micDb_.load() : -100.f;
  s.open = capture_ && open_;
  s.silentMic = capture_ && !options_.nullAudio && zeroFrames_ > 200;  // 2 s of exact zeros
  std::lock_guard<std::mutex> lock(talkersMutex_);
  for (auto& [id, talker] : talkers_)
    if (talker.talking) s.talking.push_back(id);
  return s;
}

bool Engine::devices(std::vector<std::string>& mics, std::vector<std::string>& outs, std::string& error) {
  ma_context context;
  if (!openContext(context, false)) {
    error = "no se pudo abrir el sistema de audio";
    return false;
  }
  ma_device_info *playback, *capture;
  ma_uint32 playbackCount, captureCount;
  bool ok = ma_context_get_devices(&context, &playback, &playbackCount, &capture, &captureCount) == MA_SUCCESS;
  for (ma_uint32 i = 0; ok && i < captureCount; ++i) mics.emplace_back(capture[i].name);
  for (ma_uint32 i = 0; ok && i < playbackCount; ++i) outs.emplace_back(playback[i].name);
  ma_context_uninit(&context);
  if (!ok) error = "no se pudieron listar los dispositivos";
  return ok;
}

void Engine::mix(int16_t* stereo) {
  float left[kPacket] = {}, right[kPacket] = {};
  int16_t pcm[kPacket];
  float volume = volume_;
  {
    std::lock_guard<std::mutex> lock(talkersMutex_);
    for (auto it = talkers_.begin(); it != talkers_.end();) {
      Talker& t = it->second;
      bool audible = t.jitter->pull(pcm);
      t.talking = t.jitter->talking();
      t.idlePackets = audible ? 0 : t.idlePackets + 1;
      if (audible) {
        // Where the talker stands: to the side pans, farther is quieter. The same
        // curve on screen and off it (the party anywhere on the map), so walking out
        // of view changes nothing: the voice keeps its side and its volume.
        //   - volume: 1 next to you, 0.65 at the edge of the screen, 0.5 from 10 tiles
        //     on, so the party far away is still clear;
        //   - side: dx over the distance, the sine of the direction for someone far
        //     away. At most 0.6: the other speaker keeps about a third (-10 dB), a
        //     full pan sounds like a broken speaker.
        float gain = kFarGain, pan = 0.f;
        auto pos = positions_.find(it->first);
        if (pos != positions_.end()) {
          float dx = pos->second.first, dy = pos->second.second;
          float distance = std::sqrt(dx * dx + dy * dy);
          gain = std::clamp(1.f - distance / 20.f, kFarGain, 1.f);
          pan = kMaxPan * dx / std::max(distance, 7.f);
        }
        if (!surround_) pan = 0.f;
        // Equal-power pan: the talker keeps the same loudness wherever they are.
        float angle = (pan + 1.f) * 0.785398f;
        float gl = std::cos(angle) * gain * volume * 1.41421f, gr = std::sin(angle) * gain * volume * 1.41421f;
        // Positions move a tile at a time; gliding over the packet keeps the step
        // from clicking.
        if (t.left < 0) t.left = gl, t.right = gr;
        for (int i = 0; i < kPacket; ++i) {
          float k = float(i + 1) / kPacket;
          left[i] += pcm[i] * (t.left + (gl - t.left) * k);
          right[i] += pcm[i] * (t.right + (gr - t.right) * k);
        }
        t.left = gl, t.right = gr;
      }
      if (t.idlePackets > kForgetPackets)
        it = talkers_.erase(it);
      else
        ++it;
    }
  }
  for (int i = 0; i < kPacket; ++i) {
    stereo[2 * i] = int16_t(std::clamp(left[i], -32768.f, 32767.f));
    stereo[2 * i + 1] = int16_t(std::clamp(right[i], -32768.f, 32767.f));
  }
}

void Engine::callback(int16_t* out, const int16_t* in, uint32_t frames) {
  int16_t stereo[2 * kPacket];
  while (playOut_.size() < size_t(frames) * 2) {
    mix(stereo);
    playOut_.push(stereo, 2 * kPacket);
  }
  playOut_.pop(out, size_t(frames) * 2);
  if (options_.nullAudio && !options_.recordFile.empty()) recorded_.insert(recorded_.end(), out, out + frames * 2);

  if (!capture_) return;

  // The echo canceller needs what the speakers played, as one channel.
  for (uint32_t i = 0; i < frames; ++i) {
    int16_t mono = int16_t((int32_t(out[2 * i]) + out[2 * i + 1]) / 2);
    reference_.push(&mono, 1);
  }
  if (!micFile_.empty() || options_.nullAudio) {
    for (uint32_t i = 0; i < frames; ++i) {
      int16_t v = micFilePos_ < micFile_.size() ? micFile_[micFilePos_++] : 0;
      micIn_.push(&v, 1);
    }
  } else if (in) {
    micIn_.push(in, frames);
  }

  int16_t mic[kFrame], spk[kFrame], clean[kFrame];
  while (micIn_.size() >= kFrame && reference_.size() >= kFrame) {
    micIn_.pop(mic, kFrame);
    reference_.pop(spk, kFrame);
    bool zeros = std::all_of(mic, mic + kFrame, [](int16_t v) { return v == 0; });
    zeroFrames_ = zeros ? zeroFrames_ + 1 : 0;
    auto r = processor_->process(mic, spk, clean);
    micDb_ = r.micDb;
    bool open = r.open && !muted_;
    open_ = open;
    sender_->frame(clean, open, [this](uint16_t s, uint8_t f, const uint8_t* d, size_t n) { send_(s, f, d, n); });
  }
}

}  // namespace otera::voice
