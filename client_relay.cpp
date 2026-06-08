// GrandControl relay client/controller for Linux/macOS.
//
// Compile:
//   g++ client_relay.cpp -std=c++17 -o grandcontrol-client
//
// Usage:
//   ./grandcontrol-client <relay-host> <relay-port> <admin-token>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

constexpr size_t MAX_LINE_BYTES = 24 * 1024 * 1024;

namespace ansi {
    const char* reset  = "\033[0m";
    const char* bold   = "\033[1m";
    const char* dim    = "\033[2m";
    const char* red    = "\033[31m";
    const char* green  = "\033[32m";
    const char* yellow = "\033[33m";
    const char* cyan   = "\033[36m";
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::pair<std::string, std::string> split_first(const std::string& s, char delim) {
    auto pos = s.find(delim);
    if (pos == std::string::npos) return {s, ""};
    return {s.substr(0, pos), s.substr(pos + 1)};
}

static const std::string B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string& in) {
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(B64[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(B64[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::string base64_decode(const std::string& in) {
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[(unsigned char)B64[i]] = i;

    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string json_get(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    while (pos < json.size() && json[pos] == ' ') pos++;
    if (pos >= json.size()) return "";

    if (json[pos] == '"') {
        std::string val;
        pos++;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                char c = json[pos + 1];
                if (c == 'n') val += '\n';
                else if (c == 'r') val += '\r';
                else if (c == 't') val += '\t';
                else val += c;
                pos += 2;
            } else {
                val += json[pos++];
            }
        }
        return val;
    }

    if (json[pos] == '[' || json[pos] == '{') {
        int depth = 0;
        size_t start = pos;
        while (pos < json.size()) {
            if (json[pos] == '[' || json[pos] == '{') depth++;
            else if (json[pos] == ']' || json[pos] == '}') {
                depth--;
                if (depth == 0) { pos++; break; }
            }
            pos++;
        }
        return json.substr(start, pos - start);
    }

    size_t end = json.find_first_of(",}", pos);
    return json.substr(pos, end - pos);
}

bool json_ok(const std::string& json) {
    return json.find("\"ok\":true") != std::string::npos;
}

class Connection {
public:
    int fd = -1;

    bool connect_to(const std::string& host, int port) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
        if (rc != 0 || !res) return false;

        for (addrinfo* p = res; p; p = p->ai_next) {
            fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (fd < 0) continue;
            if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
                freeaddrinfo(res);
                return true;
            }
            close(fd);
            fd = -1;
        }

        freeaddrinfo(res);
        return false;
    }

    bool send_line(const std::string& line) {
        std::string data = line + "\n";
        size_t total = 0;
        while (total < data.size()) {
            ssize_t n = send(fd, data.data() + total, data.size() - total, 0);
            if (n <= 0) return false;
            total += (size_t)n;
        }
        return true;
    }

    std::string read_line() {
        std::string out;
        char c;
        while (out.size() < MAX_LINE_BYTES) {
            ssize_t n = recv(fd, &c, 1, 0);
            if (n <= 0) return "";
            if (c == '\n') break;
            if (c != '\r') out.push_back(c);
        }
        return out;
    }

    void disconnect() {
        if (fd >= 0) close(fd);
        fd = -1;
    }
};

void print_help() {
    std::cout << ansi::bold << "Commands\n" << ansi::reset
              << "  list                         list connected agents\n"
              << "  use <device_id>              select a connected agent\n"
              << "  ping                         check agent connection\n"
              << "  approve [minutes]            show approval popup on their PC\n"
              << "  status                       show approval/session status\n"
              << "  sysinfo                      show basic system info, after approval\n"
              << "  processes                    show top processes, after approval\n"
              << "  screenshot [file.png]        save screenshot, after approval\n"
              << "  msg <text>                   show a message on their PC\n"
              << "  end                          end approved support session\n"
              << "  help                         show this help\n"
              << "  quit                         exit\n";
}

void print_err(const std::string& msg) {
    std::cout << ansi::red << "✗ " << msg << ansi::reset << "\n";
}

void print_ok(const std::string& msg) {
    std::cout << ansi::green << "✓ " << msg << ansi::reset << "\n";
}

std::string relay_request(Connection& conn, const std::string& request) {
    if (!conn.send_line(request)) return "ERR failed to send request";
    std::string line = conn.read_line();
    if (line.empty()) return "ERR relay disconnected";
    return line;
}

std::string send_agent_command(Connection& conn, const std::string& device_id, const std::string& command) {
    std::string req = "CMD " + device_id + " " + base64_encode(command);
    std::string line = relay_request(conn, req);
    if (line.rfind("OK ", 0) == 0) {
        return base64_decode(line.substr(3));
    }
    return "{\"ok\":false,\"error\":\"" + line + "\"}";
}

void handle_list(Connection& conn) {
    std::string line = relay_request(conn, "LIST");
    if (line.rfind("OK ", 0) != 0) {
        print_err(line);
        return;
    }

    std::string json = base64_decode(line.substr(3));
    std::string agents = json_get(json, "agents");
    if (agents.empty() || agents == "[]") {
        std::cout << ansi::yellow << "No agents connected.\n" << ansi::reset;
        return;
    }

    std::cout << ansi::bold << "Connected agents:\n" << ansi::reset;
    size_t pos = 0;
    while ((pos = agents.find('{', pos)) != std::string::npos) {
        size_t end = agents.find('}', pos);
        if (end == std::string::npos) break;
        std::string entry = agents.substr(pos, end - pos + 1);
        std::cout << "  " << ansi::cyan << json_get(entry, "device_id") << ansi::reset
                  << ansi::dim << "  from " << json_get(entry, "peer") << ansi::reset << "\n";
        pos = end + 1;
    }
}

void handle_response_print(const std::string& json) {
    if (!json_ok(json)) {
        print_err(json_get(json, "error"));
        return;
    }
    std::string data = json_get(json, "data");
    if (!data.empty()) {
        std::cout << ansi::dim << "─────────────────────────────────────────\n" << ansi::reset;
        std::cout << data << "\n";
        std::cout << ansi::dim << "─────────────────────────────────────────\n" << ansi::reset;
    } else {
        print_ok("OK");
    }
}

void handle_screenshot(Connection& conn, const std::string& device_id, const std::string& out_path) {
    std::cout << ansi::yellow << "Requesting screenshot...\n" << ansi::reset;
    std::string json = send_agent_command(conn, device_id, "screenshot");
    if (!json_ok(json)) {
        print_err(json_get(json, "error"));
        return;
    }

    std::string b64 = json_get(json, "base64");
    if (b64.empty()) {
        print_err("response did not contain image data");
        return;
    }

    std::string png = base64_decode(b64);
    std::string path = trim(out_path).empty() ? "grandcontrol_screenshot.png" : trim(out_path);
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        print_err("cannot write file: " + path);
        return;
    }
    f.write(png.data(), png.size());
    print_ok("Saved screenshot to " + path);
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <relay-host> <relay-port> <admin-token>\n";
        return 1;
    }

    std::string host = argv[1];
    int port = std::atoi(argv[2]);
    std::string token = argv[3];

    Connection conn;
    if (!conn.connect_to(host, port)) {
        print_err("Could not connect to relay");
        return 1;
    }

    conn.send_line("ADMIN " + token);
    std::string hello = conn.read_line();
    if (hello.rfind("OK", 0) != 0) {
        print_err("Relay login failed: " + hello);
        return 1;
    }

    std::cout << ansi::cyan << ansi::bold << "GrandControl relay client\n" << ansi::reset;
    print_help();

    std::string selected_device;
    while (true) {
        std::cout << ansi::cyan << "grandcontrol" << ansi::reset;
        if (!selected_device.empty()) std::cout << ansi::yellow << "[" << selected_device << "]" << ansi::reset;
        std::cout << "> ";

        std::string line;
        if (!std::getline(std::cin, line)) break;
        line = trim(line);
        if (line.empty()) continue;

        auto [cmd, args] = split_first(line, ' ');
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c){ return (char)std::tolower(c); });

        if (cmd == "quit" || cmd == "exit") break;
        if (cmd == "help" || cmd == "?") { print_help(); continue; }
        if (cmd == "list") { handle_list(conn); continue; }
        if (cmd == "use") {
            selected_device = trim(args);
            if (selected_device.empty()) print_err("usage: use <device_id>");
            else print_ok("Selected " + selected_device);
            continue;
        }

        if (selected_device.empty()) {
            print_err("No device selected. Run: list, then use <device_id>");
            continue;
        }

        if (cmd == "screenshot") {
            handle_screenshot(conn, selected_device, args);
            continue;
        }

        // Forward allowed user commands to the selected agent.
        std::string json = send_agent_command(conn, selected_device, line);
        handle_response_print(json);
    }

    relay_request(conn, "QUIT");
    conn.disconnect();
    return 0;
}
