// The audio devices of otera-voice serve: capture through CaptureProcessor to the
// network, and every talker's jitter buffer mixed to stereo by where they stand.
#pragma once

#include "voice_dsp.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace otera::voice {

// 48 kHz 16-bit wav files, mono or stereo, through miniaudio.
std::vector<int16_t> loadWav(const std::string& path, int channels = 1);
void saveWav(const std::string& path, const std::vector<int16_t>& pcm, int channels = 1);

struct EngineOptions {
  bool nullAudio = false;   // no devices: miniaudio's null backend keeps real-time pace
  std::string micFile;      // with nullAudio, the mic plays this wav (then silence)
  std::string recordFile;   // with nullAudio, what would reach the speakers is saved here
};

class Engine {
public:
  using Send = std::function<void(uint16_t seq, uint8_t flags, const uint8_t* opus, size_t size)>;

  Engine(EngineOptions options, Send send);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Opens the devices again: with the mic only when capturing, so nothing records
  // (and macOS shows no mic indicator) while nobody could hear the player.
  bool configure(bool capture, const std::string& mic, const std::string& out, std::string& error);
  bool capturing() const { return capture_; }

  void setSensitivity(float value);
  void setVolume(float value) { volume_ = value; }
  // Self-mute keeps the device open (no reopen lag) but sends nothing.
  void setMuted(bool muted) { muted_ = muted; }

  // Network thread.
  void onVoice(uint32_t creature, uint16_t seq, uint8_t flags, const uint8_t* opus, size_t size);

  // Where each talker stands relative to the player, in tiles. Talkers not listed
  // (a party member out of sight) are heard centered and a little lower.
  void setPositions(std::unordered_map<uint32_t, std::pair<float, float>> positions);
  void setBlocked(uint32_t creature, bool blocked);

  struct Status {
    float micDb = -100;
    bool open = false;          // the gate is open: the player is being sent
    bool silentMic = false;     // capturing, but the device gives exact zeros
    std::vector<uint32_t> talking;
  };
  Status status();

  static bool devices(std::vector<std::string>& mics, std::vector<std::string>& outs, std::string& error);

  // Audio thread.
  void callback(int16_t* out, const int16_t* in, uint32_t frames);

private:
  struct Talker {
    std::unique_ptr<JitterBuffer> jitter = std::make_unique<JitterBuffer>(3);
    bool talking = false;
    int idlePackets = 0;
  };

  void close();
  void mix(int16_t* stereo);

  EngineOptions options_;
  Send send_;
  struct Device;
  std::unique_ptr<Device> device_;
  bool capture_ = false;
  std::atomic<bool> muted_{false};
  std::atomic<float> volume_{1.f};

  std::unique_ptr<CaptureProcessor> processor_;
  std::unique_ptr<Sender> sender_;
  Ring micIn_{kRate}, reference_{kRate}, playOut_{2 * kRate};  // audio thread only
  std::vector<int16_t> micFile_;
  size_t micFilePos_ = 0;
  std::vector<int16_t> recorded_;
  int zeroFrames_ = 0;

  std::mutex talkersMutex_;
  std::unordered_map<uint32_t, Talker> talkers_;
  std::unordered_map<uint32_t, std::pair<float, float>> positions_;
  std::unordered_map<uint32_t, bool> blocked_;

  std::atomic<float> micDb_{-100};
  std::atomic<bool> open_{false};
};

}  // namespace otera::voice
