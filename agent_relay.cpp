// GrandControl outbound agent for Windows.
//
// Compile with MinGW (silent background build):
//   g++ agent_relay.cpp -std=c++17 -lws2_32 -lgdi32 -mwindows -static -o GrandControlAgent.exe
//
// The agent connects OUT to your relay — it never listens on a public port.
// It identifies itself using the Windows computer name (no token needed).

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")

constexpr size_t MAX_LINE_BYTES = 24 * 1024 * 1024;

struct Config {
    std::string relay_host = "127.0.0.1";
    int relay_port         = 8443;
    int reconnect_seconds  = 5;
};

std::string g_log_path;

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

std::string timestamp() {
    time_t now = time(nullptr);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    return buf;
}

void log_line(const std::string& msg) {
    if (g_log_path.empty()) return;
    std::ofstream f(g_log_path, std::ios::app);
    if (f) f << "[" << timestamp() << "] " << msg << "\n";
}

std::string get_exe_dir() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p(path);
    size_t slash = p.find_last_of("\\/");
    if (slash == std::string::npos) return ".";
    return p.substr(0, slash);
}

std::string get_local_appdata_dir() {
    char* local = nullptr;
    size_t len = 0;
    _dupenv_s(&local, &len, "LOCALAPPDATA");
    std::string out = local ? local : ".";
    if (local) free(local);
    return out;
}

// Use the Windows computer name as the device ID (lowercase).
std::string get_device_id() {
    char buf[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = sizeof(buf);
    if (GetComputerNameA(buf, &size)) {
        std::string id(buf);
        std::transform(id.begin(), id.end(), id.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        return id;
    }
    return "unknown-pc";
}

std::string base64_decode(const std::string& in); // forward declaration

Config load_config() {
    Config cfg;
    std::vector<std::string> paths = {
        get_exe_dir() + "\\config.ini",
        get_local_appdata_dir() + "\\GrandControl\\config.ini"
    };

    for (const auto& path : paths) {
        std::ifstream f(path);
        if (!f) continue;

        // Read entire file - it is a single base64-encoded blob
        std::string raw((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        raw = trim(raw);

        // Decode from base64 to get the plain ini text
        std::string ini = base64_decode(raw);

        std::istringstream ss(ini);
        std::string line;
        while (std::getline(ss, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            auto [k, v] = split_first(line, '=');
            k = trim(k); v = trim(v);
            if      (k == "relay_host")        cfg.relay_host        = v;
            else if (k == "relay_port")        cfg.relay_port        = std::stoi(v);
            else if (k == "reconnect_seconds") cfg.reconnect_seconds = std::stoi(v);
        }
        return cfg;
    }
    return cfg;
}

// ─── JSON helpers ─────────────────────────────────────────────────────────────

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char hex[8];
                    snprintf(hex, sizeof(hex), "\\u%04x", c);
                    out += hex;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string json_ok(const std::string& data) {
    return "{\"ok\":true,\"data\":\"" + json_escape(data) + "\"}";
}

std::string json_err(const std::string& msg) {
    return "{\"ok\":false,\"error\":\"" + json_escape(msg) + "\"}";
}

// ─── Base64 ──────────────────────────────────────────────────────────────────

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

// ─── Socket helpers ──────────────────────────────────────────────────────────

bool send_all(SOCKET s, const std::string& data) {
    size_t total = 0;
    while (total < data.size()) {
        int n = send(s, data.data() + total,
                     (int)std::min<size_t>(data.size() - total, 65536), 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

std::string read_line(SOCKET s) {
    std::string out;
    char c;
    while (out.size() < MAX_LINE_BYTES) {
        int n = recv(s, &c, 1, 0);
        if (n <= 0) return "";
        if (c == '\n') break;
        if (c != '\r') out.push_back(c);
    }
    return out;
}

SOCKET connect_to_relay(const Config& cfg) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(cfg.relay_host.c_str(),
                         std::to_string(cfg.relay_port).c_str(), &hints, &res);
    if (rc != 0 || !res) return INVALID_SOCKET;

    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* p = res; p; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == INVALID_SOCKET) continue;
        if (connect(sock, p->ai_addr, (int)p->ai_addrlen) == 0) break;
        closesocket(sock);
        sock = INVALID_SOCKET;
    }

    freeaddrinfo(res);
    return sock;
}

// ─── PowerShell runner ───────────────────────────────────────────────────────

std::string run_fixed_powershell(const std::string& ps) {
    std::string full_cmd =
        "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \""
        + ps + "\" 2>&1";

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead, hWrite;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return "ERROR: pipe failed";

    STARTUPINFOA si{};
    si.cb          = sizeof(si);
    si.hStdOutput  = hWrite;
    si.hStdError   = hWrite;
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::vector<char> mutable_cmd(full_cmd.begin(), full_cmd.end());
    mutable_cmd.push_back('\0');

    if (!CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hWrite);
        CloseHandle(hRead);
        return "ERROR: CreateProcess failed (" + std::to_string(GetLastError()) + ")";
    }

    CloseHandle(hWrite);

    std::string output;
    char buf[4096];
    DWORD bytesRead = 0;
    while (ReadFile(hRead, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        output += buf;
        if (output.size() > 4 * 1024 * 1024) {
            output += "\n[output truncated]";
            break;
        }
    }

    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);

    if (output.empty()) output = "(no output)";
    return output;
}

// ─── Commands ─────────────────────────────────────────────────────────────────

std::string cmd_sysinfo() {
    std::string info = run_fixed_powershell(
        "Write-Host ('OS: ' + (Get-CimInstance Win32_OperatingSystem).Caption);"
        "Write-Host ('User: ' + $env:USERNAME);"
        "Write-Host ('Computer: ' + $env:COMPUTERNAME);"
        "$os = Get-CimInstance Win32_OperatingSystem;"
        "Write-Host ('Uptime: ' + ((Get-Date) - $os.LastBootUpTime));"
        "Write-Host ('RAM free: ' + [math]::Round($os.FreePhysicalMemory/1MB,1) + ' GB / ' + [math]::Round($os.TotalVisibleMemorySize/1MB,1) + ' GB');"
        "$disk = Get-PSDrive C;"
        "Write-Host ('Disk C free: ' + [math]::Round($disk.Free/1GB,1) + ' GB / ' + [math]::Round(($disk.Used+$disk.Free)/1GB,1) + ' GB');"
        "Write-Host ('IPv4: ' + ((Get-NetIPAddress -AddressFamily IPv4 | Where-Object {$_.IPAddress -ne '127.0.0.1'} | Select-Object -First 1).IPAddress))"
    );
    return json_ok(info);
}

std::string cmd_processes() {
    std::string info = run_fixed_powershell(
        "Get-Process | Sort-Object CPU -Descending | Select-Object -First 25 | "
        "Format-Table -AutoSize Name,Id,CPU,WorkingSet | Out-String"
    );
    return json_ok(info);
}

std::string cmd_screenshot() {
    char temp_path[MAX_PATH]{};
    GetTempPathA(MAX_PATH, temp_path);
    std::string png_path = std::string(temp_path) + "gc_screen.png";

    std::string ps =
        "Add-Type -AssemblyName System.Windows.Forms,System.Drawing;"
        "$s=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds;"
        "$b=New-Object System.Drawing.Bitmap($s.Width,$s.Height);"
        "$g=[System.Drawing.Graphics]::FromImage($b);"
        "$g.CopyFromScreen($s.Location,[System.Drawing.Point]::Empty,$s.Size);"
        "$b.Save('" + png_path + "',[System.Drawing.Imaging.ImageFormat]::Png);"
        "$b.Dispose();$g.Dispose();"
        "[Convert]::ToBase64String([System.IO.File]::ReadAllBytes('" + png_path + "'))";

    std::string b64 = trim(run_fixed_powershell(ps));
    DeleteFileA(png_path.c_str());

    if (b64.empty() || b64.rfind("ERROR", 0) == 0)
        return json_err("Screenshot failed: " + b64);

    return "{\"ok\":true,\"format\":\"png\",\"base64\":\"" + b64 + "\"}";
}

std::string cmd_message(const std::string& args) {
    std::string text = trim(args).empty()
        ? "Your family member is trying to help with this computer."
        : args;
    MessageBoxA(nullptr, text.c_str(), "GrandControl Message",
                MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    return json_ok("Message shown.");
}

std::string dispatch(const std::string& raw) {
    std::string line = trim(raw);
    if (line.empty()) return json_err("empty command");

    auto [cmd, args] = split_first(line, ' ');
    std::transform(cmd.begin(), cmd.end(), cmd.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });

    log_line("Command: " + cmd);

    if (cmd == "ping")        return json_ok("pong");
    if (cmd == "sysinfo")     return cmd_sysinfo();
    if (cmd == "processes")   return cmd_processes();
    if (cmd == "screenshot")  return cmd_screenshot();
    if (cmd == "msg")         return cmd_message(args);

    return json_err("Unknown command: " + cmd);
}

// ─── Main loop ───────────────────────────────────────────────────────────────

void agent_loop(const Config& cfg) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;

    std::string device_id = get_device_id();
    log_line("Device ID: " + device_id);

    while (true) {
        SOCKET sock = connect_to_relay(cfg);
        if (sock == INVALID_SOCKET) {
            log_line("Could not connect to relay; retrying");
            std::this_thread::sleep_for(std::chrono::seconds(cfg.reconnect_seconds));
            continue;
        }

        log_line("Connected to relay as " + device_id);

        // Identify with just the device name — no token.
        std::string hello = "AGENT " + device_id + "\n";
        if (!send_all(sock, hello)) {
            closesocket(sock);
            continue;
        }

        std::string response = read_line(sock);
        if (response.rfind("OK", 0) != 0) {
            log_line("Relay rejected agent: " + response);
            closesocket(sock);
            std::this_thread::sleep_for(std::chrono::seconds(cfg.reconnect_seconds));
            continue;
        }

        while (true) {
            std::string line = read_line(sock);
            if (line.empty()) break;

            auto [verb, rest] = split_first(line, ' ');
            if (verb != "REQ") continue;
            auto [request_id, command_b64] = split_first(rest, ' ');
            if (request_id.empty() || command_b64.empty()) continue;

            std::string command     = base64_decode(command_b64);
            std::string result_json;
            try {
                result_json = dispatch(command);
            } catch (const std::exception& e) {
                result_json = json_err(e.what());
            } catch (...) {
                result_json = json_err("unknown error");
            }

            std::string out = "RESP " + request_id + " " + base64_encode(result_json) + "\n";
            if (!send_all(sock, out)) break;
        }

        log_line("Disconnected from relay; reconnecting");
        closesocket(sock);
        std::this_thread::sleep_for(std::chrono::seconds(cfg.reconnect_seconds));
    }

    WSACleanup();
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    std::string appdir = get_local_appdata_dir() + "\\GrandControl";
    CreateDirectoryA(appdir.c_str(), nullptr);
    g_log_path = appdir + "\\agent.log";

    Config cfg = load_config();
    log_line("GrandControl agent starting");
    agent_loop(cfg);
    return 0;
}

// Console build fallback.
int main() {
    std::string appdir = get_local_appdata_dir() + "\\GrandControl";
    CreateDirectoryA(appdir.c_str(), nullptr);
    g_log_path = appdir + "\\agent.log";

    Config cfg = load_config();
    log_line("GrandControl agent starting");
    agent_loop(cfg);
    return 0;
}
