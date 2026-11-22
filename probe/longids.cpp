// Diagnostic helper (probe branch only): copies a dataset made by the first
// generator, rewriting one in eight of its distinct short IDs (letter + 6 to 8
// of 0-9a-z, a whole field) to the letter + 32 hex characters, as scorer
// version 5 data has them. The choice depends only on the ID.
// Usage: longids SRC_DIR DST_DIR
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
namespace fs = std::filesystem;
typedef uint64_t u64;
static u64 mix(u64 x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x; }
static bool digit36(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'); }
static bool before_ok(unsigned char c) { return c == ',' || c == '"' || c == '\n'; }
static bool after_ok(unsigned char c) { return c == ',' || c == '"' || c == '\n' || c == '\r'; }
static void rewrite(const fs::path& a, const fs::path& b) {
    FILE* in = fopen(a.c_str(), "rb"); FILE* out = fopen(b.c_str(), "wb");
    if (!in || !out) { perror("open"); exit(2); }
    fseek(in, 0, SEEK_END); size_t n = size_t(ftell(in)); fseek(in, 0, SEEK_SET);
    std::vector<char> buf(n + 16, '\n');
    if (fread(buf.data(), 1, n, in) != n) { perror("read"); exit(2); }
    std::string o; o.reserve(n + n / 4);
    size_t i = 0;
    while (i < n && buf[i] != '\n') o.push_back(buf[i++]);          // the header row
    while (i < n) {
        const unsigned char c = (unsigned char)buf[i];
        if ((c == 'e' || c == 'u' || c == 'k') && before_ok((unsigned char)buf[i - 1])) {
            size_t j = i + 1;
            while (j < n && j - i - 1 < 9 && digit36((unsigned char)buf[j])) j++;
            const size_t digits = j - i - 1;
            if (digits >= 6 && digits <= 8 && after_ok((unsigned char)buf[j])) {
                u64 h = 1469598103934665603ULL;
                for (size_t k = i; k < j; k++) h = (h ^ (unsigned char)buf[k]) * 1099511628211ULL;
                const u64 h1 = mix(h), h2 = mix(h1 ^ 0x9E3779B97F4A7C15ULL);
                if ((h1 & 7) == 0) {
                    char hex[40];
                    snprintf(hex, sizeof hex, "%016llx%016llx", (unsigned long long)h2, (unsigned long long)mix(h2 + 1));
                    o.push_back((char)c); o.append(hex, 32);
                    i = j;
                    continue;
                }
            }
        }
        o.push_back((char)c); i++;
    }
    fwrite(o.data(), 1, o.size(), out);
    fclose(in); fclose(out);
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const fs::path src = argv[1], dst = argv[2];
    for (auto& e : fs::recursive_directory_iterator(src)) {
        const fs::path rel = fs::relative(e.path(), src), to = dst / rel;
        if (e.is_directory()) { fs::create_directories(to); continue; }
        fs::create_directories(to.parent_path());
        const std::string top = rel.begin()->string();
        if (top == "events" || top == "clicks" || top == "links") rewrite(e.path(), to);
        else fs::copy_file(e.path(), to, fs::copy_options::overwrite_existing);
    }
    return 0;
}
