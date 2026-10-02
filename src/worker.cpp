#include <sw/redis++/redis++.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

static std::string envs(const char *k, const std::string &def) {
    const char *v = std::getenv(k);
    return (v && *v) ? std::string(v) : def;
}
static long long envll(const char *k, long long def) {
    const char *v = std::getenv(k);
    if (v && *v) { try { return std::stoll(v); } catch (...) {} }
    return def;
}
static bool has(const std::string &s, const char *needle) {
    return s.find(needle) != std::string::npos;
}
static long long id_ms(const std::string &id) {
    auto p = id.find('-');
    if (p == std::string::npos) return -1;
    try { return std::stoll(id.substr(0, p)); } catch (...) { return -1; }
}
static long long bucket_start(long long ms, long long bucket_ms) {
    return (ms < 0) ? -1 : (ms / bucket_ms) * bucket_ms;
}

static void xgroup_create(sw::redis::Redis &r, const std::string &stream,
                         const std::string &group, const std::string &start) {
    try {
        r.command("XGROUP", "CREATE", stream, group, start, "MKSTREAM");
    } catch (const sw::redis::ReplyError &e) {
        if (has(e.what(), "BUSYGROUP")) return;
        throw;
    }
}
static void xack(sw::redis::Redis &r, const std::string &stream,
                 const std::string &group, const std::string &id) {
    r.command("XACK", stream, group, id);
}

static void ts_create(sw::redis::Redis &r, const std::string &key, long long retention_ms) {
    try {
        r.command("TS.CREATE", key,
                  "RETENTION", std::to_string(retention_ms),
                  "DUPLICATE_POLICY", "LAST");
    } catch (const sw::redis::ReplyError &e) {
        if (has(e.what(), "already exists") || has(e.what(), "BUSYKEY")) return;
        throw;
    }
}
static void ts_rule(sw::redis::Redis &r, const std::string &src, const std::string &dst,
                    const std::string &agg, long long bucket_ms) {
    try {
        r.command("TS.CREATERULE", src, dst, "AGGREGATION", agg, std::to_string(bucket_ms));
    } catch (const sw::redis::ReplyError &e) {
        if (has(e.what(), "rule already exists")) return;
        throw;
    }
}
static void ts_add(sw::redis::Redis &r, const std::string &key, long long ts_ms,
                   const std::string &value) {
    r.command("TS.ADD", key, std::to_string(ts_ms), value, "ON_DUPLICATE", "LAST");
}
static void ts_incrby(sw::redis::Redis &r, const std::string &key, long long inc, long long ts_ms) {
    r.command("TS.INCRBY", key, std::to_string(inc), "TIMESTAMP", std::to_string(ts_ms));
}

int main() {
    const std::string host = envs("REDIS_HOST", "redis");
    const int port = (int)envll("REDIS_PORT", 6379);

    const std::string stream = envs("STREAM_KEY", "sensor_data");
    const std::string group = envs("STREAM_GROUP", "rts_group");
    const std::string consumer = envs("STREAM_CONSUMER", "worker-1");

    const long long raw_retention_ms = envll("RAW_RETENTION_MS", 10LL * 60 * 1000);
    const long long ds20_retention_ms = envll("DS_20S_RETENTION_MS", 60LL * 60 * 1000);
    const long long ds1m_retention_ms = envll("DS_1M_RETENTION_MS", 24LL * 60 * 60 * 1000);

    const long long bad_thr_ms = envll("BAD_TIME_THRESHOLD_MS", 60'000);
    const long long read_count = envll("READ_COUNT", 200);
    const long long block_ms = envll("READ_BLOCK_MS", 1000);

    const long long bucket20 = 20'000;
    const long long bucket1m = 60'000;

    sw::redis::ConnectionOptions opt;
    opt.host = host;
    opt.port = port;
    opt.socket_timeout = std::chrono::milliseconds(2000);

    sw::redis::Redis redis(opt);
    xgroup_create(redis, stream, group, "0");

    std::unordered_set<std::string> ensured;

    auto ensure_tag = [&](const std::string &tag) {
        if (ensured.count(tag)) return;

        std::string raw = "ts:raw:" + tag;

        std::string avg20 = "ts:avg:20s:" + tag;
        std::string min20 = "ts:min:20s:" + tag;
        std::string max20 = "ts:max:20s:" + tag;
        std::string cnt20 = "ts:cnt:20s:" + tag;
        std::string bad20 = "ts:bad_time:20s:" + tag;

        std::string avg1m = "ts:avg:1m:" + tag;
        std::string min1m = "ts:min:1m:" + tag;
        std::string max1m = "ts:max:1m:" + tag;
        std::string cnt1m = "ts:cnt:1m:" + tag;

        ts_create(redis, raw, raw_retention_ms);

        ts_create(redis, avg20, ds20_retention_ms);
        ts_create(redis, min20, ds20_retention_ms);
        ts_create(redis, max20, ds20_retention_ms);
        ts_create(redis, cnt20, ds20_retention_ms);
        ts_create(redis, bad20, ds20_retention_ms);

        ts_create(redis, avg1m, ds1m_retention_ms);
        ts_create(redis, min1m, ds1m_retention_ms);
        ts_create(redis, max1m, ds1m_retention_ms);
        ts_create(redis, cnt1m, ds1m_retention_ms);

        ts_rule(redis, raw, avg20, "avg", bucket20);
        ts_rule(redis, raw, min20, "min", bucket20);
        ts_rule(redis, raw, max20, "max", bucket20);
        ts_rule(redis, raw, cnt20, "count", bucket20);

        ts_rule(redis, raw, avg1m, "avg", bucket1m);
        ts_rule(redis, raw, min1m, "min", bucket1m);
        ts_rule(redis, raw, max1m, "max", bucket1m);
        ts_rule(redis, raw, cnt1m, "count", bucket1m);

        ensured.insert(tag);
    };

    using Attrs = std::vector<std::pair<std::string, std::string>>;
    using Msg   = std::pair<std::string, Attrs>;
    using Msgs  = std::vector<Msg>;
    using XReadResult = std::unordered_map<std::string, Msgs>;

    bool pending = true;
    std::string start_id = "0";

    while (true) {
        try {
            std::vector<std::pair<std::string, std::string>> streams = {{stream, start_id}};

            XReadResult res;


            auto timeout = pending ? std::chrono::milliseconds(1)   
                       : std::chrono::milliseconds(block_ms);

            redis.xreadgroup(group, consumer,
                            streams.begin(), streams.end(),
                            timeout,
                            read_count,
                            std::inserter(res, res.end()));

            size_t n_msgs = 0;
            for (const auto &kv : res) n_msgs += kv.second.size();

            if (n_msgs == 0) {
                if (pending) {
                    pending = false;
                    start_id = ">";
                    // можно лог, чтобы видеть переключение
                    // std::cerr << "[worker] pending done -> switch to >\n";
                }
                continue;
            }

            long long max_ms = -1;

            for (auto &kv : res) {
                for (auto &m : kv.second) {
                    const std::string &id = m.first;
                    const Attrs &a = m.second;

                    std::string tag, val, evt, q;
                    for (auto &p : a) {
                        if (p.first == "tag_id") tag = p.second;
                        else if (p.first == "value") val = p.second;
                        else if (p.first == "event_time") evt = p.second;
                        else if (p.first == "quality") q = p.second;
                    }
                    if (tag.empty() || val.empty()) { xack(redis, stream, group, id); continue; }

                    long long arrival = id_ms(id);
                    if (arrival < 0) { xack(redis, stream, group, id); continue; }

                    ensure_tag(tag);

                    std::string status = "OK";
                    long long delta = 0;
                    if (evt.empty()) status = "NO_EVENT_TIME";
                    else {
                        try {
                            long long ev = std::stoll(evt);
                            delta = arrival - ev;
                            if (std::llabs(delta) > bad_thr_ms) status = "BAD_TIME";
                        } catch (...) { status = "BAD_EVENT_TIME"; }
                    }

                    ts_add(redis, "ts:raw:" + tag, arrival, val);

                    if (status != "OK") {
                        long long b = bucket_start(arrival, bucket20);
                        if (b >= 0) ts_incrby(redis, "ts:bad_time:20s:" + tag, 1, b);
                    }

                    {
                        std::ostringstream ss;
                        ss << val << "," << q << "," << evt << "," << id << "," << status << "," << delta;
                        redis.hset("light:current_values", tag, ss.str());
                    }

                    if (arrival > max_ms) max_ms = arrival;
                    xack(redis, stream, group, id);
                }
            }

            if (max_ms >= 0) redis.set("rts:last_ms", std::to_string(max_ms));

        } catch (const sw::redis::Error &e) {
            std::cerr << "[worker] redis error: " << e.what() << "\n";
            std::this_thread::sleep_for(1s);
        } catch (const std::exception &e) {
            std::cerr << "[worker] exception: " << e.what() << "\n";
            std::this_thread::sleep_for(1s);
        }
    }
}