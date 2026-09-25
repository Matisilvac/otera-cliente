// otera-voice: the voice chat helper of the Otera client.
//
// This first version is the audio engine alone, so it can be heard and measured
// before it talks to any server:
//
//   otera-voice devices                     microphones and outputs
//   otera-voice loopback [options]          hear yourself through the whole chain
//   otera-voice selftest far.wav near.wav   echo, gate and codec, offline, as JSON
//   otera-voice selftest-jitter             jitter buffer checks
//
// loopback runs mic -> echo cancellation -> gate -> Opus -> a simulated network
// (delay, loss, reordering) -> jitter buffer -> speakers. With the default delay
// you speak and hear it back a second later; the echo canceller is what keeps
// your speakers from feeding that back into the mic forever.

#include "voice_dsp.h"
#include "voice_engine.h"
#include "miniaudio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

using namespace otera::voice;

int cmdServe(int argc, char** argv);  // voice_serve.cpp

namespace {

std::atomic<bool> g_stop{false};

struct Wire {
  uint64_t due;
  uint16_t seq;
  uint8_t flags;
  std::vector<uint8_t> data;
};

// --------------------------------------------------------------------------
// devices
// --------------------------------------------------------------------------

int cmdDevices() {
  ma_context context;
  if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
    std::fprintf(stderr, "no se pudo abrir el sistema de audio\n");
    return 1;
  }
  ma_device_info *playback, *capture;
  ma_uint32 playbackCount, captureCount;
  if (ma_context_get_devices(&context, &playback, &playbackCount, &capture, &captureCount) != MA_SUCCESS) {
    std::fprintf(stderr, "no se pudieron listar los dispositivos\n");
    ma_context_uninit(&context);
    return 1;
  }
  std::printf("Microfonos:\n");
  for (ma_uint32 i = 0; i < captureCount; ++i)
    std::printf("  %u  %s%s\n", i, capture[i].name, capture[i].isDefault ? "  (predeterminado)" : "");
  std::printf("Salidas:\n");
  for (ma_uint32 i = 0; i < playbackCount; ++i)
    std::printf("  %u  %s%s\n", i, playback[i].name, playback[i].isDefault ? "  (predeterminado)" : "");
  ma_context_uninit(&context);
  return 0;
}

// --------------------------------------------------------------------------
// loopback
// --------------------------------------------------------------------------

struct Loopback {
  explicit Loopback(bool aec) : capture(300, aec) {}

  CaptureProcessor capture;
  Sender sender;
  JitterBuffer jitter{3};
  Ring micIn{kRate}, reference{kRate}, playOut{kRate};
  std::vector<Wire> network;
  uint64_t captured = 0, played = 0;
  uint64_t delay = 0, jitterMax = 0;
  double loss = 0;
  std::mt19937 rng{12345};

  std::atomic<float> micDb{-100}, outDb{-100}, probability{0};
  std::atomic<bool> open{false}, hearing{false};
  std::atomic<uint64_t> sent{0}, sentBytes{0};

  void send(uint16_t seq, uint8_t flags, const uint8_t* data, size_t size) {
    sent++;
    sentBytes += size;
    std::uniform_real_distribution<double> u(0, 1);
    if (u(rng) < loss) return;
    uint64_t extra = jitterMax ? uint64_t(u(rng) * double(jitterMax)) : 0;
    network.push_back({captured + delay + extra, seq, flags, std::vector<uint8_t>(data, data + size)});
  }

  void callback(int16_t* out, const int16_t* in, uint32_t frames) {
    micIn.push(in, frames);

    int16_t pcm[kPacket];
    while (playOut.size() < frames) {
      uint64_t now = played + playOut.size();
      for (auto it = network.begin(); it != network.end();) {
        if (it->due <= now) {
          jitter.push(it->seq, it->flags, it->data.data(), it->data.size());
          it = network.erase(it);
        } else {
          ++it;
        }
      }
      jitter.pull(pcm);
      playOut.push(pcm, kPacket);
    }
    playOut.pop(out, frames);
    reference.push(out, frames);
    played += frames;
    hearing = jitter.talking();

    int16_t mic[kFrame], spk[kFrame], clean[kFrame];
    while (micIn.size() >= kFrame && reference.size() >= kFrame) {
      micIn.pop(mic, kFrame);
      reference.pop(spk, kFrame);
      auto r = capture.process(mic, spk, clean);
      micDb = r.micDb;
      outDb = r.outDb;
      probability = r.probability;
      open = r.open;
      sender.frame(clean, r.open, [this](uint16_t s, uint8_t f, const uint8_t* d, size_t n) { send(s, f, d, n); });
      captured += kFrame;
    }
  }
};

void loopbackCallback(ma_device* device, void* output, const void* input, ma_uint32 frames) {
  static_cast<Loopback*>(device->pUserData)
      ->callback(static_cast<int16_t*>(output), static_cast<const int16_t*>(input), frames);
}

std::string bar(float db) {
  int n = std::clamp(int((db + 60) / 60 * 30), 0, 30);
  return std::string(size_t(n), '#') + std::string(size_t(30 - n), ' ');
}

int cmdLoopback(int argc, char** argv) {
  int delayMs = 1000, jitterMs = 0, seconds = 0, micIndex = -1, outIndex = -1;
  double lossPct = 0, sensitivity = 0.5;
  bool aec = true, json = false;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&]() -> const char* {
      if (i + 1 >= argc) throw std::runtime_error("falta el valor de " + a);
      return argv[++i];
    };
    if (a == "--delay") delayMs = std::atoi(value());
    else if (a == "--jitter") jitterMs = std::atoi(value());
    else if (a == "--loss") lossPct = std::atof(value());
    else if (a == "--sensitivity") sensitivity = std::atof(value());
    else if (a == "--seconds") seconds = std::atoi(value());
    else if (a == "--mic") micIndex = std::atoi(value());
    else if (a == "--out") outIndex = std::atoi(value());
    else if (a == "--no-aec") aec = false;
    else if (a == "--json") json = true;
    else throw std::runtime_error("opcion desconocida: " + a);
  }

  Loopback state(aec);
  state.capture.setSensitivity(float(sensitivity));
  state.delay = uint64_t(std::max(delayMs, 0)) * kRate / 1000;
  state.jitterMax = uint64_t(std::max(jitterMs, 0)) * kRate / 1000;
  state.loss = std::clamp(lossPct, 0.0, 100.0) / 100.0;

  ma_context context;
  if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
    std::fprintf(stderr, "no se pudo abrir el sistema de audio\n");
    return 1;
  }
  ma_device_info *playback, *capture;
  ma_uint32 playbackCount, captureCount;
  ma_context_get_devices(&context, &playback, &playbackCount, &capture, &captureCount);

  ma_device_config config = ma_device_config_init(ma_device_type_duplex);
  config.sampleRate = kRate;
  config.capture.format = ma_format_s16;
  config.capture.channels = 1;
  config.playback.format = ma_format_s16;
  config.playback.channels = 1;
  config.periodSizeInFrames = kFrame;
  config.performanceProfile = ma_performance_profile_low_latency;
  config.dataCallback = loopbackCallback;
  config.pUserData = &state;
  if (micIndex >= 0 && ma_uint32(micIndex) < captureCount) config.capture.pDeviceID = &capture[micIndex].id;
  if (outIndex >= 0 && ma_uint32(outIndex) < playbackCount) config.playback.pDeviceID = &playback[outIndex].id;

  ma_device device;
  if (ma_device_init(&context, &config, &device) != MA_SUCCESS || ma_device_start(&device) != MA_SUCCESS) {
    std::fprintf(stderr, "no se pudo abrir el microfono o la salida de audio\n");
    ma_context_uninit(&context);
    return 1;
  }
  if (!json) {
    std::printf("Microfono: %s\nSalida:     %s\n", device.capture.name, device.playback.name);
    std::printf("Retardo %d ms, perdida %.0f%%, cancelacion de eco %s. Ctrl+C para terminar.\n\n", delayMs,
                lossPct, aec ? "activada" : "APAGADA");
  }

  std::signal(SIGINT, [](int) { g_stop = true; });
  bool tty = isatty(fileno(stdout));
  auto start = std::chrono::steady_clock::now();
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(json ? 100 : 80));
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (seconds > 0 && elapsed >= seconds) break;
    if (json) {
      std::printf("{\"t\":%.2f,\"micDb\":%.1f,\"outDb\":%.1f,\"prob\":%.2f,\"open\":%s,\"hearing\":%s,\"sent\":%llu}\n",
                  elapsed, state.micDb.load(), state.outDb.load(), state.probability.load(),
                  state.open ? "true" : "false", state.hearing ? "true" : "false",
                  (unsigned long long)state.sent.load());
    } else {
      std::printf("%smic %4.0f dB |%s| voz %.2f  %-9s %s%s", tty ? "\r" : "", state.micDb.load(),
                  bar(state.micDb).c_str(), state.probability.load(), state.open ? "HABLANDO" : "",
                  state.hearing ? "(te estas escuchando)" : "                     ", tty ? "" : "\n");
    }
    std::fflush(stdout);
  }

  ma_device_uninit(&device);
  ma_context_uninit(&context);
  const auto& s = state.jitter.stats();
  double sec = double(state.captured) / kRate;
  std::printf("%s{\"seconds\":%.1f,\"sent\":%llu,\"kbps\":%.1f,\"received\":%llu,\"played\":%llu,"
              "\"recovered\":%llu,\"concealed\":%llu,\"late\":%llu}\n",
              json ? "" : "\n\n", sec, (unsigned long long)state.sent.load(),
              sec > 0 ? state.sentBytes.load() * 8.0 / 1000.0 / sec : 0.0, (unsigned long long)s.received,
              (unsigned long long)s.played, (unsigned long long)s.recovered, (unsigned long long)s.concealed,
              (unsigned long long)s.late);
  return 0;
}

// --------------------------------------------------------------------------
// selftest: a scripted conversation through the capture chain, no devices
// --------------------------------------------------------------------------

// Placement of a looped clip over [from, to) seconds of the timeline.
void place(std::vector<float>& track, const std::vector<int16_t>& clip, double from, double to) {
  size_t a = size_t(from * kRate), b = std::min(track.size(), size_t(to * kRate));
  for (size_t i = a; i < b; ++i) track[i] += clip[(i - a) % clip.size()];
}

double energy(const std::vector<int16_t>& pcm, double from, double to) {
  double sum = 0;
  for (size_t i = size_t(from * kRate); i < size_t(to * kRate) && i < pcm.size(); ++i) sum += double(pcm[i]) * pcm[i];
  return sum;
}

int cmdSelftest(int argc, char** argv) {
  if (argc < 4) throw std::runtime_error("uso: otera-voice selftest far.wav near.wav [--out carpeta] [--no-aec]");
  std::string outDir, tracePath;
  bool aec = true;
  double echoGain = 1.0, echoDelayMs = 30;
  for (int i = 4; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--out" && i + 1 < argc) outDir = argv[++i];
    else if (a == "--trace" && i + 1 < argc) tracePath = argv[++i];
    else if (a == "--echo-gain" && i + 1 < argc) echoGain = std::atof(argv[++i]);
    else if (a == "--echo-delay" && i + 1 < argc) echoDelayMs = std::atof(argv[++i]);
    else if (a == "--no-aec") aec = false;
    else throw std::runtime_error("opcion desconocida: " + a);
  }
  auto far = loadWav(argv[2]);
  auto near = loadWav(argv[3]);

  // Timeline (seconds). The far-end voice is another player coming out of the
  // speakers; the near-end one is the local player.
  struct Segment {
    const char* name;
    double from, to;
  };
  const Segment segments[] = {
      {"silence", 0, 2},       // room noise only
      {"far_learning", 2, 4},  // the canceller has not converged yet
      {"far", 4, 10},          // only the other player talks
      {"near", 10, 14},        // only the local player talks
      {"double", 14, 18},      // both at once
      {"double_hold", 18, 18.5},  // the gate hold after double talk, near silence
      {"far_after", 18.5, 22},    // did double talk make the canceller diverge?
      {"silence_end", 22, 24},
  };
  const double total = 24;
  size_t n = size_t(total * kRate);

  std::vector<float> speaker(n, 0.f), nearTrack(n, 0.f);
  place(speaker, far, 2, 10);
  place(speaker, far, 14, 22);
  place(nearTrack, near, 10, 18);

  // Echo path: device latency to the first arrival, then a decaying tail of
  // reflections. --echo-gain 0 is headphones; the default is loud laptop speakers.
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(-1, 1);
  float first = float(echoDelayMs / 1000), gain = float(echoGain);
  std::vector<std::pair<size_t, float>> taps{{size_t(first * kRate), 0.5f * gain}};
  for (int k = 0; k < 60; ++k) {
    float t = 0.002f + 0.12f * (k + 1) / 60.f;
    taps.push_back({size_t((first + t) * kRate), gain * 0.25f * std::exp(-t / 0.04f) * (u(rng) > 0 ? 1.f : -1.f)});
  }
  std::normal_distribution<float> noise(0, 30);  // about -60 dBFS
  std::vector<int16_t> mic(n), spk(n), out(n);
  for (size_t i = 0; i < n; ++i) {
    float echo = 0;
    for (auto& [d, g] : taps)
      if (i >= d) echo += g * speaker[i - d];
    float v = echo + nearTrack[i] + noise(rng);
    mic[i] = int16_t(std::clamp(v, -32768.f, 32767.f));
    spk[i] = int16_t(std::clamp(speaker[i], -32768.f, 32767.f));
  }

  CaptureProcessor capture(300, aec);
  std::vector<bool> open(n / kFrame);
  std::vector<float> prob(n / kFrame);
  FILE* trace = tracePath.empty() ? nullptr : std::fopen(tracePath.c_str(), "w");
  if (trace) std::fprintf(trace, "t,far_db,mic_db,out_db,prob,open\n");
  for (size_t f = 0; f < n / kFrame; ++f) {
    auto r = capture.process(&mic[f * kFrame], &spk[f * kFrame], &out[f * kFrame]);
    open[f] = r.open;
    prob[f] = r.probability;
    if (trace)
      std::fprintf(trace, "%.2f,%.1f,%.1f,%.1f,%.2f,%d\n", f * 0.01, levelDb(&spk[f * kFrame], kFrame), r.micDb,
                   r.outDb, r.probability, int(r.open));
  }
  if (trace) std::fclose(trace);

  // What would go on the wire, through a network with 10% loss and swapped pairs.
  Sender sender;
  std::vector<Wire> wire;
  for (size_t f = 0; f < n / kFrame; ++f)
    sender.frame(&out[f * kFrame], open[f], [&](uint16_t s, uint8_t fl, const uint8_t* d, size_t sz) {
      wire.push_back({f, s, fl, std::vector<uint8_t>(d, d + sz)});
    });
  size_t bytes = 0;
  for (auto& w : wire) bytes += w.data.size();
  std::mt19937 netRng(99);
  std::vector<Wire> delivered;
  for (size_t i = 0; i < wire.size(); ++i) {
    if (std::uniform_real_distribution<double>(0, 1)(netRng) < 0.10) continue;
    delivered.push_back(wire[i]);
    if (delivered.size() >= 2 && std::uniform_real_distribution<double>(0, 1)(netRng) < 0.10)
      std::swap(delivered[delivered.size() - 1], delivered[delivered.size() - 2]);
  }
  JitterBuffer jitter(3);
  std::vector<int16_t> heard;
  size_t next = 0;
  int16_t pcm[kPacket];
  // One pull per 20 ms of timeline; packets arrive when they were sent.
  for (size_t f = 0; f < n / kFrame + 40; f += kPacketFrames) {
    while (next < delivered.size() && delivered[next].due <= f) {
      jitter.push(delivered[next].seq, delivered[next].flags, delivered[next].data.data(), delivered[next].data.size());
      ++next;
    }
    jitter.pull(pcm);
    heard.insert(heard.end(), pcm, pcm + kPacket);
  }

  std::printf("{\n  \"echo_cancellation\": %s,\n  \"segments\": {\n", aec ? "true" : "false");
  for (size_t s = 0; s < std::size(segments); ++s) {
    const auto& seg = segments[s];
    size_t a = size_t(seg.from * kRate) / kFrame, b = size_t(seg.to * kRate) / kFrame;
    size_t opened = 0;
    double p = 0;
    for (size_t f = a; f < b; ++f) {
      opened += open[f];
      p += prob[f];
    }
    double eIn = energy(mic, seg.from, seg.to), eOut = energy(out, seg.from, seg.to);
    std::printf("    \"%s\": {\"gate_open\": %.3f, \"mean_prob\": %.3f, \"out_vs_in_db\": %.1f}%s\n", seg.name,
                double(opened) / double(b - a), p / double(b - a),
                eIn > 0 && eOut > 0 ? 10 * std::log10(eOut / eIn) : -100.0, s + 1 < std::size(segments) ? "," : "");
  }
  const auto& st = jitter.stats();
  std::printf("  },\n  \"network\": {\"packets\": %zu, \"kbps_while_talking\": %.1f, \"received\": %llu, "
              "\"played\": %llu, \"recovered\": %llu, \"concealed\": %llu, \"late\": %llu, \"dropped\": %llu}\n}\n",
              wire.size(), wire.empty() ? 0.0 : bytes * 8.0 / 1000.0 / (wire.size() * 0.02),
              (unsigned long long)st.received, (unsigned long long)st.played, (unsigned long long)st.recovered,
              (unsigned long long)st.concealed, (unsigned long long)st.late, (unsigned long long)st.dropped);

  if (!outDir.empty()) {
    saveWav(outDir + "/mic.wav", mic);
    saveWav(outDir + "/processed.wav", out);
    saveWav(outDir + "/heard.wav", heard);
  }
  return 0;
}

// --------------------------------------------------------------------------
// selftest-jitter
// --------------------------------------------------------------------------

int failures = 0;
void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

int cmdSelftestJitter() {
  Encoder enc;
  std::vector<int16_t> tone(kPacket);
  for (int i = 0; i < kPacket; ++i) tone[i] = int16_t(8000 * std::sin(2 * 3.14159265358979 * 440 * i / kRate));
  auto packet = [&]() {
    std::vector<uint8_t> b(kMaxOpusBytes);
    b.resize(size_t(enc.encode(tone.data(), b.data(), int(b.size()))));
    return b;
  };
  int16_t out[kPacket];

  {
    JitterBuffer jb(3);
    check(!jb.pull(out) && !jb.talking(), "idle hasta el primer paquete");
    for (uint16_t s = 0; s < 5; ++s) {
      auto p = packet();
      jb.push(s, s == 4 ? JitterBuffer::kEnd : 0, p.data(), p.size());
    }
    int played = 0;
    while (jb.pull(out)) ++played;
    check(played == 5 && jb.stats().played == 5, "reproduce los 5 paquetes y corta en el de fin");
    check(!jb.talking(), "el icono se apaga con el paquete de fin");
    auto p = packet();
    jb.push(3, 0, p.data(), p.size());
    check(!jb.pull(out) && jb.stats().late == 1, "un paquete viejo despues del fin se descarta");
  }
  {
    JitterBuffer jb(3);
    std::vector<uint16_t> order{0, 2, 1, 3, 5, 6, 7};  // 1 llega tarde pero a tiempo, 4 se pierde
    for (auto s : order) {
      auto p = packet();
      jb.push(s, s == 7 ? JitterBuffer::kEnd : 0, p.data(), p.size());
    }
    int pulls = 0;
    while (jb.pull(out)) ++pulls;
    const auto& st = jb.stats();
    check(pulls == 8, "8 cuadros de 20 ms para 0..7");
    check(st.played == 7 && st.recovered == 1, "el 4 perdido sale del FEC del 5");
  }
  {
    JitterBuffer jb(3);
    for (uint32_t s = 65530; s < 65530 + 10; ++s) {
      auto p = packet();
      jb.push(uint16_t(s), s == 65539 ? JitterBuffer::kEnd : 0, p.data(), p.size());
    }
    int pulls = 0;
    while (jb.pull(out)) ++pulls;
    check(pulls == 10 && jb.stats().played == 10, "la secuencia pasa por 65535 -> 0 sin cortar");
  }
  {
    JitterBuffer jb(3);
    for (uint16_t s = 0; s < 3; ++s) {
      auto p = packet();
      jb.push(s, 0, p.data(), p.size());
    }
    int pulls = 0;
    while (jb.pull(out) && pulls < 100) ++pulls;
    check(pulls == 3 + 10 && !jb.talking(), "sin paquete de fin, se rinde a los 200 ms");
  }
  {
    JitterBuffer jb(3);
    auto p = packet();
    jb.push(0, JitterBuffer::kEnd, p.data(), p.size());
    int heard = 0;
    for (int i = 0; i < 10; ++i) heard += jb.pull(out);
    check(heard == 1, "una rafaga de un solo paquete igual se escucha");
  }
  return failures ? 1 : 0;
}

}  // namespace

static int run(int argc, char** argv) {
  std::string cmd = argc > 1 ? argv[1] : "";
  try {
    if (cmd == "serve") return cmdServe(argc, argv);
    if (cmd == "devices") return cmdDevices();
    if (cmd == "loopback") return cmdLoopback(argc, argv);
    if (cmd == "selftest") return cmdSelftest(argc, argv);
    if (cmd == "selftest-jitter") return cmdSelftestJitter();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "otera-voice: %s\n", e.what());
    return 2;
  }
  std::fprintf(stderr,
               "uso: otera-voice serve <status.json> [--null-audio] [--mic-file wav] [--record wav]\n"
               "     otera-voice devices\n"
               "     otera-voice loopback [--delay ms] [--loss %%] [--jitter ms] [--sensitivity 0-1]\n"
               "                          [--mic n] [--out n] [--no-aec] [--seconds n] [--json]\n"
               "     otera-voice selftest far.wav near.wav [--out carpeta] [--trace csv] [--no-aec]\n"
               "                          [--echo-gain g] [--echo-delay ms]\n"
               "     otera-voice selftest-jitter\n");
  return 2;
}

#ifdef _WIN32
#include <windows.h>

// Arguments as UTF-8 whatever the code page: the status file lives in the install
// folder, and a user name with accents must not turn into question marks.
int wmain(int argc, wchar_t** wargv) {
  std::vector<std::string> args;
  for (int i = 0; i < argc; ++i) {
    int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string a(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, a.data(), n, nullptr, nullptr);
    args.push_back(a);
  }
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  return run(argc, argv.data());
}
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
