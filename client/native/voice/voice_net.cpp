#include "voice_net.h"

#include <chrono>
#include <cstring>

#include <mbedtls/chachapoly.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
using Socket = SOCKET;
const Socket kBadSocket = INVALID_SOCKET;
#define CLOSE_SOCKET closesocket
#else
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
const Socket kBadSocket = -1;
#define CLOSE_SOCKET close
#endif

// The header keeps the socket as intptr_t so it does not need the system headers.
#define SOCK(x) Socket(x)

namespace otera::voice {

namespace {

constexpr uint8_t PING = 0x01, VOICE = 0x02, PONG = 0x81, VOICE_IN = 0x82;
constexpr size_t HEADER = 8, TAG = 16, NONCE = 12;
constexpr int PING_EVERY_MS = 2000;
// Three pings without an answer: the relay (or the tunnel) is gone.
constexpr int HEALTHY_MS = 6500;

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void nonceFor(uint8_t direction, uint32_t counter, uint32_t session, uint8_t* nonce) {
  std::memset(nonce, 0, NONCE);
  nonce[0] = direction;
  std::memcpy(nonce + 4, &counter, 4);
  std::memcpy(nonce + 8, &session, 4);
}

}  // namespace

UdpLink::UdpLink(OnVoice onVoice) : onVoice_(std::move(onVoice)) {}

UdpLink::~UdpLink() { stop(); }

bool UdpLink::start(const std::string& host, uint16_t port, uint32_t session, const Key& key, std::string& error) {
  stop();
#ifdef _WIN32
  static bool wsa = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  if (!wsa) {
    error = "winsock";
    return false;
  }
#endif
  addrinfo hints{}, *found = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found) {
    error = "no se encontro " + host;
    return false;
  }
  intptr_t s = -1;
  for (addrinfo* a = found; a; a = a->ai_next) {
    Socket candidate = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (candidate == kBadSocket) continue;
    // A connected UDP socket only accepts datagrams from the relay.
    if (connect(candidate, a->ai_addr, socklen_t(a->ai_addrlen)) == 0) {
      s = intptr_t(candidate);
      break;
    }
    CLOSE_SOCKET(candidate);
  }
  freeaddrinfo(found);
  if (s == -1) {
    error = "no se pudo abrir el socket UDP";
    return false;
  }
  socket_ = s;
  session_ = session;
  key_ = key;
  sent_ = 0;
  highest_ = 0;
  window_ = 0;
  anyReceived_ = false;
  lastPong_ = 0;
  rtt_ = -1;
  running_ = true;
  thread_ = std::thread([this] { run(); });
  return true;
}

void UdpLink::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
  if (socket_ != -1) CLOSE_SOCKET(SOCK(socket_));
  socket_ = -1;
}

bool UdpLink::healthy() const {
  int64_t last = lastPong_;
  return last && nowMs() - last < HEALTHY_MS;
}

void UdpLink::sendPlain(const uint8_t* plain, size_t size) {
  if (socket_ == -1 || size > 1024) return;
  uint8_t out[HEADER + 1024 + TAG];
  uint32_t counter = ++sent_;
  std::memcpy(out, &session_, 4);
  std::memcpy(out + 4, &counter, 4);
  uint8_t nonce[NONCE];
  nonceFor(0, counter, session_, nonce);
  // RFC 8439 ChaCha20-Poly1305, ciphertext then tag: the layout libsodium's
  // crypto_aead_chacha20poly1305_ietf uses on the relay side.
  mbedtls_chachapoly_context ctx;
  mbedtls_chachapoly_init(&ctx);
  mbedtls_chachapoly_setkey(&ctx, key_.data());
  int ok = mbedtls_chachapoly_encrypt_and_tag(&ctx, size, nonce, out, HEADER, plain, out + HEADER, out + HEADER + size);
  mbedtls_chachapoly_free(&ctx);
  if (ok != 0) return;
  send(SOCK(socket_), reinterpret_cast<const char*>(out), int(HEADER + size + TAG), 0);
}

void UdpLink::sendVoice(uint16_t seq, uint8_t flags, const uint8_t* opus, size_t size) {
  if (!running_ || size > 400) return;
  uint8_t plain[4 + 400];
  plain[0] = VOICE;
  std::memcpy(plain + 1, &seq, 2);
  plain[3] = flags;
  std::memcpy(plain + 4, opus, size);
  sendPlain(plain, 4 + size);
}

bool UdpLink::acceptCounter(uint32_t counter) {
  if (!anyReceived_ || counter > highest_) {
    uint32_t shift = anyReceived_ ? counter - highest_ : 64;
    window_ = shift >= 64 ? 1 : (window_ << shift) | 1;
    highest_ = counter;
    anyReceived_ = true;
    return true;
  }
  uint32_t back = highest_ - counter;
  if (back >= 64 || (window_ >> back) & 1) return false;
  window_ |= uint64_t(1) << back;
  return true;
}

void UdpLink::run() {
  Socket s = SOCK(socket_);
  int64_t nextPing = 0;
  uint8_t in[2048], plain[2048];
  while (running_) {
    int64_t now = nowMs();
    if (now >= nextPing) {
      // The token is the send time, so the pong carries its own round trip.
      uint8_t ping[5] = {PING};
      uint32_t token = uint32_t(now);
      std::memcpy(ping + 1, &token, 4);
      sendPlain(ping, sizeof(ping));
      nextPing = now + PING_EVERY_MS;
    }
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval timeout{0, 200000};
    if (select(0, &set, nullptr, nullptr, &timeout) <= 0) continue;
#else
    pollfd p{s, POLLIN, 0};
    if (poll(&p, 1, 200) <= 0) continue;
#endif
    int n = recv(s, reinterpret_cast<char*>(in), sizeof(in), 0);
    if (n < int(HEADER + 1 + TAG)) continue;
    uint32_t session, counter;
    std::memcpy(&session, in, 4);
    std::memcpy(&counter, in + 4, 4);
    if (session != session_) continue;
    uint8_t nonce[NONCE];
    nonceFor(1, counter, session, nonce);
    size_t size = size_t(n) - HEADER - TAG;
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    mbedtls_chachapoly_setkey(&ctx, key_.data());
    int ok = mbedtls_chachapoly_auth_decrypt(&ctx, size, nonce, in, HEADER, in + HEADER + size, in + HEADER, plain);
    mbedtls_chachapoly_free(&ctx);
    if (ok != 0) continue;
    if (!acceptCounter(counter)) continue;
    if (plain[0] == PONG && size == 5) {
      uint32_t token;
      std::memcpy(&token, plain + 1, 4);
      int64_t t = nowMs();
      lastPong_ = t;
      rtt_ = int(uint32_t(t) - token);
    } else if (plain[0] == VOICE_IN && size > 8) {
      uint32_t creature;
      uint16_t seq;
      std::memcpy(&creature, plain + 1, 4);
      std::memcpy(&seq, plain + 5, 2);
      onVoice_(creature, seq, plain[7], plain + 8, size - 8);
    }
  }
}

}  // namespace otera::voice
