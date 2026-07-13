#include "raylib.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#error q2connect currently implements the Q2 status scanner for POSIX sockets.
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr int kQ2Port = 27910;
constexpr int kMaxServers = 1024;
constexpr int kMaxStatusPlayers = 64;
constexpr double kPingTimeout = 3.0;

struct Config {
    std::string theme = "cyber";
    std::vector<std::string> masters = {"+https://q2servers.com/?raw=2"};
    std::vector<std::string> favorites;
    bool broadcast = false;
    bool hideWallFly = true;
    int pingRate = 80;
    std::string clientPath = "/home/user/Projects/q2manager/dist/_sources/q2pro/build-lin64/q2pro";
    bool fullscreen = false;
    int windowWidth = 1280;
    int windowHeight = 720;
    fs::path serverCachePath;
};

struct ThemePalette {
    Color background;
    Color panel;
    Color panel2;
    Color border;
    Color text;
    Color muted;
    Color good;
    Color warn;
    Color error;
    Color selected;
};

static const ThemePalette kCyber{
    Color{5, 8, 13, 255}, Color{4, 12, 19, 230}, Color{8, 18, 28, 240}, Color{65, 215, 255, 255},
    Color{220, 245, 255, 255}, Color{145, 195, 210, 255}, Color{95, 255, 145, 255}, Color{255, 210, 80, 255},
    Color{255, 105, 115, 255}, Color{18, 76, 102, 255}
};
static const ThemePalette kMatrix{
    Color{0, 8, 2, 255}, Color{0, 18, 5, 235}, Color{0, 26, 8, 245}, Color{65, 255, 90, 255},
    Color{215, 255, 220, 255}, Color{120, 220, 130, 255}, Color{95, 255, 110, 255}, Color{220, 255, 105, 255},
    Color{255, 90, 90, 255}, Color{10, 78, 22, 255}
};

enum class SlotStatus { Idle, Pending, Error, Valid };

struct PlayerRow {
    int score = 0;
    int ping = 0;
    std::string name;
};

struct ServerSlot {
    std::string address;
    sockaddr_in addr{};
    SlotStatus status = SlotStatus::Idle;
    std::string hostname = "???";
    std::string mod = "???";
    std::string map = "???";
    int players = 0;
    std::string maxClients = "?";
    int rtt = 0;
    std::map<std::string, std::string> rules;
    std::vector<PlayerRow> playerRows;
    double sentAt = 0.0;
};

struct BrowserState {
    std::vector<ServerSlot> servers;
    std::string status = "Press R to refresh";
    bool scanning = false;
    int scanned = 0;
    int total = 0;
};

static std::mutex gStateMutex;
static BrowserState gState;
static std::atomic_bool gStopWorker{false};
static std::thread gWorker;

static double NowSeconds() {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

static std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t first = 0;
    while (first < s.size() && (s[first] == ' ' || s[first] == '\t')) ++first;
    return s.substr(first);
}

static std::string ShellQuote(const std::string &s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? "'\\''" : std::string(1, c);
    out += "'";
    return out;
}

static json ConfigJson(const Config &config) {
    return json{{"theme", config.theme}, {"masters", config.masters}, {"favorites", config.favorites}, {"broadcast", config.broadcast},
                {"hide_wallfly", config.hideWallFly}, {"ping_rate", config.pingRate}, {"client_path", config.clientPath}, {"fullscreen", config.fullscreen},
                {"window_width", config.windowWidth}, {"window_height", config.windowHeight}};
}

static void SaveConfig(const fs::path &path, const Config &config) {
    std::ofstream out(path);
    if (out) out << ConfigJson(config).dump(2) << '\n';
}

static Config LoadConfig() {
    Config config;
    const fs::path path = fs::path(GetApplicationDirectory()) / "q2connect.json";
    config.serverCachePath = path.parent_path() / "q2servers-cache.txt";
    std::ifstream in(path);
    if (!in) {
        SaveConfig(path, config);
        return config;
    }
    try {
        const json data = json::parse(in);
        config.theme = data.value("theme", config.theme);
        config.masters = data.value("masters", config.masters);
        config.favorites = data.value("favorites", config.favorites);
        config.broadcast = data.value("broadcast", config.broadcast);
        config.hideWallFly = data.value("hide_wallfly", config.hideWallFly);
        config.pingRate = std::clamp(data.value("ping_rate", config.pingRate), 1, 100);
        config.clientPath = data.value("client_path", config.clientPath);
        config.fullscreen = data.value("fullscreen", config.fullscreen);
        config.windowWidth = std::clamp(data.value("window_width", config.windowWidth), 800, 7680);
        config.windowHeight = std::clamp(data.value("window_height", config.windowHeight), 480, 4320);
    } catch (...) {
        SaveConfig(path, config);
    }
    return config;
}

static const ThemePalette &Theme(const Config &config) {
    return config.theme == "matrix" ? kMatrix : kCyber;
}

static void LoadAppIcon() {
    const fs::path iconPath = fs::path(GetApplicationDirectory()) / "assets" / "icon.png";
    if (fs::is_regular_file(iconPath)) {
        Image icon = LoadImage(iconPath.string().c_str());
        if (icon.data) {
            SetWindowIcon(icon);
            UnloadImage(icon);
            return;
        }
    }

    Image icon = GenImageColor(256, 256, Color{3, 8, 14, 255});
    ImageDrawCircle(&icon, 128, 128, 92, Color{10, 50, 72, 255});
    ImageDrawCircleLines(&icon, 128, 128, 76, Color{80, 220, 255, 255});
    ImageDrawCircleLines(&icon, 128, 128, 42, Color{80, 220, 255, 170});
    ImageDrawLineEx(&icon, Vector2{52, 128}, Vector2{204, 128}, 3, Color{80, 220, 255, 150});
    ImageDrawLineEx(&icon, Vector2{128, 52}, Vector2{128, 204}, 3, Color{80, 220, 255, 150});
    ImageDrawLineEx(&icon, Vector2{82, 158}, Vector2{112, 96}, 10, WHITE);
    ImageDrawLineEx(&icon, Vector2{112, 96}, Vector2{140, 144}, 10, WHITE);
    ImageDrawLineEx(&icon, Vector2{140, 144}, Vector2{157, 115}, 10, WHITE);
    ImageDrawLineEx(&icon, Vector2{157, 115}, Vector2{178, 158}, 10, WHITE);
    ImageDrawCircle(&icon, 182, 84, 12, Color{255, 95, 210, 255});
    ImageDrawCircle(&icon, 70, 112, 7, Color{120, 255, 120, 255});
    ImageDrawCircle(&icon, 188, 177, 8, Color{255, 210, 80, 255});
    SetWindowIcon(icon);
    UnloadImage(icon);
}

static std::optional<sockaddr_in> ResolveAddress(const std::string &hostPort) {
    std::string host = hostPort;
    std::string port = std::to_string(kQ2Port);
    const size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos && hostPort.find(']') == std::string::npos) {
        host = hostPort.substr(0, colon);
        port = hostPort.substr(colon + 1);
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *result = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0 || !result) return std::nullopt;
    sockaddr_in addr = *reinterpret_cast<sockaddr_in *>(result->ai_addr);
    freeaddrinfo(result);
    if (ntohs(addr.sin_port) < 1024) return std::nullopt;
    return addr;
}

static std::string AddressToString(const sockaddr_in &addr) {
    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

static std::string SockKey(const sockaddr_in &addr) {
    return std::to_string(addr.sin_addr.s_addr) + ":" + std::to_string(addr.sin_port);
}

struct FetchResult {
    std::string data;
    std::string error;
};

static FetchResult FetchUrl(const std::string &url) {
    const std::string cmd = "curl -fsSL --max-time 20 " + ShellQuote(url) + " 2>/dev/null";
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {{}, "could not start curl"};
    std::string data;
    std::array<char, 4096> buffer{};
    while (true) {
        const size_t read = fread(buffer.data(), 1, buffer.size(), pipe);
        if (read > 0) data.append(buffer.data(), read);
        if (read < buffer.size()) break;
    }
    const int result = pclose(pipe);
    if (result == -1) return {{}, "could not read curl result"};
    if (!WIFEXITED(result) || WEXITSTATUS(result) != 0) {
        const int code = WIFEXITED(result) ? WEXITSTATUS(result) : -1;
        return {{}, code == 22 ? "HTTP server returned an error" : "curl failed (exit " + std::to_string(code) + ")"};
    }
    if (data.empty()) return {{}, "server returned an empty response"};
    return {std::move(data), {}};
}

static void AddServer(std::vector<ServerSlot> &servers, std::unordered_map<std::string, size_t> &seen, const sockaddr_in &addr, const std::string &display) {
    if (servers.size() >= kMaxServers) return;
    const std::string key = SockKey(addr);
    if (seen.contains(key)) return;
    seen[key] = servers.size();
    ServerSlot slot;
    slot.addr = addr;
    slot.address = display.empty() ? AddressToString(addr) : display;
    servers.push_back(std::move(slot));
}

static void ParsePlainList(std::vector<ServerSlot> &servers, std::unordered_map<std::string, size_t> &seen, const std::string &data) {
    std::istringstream lines(data);
    std::string line;
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (line.empty()) continue;
        if (auto addr = ResolveAddress(line)) AddServer(servers, seen, *addr, line);
    }
}

static void ParseBinaryList(std::vector<ServerSlot> &servers, std::unordered_map<std::string, size_t> &seen, const std::string &data, size_t chunk) {
    if (chunk < 6) chunk = 6;
    for (size_t i = 0; i + 5 < data.size(); i += chunk) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        std::memcpy(&addr.sin_addr.s_addr, data.data() + i, 4);
        std::memcpy(&addr.sin_port, data.data() + i + 4, 2);
        if (ntohs(addr.sin_port) >= 1024) AddServer(servers, seen, addr, AddressToString(addr));
    }
}

static bool IsQ2ServersUrl(const std::string &url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return false;
    const size_t hostStart = scheme + 3;
    const size_t hostEnd = url.find_first_of("/:?#", hostStart);
    const std::string host = url.substr(hostStart, hostEnd - hostStart);
    return host == "q2servers.com" || host == "www.q2servers.com";
}

static bool SaveServerCache(const fs::path &path, const std::vector<ServerSlot> &servers) {
    if (path.empty() || servers.empty()) return false;
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        if (!out) return false;
        for (const ServerSlot &server : servers) out << AddressToString(server.addr) << '\n';
        if (!out) return false;
    }
    std::error_code error;
    fs::rename(temporary, path, error);
    if (!error) return true;
    fs::remove(path, error);
    error.clear();
    fs::rename(temporary, path, error);
    if (error) fs::remove(temporary);
    return !error;
}

static bool LoadServerCache(const fs::path &path, std::vector<ServerSlot> &servers,
                            std::unordered_map<std::string, size_t> &seen) {
    std::ifstream in(path);
    if (!in) return false;
    const size_t before = servers.size();
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ParsePlainList(servers, seen, data);
    return servers.size() > before;
}

static std::vector<std::string> SplitCommandish(const std::vector<std::string> &items) {
    std::vector<std::string> out;
    for (const std::string &item : items) {
        std::istringstream input(item);
        std::string token;
        while (input >> token) out.push_back(token);
    }
    return out;
}

static std::vector<ServerSlot> LoadServerSources(const Config &config, std::string &status) {
    std::vector<ServerSlot> servers;
    std::unordered_map<std::string, size_t> seen;
    std::vector<std::string> errors;

    for (const std::string &fav : config.favorites) {
        if (auto addr = ResolveAddress(fav)) AddServer(servers, seen, *addr, fav);
    }

    for (std::string source : SplitCommandish(config.masters)) {
        bool binary = false;
        size_t chunk = 6;
        if (!source.empty() && (source[0] == '+' || source[0] == '-')) {
            binary = true;
            source.erase(source.begin());
            size_t used = 0;
            try {
                const size_t parsed = std::stoul(source, &used);
                if (used > 0) {
                    chunk = std::max<size_t>(6, parsed);
                    source.erase(0, used);
                }
            } catch (...) {}
        }

        if (source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0) {
            status = "Fetching " + source;
            FetchResult fetched = FetchUrl(source);
            if (!fetched.error.empty()) {
                if (IsQ2ServersUrl(source) && LoadServerCache(config.serverCachePath, servers, seen))
                    errors.push_back(source + ": " + fetched.error + "; using cached server list");
                else
                    errors.push_back(source + ": " + fetched.error);
                continue;
            }
            if (IsQ2ServersUrl(source)) {
                std::vector<ServerSlot> fetchedServers;
                std::unordered_map<std::string, size_t> fetchedSeen;
                if (binary) ParseBinaryList(fetchedServers, fetchedSeen, fetched.data, chunk);
                else ParsePlainList(fetchedServers, fetchedSeen, fetched.data);
                if (fetchedServers.empty()) {
                    if (LoadServerCache(config.serverCachePath, servers, seen))
                        errors.push_back(source + ": response contained no valid servers; using cached server list");
                    else
                        errors.push_back(source + ": response contained no valid servers");
                    continue;
                }
                SaveServerCache(config.serverCachePath, fetchedServers);
                for (const ServerSlot &server : fetchedServers) AddServer(servers, seen, server.addr, server.address);
            } else if (binary) ParseBinaryList(servers, seen, fetched.data, chunk);
            else ParsePlainList(servers, seen, fetched.data);
        } else if (source.rfind("file://", 0) == 0) {
            std::ifstream in(source.substr(7), std::ios::binary);
            std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (binary) ParseBinaryList(servers, seen, data, chunk);
            else ParsePlainList(servers, seen, data);
        } else if (source.rfind("quake2://", 0) == 0) {
            const std::string addrText = source.substr(9);
            if (auto addr = ResolveAddress(addrText)) AddServer(servers, seen, *addr, addrText);
        } else if (!source.empty()) {
            if (auto addr = ResolveAddress(source)) AddServer(servers, seen, *addr, source);
        }
    }

    if (servers.empty() && !errors.empty()) status = "Server list unavailable: " + errors.front();
    else if (!errors.empty()) status = "Loaded " + std::to_string(servers.size()) + " servers; " + errors.front();
    else status = "Loaded " + std::to_string(servers.size()) + " servers";
    return servers;
}

static std::map<std::string, std::string> ParseInfoString(const std::string &info) {
    std::map<std::string, std::string> rules;
    size_t pos = 0;
    while (pos < info.size()) {
        if (info[pos] == '\\') ++pos;
        const size_t keyEnd = info.find('\\', pos);
        if (keyEnd == std::string::npos) break;
        const std::string key = info.substr(pos, keyEnd - pos);
        pos = keyEnd + 1;
        const size_t valueEnd = info.find('\\', pos);
        const std::string value = valueEnd == std::string::npos ? info.substr(pos) : info.substr(pos, valueEnd - pos);
        if (!key.empty()) rules[key] = value;
        if (valueEnd == std::string::npos) break;
        pos = valueEnd + 1;
    }
    return rules;
}

static std::string ParseToken(const std::string &line, size_t &pos) {
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
    if (pos >= line.size()) return {};
    if (line[pos] == '"') {
        ++pos;
        std::string token;
        while (pos < line.size() && line[pos] != '"') token += line[pos++];
        if (pos < line.size()) ++pos;
        return token;
    }
    const size_t start = pos;
    while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
    return line.substr(start, pos - start);
}

static void ApplyStatusResponse(ServerSlot &slot, const std::string &payload, int rtt, bool hideWallFly) {
    std::string text = payload;
    if (text.rfind("print\n", 0) == 0) text.erase(0, 6);
    if (text.rfind("print\r\n", 0) == 0) text.erase(0, 7);
    std::istringstream lines(text);
    std::string info;
    std::getline(lines, info);
    info = Trim(info);
    slot.rules = ParseInfoString(info);
    slot.hostname = slot.rules.contains("hostname") && !slot.rules["hostname"].empty() ? slot.rules["hostname"] : slot.address;
    slot.mod = slot.rules.contains("game") && !slot.rules["game"].empty() ? slot.rules["game"] : "baseq2";
    slot.map = slot.rules.contains("mapname") && !slot.rules["mapname"].empty() ? slot.rules["mapname"] : "???";
    slot.maxClients = slot.rules.contains("maxclients") && !slot.rules["maxclients"].empty() ? slot.rules["maxclients"] : "?";
    slot.playerRows.clear();

    std::string line;
    while (slot.playerRows.size() < kMaxStatusPlayers && std::getline(lines, line)) {
        line = Trim(line);
        if (line.empty()) continue;
        size_t pos = 0;
        PlayerRow player;
        try {
            player.score = std::stoi(ParseToken(line, pos));
            player.ping = std::stoi(ParseToken(line, pos));
            player.name = ParseToken(line, pos);
            if (hideWallFly && (player.name == "WallFly[BZZZ]" || player.name == "Ghost[BOOO!]")) continue;
            slot.playerRows.push_back(std::move(player));
        } catch (...) {}
    }
    std::sort(slot.playerRows.begin(), slot.playerRows.end(), [](const PlayerRow &a, const PlayerRow &b) { return a.score > b.score; });
    slot.players = static_cast<int>(slot.playerRows.size());
    slot.rtt = std::clamp(rtt, 0, 999);
    slot.status = SlotStatus::Valid;
}

static bool SendStatus(int sock, const sockaddr_in &addr) {
    constexpr std::array<unsigned char, 11> packet = {255, 255, 255, 255, 's', 't', 'a', 't', 'u', 's', 0};
    return sendto(sock, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == static_cast<ssize_t>(packet.size());
}

static void WorkerScan(Config config) {
    std::string status = "Resolving servers...";
    {
        std::scoped_lock lock(gStateMutex);
        gState = {};
        gState.status = status;
        gState.scanning = true;
    }

    std::vector<ServerSlot> servers = LoadServerSources(config, status);
    {
        std::scoped_lock lock(gStateMutex);
        gState.servers = servers;
        gState.status = status;
        gState.total = static_cast<int>(servers.size());
    }
    if (servers.empty() || gStopWorker) {
        std::scoped_lock lock(gStateMutex);
        gState.scanning = false;
        return;
    }

    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::scoped_lock lock(gStateMutex);
        gState.status = "Failed to open UDP socket";
        gState.scanning = false;
        return;
    }
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    std::unordered_map<std::string, size_t> byAddress;
    for (size_t i = 0; i < servers.size(); ++i) byAddress[SockKey(servers[i].addr)] = i;

    const double interval = 1.0 / static_cast<double>(std::max(1, config.pingRate));
    size_t nextToSend = 0;
    double nextSendAt = NowSeconds();
    int completed = 0;
    while (!gStopWorker && completed < static_cast<int>(servers.size())) {
        const double now = NowSeconds();
        if (nextToSend < servers.size() && now >= nextSendAt) {
            servers[nextToSend].status = SlotStatus::Pending;
            servers[nextToSend].sentAt = now;
            SendStatus(sock, servers[nextToSend].addr);
            ++nextToSend;
            nextSendAt = now + interval;
        }

        std::array<char, 4096> buffer{};
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        const ssize_t len = recvfrom(sock, buffer.data(), buffer.size() - 1, 0, reinterpret_cast<sockaddr *>(&from), &fromLen);
        if (len > 4) {
            buffer[static_cast<size_t>(len)] = 0;
            const std::string key = SockKey(from);
            const auto found = byAddress.find(key);
            if (found != byAddress.end()) {
                ServerSlot &slot = servers[found->second];
                if (slot.status != SlotStatus::Valid) ++completed;
                const int rtt = static_cast<int>((NowSeconds() - slot.sentAt) * 1000.0);
                std::string payload(buffer.data() + 4, static_cast<size_t>(len - 4));
                ApplyStatusResponse(slot, payload, rtt, config.hideWallFly);
            }
        }

        for (ServerSlot &slot : servers) {
            if (slot.status == SlotStatus::Pending && now - slot.sentAt > kPingTimeout) {
                slot.status = SlotStatus::Error;
                slot.rtt = 999;
                ++completed;
            }
        }

        {
            std::scoped_lock lock(gStateMutex);
            gState.servers = servers;
            gState.scanned = completed;
            gState.status = "Pinging servers...";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    close(sock);
    {
        std::scoped_lock lock(gStateMutex);
        gState.servers = servers;
        gState.scanned = completed;
        gState.status = "Scan complete";
        gState.scanning = false;
    }
}

static void StartScan(const Config &config) {
    gStopWorker = true;
    if (gWorker.joinable()) gWorker.join();
    gStopWorker = false;
    gWorker = std::thread(WorkerScan, config);
}

static int StatusRank(SlotStatus status) {
    if (status == SlotStatus::Valid) return 0;
    if (status == SlotStatus::Pending) return 1;
    if (status == SlotStatus::Idle) return 2;
    return 3;
}

static void SortServers(std::vector<ServerSlot> &servers, int column, bool descending) {
    std::sort(servers.begin(), servers.end(), [&](const ServerSlot &a, const ServerSlot &b) {
        if (StatusRank(a.status) != StatusRank(b.status)) return StatusRank(a.status) < StatusRank(b.status);
        int cmp = 0;
        switch (column) {
            case 1: cmp = a.mod.compare(b.mod); break;
            case 2: cmp = a.map.compare(b.map); break;
            case 3: cmp = a.players == b.players ? 0 : (a.players < b.players ? -1 : 1); break;
            case 4: cmp = a.rtt == b.rtt ? 0 : (a.rtt < b.rtt ? -1 : 1); break;
            default: cmp = a.hostname.compare(b.hostname); break;
        }
        if (cmp == 0) cmp = a.address.compare(b.address);
        return descending ? cmp > 0 : cmp < 0;
    });
}

static void CopyText(const std::string &text) {
    SetClipboardText(text.c_str());
}

static void LaunchQ2PRO(const Config &config, const std::string &address) {
    const pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl(config.clientPath.c_str(), config.clientPath.c_str(), "+connect", address.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
}

static std::string Ellipsize(const std::string &text, int maxPixels, int fontSize) {
    if (MeasureText(text.c_str(), fontSize) <= maxPixels) return text;
    std::string out = text;
    while (!out.empty() && MeasureText((out + "...").c_str(), fontSize) > maxPixels) out.pop_back();
    return out + "...";
}

static void DrawCell(const std::string &text, int x, int y, int width, int fontSize, Color color) {
    DrawText(Ellipsize(text, width - 8, fontSize).c_str(), x + 4, y, fontSize, color);
}

int main() {
    Config config = LoadConfig();
    const ThemePalette &theme = Theme(config);

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(config.windowWidth, config.windowHeight, "q2connect");
    LoadAppIcon();
    if (config.fullscreen) ToggleFullscreen();
    SetTargetFPS(120);

    StartScan(config);
    int selected = 0;
    int scroll = 0;
    int ruleScroll = 0;
    int playerScroll = 0;
    std::string ruleScrollAddress;
    std::string playerScrollAddress;
    int sortColumn = 4;
    bool sortDescending = false;
    std::string toast;
    double toastUntil = 0.0;
    double lastClickAt = 0.0;
    int lastClickIndex = -1;
    bool draggingScrollbar = false;
    float scrollbarDragOffset = 0.0f;
    bool draggingRuleScrollbar = false;
    float ruleScrollbarDragOffset = 0.0f;
    bool draggingPlayerScrollbar = false;
    float playerScrollbarDragOffset = 0.0f;

    while (!WindowShouldClose()) {
        BrowserState state;
        {
            std::scoped_lock lock(gStateMutex);
            state = gState;
        }

        if (IsKeyPressed(KEY_R) || IsKeyPressed(KEY_F5)) StartScan(config);
        if (IsKeyPressed(KEY_ONE)) { sortColumn = 0; sortDescending = false; }
        if (IsKeyPressed(KEY_TWO)) { sortColumn = 1; sortDescending = false; }
        if (IsKeyPressed(KEY_THREE)) { sortColumn = 2; sortDescending = false; }
        if (IsKeyPressed(KEY_FOUR)) { sortColumn = 3; sortDescending = true; }
        if (IsKeyPressed(KEY_FIVE)) { sortColumn = 4; sortDescending = false; }
        SortServers(state.servers, sortColumn, sortDescending);

        // Raylib's built-in font is a bitmap font. Multiples of its native
        // 10-pixel size stay crisp instead of being resampled unevenly.
        constexpr int bodyFontSize = 20;
        constexpr int smallFontSize = 20;
        const int tableTop = 76;
        const int rowHeight = 30;
        const int statusHeight = 38;
        const int detailsHeight = std::min(240, GetScreenHeight() / 3);
        const int tableHeight = GetScreenHeight() - tableTop - detailsHeight - statusHeight - 16;
        const int visibleRows = std::max(1, tableHeight / rowHeight - 1);

        selected = std::clamp(selected, 0, std::max(0, static_cast<int>(state.servers.size()) - 1));
        const bool selectionMoved = IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_UP) ||
                                    IsKeyPressed(KEY_PAGE_DOWN) || IsKeyPressed(KEY_PAGE_UP);
        if (IsKeyPressed(KEY_DOWN)) ++selected;
        if (IsKeyPressed(KEY_UP)) --selected;
        if (IsKeyPressed(KEY_PAGE_DOWN)) selected += visibleRows;
        if (IsKeyPressed(KEY_PAGE_UP)) selected -= visibleRows;
        selected = std::clamp(selected, 0, std::max(0, static_cast<int>(state.servers.size()) - 1));
        const int maxScroll = std::max(0, static_cast<int>(state.servers.size()) - visibleRows);
        const int margin = 18;
        const int tableWidth = GetScreenWidth() - margin * 2;
        const int detailTop = tableTop + tableHeight + 8;
        const int scrollbarWidth = 14;
        const Rectangle scrollbarTrack{
            static_cast<float>(margin + tableWidth - scrollbarWidth),
            static_cast<float>(tableTop + rowHeight),
            static_cast<float>(scrollbarWidth),
            static_cast<float>(tableHeight - rowHeight)
        };
        const float thumbHeight = maxScroll > 0
            ? std::max(28.0f, scrollbarTrack.height * visibleRows / static_cast<float>(state.servers.size()))
            : scrollbarTrack.height;
        const float thumbTravel = std::max(0.0f, scrollbarTrack.height - thumbHeight);
        const float thumbY = scrollbarTrack.y + (maxScroll > 0 ? thumbTravel * scroll / maxScroll : 0.0f);
        const Rectangle scrollbarThumb{scrollbarTrack.x, thumbY, scrollbarTrack.width, thumbHeight};

        const Vector2 mouse = GetMousePosition();
        const float mouseWheel = GetMouseWheelMove();
        if (CheckCollisionPointRec(mouse, Rectangle{static_cast<float>(margin), static_cast<float>(tableTop),
                                                    static_cast<float>(tableWidth), static_cast<float>(tableHeight)})) {
            scroll -= static_cast<int>(mouseWheel) * 3;
        }
        if (selectionMoved) {
            if (selected < scroll) scroll = selected;
            if (selected >= scroll + visibleRows) scroll = selected - visibleRows + 1;
        }
        if (maxScroll > 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, scrollbarTrack)) {
            if (CheckCollisionPointRec(mouse, scrollbarThumb)) {
                draggingScrollbar = true;
                scrollbarDragOffset = mouse.y - scrollbarThumb.y;
            } else {
                scroll = static_cast<int>(((mouse.y - scrollbarTrack.y - thumbHeight / 2.0f) / thumbTravel) * maxScroll + 0.5f);
                draggingScrollbar = true;
                scrollbarDragOffset = thumbHeight / 2.0f;
            }
        }
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) draggingScrollbar = false;
        if (draggingScrollbar && maxScroll > 0 && thumbTravel > 0.0f) {
            scroll = static_cast<int>(((mouse.y - scrollbarTrack.y - scrollbarDragOffset) / thumbTravel) * maxScroll + 0.5f);
        }
        scroll = std::clamp(scroll, 0, maxScroll);

        bool activate = IsKeyPressed(KEY_ENTER);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mouse, scrollbarTrack) &&
            GetMouseY() >= tableTop + rowHeight && GetMouseY() < tableTop + tableHeight) {
            const int clicked = scroll + (GetMouseY() - tableTop - rowHeight) / rowHeight;
            if (clicked >= 0 && clicked < static_cast<int>(state.servers.size())) {
                activate = clicked == lastClickIndex && GetTime() - lastClickAt < 0.35;
                selected = clicked;
                lastClickIndex = clicked;
                lastClickAt = GetTime();
            }
        }
        if (activate && selected >= 0 && selected < static_cast<int>(state.servers.size())) {
            const ServerSlot &slot = state.servers[static_cast<size_t>(selected)];
            LaunchQ2PRO(config, slot.address);
            toast = "Launching q2pro +connect " + slot.address;
            toastUntil = GetTime() + 2.5;
        }
        if (IsKeyPressed(KEY_C) && selected >= 0 && selected < static_cast<int>(state.servers.size())) {
            CopyText("connect " + state.servers[static_cast<size_t>(selected)].address);
            toast = "Copied connect " + state.servers[static_cast<size_t>(selected)].address;
            toastUntil = GetTime() + 2.0;
        }

        const int playerRowHeight = 24;
        const int visiblePlayerRows = std::max(1, (detailsHeight - 78) / playerRowHeight);
        const Rectangle ruleViewport{
            static_cast<float>(margin + 12),
            static_cast<float>(detailTop + 70),
            static_cast<float>(tableWidth / 2 - 24),
            static_cast<float>(visiblePlayerRows * playerRowHeight)
        };
        const Rectangle playerViewport{
            static_cast<float>(margin + tableWidth / 2),
            static_cast<float>(detailTop + 70),
            static_cast<float>(tableWidth / 2 - 12),
            static_cast<float>(visiblePlayerRows * playerRowHeight)
        };
        int maxRuleScroll = 0;
        int maxPlayerScroll = 0;
        if (selected >= 0 && selected < static_cast<int>(state.servers.size())) {
            const ServerSlot &slot = state.servers[static_cast<size_t>(selected)];
            if (ruleScrollAddress != slot.address) {
                ruleScrollAddress = slot.address;
                ruleScroll = 0;
                draggingRuleScrollbar = false;
            }
            if (playerScrollAddress != slot.address) {
                playerScrollAddress = slot.address;
                playerScroll = 0;
                draggingPlayerScrollbar = false;
            }
            maxRuleScroll = std::max(0, static_cast<int>(slot.rules.size()) - visiblePlayerRows);
            maxPlayerScroll = std::max(0, static_cast<int>(slot.playerRows.size()) - visiblePlayerRows);
        } else {
            ruleScrollAddress.clear();
            ruleScroll = 0;
            playerScrollAddress.clear();
            playerScroll = 0;
        }
        if (CheckCollisionPointRec(mouse, ruleViewport)) {
            ruleScroll -= static_cast<int>(mouseWheel) * 3;
        }
        ruleScroll = std::clamp(ruleScroll, 0, maxRuleScroll);
        if (CheckCollisionPointRec(mouse, playerViewport)) {
            playerScroll -= static_cast<int>(mouseWheel) * 3;
        }
        playerScroll = std::clamp(playerScroll, 0, maxPlayerScroll);

        const Rectangle ruleScrollbarTrack{
            ruleViewport.x + ruleViewport.width - scrollbarWidth,
            ruleViewport.y,
            static_cast<float>(scrollbarWidth),
            ruleViewport.height
        };
        const float ruleThumbHeight = maxRuleScroll > 0
            ? std::max(28.0f, ruleScrollbarTrack.height * visiblePlayerRows /
                                   static_cast<float>(visiblePlayerRows + maxRuleScroll))
            : ruleScrollbarTrack.height;
        const float ruleThumbTravel = std::max(0.0f, ruleScrollbarTrack.height - ruleThumbHeight);
        const float ruleThumbY = ruleScrollbarTrack.y +
            (maxRuleScroll > 0 ? ruleThumbTravel * ruleScroll / maxRuleScroll : 0.0f);
        const Rectangle ruleScrollbarThumb{
            ruleScrollbarTrack.x, ruleThumbY, ruleScrollbarTrack.width, ruleThumbHeight
        };
        if (maxRuleScroll > 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(mouse, ruleScrollbarTrack)) {
            if (CheckCollisionPointRec(mouse, ruleScrollbarThumb)) {
                draggingRuleScrollbar = true;
                ruleScrollbarDragOffset = mouse.y - ruleScrollbarThumb.y;
            } else {
                ruleScroll = static_cast<int>(((mouse.y - ruleScrollbarTrack.y - ruleThumbHeight / 2.0f) /
                                               ruleThumbTravel) * maxRuleScroll + 0.5f);
                draggingRuleScrollbar = true;
                ruleScrollbarDragOffset = ruleThumbHeight / 2.0f;
            }
        }
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) draggingRuleScrollbar = false;
        if (draggingRuleScrollbar && maxRuleScroll > 0 && ruleThumbTravel > 0.0f) {
            ruleScroll = static_cast<int>(((mouse.y - ruleScrollbarTrack.y - ruleScrollbarDragOffset) /
                                           ruleThumbTravel) * maxRuleScroll + 0.5f);
        }
        ruleScroll = std::clamp(ruleScroll, 0, maxRuleScroll);

        const Rectangle playerScrollbarTrack{
            playerViewport.x + playerViewport.width - scrollbarWidth,
            playerViewport.y,
            static_cast<float>(scrollbarWidth),
            playerViewport.height
        };
        const float playerThumbHeight = maxPlayerScroll > 0
            ? std::max(28.0f, playerScrollbarTrack.height * visiblePlayerRows /
                                   static_cast<float>(visiblePlayerRows + maxPlayerScroll))
            : playerScrollbarTrack.height;
        const float playerThumbTravel = std::max(0.0f, playerScrollbarTrack.height - playerThumbHeight);
        const float playerThumbY = playerScrollbarTrack.y +
            (maxPlayerScroll > 0 ? playerThumbTravel * playerScroll / maxPlayerScroll : 0.0f);
        const Rectangle playerScrollbarThumb{
            playerScrollbarTrack.x, playerThumbY, playerScrollbarTrack.width, playerThumbHeight
        };
        if (maxPlayerScroll > 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(mouse, playerScrollbarTrack)) {
            if (CheckCollisionPointRec(mouse, playerScrollbarThumb)) {
                draggingPlayerScrollbar = true;
                playerScrollbarDragOffset = mouse.y - playerScrollbarThumb.y;
            } else {
                playerScroll = static_cast<int>(((mouse.y - playerScrollbarTrack.y - playerThumbHeight / 2.0f) /
                                                 playerThumbTravel) * maxPlayerScroll + 0.5f);
                draggingPlayerScrollbar = true;
                playerScrollbarDragOffset = playerThumbHeight / 2.0f;
            }
        }
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) draggingPlayerScrollbar = false;
        if (draggingPlayerScrollbar && maxPlayerScroll > 0 && playerThumbTravel > 0.0f) {
            playerScroll = static_cast<int>(((mouse.y - playerScrollbarTrack.y - playerScrollbarDragOffset) /
                                             playerThumbTravel) * maxPlayerScroll + 0.5f);
        }
        playerScroll = std::clamp(playerScroll, 0, maxPlayerScroll);

        int totalPlayers = 0;
        int validServers = 0;
        for (const ServerSlot &slot : state.servers) {
            if (slot.status == SlotStatus::Valid) {
                ++validServers;
                totalPlayers += slot.players;
            }
        }

        BeginDrawing();
        ClearBackground(theme.background);
        DrawRectangle(0, 0, GetScreenWidth(), 60, theme.panel);
        DrawText("q2connect", 18, 15, 30, theme.text);
        DrawText("Q2PRO-style server browser", 185, 22, bodyFontSize, theme.muted);

        const std::string summary = std::to_string(totalPlayers) + " players on " + std::to_string(validServers) + " servers";
        DrawText(summary.c_str(), GetScreenWidth() - MeasureText(summary.c_str(), bodyFontSize) - 18, 20, bodyFontSize, theme.good);

        const std::array<int, 5> widths = {std::max(260, tableWidth - scrollbarWidth - 650), 125, 135, 105, 80};
        const std::array<const char *, 5> headers = {"Hostname", "Mod", "Map", "Players", "RTT"};
        DrawRectangle(margin, tableTop, tableWidth, tableHeight, theme.panel);
        DrawRectangleLinesEx(Rectangle{static_cast<float>(margin), static_cast<float>(tableTop), static_cast<float>(tableWidth), static_cast<float>(tableHeight)}, 1.0f, theme.border);

        int x = margin + 8;
        for (size_t i = 0; i < headers.size(); ++i) {
            const std::string label = std::string(headers[i]) + (sortColumn == static_cast<int>(i) ? (sortDescending ? " v" : " ^") : "");
            DrawCell(label, x, tableTop + 5, widths[i], bodyFontSize, theme.border);
            x += widths[i];
        }
        DrawLine(margin, tableTop + rowHeight, margin + tableWidth, tableTop + rowHeight, theme.border);

        for (int row = 0; row < visibleRows; ++row) {
            const int index = scroll + row;
            if (index >= static_cast<int>(state.servers.size())) break;
            const ServerSlot &slot = state.servers[static_cast<size_t>(index)];
            const int y = tableTop + rowHeight * (row + 1) + 4;
            if (index == selected) DrawRectangle(margin + 1, y - 4, tableWidth - scrollbarWidth - 1, rowHeight, theme.selected);
            Color rowColor = theme.text;
            if (slot.status == SlotStatus::Pending) rowColor = theme.warn;
            if (slot.status == SlotStatus::Error) rowColor = theme.error;
            x = margin + 8;
            DrawCell(slot.status == SlotStatus::Valid ? slot.hostname : slot.address, x, y, widths[0], bodyFontSize, rowColor); x += widths[0];
            DrawCell(slot.mod, x, y, widths[1], bodyFontSize, rowColor); x += widths[1];
            DrawCell(slot.map, x, y, widths[2], bodyFontSize, rowColor); x += widths[2];
            const std::string players = slot.status == SlotStatus::Valid ? std::to_string(slot.players) + "/" + slot.maxClients : (slot.status == SlotStatus::Pending ? "ping" : "down");
            DrawCell(players, x, y, widths[3], bodyFontSize, rowColor); x += widths[3];
            DrawCell(slot.status == SlotStatus::Valid ? std::to_string(slot.rtt) : "???", x, y, widths[4], bodyFontSize, rowColor);
        }

        DrawRectangleRec(scrollbarTrack, theme.panel2);
        DrawLine(static_cast<int>(scrollbarTrack.x), static_cast<int>(scrollbarTrack.y),
                 static_cast<int>(scrollbarTrack.x), static_cast<int>(scrollbarTrack.y + scrollbarTrack.height), theme.border);
        const float drawnThumbY = scrollbarTrack.y + (maxScroll > 0 ? thumbTravel * scroll / maxScroll : 0.0f);
        DrawRectangleRec(Rectangle{scrollbarTrack.x + 3.0f, drawnThumbY + 2.0f,
                                   scrollbarTrack.width - 6.0f, std::max(0.0f, thumbHeight - 4.0f)},
                         maxScroll > 0 ? theme.border : theme.muted);

        DrawRectangle(margin, detailTop, tableWidth, detailsHeight, theme.panel2);
        DrawRectangleLinesEx(Rectangle{static_cast<float>(margin), static_cast<float>(detailTop), static_cast<float>(tableWidth), static_cast<float>(detailsHeight)}, 1.0f, theme.border);

        if (selected >= 0 && selected < static_cast<int>(state.servers.size())) {
            const ServerSlot &slot = state.servers[static_cast<size_t>(selected)];
            DrawText(slot.address.c_str(), margin + 12, detailTop + 10, bodyFontSize, theme.good);
            DrawText("Rules", margin + 12, detailTop + 42, bodyFontSize, theme.border);
            int y = detailTop + 70;
            int index = 0;
            for (const auto &[key, value] : slot.rules) {
                if (index++ < ruleScroll) continue;
                if (index > ruleScroll + visiblePlayerRows) break;
                DrawCell(key + " = " + value, margin + 12, y,
                         tableWidth / 2 - scrollbarWidth - 24, smallFontSize, theme.muted);
                y += 24;
            }
            if (maxRuleScroll > 0) {
                DrawRectangleRec(ruleScrollbarTrack, theme.panel);
                DrawLine(static_cast<int>(ruleScrollbarTrack.x), static_cast<int>(ruleScrollbarTrack.y),
                         static_cast<int>(ruleScrollbarTrack.x),
                         static_cast<int>(ruleScrollbarTrack.y + ruleScrollbarTrack.height), theme.border);
                const float drawnRuleThumbY = ruleScrollbarTrack.y +
                    ruleThumbTravel * ruleScroll / maxRuleScroll;
                DrawRectangleRec(Rectangle{ruleScrollbarTrack.x + 3.0f, drawnRuleThumbY + 2.0f,
                                           ruleScrollbarTrack.width - 6.0f,
                                           std::max(0.0f, ruleThumbHeight - 4.0f)}, theme.border);
            }
            DrawText("Players", margin + tableWidth / 2, detailTop + 42, bodyFontSize, theme.border);
            y = detailTop + 70;
            for (int row = 0; row < visiblePlayerRows; ++row) {
                const int index = playerScroll + row;
                if (index >= static_cast<int>(slot.playerRows.size())) break;
                const PlayerRow &p = slot.playerRows[static_cast<size_t>(index)];
                DrawCell(std::to_string(p.score) + "  " + std::to_string(p.ping) + "  " + p.name,
                         margin + tableWidth / 2, y, tableWidth / 2 - scrollbarWidth - 16,
                         smallFontSize, theme.text);
                y += 24;
            }
            if (maxPlayerScroll > 0) {
                DrawRectangleRec(playerScrollbarTrack, theme.panel);
                DrawLine(static_cast<int>(playerScrollbarTrack.x), static_cast<int>(playerScrollbarTrack.y),
                         static_cast<int>(playerScrollbarTrack.x),
                         static_cast<int>(playerScrollbarTrack.y + playerScrollbarTrack.height), theme.border);
                const float drawnPlayerThumbY = playerScrollbarTrack.y +
                    playerThumbTravel * playerScroll / maxPlayerScroll;
                DrawRectangleRec(Rectangle{playerScrollbarTrack.x + 3.0f, drawnPlayerThumbY + 2.0f,
                                           playerScrollbarTrack.width - 6.0f,
                                           std::max(0.0f, playerThumbHeight - 4.0f)}, theme.border);
            }
        }

        const int statusTop = GetScreenHeight() - statusHeight;
        DrawRectangle(0, statusTop, GetScreenWidth(), statusHeight, theme.panel);
        if (state.total > 0) {
            const float progress = static_cast<float>(state.scanned) / static_cast<float>(state.total);
            DrawRectangle(0, statusTop, static_cast<int>(GetScreenWidth() * progress), statusHeight, Color{theme.border.r, theme.border.g, theme.border.b, 70});
        }
        std::string footer = state.status + "   R/F5 refresh   Enter launch   C copy   1-5 sort   Esc quit";
        if (!toast.empty() && GetTime() < toastUntil) footer = toast;
        DrawText(footer.c_str(), 18, statusTop + 9, smallFontSize, theme.text);
        EndDrawing();
    }

    gStopWorker = true;
    if (gWorker.joinable()) gWorker.join();
    CloseWindow();
    return 0;
}
