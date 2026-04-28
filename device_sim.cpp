#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>

// atomic gives well-defined behaviour for reads/writes shared between
// the main loop and the signal handler. Without it, the access is UB.
std::atomic<bool> g_stop{false};

void handle_sigint(int) { g_stop.store(true); }

// sigaction is used instead of signal() because signal() resets the handler to default after the first signal on some systems.
void install_sigint_handler() {
    struct sigaction sa{};
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
}

// Loops over send() until all bytes are delivered or an error occurs.
// MSG_NOSIGNAL prevents SIGPIPE if the remote end closes the connection.
bool send_all(int fd, const std::string &data) {
    const char *ptr = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        ssize_t sent = send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (sent <= 0) return false;
        ptr += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
    return true;
}

// Runtime configuration with defaults.
struct Config {
    std::string host{"127.0.0.1"};
    std::string port{"9000"};
    std::string device_id{"bed-1"};
    int interval_ms{1000};
};

Config parse_args(int argc, char *argv[]) {
    Config cfg;
    int i = 1;  // start at 1 to skip argv[0] (the program name)

    // Lambda that captures i and argc by reference so it can advance i.
    // [&] means the lambda can read/write variables from the surrounding scope.
    // It peeks ahead to ensure a value follows the flag, then returns it.
    auto next_arg = [&](const char *flag) -> std::string {
        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << flag << "\n";
            std::exit(1);
        }
        return argv[++i];  // advance i, then return the value at the new position
    };

    for (; i < argc; ++i) {
        std::string arg{argv[i]}; 
        if (arg == "--host") {
            cfg.host = next_arg("--host");
        } else if (arg == "--port") {
            cfg.port = next_arg("--port");
        } else if (arg == "--id") {
            cfg.device_id = next_arg("--id");
        } else if (arg == "--interval") {
            // std::stoi converts a string like "500" to int 500
            cfg.interval_ms = std::stoi(next_arg("--interval"));
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            std::exit(1);
        }
    }
    return cfg;  // returns a copy; Config is small so this is fine
}

// Resolves host/port via getaddrinfo and returns a connected socket fd, or -1.
// getaddrinfo handles hostnames, IPv4, and IPv6; gethostbyname does not.
int tcp_connect(const std::string &host, const std::string &port) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *res = nullptr;
    int err = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (err != 0) {
        std::cerr << "getaddrinfo: " << gai_strerror(err) << "\n";
        return -1;
    }

    int fd = -1;
    for (addrinfo *rp = res; rp != nullptr; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

struct Vitals {
    double hr{72.0};    // heart rate (bpm)
    double spo2{98.0};  // oxygen saturation (%)
    double sys{120.0};  // systolic blood pressure (mmHg)
    double dia{80.0};   // diastolic blood pressure (mmHg)
};

double clamp(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Adds a small random nudge to each vital and clamps to a realistic range.
void random_walk(Vitals &v, std::mt19937 &rng) {
    std::normal_distribution<double> jitter{0.0, 0.8};
    v.hr   = clamp(v.hr   + jitter(rng), 50.0, 110.0);  // 50: athletic bradycardia; 110: mild tachycardia
    v.spo2 = clamp(v.spo2 + jitter(rng), 90.0, 100.0);  // below 90% is clinical hypoxemia
    v.sys  = clamp(v.sys  + jitter(rng), 90.0, 160.0);  // 90: hypotension threshold; 160: stage 2 hypertension
    v.dia  = clamp(v.dia  + jitter(rng), 60.0, 100.0);  // 60: low diastolic; 100: hypertension threshold
}

// Builds a newline-terminated JSON string (NDJSON).
// The trailing '\n' lets a receiver split the TCP stream into individual messages.
std::string build_json(const std::string &device_id, const Vitals &v) {
    std::ostringstream oss;
    oss << "{\"device_id\":\"" << device_id << "\""
        << ",\"ts\":"   << static_cast<long long>(std::time(nullptr))
        << ",\"hr\":"   << static_cast<int>(v.hr)
        << ",\"spo2\":" << static_cast<int>(v.spo2)
        << ",\"sys\":"  << static_cast<int>(v.sys)
        << ",\"dia\":"  << static_cast<int>(v.dia)
        << "}\n";
    return oss.str();
}

int main(int argc, char *argv[]) {
    install_sigint_handler();

    Config cfg = parse_args(argc, argv);

    int fd = tcp_connect(cfg.host, cfg.port);
    if (fd == -1) {
        std::cerr << "Could not connect to " << cfg.host << ":" << cfg.port << "\n";
        return 1;
    }
    std::cout << "Connected to " << cfg.host << ":" << cfg.port
              << " as " << cfg.device_id << "\n";

    // Seed from the OS entropy source so each run produces a different sequence.
    std::mt19937 rng{std::random_device{}()};
    Vitals vitals{};
    auto interval = std::chrono::milliseconds{cfg.interval_ms};

    while (!g_stop.load()) {
        std::string line = build_json(cfg.device_id, vitals);
        if (!send_all(fd, line)) {
            std::cerr << "peer disconnected\n";
            break;
        }

        // Sleep in 50 ms steps so Ctrl-C is noticed quickly.
        auto deadline = std::chrono::steady_clock::now() + interval;
        while (!g_stop.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }

        random_walk(vitals, rng);
    }

    close(fd);
    std::cout << "\nDisconnected cleanly.\n";
    return 0;
}
