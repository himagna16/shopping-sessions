// Writes a synthetic dataset in the layout of /data, shaped like the contest's
// inputs (ID forms, the three timestamp forms, quoting, duplicates, dirt), for
// training a profile-guided build inside the image, where no real data exists.
// It is not the organizers' generator and does not try to match its output.
// Usage: synth OUT_DIR EVENTS
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <sys/stat.h>

typedef uint64_t u64;
typedef int64_t i64;

static u64 g_state = 0x9E3779B97F4A7C15ULL;
static inline u64 rnd() {
    u64 z = (g_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static inline u64 below(u64 n) { return (unsigned __int128)rnd() * n >> 64; }
static inline bool chance(double p) { return double(rnd() >> 11) / double(1ULL << 53) < p; }

static void base36(std::string& out, u64 v) {
    char buf[13];
    int at = 13;
    do { buf[--at] = "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36]; v /= 36; } while (v);
    out.append(buf + at, size_t(13 - at));
}
static u64 scramble(u64 v, u64 key) {
    const u64 mask = (1ULL << 40) - 1;
    v = ((v & mask) * 0x9E3779B97F4BULL ^ key) & mask;
    v ^= v >> 19;
    return v * 0xC2B2AE3D27D5ULL & mask;
}

static void civil(std::string& out, i64 ms, char sep) {
    i64 days = ms / 86400000, sod = ms % 86400000 / 1000;
    i64 z = days + 719468, era = z / 146097, doe = z - era * 146097;
    i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    i64 d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (m <= 2);
    char buf[32];
    snprintf(buf, sizeof buf, "%04lld-%02lld-%02lld%c%02lld:%02lld:%02lld", (long long)y, (long long)m, (long long)d, sep,
             (long long)(sod / 3600), (long long)(sod / 60 % 60), (long long)(sod % 60));
    out += buf;
}
static void timestamp(std::string& out, i64 ms) {
    u64 pick = below(10);
    if (ms % 1000 || pick >= 7) { out += std::to_string(ms); return; }
    if (pick == 6) {
        static const int offsets[] = {-600, -480, -300, -240, 0, 60, 120, 330, 480, 540, 720};
        int off = offsets[below(11)];
        civil(out, ms + i64(off) * 60000, 'T');
        char buf[8];
        snprintf(buf, sizeof buf, "%c%02d:%02d", off < 0 ? '-' : '+', abs(off) / 60, abs(off) % 60);
        out += buf;
        return;
    }
    civil(out, ms, 'T');
    out += 'Z';
}

struct Files {
    std::vector<std::string> data;
    explicit Files(size_t n) : data(n) {}
    // Fields joined by commas, each quoted one time in five; rarely malformed.
    void row(const std::vector<std::string>& fields) {
        std::string& out = data[below(data.size()) * below(data.size()) / data.size()];   // uneven file sizes
        std::string line;
        for (size_t i = 0; i < fields.size(); i++) {
            if (i) line += ',';
            bool quote = chance(0.2);
            if (quote) line += '"';
            line += fields[i];
            if (quote) line += '"';
        }
        if (chance(0.0005)) {
            switch (below(4)) {
                case 0: line += ",x"; break;
                case 1: line.insert(0, "\""); break;
                case 2: line += '"'; break;
                default: line.clear();
            }
        }
        out += line;
        out += '\n';
    }
    void write(const std::string& dir, const char* header) {
        mkdir(dir.c_str(), 0777);
        for (size_t i = 0; i < data.size(); i++) {
            // Shuffle the lines so no file is ordered by user or time.
            std::vector<size_t> starts{0};
            for (size_t k = 0; k < data[i].size(); k++) if (data[i][k] == '\n') starts.push_back(k + 1);
            size_t n = starts.size() - 1;
            std::vector<size_t> order(n);
            for (size_t k = 0; k < n; k++) order[k] = k;
            for (size_t k = n; k > 1; k--) std::swap(order[k - 1], order[below(k)]);
            char name[64];
            snprintf(name, sizeof name, "/part-%05zu.csv", i);
            FILE* f = fopen((dir + name).c_str(), "wb");
            if (!f) { perror(name); exit(1); }
            fprintf(f, "%s\n", header);
            for (size_t k : order) fwrite(data[i].data() + starts[k], 1, starts[k + 1] - starts[k], f);
            fclose(f);
        }
    }
};

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: synth OUT_DIR EVENTS\n"); return 2; }
    std::string out = argv[1];
    const u64 total = strtoull(argv[2], nullptr, 10);
    mkdir(out.c_str(), 0777);
    const i64 day = 86400000, start = 20800LL * day, end = start + 14 * day;      // a window in late 2026
    const u64 ukey = rnd(), ekey = rnd(), ckey = rnd();
    Files events(30), clicks(16), links(8);
    u64 event_no = 0, click_no = 0;
    for (u64 user = 0; event_no < total; user++) {
        u64 naliases = below(100) < 45 ? 1 : 2 + below(3);
        std::vector<std::string> ids;
        for (u64 a = 0; a < naliases; a++) {
            std::string id = "u";
            base36(id, scramble(user * 512 + a, ukey));
            ids.push_back(id);
            if (a) {
                links.row({ids[a], ids[below(a)]});
                if (chance(0.08)) links.row({ids[below(a)], ids[a]});
            }
        }
        u64 nsessions = 1 + below(10);
        bool clicks_ads = chance(0.65);
        for (u64 s = 0; s < nsessions; s++) {
            i64 ts = start + i64(below(14 * 12)) * 7200000 + i64(below(1500)) * 1000 + (chance(0.03) ? i64(below(999)) : 0);
            if (clicks_ads && chance(0.6)) {
                u64 n = 1 + below(3);
                for (u64 c = 0; c < n; c++) {
                    std::string id = "k", campaign = "c_", when;
                    base36(id, scramble(click_no++, ckey));
                    campaign += std::to_string(chance(0.005) ? 2001 + below(7000) : chance(0.3) ? 1 + below(4) : 1 + below(2000));
                    timestamp(when, ts - i64(below(8 * 86400)) * 1000);
                    std::vector<std::string> row{id, chance(0.002) ? "" : ids[below(naliases)], campaign, when};
                    clicks.row(row);
                    if (chance(0.02)) clicks.row(row);
                }
            }
            u64 nevents = 1 + below(10);
            for (u64 e = 0; e < nevents && event_no < total; e++) {
                ts += i64(1 + below(240)) * 1000;
                std::string id = "e", when, ingest, product = "p", price;
                base36(id, scramble(event_no++, ekey));
                if (chance(0.005)) when = chance(0.5) ? "" : "2026-13-01T00:00:00Z"; else timestamp(when, ts);
                i64 seen = ts - ts % 1000 + i64(below(600)) * 1000 + (chance(0.01) ? day + 1000 + i64(below(86400)) * 1000 : 0);
                civil(ingest, seen, 'T');
                ingest += 'Z';
                u64 kind = below(100);
                const char* type = chance(0.001) ? "View" : kind < 3 ? "purchase" : kind < 13 ? "cart" : "view";
                u64 pid = 1 + below(below(2) ? 500 : 50000);
                product += std::to_string(chance(0.005) ? 50001 + below(40000) : pid);
                u64 cents = 100 + pid % 49900;
                if (kind < 3) price = chance(0.05) ? "19.99" : std::to_string(cents);
                else if (chance(0.25)) price = std::to_string(cents);
                std::vector<std::string> row{id, chance(0.002) ? "" : ids[chance(0.5) ? 0 : below(naliases)], when, ingest, type, product, price};
                events.row(row);
                if (chance(0.03)) {
                    u64 copies = 1 + below(3);
                    for (u64 c = 0; c < copies; c++) {
                        row[3].clear();
                        civil(row[3], seen + i64(1 + below(600)) * 1000, 'T');
                        row[3] += 'Z';
                        events.row(row);
                    }
                }
            }
        }
    }
    events.write(out + "/events", "event_id,user_id,ts,ingest_ts,event_type,product_id,price_cents");
    clicks.write(out + "/clicks", "click_id,user_id,campaign_id,ts");
    links.write(out + "/links", "user_id_a,user_id_b");
    FILE* f = fopen((out + "/products.csv").c_str(), "wb");
    fprintf(f, "product_id,category\n");
    static const char* cats[] = {"apparel", "automotive", "baby", "beauty", "books", "electronics", "garden", "grocery", "home", "jewelry",
                                 "kitchen", "music", "office", "outdoors", "pets", "shoes", "sports", "tools", "toys", "video_games"};
    for (int i = 1; i <= 50000; i++) fprintf(f, "p%d,%s\n", i, cats[below(20)]);
    fclose(f);
    f = fopen((out + "/campaigns.csv").c_str(), "wb");
    fprintf(f, "campaign_id,channel\n");
    static const char* channels[] = {"affiliate", "display", "email", "search", "social"};
    for (int i = 1; i <= 2000; i++) fprintf(f, "c_%d,%s\n", i, channels[below(5)]);
    fclose(f);
    f = fopen((out + "/manifest.json").c_str(), "wb");
    std::string a, b;
    civil(a, start, 'T');
    civil(b, end, 'T');
    fprintf(f, "{\n  \"window_start\": \"%sZ\",\n  \"window_end\": \"%sZ\"\n}\n", a.c_str(), b.c_str());
    fclose(f);
    return 0;
}
