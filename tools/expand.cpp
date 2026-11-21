// Makes a larger dataset out of a small one, for benchmarking and for
// training profile-guided builds: COPIES copies of every row, each copy with
// its event/click/user IDs shifted so the copies are different events,
// clicks and people with the same shapes and distributions.
// Usage: expand SRC DST COPIES
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>

static std::string slurp(const std::string& path) {
    std::string out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

// Shifts the two characters after the ID's first letter within their own
// 36-symbol alphabet (0-9a-z, or 0-9A-Z for an upper-cased ID): copy k moves
// the first by k % 36 and the second by k / 36, so up to 1,296 copies of an ID
// stay distinct.
static inline void shift_char(char& c, bool upper, int by) {
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (!upper && c >= 'a' && c <= 'z') v = c - 'a' + 10;
    else if (upper && c >= 'A' && c <= 'Z') v = c - 'A' + 10;
    else return;
    v = (v + by) % 36;
    c = char(v < 10 ? '0' + v : (upper ? 'A' : 'a') + v - 10);
}
static inline void shift_id(char* field, char* end, int by) {
    if (field < end && *field == '"') field++;
    if (end - field < 2) return;
    const bool upper = field[0] >= 'A' && field[0] <= 'Z';
    shift_char(field[1], upper, by % 36);
    if (by >= 36 && end - field >= 3 && field[2] != '"') shift_char(field[2], upper, by / 36);
}

static void expand_file(const std::string& src, const std::string& dst, int copies, int fields_to_shift) {
    std::string data = slurp(src);
    FILE* out = fopen(dst.c_str(), "wb");
    if (!out) { perror(dst.c_str()); exit(1); }
    size_t header = data.find('\n');
    if (header == std::string::npos) { fwrite(data.data(), 1, data.size(), out); fclose(out); return; }
    fwrite(data.data(), 1, header + 1, out);
    std::string body = data.substr(header + 1);
    std::string work;
    for (int k = 0; k < copies; k++) {
        work = body;
        if (k) {
            char* p = &work[0];
            char* end = p + work.size();
            while (p < end) {
                char* nl = (char*)memchr(p, '\n', size_t(end - p));
                if (!nl) nl = end;
                char* field = p;
                for (int f = 0; f < fields_to_shift && field < nl; f++) {
                    char* comma = (char*)memchr(field, ',', size_t(nl - field));
                    char* stop = comma ? comma : nl;
                    shift_id(field, stop, k);
                    field = stop + 1;
                }
                p = nl + 1;
            }
        }
        fwrite(work.data(), 1, work.size(), out);
    }
    fclose(out);
}

int main(int argc, char** argv) {
    if (argc != 4) { fprintf(stderr, "usage: expand SRC DST COPIES\n"); return 2; }
    std::string src = argv[1], dst = argv[2];
    int copies = atoi(argv[3]);
    mkdir(dst.c_str(), 0777);
    struct Job { std::string from, to; int fields; };
    std::vector<Job> jobs;
    for (const char* folder : {"events", "clicks", "links"}) {
        mkdir((dst + "/" + folder).c_str(), 0777);
        DIR* d = opendir((src + "/" + folder).c_str());
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            std::string name = e->d_name;
            if (name.size() < 4 || name[0] == '.' || name.compare(name.size() - 4, 4, ".csv")) continue;
            jobs.push_back({src + "/" + folder + "/" + name, dst + "/" + folder + "/" + name, 2});
        }
        closedir(d);
    }
    std::atomic<size_t> next{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; t++)
        threads.emplace_back([&] {
            for (;;) {
                size_t i = next.fetch_add(1);
                if (i >= jobs.size()) return;
                expand_file(jobs[i].from, jobs[i].to, copies, jobs[i].fields);
            }
        });
    for (auto& th : threads) th.join();
    for (const char* name : {"products.csv", "campaigns.csv", "manifest.json"}) {
        std::string data = slurp(src + "/" + name);
        FILE* out = fopen((dst + "/" + name).c_str(), "wb");
        if (out) { fwrite(data.data(), 1, data.size(), out); fclose(out); }
    }
    return 0;
}
