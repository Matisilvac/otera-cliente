// otera-updater: downloads, verifies and applies Otera releases.
//
// The game never replaces its own files. It runs this helper and reads the status
// file the helper writes:
//
//   otera-updater check <install> <status>        is there a newer signed release?
//   otera-updater stage <install> <status>        download what changed, verify it
//   otera-updater apply <install> <status> <pid>  wait for the game to exit, swap
//                                                 the files, relaunch the game
//   otera-updater version <install> <status>      identify this build
//
// Trust: a release is `otera-release.json`, a payload signed with ECDSA P-256 whose
// public key is compiled below. The payload names each platform manifest by
// SHA-256, and the manifest names every file by SHA-256, so nothing downloaded is
// used before it matches the signature chain. The origin is fixed at compile time;
// the server cannot point the helper anywhere else.
//
// Safety: files are staged by content hash under `.otera-update/`, then swapped
// with renames on the same volume. Every original goes to a backup first and a
// journal lists the swap, so a failure, a crash or a power cut rolls back to the
// previous version on the next run instead of leaving half of each.
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXSocketTLSOptions.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <nlohmann/json.hpp>
#include <psa/crypto.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h> // NOMINMAX comes from CMakeLists.txt, ahead of every header
#else
#include <cerrno>
#include <csignal>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/proc.h>
#include <sys/sysctl.h>
#endif
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// Test builds (tests/client_updater.py) swap the origin and the key for local
// ones; the automated ones also define OTERA_UPDATER_NO_LAUNCH. Shipped builds
// have none of it.
#ifdef OTERA_UPDATER_TEST_CONFIG
#include OTERA_UPDATER_TEST_CONFIG
#define OTERA_UPDATER_TESTING 1
#endif

#ifndef OTERA_UPDATER_ORIGIN
#define OTERA_UPDATER_ORIGIN "https://github.com/Matisilvac/otera-cliente/releases/"
#endif

// client/firma-actualizaciones.pub.pem
#ifndef OTERA_UPDATER_PUBLIC_KEY
#define OTERA_UPDATER_PUBLIC_KEY                                          \
    "-----BEGIN PUBLIC KEY-----\n"                                       \
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEKn9HNJPG/QWs+He295/jCGr8osXB\n" \
    "+cukBtXOD7hrYQYog84NBF98u6f2kdZeIGqAlJws8/OIcWUEH88c5EZe0g==\n"     \
    "-----END PUBLIC KEY-----\n"
#endif

namespace {

constexpr const char* ORIGIN = OTERA_UPDATER_ORIGIN;
constexpr int FORMAT = 1;
// Installer family this helper implements. A release with another family needs
// a new full package (see client/update_packaging.py, INSTALLER).
constexpr int INSTALLER = 6;
constexpr const char* STATE = ".otera-update";
constexpr std::uint64_t MAX_FILE = 4ull << 30;

constexpr const char* PUBLIC_KEY = OTERA_UPDATER_PUBLIC_KEY;

#if defined(_WIN32)
constexpr const char* PLATFORM = "windows";
#elif defined(__APPLE__)
constexpr const char* PLATFORM = "macos";
#else
constexpr const char* PLATFORM = "linux";
#endif

// ---------------------------------------------------------------------------
// Errors carry the sentence the player reads; `detail` goes to the log.

struct Failure : std::runtime_error
{
    std::string code;
    Failure(std::string code, const std::string& detail) : std::runtime_error(detail), code(std::move(code)) {}
};

std::string playerMessage(const std::string& code)
{
    static const std::map<std::string, std::string> messages = {
        {"network", "No pudimos conectar con las actualizaciones de Otera. Revisa tu conexion y pulsa Reintentar."},
        {"signature", "La actualizacion no tiene una firma valida de Otera. No se instalo nada."},
        {"integrity", "Un archivo descargado no coincide con la version publicada. No se instalo nada; pulsa Reintentar."},
        {"disk", "No hay espacio suficiente en el disco para actualizar Otera."},
        {"permissions", "Otera no puede escribir en su carpeta. Muevela a una carpeta tuya (por ejemplo Documentos) y vuelve a abrirla."},
        {"busy", "Hay otra actualizacion en curso. Cierra las demas ventanas de Otera y pulsa Reintentar."},
        {"running", "Otera sigue abierto. Cierra todas sus ventanas y vuelve a abrirlo."},
        {"installer", "Esta version necesita el instalador completo. Descargalo desde oteraot.com/downloads."},
        {"install", "La instalacion de Otera esta incompleta. Descarga el paquete completo desde oteraot.com/downloads."},
        {"apply", "No se pudo terminar la actualizacion y se restauro la version anterior. Pulsa Reintentar."},
    };
    auto it = messages.find(code);
    return it != messages.end() ? it->second : "La actualizacion fallo. Pulsa Reintentar.";
}

std::string utf8(const fs::path& path)
{
    auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// Forward slashes, UTF-8: the form manifest paths use on every system. Going
// through std::string -> fs::path would use the ANSI code page on Windows.
std::string genericUtf8(const fs::path& path)
{
    auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

fs::path fromUtf8(const std::string& text)
{
    return fs::u8path(text);
}

// Missing keys read as null: operator[] on a const json is undefined behavior.
const json& field(const json& object, const std::string& key)
{
    static const json missing;
    if (!object.is_object()) return missing;
    const auto it = object.find(key);
    return it == object.end() ? missing : *it;
}

// ---------------------------------------------------------------------------
// Files

std::string readFile(const fs::path& path, std::uint64_t limit)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw Failure("install", "cannot read " + utf8(path));
    std::string data;
    char buffer[1 << 16];
    while (file.read(buffer, sizeof buffer) || file.gcount()) {
        data.append(buffer, static_cast<size_t>(file.gcount()));
        if (data.size() > limit) throw Failure("integrity", utf8(path) + " is larger than allowed");
    }
    return data;
}

void writeAtomic(const fs::path& path, const std::string& data)
{
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        file.flush();
        if (!file) throw Failure("permissions", "cannot write " + utf8(temporary));
    }
    std::error_code error;
    fs::rename(temporary, path, error);
    if (error) throw Failure("permissions", "cannot replace " + utf8(path) + ": " + error.message());
}

std::string hex(const unsigned char* data, size_t size)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 15]);
    }
    return out;
}

class Sha256
{
public:
    Sha256() { mbedtls_sha256_init(&m_context); mbedtls_sha256_starts(&m_context, 0); }
    ~Sha256() { mbedtls_sha256_free(&m_context); }
    void update(const void* data, size_t size) { mbedtls_sha256_update(&m_context, static_cast<const unsigned char*>(data), size); }
    std::string finish()
    {
        unsigned char digest[32];
        mbedtls_sha256_finish(&m_context, digest);
        return hex(digest, sizeof digest);
    }

private:
    mbedtls_sha256_context m_context;
};

std::string sha256(const std::string& data)
{
    Sha256 hash;
    hash.update(data.data(), data.size());
    return hash.finish();
}

// Returns an empty string when the file is missing or not a regular file.
std::string sha256File(const fs::path& path, const std::function<void(std::uint64_t)>& progress = {})
{
    std::error_code error;
    if (!fs::is_regular_file(path, error)) return {};
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    Sha256 hash;
    std::vector<char> buffer(1 << 20);
    while (file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || file.gcount()) {
        hash.update(buffer.data(), static_cast<size_t>(file.gcount()));
        if (progress) progress(static_cast<std::uint64_t>(file.gcount()));
    }
    return hash.finish();
}

void renameRetry(const fs::path& from, const fs::path& to)
{
    // Antivirus scanners and indexers hold freshly written files for a moment on
    // Windows; a rename that fails with a sharing violation succeeds shortly after.
    std::error_code error;
    for (int attempt = 0; attempt < 40; ++attempt) {
        fs::rename(from, to, error);
        if (!error) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    throw Failure("apply", "cannot move " + utf8(from) + " to " + utf8(to) + ": " + error.message());
}

void setExecutable(const fs::path& path, bool executable)
{
#ifndef _WIN32
    if (chmod(path.c_str(), executable ? 0755 : 0644) != 0)
        throw Failure("apply", "cannot set permissions on " + utf8(path));
#else
    (void)path;
    (void)executable;
#endif
}

// ---------------------------------------------------------------------------
// Processes

bool processAlive(long pid)
{
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process) return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
#else
    if (kill(static_cast<pid_t>(pid), 0) != 0 && errno != EPERM) return false;
    // An exited game whose parent has not reaped it yet is a zombie: it holds no
    // files, but kill() still finds it. Launchers and shells do not always reap.
#ifdef __APPLE__
    int query[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
    kinfo_proc info{};
    size_t size = sizeof info;
    if (sysctl(query, 4, &info, &size, nullptr, 0) != 0) return true;
    return size != 0 && info.kp_proc.p_stat != SZOMB;
#else
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(stat, line)) return true;
    const auto close = line.rfind(')');
    return close == std::string::npos || close + 2 >= line.size() || line[close + 2] != 'Z';
#endif
#endif
}

long currentPid()
{
#ifdef _WIN32
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(getpid());
#endif
}

void writeAtomic(const fs::path& path, const std::string& data);

void launch(const fs::path& install, const std::string& target)
{
    const fs::path path = install / fromUtf8(target);
#ifdef OTERA_UPDATER_NO_LAUNCH
    writeAtomic(install / STATE / "launched", target);
    return;
#endif
#ifdef _WIN32
    std::wstring command = L"\"" + path.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        install.c_str(), &startup, &process))
        throw Failure("install", "cannot start " + utf8(path));
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    const pid_t child = fork();
    if (child < 0) throw Failure("install", "cannot start " + utf8(path));
    if (child == 0) {
        setsid();
        if (chdir(install.c_str()) != 0) _exit(127);
#ifdef __APPLE__
        // LaunchServices, not the bare executable: the app gets its Dock icon,
        // its bundle identity and the same environment as a double click.
        execl("/usr/bin/open", "open", "-n", path.c_str(), static_cast<char*>(nullptr));
#else
        execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
#endif
        _exit(127);
    }
#endif
}

// ---------------------------------------------------------------------------
// Status file read by the game, and the log

class Log
{
public:
    explicit Log(const fs::path& state) : m_path(state / "updater.log")
    {
        std::error_code error;
        if (fs::file_size(m_path, error) > (1u << 20)) fs::remove(m_path, error);
    }
    void write(const std::string& line) const
    {
        std::ofstream file(m_path, std::ios::app);
        const std::time_t now = std::time(nullptr);
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", std::gmtime(&now));
        file << stamp << "Z [" << currentPid() << "] " << line << '\n';
    }

private:
    fs::path m_path;
};

class Status
{
public:
    Status(fs::path path, std::string command) : m_path(std::move(path)), m_command(std::move(command)) {}

    void progress(const std::string& phase, std::uint64_t done, std::uint64_t total, bool force = false)
    {
        const auto now = Clock::now();
        if (!force && now - m_last < std::chrono::milliseconds(200)) return;
        m_last = now;
        write({{"state", "working"}, {"phase", phase}, {"done", done}, {"total", total}});
    }
    void done(json result)
    {
        result["state"] = "done";
        write(std::move(result));
    }
    void fail(const Failure& failure)
    {
        write({{"state", "error"}, {"code", failure.code}, {"message", playerMessage(failure.code)},
               {"detail", std::string(failure.what()).substr(0, 400)}});
    }

private:
    void write(json value)
    {
        value["command"] = m_command;
        value["pid"] = currentPid();
        try { writeAtomic(m_path, value.dump()); } catch (...) {}
    }

    fs::path m_path;
    std::string m_command;
    Clock::time_point m_last{};
};

// One helper at a time per installation.
class Lock
{
public:
    explicit Lock(const fs::path& state) : m_path(state / "lock")
    {
        for (int attempt = 0; attempt < 2; ++attempt) {
#ifdef _WIN32
            FILE* file = _wfopen(m_path.c_str(), L"wx");
#else
            FILE* file = std::fopen(m_path.c_str(), "wx");
#endif
            if (file) {
                std::fprintf(file, "%ld", currentPid());
                std::fclose(file);
                return;
            }
            long owner = 0;
            try { owner = std::stol(readFile(m_path, 64)); } catch (...) {}
            if (owner != currentPid() && processAlive(owner)) throw Failure("busy", "updater already running as " + std::to_string(owner));
            std::error_code error;
            fs::remove(m_path, error);
        }
        throw Failure("permissions", "cannot create " + utf8(m_path));
    }
    ~Lock()
    {
        std::error_code error;
        fs::remove(m_path, error);
    }

private:
    fs::path m_path;
};

// ---------------------------------------------------------------------------
// Network: HTTPS only, pinned CA bundle, fixed origin, streamed to disk

class Net
{
public:
    explicit Net(const fs::path& install)
    {
        m_tls.caFile = readFile(install / "cacert.pem", 4u << 20); // in memory: Unicode paths work
        if (m_tls.caFile.find("-----BEGIN CERTIFICATE-----") == std::string::npos)
            throw Failure("install", "missing cacert.pem");
    }

    std::string fetch(const std::string& url, std::uint64_t limit, const fs::path& scratch)
    {
        get(url, 0, 0, scratch, limit, {});
        auto data = readFile(scratch, limit);
        std::error_code error;
        fs::remove(scratch, error);
        return data;
    }

    // Bytes [first, last] of `url` into `out`.
    void range(const std::string& url, std::uint64_t first, std::uint64_t last, const fs::path& out,
               const std::function<void(std::uint64_t)>& progress)
    {
        get(url, first, last + 1, out, last - first + 1, progress);
        std::error_code error;
        if (fs::file_size(out, error) != last - first + 1)
            throw Failure("network", "short range from " + url);
    }

private:
    void get(std::string url, std::uint64_t first, std::uint64_t end, const fs::path& out, std::uint64_t limit,
             const std::function<void(std::uint64_t)>& progress)
    {
        if (url.rfind(ORIGIN, 0) != 0) throw Failure("signature", "URL outside the official origin: " + url);
        const bool ranged = end > 0;
        for (int hop = 0; hop < 8; ++hop) {
            if (url.rfind("https://", 0) != 0) throw Failure("network", "redirect away from HTTPS");
            std::ofstream file(out, std::ios::binary | std::ios::trunc);
            if (!file) throw Failure("permissions", "cannot write " + utf8(out));

            ix::HttpClient client;
            client.setTLSOptions(m_tls);
            auto args = client.createRequest(url, "GET");
            args->followRedirects = false;
            args->compress = false;
            args->connectTimeout = 15;
            args->transferTimeout = 6 * 3600;
            args->extraHeaders["User-Agent"] = "Otera-Updater/1";
            args->extraHeaders["Accept-Encoding"] = "identity";
            if (ranged) args->extraHeaders["Range"] = "bytes=" + std::to_string(first) + "-" + std::to_string(end - 1);

            std::atomic<std::uint64_t> written{0};
            std::atomic<bool> overflow{false};
            std::atomic<Clock::rep> lastByte{Clock::now().time_since_epoch().count()};
            args->onChunkCallback = [&](const std::string& chunk) {
                file.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                written += chunk.size();
                lastByte = Clock::now().time_since_epoch().count();
                if (written > limit) { overflow = true; args->cancel = true; }
                if (progress) progress(chunk.size());
            };
            // A stalled connection never errors on its own: cancel after a minute
            // without a single byte, and let the player retry.
            std::atomic<bool> finished{false};
            std::thread watchdog([&] {
                while (!finished) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    const auto idle = Clock::now() - Clock::time_point(Clock::duration(lastByte.load()));
                    if (idle > std::chrono::seconds(60)) { args->cancel = true; return; }
                }
            });
            auto response = client.get(url, args);
            finished = true;
            watchdog.join();
            file.close();

            if (overflow) throw Failure("integrity", url + " is larger than expected");
            if (!response || response->errorCode != ix::HttpErrorCode::Ok)
                throw Failure("network", url + ": " + (response ? response->errorMsg : std::string("no response")));
            if (response->statusCode >= 300 && response->statusCode <= 399) {
                auto location = response->headers.find("Location");
                if (location == response->headers.end()) throw Failure("network", "redirect without Location");
                // RFC 9110 allows a Location relative to the current host.
                if (location->second.rfind("/", 0) == 0 && location->second.rfind("//", 0) != 0)
                    url = url.substr(0, url.find('/', url.find("://") + 3)) + location->second;
                else
                    url = location->second;
                continue;
            }
            if (ranged) {
                if (response->statusCode != 206) throw Failure("network", "range not honored: HTTP " + std::to_string(response->statusCode));
                auto contentRange = response->headers.find("Content-Range");
                const std::string expected = "bytes " + std::to_string(first) + "-" + std::to_string(end - 1) + "/";
                if (contentRange == response->headers.end() || contentRange->second.rfind(expected, 0) != 0)
                    throw Failure("network", "unexpected Content-Range");
            } else if (response->statusCode != 200) {
                throw Failure("network", url + ": HTTP " + std::to_string(response->statusCode));
            }
            if (!file) throw Failure("disk", "cannot write " + utf8(out));
            return;
        }
        throw Failure("network", "too many redirects");
    }

    ix::SocketTLSOptions m_tls;
};

// ---------------------------------------------------------------------------
// Signed metadata

std::string base64Decode(const std::string& text)
{
    size_t size = 0;
    mbedtls_base64_decode(nullptr, 0, &size, reinterpret_cast<const unsigned char*>(text.data()), text.size());
    std::string out(size, '\0');
    if (mbedtls_base64_decode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &size,
                              reinterpret_cast<const unsigned char*>(text.data()), text.size()) != 0)
        throw Failure("signature", "invalid base64");
    out.resize(size);
    return out;
}

// The payload of a signed envelope, only if our key signed it.
json verifyEnvelope(const std::string& text)
{
    json envelope = json::parse(text, nullptr, false);
    if (!envelope.is_object() || envelope.value("format", "") != "otera-signed/1" || !field(envelope, "payload").is_string()
        || !field(envelope, "signatures").is_array())
        throw Failure("signature", "not a signed envelope");
    const std::string payload = base64Decode(field(envelope, "payload").get<std::string>());
    unsigned char digest[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest, 0);

    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    const int parsed = mbedtls_pk_parse_public_key(&key, reinterpret_cast<const unsigned char*>(PUBLIC_KEY),
                                                   std::strlen(PUBLIC_KEY) + 1);
    bool valid = false;
    for (const auto& entry : field(envelope, "signatures")) {
        if (parsed != 0 || !entry.is_object() || entry.value("algorithm", "") != "ecdsa-p256-sha256" || !field(entry, "signature").is_string())
            continue;
        const std::string signature = base64Decode(field(entry, "signature").get<std::string>());
        if (mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, sizeof digest,
                              reinterpret_cast<const unsigned char*>(signature.data()), signature.size()) == 0) {
            valid = true;
            break;
        }
    }
    mbedtls_pk_free(&key);
    if (!valid) throw Failure("signature", "release signature does not verify");
    json value = json::parse(payload, nullptr, false);
    if (!value.is_object()) throw Failure("signature", "signed payload is not JSON");
    return value;
}

bool isHex64(const json& value)
{
    static const std::regex pattern("^[0-9a-f]{64}$");
    return value.is_string() && std::regex_match(value.get<std::string>(), pattern);
}

bool isCount(const json& value, std::uint64_t max)
{
    return value.is_number_unsigned() && value.get<std::uint64_t>() <= max;
}

bool validPath(const std::string& path)
{
    if (path.empty() || path.size() > 400) return false;
    for (unsigned char c : path)
        if (c < 32 || c == 127 || std::strchr("\\:*?\"<>|", c)) return false;
    size_t start = 0;
    bool first = true;
    while (true) {
        const size_t slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == ".." || part.front() == ' ' || part.back() == ' ' || part.back() == '.')
            return false;
        if (first && part == STATE) return false;
        first = false;
        if (slash == std::string::npos) return true;
        start = slash + 1;
    }
}

std::string folded(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

struct Release
{
    std::string version;
    std::int64_t generation = 0;
    std::int64_t installer = 0;
    std::string manifestName, manifestSha;
    std::uint64_t manifestSize = 0;
    std::string envelope;
};

Release parseRelease(const std::string& envelope)
{
    const json payload = verifyEnvelope(envelope);
    static const std::regex version("^[0-9]+\\.[0-9]+\\.[0-9]+$");
    static const std::regex name("^otera-manifest-[a-z]+-g[0-9]+\\.json$");
    if (payload.value("type", "") != "otera-release" || payload.value("format", 0) != FORMAT
        || !field(payload, "version").is_string() || !std::regex_match(field(payload, "version").get<std::string>(), version)
        || !field(payload, "generation").is_number_integer() || !field(payload, "installer").is_number_integer()
        || !field(payload, "manifests").is_object())
        throw Failure("signature", "malformed release payload");
    Release release;
    release.version = field(payload, "version").get<std::string>();
    release.generation = field(payload, "generation").get<std::int64_t>();
    release.installer = field(payload, "installer").get<std::int64_t>();
    release.envelope = envelope;
    if (release.installer == INSTALLER) {
        const json& entry = field(field(payload, "manifests"), PLATFORM);
        if (!field(entry, "name").is_string() || !std::regex_match(field(entry, "name").get<std::string>(), name)
            || !isHex64(field(entry, "sha256")) || !isCount(field(entry, "size"), 64u << 20))
            throw Failure("signature", std::string("no manifest for ") + PLATFORM);
        release.manifestName = field(entry, "name").get<std::string>();
        release.manifestSha = field(entry, "sha256").get<std::string>();
        release.manifestSize = field(entry, "size").get<std::uint64_t>();
    }
    return release;
}

struct FileEntry
{
    std::string path, sha;
    std::uint64_t size = 0;
    bool exec = false;
};

struct Blob
{
    std::string pack;
    std::uint64_t offset = 0, length = 0;
    bool deflate = false;
};

struct Pack
{
    std::string tag, name;
    std::uint64_t size = 0;
};

struct Manifest
{
    std::string launch;
    std::vector<FileEntry> files;
    std::map<std::string, Pack> packs;
    std::map<std::string, Blob> blobs;
};

Manifest parseManifest(const std::string& data, const Release& release)
{
    if (data.size() != release.manifestSize || sha256(data) != release.manifestSha)
        throw Failure("integrity", "manifest does not match the signed release");
    const json value = json::parse(data, nullptr, false);
    if (!value.is_object() || value.value("type", "") != "otera-manifest" || value.value("format", 0) != FORMAT
        || value.value("platform", "") != PLATFORM || value.value("version", "") != release.version
        || value.value("generation", std::int64_t(-1)) != release.generation || !field(value, "launch").is_string()
        || !field(value, "files").is_array() || !field(value, "packs").is_object() || !field(value, "blobs").is_object())
        throw Failure("integrity", "malformed manifest");
    static const std::regex tag("^v[0-9]+\\.[0-9]+\\.[0-9]+$");
    static const std::regex packName("^otera-pack-g[0-9]+-[0-9]+\\.bin$");
    Manifest manifest;
    manifest.launch = field(value, "launch").get<std::string>();
    if (!validPath(manifest.launch)) throw Failure("integrity", "invalid launch target");
    for (const auto& [id, entry] : field(value, "packs").items()) {
        if (!field(entry, "tag").is_string() || !std::regex_match(field(entry, "tag").get<std::string>(), tag)
            || !field(entry, "name").is_string() || !std::regex_match(field(entry, "name").get<std::string>(), packName)
            || !isCount(field(entry, "size"), 2ull << 30))
            throw Failure("integrity", "invalid pack " + id);
        manifest.packs[id] = {field(entry, "tag").get<std::string>(), field(entry, "name").get<std::string>(),
                              field(entry, "size").get<std::uint64_t>()};
    }
    for (const auto& [digest, entry] : field(value, "blobs").items()) {
        if (!isHex64(json(digest)) || !field(entry, "pack").is_string()
            || !manifest.packs.count(field(entry, "pack").get<std::string>()) || !isCount(field(entry, "offset"), 2ull << 30)
            || !isCount(field(entry, "length"), MAX_FILE) || !field(entry, "encoding").is_string())
            throw Failure("integrity", "invalid blob " + digest);
        const std::string encoding = field(entry, "encoding").get<std::string>();
        Blob blob{field(entry, "pack").get<std::string>(), field(entry, "offset").get<std::uint64_t>(),
                  field(entry, "length").get<std::uint64_t>(), encoding == "deflate"};
        if (!blob.deflate && encoding != "identity") throw Failure("integrity", "unknown encoding");
        if (blob.offset + blob.length > manifest.packs[blob.pack].size) throw Failure("integrity", "blob outside its pack");
        manifest.blobs[digest] = blob;
    }
    std::set<std::string> seen;
    for (const auto& entry : field(value, "files")) {
        if (!field(entry, "path").is_string() || !isHex64(field(entry, "sha256"))
            || !isCount(field(entry, "size"), MAX_FILE) || !field(entry, "exec").is_boolean())
            throw Failure("integrity", "invalid file entry");
        FileEntry file{field(entry, "path").get<std::string>(), field(entry, "sha256").get<std::string>(),
                       field(entry, "size").get<std::uint64_t>(), field(entry, "exec").get<bool>()};
        if (!validPath(file.path) || !seen.insert(folded(file.path)).second)
            throw Failure("integrity", "invalid or duplicate path " + file.path);
        if (!manifest.blobs.count(file.sha)) throw Failure("integrity", "no blob for " + file.path);
        manifest.files.push_back(std::move(file));
    }
    if (!seen.count(folded("otera-version.json"))) throw Failure("integrity", "manifest without otera-version.json");
    return manifest;
}

// ---------------------------------------------------------------------------
// The installation

struct Installed
{
    std::string version;
    std::int64_t generation = 0;
    std::vector<std::string> files;
};

Installed readInstalled(const fs::path& install)
{
    Installed installed;
    const json version = json::parse(readFile(install / "otera-version.json", 1 << 16), nullptr, false);
    if (!field(version, "version").is_string() || !field(version, "generation").is_number_integer())
        throw Failure("install", "invalid otera-version.json");
    installed.version = field(version, "version").get<std::string>();
    installed.generation = field(version, "generation").get<std::int64_t>();
    std::error_code error;
    if (fs::exists(install / "otera-installed.json", error)) {
        const json list = json::parse(readFile(install / "otera-installed.json", 16u << 20), nullptr, false);
        if (field(list, "files").is_array())
            for (const auto& path : field(list, "files"))
                if (path.is_string() && validPath(path.get<std::string>())) installed.files.push_back(path.get<std::string>());
    }
    return installed;
}

class Updater
{
public:
    Updater(fs::path install, Status& status)
        : m_install(std::move(install)), m_state(m_install / STATE), m_status(status), m_log((prepare(m_install / STATE), m_state))
    {
    }

    Release fetchRelease()
    {
        Net net(m_install);
        const std::string envelope = net.fetch(std::string(ORIGIN) + "latest/download/otera-release.json", 256u << 10,
                                               m_state / "release.download");
        auto release = parseRelease(envelope);
        m_log.write("release " + release.version + " generation " + std::to_string(release.generation));
        return release;
    }

    json check(const fs::path& ownStatus)
    {
        recover();
        // Status files of commands the game did not wait for (apply exits the game).
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(m_state, error)) {
            const std::string name = genericUtf8(entry.path().filename());
            if (name.rfind("status-", 0) == 0 && !fs::equivalent(entry.path(), ownStatus, error))
                fs::remove(entry.path(), error);
        }
        const Installed installed = readInstalled(m_install);
        const Release release = fetchRelease();
        return {{"current", installed.version},
                {"version", release.version},
                {"generation", release.generation},
                {"update", release.generation > installed.generation},
                {"compatible", release.installer == INSTALLER}};
    }

    json stage()
    {
        recover();
        const Installed installed = readInstalled(m_install);
        const Release release = fetchRelease();
        if (release.generation <= installed.generation) return {{"staged", false}, {"version", installed.version}};
        if (release.installer != INSTALLER) throw Failure("installer", "release needs installer family " + std::to_string(release.installer));

        Net net(m_install);
        const std::string manifestData = net.fetch(std::string(ORIGIN) + "download/v" + release.version + "/" + release.manifestName,
                                                   release.manifestSize, m_state / "manifest.download");
        const Manifest manifest = parseManifest(manifestData, release);

        // Which contents are missing locally. Hashing the installation instead of
        // trusting a record also repairs files that were damaged or edited.
        std::uint64_t totalBytes = 0, hashed = 0;
        for (const auto& file : manifest.files) totalBytes += file.size;
        std::set<std::string> needed;
        std::map<std::string, std::uint64_t> sizes;
        for (const auto& file : manifest.files) sizes[file.sha] = file.size;
        const auto names = listing();
        for (const auto& file : manifest.files) {
            const std::string local = sha256File(m_install / fromUtf8(file.path), [&](std::uint64_t n) {
                hashed += n;
                m_status.progress("verify", hashed, totalBytes);
            });
            if ((local != file.sha || wrongCase(names, file.path)) && !staged(file.sha)) needed.insert(file.sha);
        }

        // Coalesce the missing blobs of each pack into few range requests.
        struct Span { std::string pack; std::uint64_t first, last; std::vector<std::string> blobs; };
        std::map<std::string, std::vector<std::pair<std::uint64_t, std::string>>> byPack;
        for (const auto& digest : needed) byPack[manifest.blobs.at(digest).pack].push_back({manifest.blobs.at(digest).offset, digest});
        std::vector<Span> spans;
        std::uint64_t download = 0, unpacked = 0;
        for (auto& [pack, list] : byPack) {
            std::sort(list.begin(), list.end());
            for (const auto& [offset, digest] : list) {
                const Blob& blob = manifest.blobs.at(digest);
                const std::uint64_t last = blob.offset + blob.length - 1;
                // Merge across small gaps only: a few KB of waste saves a request,
                // a large gap would download files that did not change.
                if (!spans.empty() && spans.back().pack == pack && offset <= spans.back().last + 1 + (64u << 10)
                    && last - spans.back().first < (128u << 20)) {
                    spans.back().last = std::max(spans.back().last, last);
                } else {
                    spans.push_back({pack, blob.offset, last, {}});
                }
                spans.back().blobs.push_back(digest);
            }
        }
        for (const auto& span : spans) download += span.last - span.first + 1;
        for (const auto& digest : needed) unpacked += sizes.at(digest);

        std::error_code error;
        const auto space = fs::space(m_install, error);
        if (!error && space.available < download + unpacked + (64u << 20))
            throw Failure("disk", "needs " + std::to_string(download + unpacked) + " bytes");

        m_log.write("staging " + std::to_string(needed.size()) + " blobs, " + std::to_string(download) + " bytes in "
                    + std::to_string(spans.size()) + " ranges");
        std::uint64_t received = 0;
        m_status.progress("download", 0, download, true);
        fs::create_directories(m_state / "blobs");
        for (const auto& span : spans) {
            const Pack& pack = manifest.packs.at(span.pack);
            const fs::path part = m_state / "range.part";
            net.range(std::string(ORIGIN) + "download/" + pack.tag + "/" + pack.name, span.first, span.last, part,
                      [&](std::uint64_t n) {
                          received += n;
                          m_status.progress("download", received, download);
                      });
            for (const auto& digest : span.blobs) {
                const Blob& blob = manifest.blobs.at(digest);
                extract(part, blob.offset - span.first, blob, digest, sizes.at(digest));
            }
            fs::remove(part, error);
        }

        writeAtomic(m_state / "release.json", release.envelope);
        writeAtomic(m_state / "manifest.json", manifestData);
        m_log.write("staged " + release.version);
        return {{"staged", true}, {"version", release.version}, {"downloaded", download}};
    }

    json apply(long gamePid)
    {
        recover();
        // The game exits right after starting us; give it time to release its files.
        const auto deadline = Clock::now() + std::chrono::seconds(60);
        while (processAlive(gamePid)) {
            if (Clock::now() > deadline) throw Failure("running", "game process " + std::to_string(gamePid) + " did not exit");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        const Installed installed = readInstalled(m_install);
        const Release release = parseRelease(readFile(m_state / "release.json", 256u << 10));
        if (release.installer != INSTALLER || release.generation <= installed.generation)
            throw Failure("install", "staged release is not newer than the installation");
        const Manifest manifest = parseManifest(readFile(m_state / "manifest.json", release.manifestSize), release);

        json ops = json::array();
        std::set<std::string> owned;
        std::map<std::string, std::string> ownedFolded;
        for (const auto& file : manifest.files) {
            owned.insert(file.path);
            ownedFolded[folded(file.path)] = file.path;
        }
        const auto names = listing();
        for (const auto& file : manifest.files) {
            const fs::path target = m_install / fromUtf8(file.path);
            if (sha256File(target) == file.sha && !wrongCase(names, file.path)) continue;
            if (!staged(file.sha)) throw Failure("integrity", "blob for " + file.path + " is not staged");
            std::error_code error;
            ops.push_back({{"path", file.path}, {"sha256", file.sha}, {"exec", file.exec},
                           {"created", !fs::exists(target, error)}, {"remove", false}});
        }
        for (const auto& path : installed.files) {
            std::error_code error;
            const fs::path old = m_install / fromUtf8(path);
            if (owned.count(path) || !fs::exists(old, error)) continue;
            // Renamed only by case: on a case-insensitive disk it is the same file
            // as the new name, which the replacement above already handles.
            const auto renamed = ownedFolded.find(folded(path));
            if (renamed != ownedFolded.end() && fs::equivalent(old, m_install / fromUtf8(renamed->second), error)) continue;
            ops.push_back({{"path", path}, {"created", false}, {"remove", true}});
        }
        // The helper, the executable and the version marker go last: until they
        // move, a crash leaves a game that still starts and rolls the rest back.
        auto rank = [&](const json& op) {
            const std::string path = field(op, "path").get<std::string>();
            if (path == "otera-version.json") return 3;
            if (path == manifest.launch || path.rfind(manifest.launch + "/", 0) == 0) return 2;
            if (path.rfind("bin/otera-updater", 0) == 0) return 1;
            return 0;
        };
        std::stable_sort(ops.begin(), ops.end(), [&](const json& a, const json& b) { return rank(a) < rank(b); });

        json list = json::array();
        for (const auto& file : manifest.files) list.push_back(file.path);
        const std::string installedList = json({{"format", FORMAT}, {"version", release.version},
                                                {"generation", release.generation}, {"files", list}}).dump(1);

        writeAtomic(m_state / "journal.json", json({{"state", "applying"}, {"version", release.version}, {"ops", ops}}).dump());
        m_log.write("applying " + std::to_string(ops.size()) + " changes for " + release.version);
        try {
            std::size_t done = 0;
            for (const auto& op : ops) {
                const std::string path = field(op, "path").get<std::string>();
                const fs::path target = m_install / fromUtf8(path);
                const fs::path backup = m_state / "backup" / fromUtf8(path);
                std::error_code error;
                if (field(op, "remove").get<bool>()) {
                    fs::create_directories(backup.parent_path());
                    renameRetry(target, backup);
                } else {
                    fs::create_directories(target.parent_path());
                    fs::path incoming = target;
                    incoming += ".otera-new";
                    if (!fs::copy_file(m_state / "blobs" / field(op, "sha256").get<std::string>(), incoming,
                                       fs::copy_options::overwrite_existing, error))
                        throw Failure(error == std::errc::no_space_on_device ? "disk" : "apply",
                                      "cannot copy " + path + ": " + error.message());
                    setExecutable(incoming, field(op, "exec").get<bool>());
                    if (!field(op, "created").get<bool>()) {
                        fs::create_directories(backup.parent_path());
                        renameRetry(target, backup);
                    }
                    renameRetry(incoming, target);
                }
                m_status.progress("apply", ++done, ops.size());
#ifdef OTERA_UPDATER_TESTING
                // Fault injection: fail (rolled back here) or die (rolled back on
                // the next run) after N operations.
                if (const char* n = std::getenv("OTERA_TEST_FAIL_AFTER"); n && done == std::strtoul(n, nullptr, 10))
                    throw Failure("apply", "injected failure");
                if (const char* n = std::getenv("OTERA_TEST_CRASH_AFTER"); n && done == std::strtoul(n, nullptr, 10))
                    std::_Exit(3);
#endif
            }
            writeAtomic(m_install / "otera-installed.json", installedList);
        } catch (const std::exception& error) {
            // Any failure, including filesystem exceptions, undoes the whole swap.
            m_log.write(std::string("apply failed: ") + error.what());
            rollback();
            const auto* failure = dynamic_cast<const Failure*>(&error);
            throw Failure(failure && failure->code == "disk" ? "disk" : "apply", error.what());
        }

        writeAtomic(m_state / "journal.json", json({{"state", "committed"}, {"version", release.version}}).dump());
        finish();
        m_log.write("applied " + release.version);
        return {{"applied", true}, {"version", release.version}, {"launch", manifest.launch}};
    }

    std::string launchTarget() const
    {
        try {
            const Release release = parseRelease(readFile(m_state / "release.json", 256u << 10));
            return parseManifest(readFile(m_state / "manifest.json", release.manifestSize), release).launch;
        } catch (...) {
        }
        return std::string(PLATFORM) == "windows" ? "Otera.exe" : std::string(PLATFORM) == "macos" ? "Otera.app" : "Otera";
    }

    // Finish or undo an interrupted swap. Safe to run any number of times.
    void recover()
    {
        std::error_code error;
        if (!fs::exists(m_state / "journal.json", error)) return;
        const json journal = json::parse(readFile(m_state / "journal.json", 64u << 20), nullptr, false);
        if (journal.is_object() && journal.value("state", "") == "committed") {
            finish();
            return;
        }
        m_log.write("rolling back an interrupted update");
        rollback();
    }

private:
    static void prepare(const fs::path& state)
    {
        std::error_code error;
        fs::create_directories(state, error);
        if (error) throw Failure("permissions", "cannot create " + utf8(state));
    }

    // Every file name as the disk spells it. On case-insensitive disks (macOS,
    // Windows) `exists("case.txt")` finds "Case.txt"; a rename only by case must
    // still be applied, so names are compared against the real directory entries.
    std::set<std::string> listing() const
    {
        std::set<std::string> names;
        std::error_code error;
        for (auto it = fs::recursive_directory_iterator(m_install, error); !error && it != fs::recursive_directory_iterator();
             it.increment(error)) {
            const std::string rel = genericUtf8(it->path().lexically_relative(m_install));
            if (rel.rfind(STATE, 0) == 0) { it.disable_recursion_pending(); continue; }
            names.insert(rel);
        }
        return names;
    }

    bool wrongCase(const std::set<std::string>& names, const std::string& path) const
    {
        std::error_code error;
        return !names.count(path) && fs::exists(m_install / fromUtf8(path), error);
    }

    bool staged(const std::string& digest)
    {
        const fs::path path = m_state / "blobs" / digest;
        std::error_code error;
        if (!fs::exists(path, error)) return false;
        if (sha256File(path) == digest) return true;
        fs::remove(path, error);
        return false;
    }

    // Carve one blob out of a downloaded range, inflate it, check its hash.
    void extract(const fs::path& part, std::uint64_t offset, const Blob& blob, const std::string& digest, std::uint64_t size)
    {
        std::ifstream in(part, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(offset));
        const fs::path incoming = m_state / "blobs" / (digest + ".part");
        std::ofstream out(incoming, std::ios::binary | std::ios::trunc);
        if (!in || !out) throw Failure("disk", "cannot stage " + digest);
        Sha256 hash;
        std::uint64_t produced = 0;
        std::vector<char> input(1 << 16), output(1 << 16);
        auto emit = [&](const char* data, size_t n) {
            produced += n;
            if (produced > size) throw Failure("integrity", "blob " + digest + " is larger than its file");
            hash.update(data, n);
            out.write(data, static_cast<std::streamsize>(n));
        };
        std::uint64_t remaining = blob.length;
        z_stream z{};
        if (blob.deflate && inflateInit(&z) != Z_OK) throw Failure("integrity", "zlib init failed");
        int result = Z_OK;
        while (remaining > 0) {
            const auto take = static_cast<size_t>(std::min<std::uint64_t>(remaining, input.size()));
            in.read(input.data(), static_cast<std::streamsize>(take));
            if (static_cast<size_t>(in.gcount()) != take) throw Failure("integrity", "truncated range");
            remaining -= take;
            if (!blob.deflate) { emit(input.data(), take); continue; }
            z.next_in = reinterpret_cast<Bytef*>(input.data());
            z.avail_in = static_cast<uInt>(take);
            while (z.avail_in > 0 && result != Z_STREAM_END) {
                z.next_out = reinterpret_cast<Bytef*>(output.data());
                z.avail_out = static_cast<uInt>(output.size());
                result = inflate(&z, Z_NO_FLUSH);
                if (result != Z_OK && result != Z_STREAM_END) { inflateEnd(&z); throw Failure("integrity", "corrupt blob " + digest); }
                emit(output.data(), output.size() - z.avail_out);
            }
        }
        if (blob.deflate) {
            inflateEnd(&z);
            if (result != Z_STREAM_END) throw Failure("integrity", "incomplete blob " + digest);
        }
        out.close();
        if (!out) throw Failure("disk", "cannot write blob " + digest);
        if (produced != size || hash.finish() != digest) {
            std::error_code error;
            fs::remove(incoming, error);
            throw Failure("integrity", "blob " + digest + " does not match");
        }
        renameRetry(incoming, m_state / "blobs" / digest);
    }

    void rollback()
    {
        std::error_code error;
        const json journal = json::parse(readFile(m_state / "journal.json", 64u << 20), nullptr, false);
        const json& ops = field(journal, "ops");
        if (ops.is_array()) {
            for (auto it = ops.rbegin(); it != ops.rend(); ++it) {
                const json& path = field(*it, "path");
                if (!path.is_string() || !validPath(path.get<std::string>())) continue;
                const fs::path target = m_install / fromUtf8(path.get<std::string>());
                const fs::path backup = m_state / "backup" / fromUtf8(path.get<std::string>());
                fs::path incoming = target;
                incoming += ".otera-new";
                fs::remove(incoming, error);
                if (fs::exists(backup, error)) {
                    fs::create_directories(target.parent_path(), error);
                    renameRetry(backup, target);
                } else if (field(*it, "created").is_boolean() && field(*it, "created").get<bool>()) {
                    fs::remove(target, error);
                }
            }
        }
        fs::remove_all(m_state / "backup", error);
        fs::remove(m_state / "journal.json", error);
        m_log.write("rollback complete");
    }

    void finish()
    {
        std::error_code error;
        // A running helper replaced on Windows stays in backup/ until next time.
        fs::remove_all(m_state / "backup", error);
        fs::remove_all(m_state / "blobs", error);
        for (const char* name : {"release.json", "manifest.json", "range.part"}) fs::remove(m_state / name, error);
        fs::remove(m_state / "journal.json", error);
    }

    fs::path m_install, m_state;
    Status& m_status;
    Log m_log;
};

int run(const std::string& command, const fs::path& install, const fs::path& statusPath, long pid)
{
    {
        // The game reads this file to follow us; it must exist even if we fail early.
        std::error_code error;
        fs::create_directories(statusPath.parent_path(), error);
    }
    Status status(statusPath, command);
    if (command == "version") {
        status.done({{"updater", FORMAT}, {"installer", INSTALLER}, {"origin", ORIGIN}, {"platform", PLATFORM},
                     {"key", sha256(PUBLIC_KEY)},
#ifdef OTERA_UPDATER_TESTING
                     {"testing", true}
#else
                     {"testing", false}
#endif
        });
        return 0;
    }
    try {
        if (!fs::is_directory(install)) throw Failure("install", "not a directory: " + utf8(install));
        ix::initNetSystem();
        psa_crypto_init();
        Updater updater(install, status);
        Lock lock(install / STATE);
        if (command == "check") {
            status.done(updater.check(statusPath));
        } else if (command == "stage") {
            status.done(updater.stage());
        } else if (command == "apply") {
            std::string target = updater.launchTarget();
            try {
                json result = updater.apply(pid);
                target = field(result, "launch").get<std::string>();
                writeAtomic(install / STATE / "result.json", json({{"ok", true}, {"version", result.value("version", "")}}).dump());
                status.done(std::move(result));
            } catch (const Failure& failure) {
                // Whatever happened, the player gets a working game back, unless
                // the old one never closed: then it is still on screen.
                status.fail(failure);
                writeAtomic(install / STATE / "result.json", json({{"ok", false}, {"message", playerMessage(failure.code)}}).dump());
                if (failure.code != "running") launch(install, target);
                return 1;
            }
            launch(install, target);
        } else {
            return 2;
        }
        return 0;
    } catch (const Failure& failure) {
        status.fail(failure);
        try { Log(install / STATE).write(std::string(failure.code) + ": " + failure.what()); } catch (...) {}
        return 1;
    } catch (const std::exception& error) {
        status.fail(Failure("apply", error.what()));
        return 1;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    if (argc < 4) return 2;
    std::string command;
    for (const wchar_t* c = argv[1]; *c; ++c) command.push_back(static_cast<char>(*c));
    long pid = 0;
    if (argc > 4) pid = std::wcstol(argv[4], nullptr, 10);
    return run(command, fs::path(argv[2]), fs::path(argv[3]), pid);
}
#else
int main(int argc, char** argv)
{
    if (argc < 4) return 2;
    signal(SIGPIPE, SIG_IGN);
    const long pid = argc > 4 ? std::strtol(argv[4], nullptr, 10) : 0;
    return run(argv[1], fs::u8path(argv[2]), fs::u8path(argv[3]), pid);
}
#endif
