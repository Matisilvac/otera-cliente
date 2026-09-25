// otera-voice serve: what the game client runs.
//
//   otera-voice serve <status.json> [--null-audio] [--mic-file wav] [--record wav]
//
// It listens for the game client (modules/game_voice) on a websocket on
// 127.0.0.1, with a random port and token that it writes to <status.json>; the
// client reads them and connects to ws://127.0.0.1:<port>/<token>. The token keeps
// out anything else on the machine, a web page included: browsers can open
// websockets to localhost, but cannot read the file. Requests with an Origin
// header (browsers always send one; OTClient does not) are refused too.
//
// Messages are JSON objects with a "t" field.
//   client -> helper
//     session  {host, port, id, key}   the relay session from opcode 110
//     capture  {on}                    open the mic (someone can hear the player)
//     mute     {on}                    self-mute, the device stays open
//     settings {sensitivity, volume, mic, out}
//     pos      {c: {"<creature>": [dx, dy]}}   talkers on screen, in tiles
//     block    {id, on}                per-player mute
//     devices  {}                      list mics and outputs
//     quit     {}
//   helper -> client
//     level    {db, open}              10 per second while capturing (the meter)
//     talking  {ids}                   who is audible, on change
//     net      {ok, rtt}               the relay answers, on change
//     devices  {mics, outs}
//     error    {code, message}         mic-silent, audio, network
//
// The helper exits when the client goes away: no connection within 20 s of
// starting, or 5 s after the connection closes without a new one.

#include "voice_engine.h"
#include "voice_net.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>

using json = nlohmann::json;
using namespace otera::voice;

namespace {

using Clock = std::chrono::steady_clock;

struct Serve {
  std::mutex mutex;  // everything below, except what the engine and link guard themselves
  ix::WebSocket* client = nullptr;
  std::unique_ptr<Engine> engine;
  std::shared_ptr<UdpLink> link;
  // Replaced links are stopped at once but kept alive until exit: the audio thread
  // may still hold one for a moment, and must never be the one that frees it.
  std::vector<std::shared_ptr<UdpLink>> retired;
  bool capture = false;
  std::string mic, out;
  bool quit = false;
  Clock::time_point lastSeen = Clock::now();
  bool everConnected = false;

  void send(const json& message) {
    // Lock held by the caller.
    if (client) client->sendText(message.dump());
  }
};

// Paths arrive as UTF-8 (see wmain); u8path keeps them intact on Windows too.
std::filesystem::path pathOf(const std::string& utf8) { return std::filesystem::u8path(utf8); }

bool writeStatus(const std::string& path, int port, const std::string& token) {
  std::error_code ignored;
  auto final = pathOf(path), temporary = pathOf(path + ".tmp");
  std::filesystem::create_directories(final.parent_path(), ignored);
  {
    std::ofstream f(temporary, std::ios::trunc);
    if (!f) return false;
    f << json{{"port", port}, {"token", token}, {"version", 1}}.dump();
  }
  // Atomic: the client never reads half a file.
  std::filesystem::rename(temporary, final, ignored);
  return !ignored;
}

bool fromHex(const std::string& hex, uint8_t* out, size_t size) {
  if (hex.size() != size * 2) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < size; ++i) {
    int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = uint8_t(hi * 16 + lo);
  }
  return true;
}

void handle(Serve& s, const json& m) {
  const std::string t = m.value("t", "");
  if (t == "session") {
    std::string keyHex = m.value("key", "");
    UdpLink::Key key{};
    if (!fromHex(keyHex, key.data(), key.size())) {
      s.send({{"t", "error"}, {"code", "network"}, {"message", "clave de sesion invalida"}});
      return;
    }
    auto link = std::make_shared<UdpLink>([&s](uint32_t c, uint16_t seq, uint8_t f, const uint8_t* d, size_t n) {
      // The engine outlives every link (links are dropped first on exit).
      if (s.engine) s.engine->onVoice(c, seq, f, d, n);
    });
    std::string error;
    if (!link->start(m.value("host", ""), uint16_t(m.value("port", 0)), m.value("id", 0u), key, error)) {
      s.send({{"t", "error"}, {"code", "network"}, {"message", error}});
      return;
    }
    if (auto old = std::atomic_exchange(&s.link, link)) {
      old->stop();
      s.retired.push_back(old);
    }
  } else if (t == "capture") {
    bool on = m.value("on", false);
    if (on != s.capture) {
      s.capture = on;
      std::string error;
      if (!s.engine->configure(on, s.mic, s.out, error))
        s.send({{"t", "error"}, {"code", "audio"}, {"message", error}});
    }
  } else if (t == "mute") {
    s.engine->setMuted(m.value("on", false));
  } else if (t == "settings") {
    if (m.contains("sensitivity")) s.engine->setSensitivity(m["sensitivity"].get<float>());
    if (m.contains("volume")) s.engine->setVolume(m["volume"].get<float>());
    std::string mic = m.value("mic", s.mic), out = m.value("out", s.out);
    if (mic != s.mic || out != s.out) {
      s.mic = mic;
      s.out = out;
      std::string error;
      if (!s.engine->configure(s.capture, s.mic, s.out, error))
        s.send({{"t", "error"}, {"code", "audio"}, {"message", error}});
    }
  } else if (t == "pos") {
    std::unordered_map<uint32_t, std::pair<float, float>> positions;
    if (m.contains("c") && m["c"].is_object())
      for (auto& [id, xy] : m["c"].items())
        if (xy.is_array() && xy.size() == 2)
          positions[uint32_t(std::stoul(id))] = {xy[0].get<float>(), xy[1].get<float>()};
    s.engine->setPositions(std::move(positions));
  } else if (t == "block") {
    s.engine->setBlocked(m.value("id", 0u), m.value("on", false));
  } else if (t == "devices") {
    std::vector<std::string> mics, outs;
    std::string error;
    if (Engine::devices(mics, outs, error))
      s.send({{"t", "devices"}, {"mics", mics}, {"outs", outs}});
    else
      s.send({{"t", "error"}, {"code", "audio"}, {"message", error}});
  } else if (t == "quit") {
    s.quit = true;
  }
}

}  // namespace

int cmdServe(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "uso: otera-voice serve <status.json> [--null-audio] [--mic-file wav] [--record wav]\n");
    return 2;
  }
  std::string statusPath = argv[2];
  EngineOptions options;
  for (int i = 3; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--null-audio") options.nullAudio = true;
    else if (a == "--mic-file" && i + 1 < argc) options.micFile = argv[++i];
    else if (a == "--record" && i + 1 < argc) options.recordFile = argv[++i];
    else {
      std::fprintf(stderr, "otera-voice: opcion desconocida: %s\n", a.c_str());
      return 2;
    }
  }
  ix::initNetSystem();

  Serve s;
  s.engine = std::make_unique<Engine>(options, [&s](uint16_t seq, uint8_t flags, const uint8_t* d, size_t n) {
    // Audio thread: the link can be swapped at any time by a new session.
    if (auto link = std::atomic_load(&s.link)) link->sendVoice(seq, flags, d, n);
  });
  {
    // Playback only until the client says someone can hear the player.
    std::string error;
    if (!s.engine->configure(false, "", "", error)) std::fprintf(stderr, "otera-voice: %s\n", error.c_str());
  }

  // std::random_device is the system CSPRNG with libc++, libstdc++ and MSVC.
  std::random_device random;
  std::string token;
  for (int i = 0; i < 4; ++i) {
    char chunk[9];
    std::snprintf(chunk, sizeof(chunk), "%08x", unsigned(random()));
    token += chunk;
  }

  std::unique_ptr<ix::WebSocketServer> server;
  int port = 0;
  std::mt19937 rng{random()};
  for (int attempt = 0; attempt < 20 && !server; ++attempt) {
    port = 20000 + int(rng() % 40000);
    auto candidate = std::make_unique<ix::WebSocketServer>(port, "127.0.0.1", ix::SocketServer::kDefaultTcpBacklog, 1);
    candidate->disablePerMessageDeflate();
    if (candidate->listen().first) server = std::move(candidate);
  }
  if (!server) {
    std::fprintf(stderr, "otera-voice: no hay puerto libre en 127.0.0.1\n");
    return 1;
  }

  server->setOnClientMessageCallback(
      [&s, &token](std::shared_ptr<ix::ConnectionState>, ix::WebSocket& ws, const ix::WebSocketMessagePtr& msg) {
        bool reject = false;
        {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (msg->type == ix::WebSocketMessageType::Open) {
          bool browser = msg->openInfo.headers.count("Origin") > 0;
          if (msg->openInfo.uri != "/" + token || browser || s.client) {
            reject = true;
          } else {
            s.client = &ws;
            s.everConnected = true;
            s.lastSeen = Clock::now();
          }
        } else if (msg->type == ix::WebSocketMessageType::Close) {
          if (s.client == &ws) {
            s.client = nullptr;
            s.lastSeen = Clock::now();
          }
        } else if (msg->type == ix::WebSocketMessageType::Message && s.client == &ws) {
          json m = json::parse(msg->str, nullptr, false);
          if (m.is_object()) {
            try {
              handle(s, m);
            } catch (const std::exception&) {
              // A malformed field is the client's bug; the helper keeps running.
            }
          }
        }
        }
        // Outside the lock: closing can call back into this same handler.
        if (reject) ws.close();
      });
  server->start();
  if (!writeStatus(statusPath, port, token)) {
    std::fprintf(stderr, "otera-voice: no se pudo escribir %s\n", statusPath.c_str());
    server->stop();
    return 1;
  }

  auto started = Clock::now();
  std::vector<uint32_t> lastTalking;
  int lastNet = -1;
  bool warnedSilent = false;
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::lock_guard<std::mutex> lock(s.mutex);
    auto now = Clock::now();
    if (s.quit) break;
    if (!s.client) {
      auto idle = now - (s.everConnected ? s.lastSeen : started);
      if (idle > std::chrono::seconds(s.everConnected ? 5 : 20)) break;
      continue;
    }
    auto status = s.engine->status();
    if (s.capture) s.send({{"t", "level"}, {"db", int(status.micDb)}, {"open", status.open}});
    std::sort(status.talking.begin(), status.talking.end());
    if (status.talking != lastTalking) {
      s.send({{"t", "talking"}, {"ids", status.talking}});
      lastTalking = status.talking;
    }
    if (status.silentMic && !warnedSilent) {
      // On macOS a denied microphone records exact silence instead of failing.
      s.send({{"t", "error"}, {"code", "mic-silent"}, {"message", "el microfono no da senal"}});
      warnedSilent = true;
    }
    if (auto link = std::atomic_load(&s.link)) {
      int ok = link->healthy() ? 1 : 0;
      if (ok != lastNet) {
        s.send({{"t", "net"}, {"ok", ok == 1}, {"rtt", link->rttMs()}});
        lastNet = ok;
      }
    }
  }

  server->stop();
  // The network thread feeds the engine: stop it before the engine goes.
  if (auto link = std::atomic_exchange(&s.link, std::shared_ptr<UdpLink>())) {
    link->stop();
    s.retired.push_back(link);
  }
  s.engine.reset();
  s.retired.clear();
  std::error_code ignored;
  std::filesystem::remove(pathOf(statusPath), ignored);
  return 0;
}
