#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sw/redis++/redis++.h>

using namespace std::chrono;

static std::atomic<bool> running{true};

static void on_signal(int) {
    running = false;
}

static long long now_ms() {
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : fallback;
}

static long long envll(const char* name, long long fallback) {
    const char* v = std::getenv(name);
    if (v && *v) {
        try { return std::stoll(v); } catch (...) {}
    }
    return fallback;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const int num_tags   = (argc > 1) ? std::atoi(argv[1]) : 30;
    const int period_ms  = (argc > 2) ? std::atoi(argv[2]) : 100;
    const int duration_s = (argc > 3) ? std::atoi(argv[3]) : 10000; // 0 = бесконечно

    if (num_tags <= 0 || period_ms <= 0) {
        std::cerr << "usage: loader [num_tags] [period_ms] [duration_sec]\n";
        return 2;
    }

    // --- настройки тримминга stream
    const long long stream_retention_ms = envll("STREAM_RETENTION_MS", 20LL * 60 * 1000); // 20 минут
    const long long trim_every_ms       = envll("STREAM_TRIM_EVERY_MS", 1000);            // раз в 1s

    // (опционально) страховка по длине: 0 = выключено
    const long long stream_maxlen = envll("STREAM_MAXLEN", 0);

    long long last_trim_wall_ms = 0;

    const std::string uri = std::string("tcp://") + env_or("REDIS_HOST", "redis") + ":6379";
    const std::string stream = "sensor_data";

    try {
        sw::redis::Redis redis(uri);
        std::random_device rd;
        std::mt19937 gen(rd());

        std::uniform_int_distribution<int> distrib(5, 15);
        std::normal_distribution<double> noise(0.0, 0.5);

        auto period = milliseconds(period_ms);
        auto next   = steady_clock::now();
        auto start  = next;
        auto until  = (duration_s > 0) ? (start + seconds(duration_s)) : time_point<steady_clock>::max();

        long long written = 0;
        auto last_stat = start;
        

        std::cout << "loader tags=" << num_tags
                  << " period_ms=" << period_ms
                  << " duration_s=" << duration_s
                  << " redis=" << uri
                  << " stream_retention_ms=" << stream_retention_ms
                  << " trim_every_ms=" << trim_every_ms
                  << " stream_maxlen=" << stream_maxlen
                  << "\n";

        while (running && steady_clock::now() < until) {
            for (int tag = 0; tag < num_tags && running; ++tag) {
                // время берём отдельно для каждого тега
                const long long t_ms = now_ms();
                

                const double value = distrib(gen) + noise(gen);

                std::vector<std::pair<std::string, std::string>> fields{
                    {"tag_id",     std::to_string(tag)},
                    {"value",      std::to_string(value)},
                    {"event_time", std::to_string(t_ms)},
                    {"quality",    "OK"}
                };

                redis.xadd(stream, "*", fields.begin(), fields.end());
                ++written;
            }

            auto now = steady_clock::now();
            if (now - last_stat >= seconds(5)) {
                std::cout << "written=" << written
                          << " elapsed_s=" << duration_cast<seconds>(now - start).count()
                          << "\n";
                last_stat = now;
            }

            // --- XTRIM по времени (MINID ~ <cut_ms>-0)
            const long long wall_ms = now_ms();
            if (stream_retention_ms > 0 && trim_every_ms > 0 &&
                (wall_ms - last_trim_wall_ms) >= trim_every_ms) {

                const long long cut_ms = wall_ms - stream_retention_ms;
                if (cut_ms > 0) {
                    redis.command("XTRIM", stream, "MINID", "~", std::to_string(cut_ms) + "-0");
                }

                if (stream_maxlen > 0) {
                    redis.command("XTRIM", stream, "MAXLEN", "~", std::to_string(stream_maxlen));
                }

                last_trim_wall_ms = wall_ms;
            }

            next += period;
            if (next < now) next = now;
            std::this_thread::sleep_until(next);
        }

        std::cout << "stopped, written=" << written << "\n";
        return 0;

    } catch (const sw::redis::Error& e) {
        std::cerr << "Redis error: " << e.what() << "\n";
        return 1;
    }
}