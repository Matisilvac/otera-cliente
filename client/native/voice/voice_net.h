// The UDP side of otera-voice: one encrypted session with the relay in TFS
// (src/voice.cpp, which documents the datagram format).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace otera::voice {

class UdpLink {
public:
  using Key = std::array<uint8_t, 32>;
  using OnVoice = std::function<void(uint32_t creature, uint16_t seq, uint8_t flags, const uint8_t* opus, size_t size)>;

  explicit UdpLink(OnVoice onVoice);
  ~UdpLink();
  UdpLink(const UdpLink&) = delete;
  UdpLink& operator=(const UdpLink&) = delete;

  bool start(const std::string& host, uint16_t port, uint32_t session, const Key& key, std::string& error);
  void stop();

  // Any thread (the audio callback sends from its own).
  void sendVoice(uint16_t seq, uint8_t flags, const uint8_t* opus, size_t size);

  // The relay answered a ping in the last few seconds.
  bool healthy() const;
  int rttMs() const { return rtt_; }

private:
  void run();
  void sendPlain(const uint8_t* plain, size_t size);
  bool acceptCounter(uint32_t counter);

  OnVoice onVoice_;
  intptr_t socket_ = -1;
  std::thread thread_;
  std::atomic<bool> running_{false};
  uint32_t session_ = 0;
  Key key_{};
  std::atomic<uint32_t> sent_{0};
  uint32_t highest_ = 0;
  uint64_t window_ = 0;
  bool anyReceived_ = false;
  std::atomic<int64_t> lastPong_{0};
  std::atomic<int> rtt_{-1};
};

}  // namespace otera::voice
