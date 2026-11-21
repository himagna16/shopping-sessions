// Shopping Sessions: clean click-stream CSVs into two daily Parquet tables.
// The rules are in rules.md; reference/ref_impl.py is the slow oracle this
// program is tested against.
//
// Phases:
//   1. parse   links on thread 0 (union-find -> alias map), events and clicks on
//              every thread; events are scattered by event_id hash, clicks by person
//   2. dedup   per event bucket: keep the smallest ingest_ts copy, drop it if
//              invalid, resolve the person, scatter by person
//   3. people  per person bucket: order by time, sessions, click credit and
//              assists, daily rows; each worker writes its own Parquet file
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "parquet.h"

#ifndef SS_PROBE
#define SS_PROBE 0
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t i32;
typedef int64_t i64;

static const i64 DAY = 86'400'000;
static const i64 SESSION_GAP = 1'800'000;
static const i64 REACH = 7 * DAY;
static const i64 TS_BAD = INT64_MIN;
static const u32 DEAD = 0xFFFFFFFFu;        // RecA.ts: invalid copy, or a copy that lost dedup

[[noreturn]] static void die(const char* what) {
    fprintf(stderr, "fatal: %s\n", what);
    _exit(3);
}

static double now() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
}

static inline u64 rd64(const u8* p) { u64 v; memcpy(&v, p, 8); return v; }
static inline u32 rd32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
static inline u16 rd16(const u8* p) { u16 v; memcpy(&v, p, 2); return v; }

static inline u64 mix64(u64 x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static inline i64 floor_div(i64 a, i64 b) { i64 q = a / b; return q - ((a % b != 0) && ((a < 0) != (b < 0))); }

// ---------------------------------------------------------------- IDs
// An ID of 1 to 9 ASCII bytes (none NUL) packs into 63 bits, 7 bits a byte,
// first byte highest, so integer order is byte-wise order. Anything else is
// interned: bit 63 plus an index into a shared table.
static const u64 INTERN_BIT = 1ULL << 63;

static std::mutex g_intern_lock;
static std::unordered_map<std::string, u64> g_intern_map;
static std::vector<std::string*> g_intern_strs;

static u64 intern_id(const u8* p, size_t n) {
    std::string s((const char*)p, n);
    std::lock_guard<std::mutex> guard(g_intern_lock);
    auto it = g_intern_map.find(s);
    if (it != g_intern_map.end()) return it->second;
    u64 key = INTERN_BIT | g_intern_strs.size();
    g_intern_strs.push_back(new std::string(s));
    g_intern_map.emplace(std::move(s), key);
    return key;
}

static inline u64 compact7(u64 x) {
#if defined(__BMI2__)
    return _pext_u64(x, 0x7F7F7F7F7F7F7F7FULL);
#else
    x = ((x & 0x7F007F007F007F00ULL) >> 1) | (x & 0x007F007F007F007FULL);
    x = ((x & 0x3FFF00003FFF0000ULL) >> 2) | (x & 0x00003FFF00003FFFULL);
    x = ((x & 0x0FFFFFFF00000000ULL) >> 4) | (x & 0x000000000FFFFFFFULL);
    return x;
#endif
}

// p must be readable for 9 bytes whatever n is.
static inline u64 pack_id(const u8* p, size_t n) {
#ifdef SS_FORCE_SLOW
    return intern_id(p, n);
#endif
    if (n - 1 < 9) {
        u64 v = __builtin_bswap64(rd64(p));
        u64 ninth = 0;
        u64 fill = 0;
        if (n == 9) {
            ninth = p[8];
            if (ninth - 1 >= 127) return intern_id(p, n);
        } else if (n < 8) {
            v &= ~0ULL << (8 * (8 - n));
            fill = 0x0101010101010101ULL >> (8 * n);
        }
        u64 t = v | fill;
        const u64 high = 0x8080808080808080ULL;
        if ((v & high) | ((t - 0x0101010101010101ULL) & ~t & high)) return intern_id(p, n);
        return (compact7(v) << 7) | ninth;
    }
    return intern_id(p, n);
}

static std::string id_string(u64 key) {
    if (key & INTERN_BIT) {
        std::lock_guard<std::mutex> guard(g_intern_lock);
        return *g_intern_strs[key & ~INTERN_BIT];
    }
    char out[9];
    int n = 0;
    for (int shift = 56; shift >= 0; shift -= 7) {
        char c = char((key >> shift) & 0x7F);
        if (!c) break;
        out[n++] = c;
    }
    return std::string(out, n);
}

static bool id_less_slow(u64 a, u64 b) { return id_string(a) < id_string(b); }   // std::string compares bytes as unsigned
static inline bool id_less(u64 a, u64 b) {
    if (!((a | b) & INTERN_BIT)) return a < b;
    return id_less_slow(a, b);
}

// ---------------------------------------------------------------- timestamps
static inline i64 days_from_civil(i64 y, i64 m, i64 d) {
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    i64 yoe = y - era * 400;
    i64 doy = (153 * ((m + 9) % 12) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

// Remembers `YYYY-MM-DD` texts already validated. A hit needs all 10 bytes equal.
struct DateCache {
    u64 a[64];
    u16 b[64];
    i64 ms[64];
    DateCache() {
        memset(a, 0, sizeof a);
        memset(b, 0, sizeof b);
        for (i64& v : ms) v = TS_BAD;       // ten NUL bytes match an empty slot, and are not a date
    }
};

// `YYYY-MM-DD` at p -> epoch ms of that midnight, or TS_BAD.
static inline i64 parse_date(const u8* p, DateCache& cache) {
    u64 a = rd64(p);
    u16 b = rd16(p + 8);
    u32 slot = ((p[9] & 15) + (p[8] & 3) * 10 + (p[6] & 1) * 32) & 63;
    if (cache.a[slot] == a && cache.b[slot] == b) return cache.ms[slot];
    u32 bad = (p[4] != '-') | (p[7] != '-');
    u32 d0 = p[0] - '0', d1 = p[1] - '0', d2 = p[2] - '0', d3 = p[3] - '0';
    u32 d5 = p[5] - '0', d6 = p[6] - '0', d8 = p[8] - '0', d9 = p[9] - '0';
    bad |= (d0 > 9) | (d1 > 9) | (d2 > 9) | (d3 > 9) | (d5 > 9) | (d6 > 9) | (d8 > 9) | (d9 > 9);
    if (bad) return TS_BAD;
    i64 year = d0 * 1000 + d1 * 100 + d2 * 10 + d3;
    i64 month = d5 * 10 + d6, day = d8 * 10 + d9;
    if (month < 1 || month > 12 || day < 1) return TS_BAD;
    static const int DIM[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int dim = DIM[month];
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) dim = 29;
    if (day > dim) return TS_BAD;
    i64 ms = days_from_civil(year, month, day) * DAY;
    cache.a[slot] = a; cache.b[slot] = b; cache.ms[slot] = ms;
    return ms;
}

// `HH:MM:SS` at p -> ms since midnight, or -1.
static inline i64 parse_tod(const u8* p) {
    u64 x = rd64(p) ^ 0x30303A30303A3030ULL;      // digits become 0..9, colons 0
    u64 over = ((x + 0x7676767676767676ULL) | x) & 0x8080808080808080ULL;
    u32 h = u32(x & 0xFF) * 10 + u32((x >> 8) & 0xFF);
    u32 m = u32((x >> 24) & 0xFF) * 10 + u32((x >> 32) & 0xFF);
    u32 s = u32((x >> 48) & 0xFF) * 10 + u32((x >> 56) & 0xFF);
    u32 colons = u32((x >> 16) & 0xFF) | u32((x >> 40) & 0xFF);
    if (over | colons | (h > 23) | (m > 59) | (s > 59)) return -1;
    return i64(h * 3600 + m * 60 + s) * 1000;
}

// F1, exactly `YYYY-MM-DDTHH:MM:SSZ` (20 bytes at p).
static inline i64 parse_f1(const u8* p, DateCache& cache) {
    if ((p[10] != 'T') | (p[19] != 'Z')) return TS_BAD;
    i64 date = parse_date(p, cache);
    i64 tod = parse_tod(p + 11);
    if (date == TS_BAD || tod < 0) return TS_BAD;
    return date + tod;
}

static inline bool parse8(const u8* p, u64& out) {
    u64 v = rd64(p);
    u64 x = v ^ 0x3030303030303030ULL;
    if (((x + 0x7676767676767676ULL) | x) & 0x8080808080808080ULL) return false;
    x = (x * 2561) >> 8;
    x = ((x & 0x00FF00FF00FF00FFULL) * 6553601) >> 16;
    out = ((x & 0x0000FFFF0000FFFFULL) * 42949672960001ULL) >> 32;
    return true;
}

static i64 parse_f3_slow(const u8* p, size_t n) {
    if (n == 0) return TS_BAD;
    u64 v = 0;
    bool huge = false;
    for (size_t i = 0; i < n; i++) {
        u32 d = p[i] - '0';
        if (d > 9) return TS_BAD;
        if (v > (u64(INT64_MAX) - 9) / 10) huge = true; else v = v * 10 + d;
    }
    return huge ? INT64_MAX : i64(v);       // too long is not malformed, only outside any window
}

// Any `ts`: F1, F2 or F3 -> epoch ms, or TS_BAD. p readable for n + 8 bytes.
static inline i64 parse_ts(const u8* p, size_t n, DateCache& cache) {
#ifndef SS_FORCE_SLOW
    if (n == 20) {
        if (p[19] == 'Z') return parse_f1(p, cache);
    } else if (n == 13) {
        u64 hi, lo;
        if (parse8(p, hi) && parse8(p + 5, lo)) return i64(hi * 100000 + lo % 100000);
        return TS_BAD;
    }
#else
    if (n == 20 && p[19] == 'Z') return parse_f1(p, cache);
#endif
    if (n == 25 && p[10] == 'T' && (p[19] == '+' || p[19] == '-') && p[22] == ':') {
        i64 date = parse_date(p, cache);
        i64 tod = parse_tod(p + 11);
        u32 h0 = p[20] - '0', h1 = p[21] - '0', m0 = p[23] - '0', m1 = p[24] - '0';
        if (date == TS_BAD || tod < 0 || h0 > 9 || h1 > 9 || m0 > 9 || m1 > 9) return TS_BAD;
        u32 oh = h0 * 10 + h1, om = m0 * 10 + m1;
        if (oh > 23 || om > 59) return TS_BAD;
        i64 offset = i64(oh * 60 + om) * 60000;
        return p[19] == '+' ? date + tod - offset : date + tod + offset;
    }
    return parse_f3_slow(p, n);
}

// ---------------------------------------------------------------- CSV scan
struct Masks { u64 comma, nl, quote; };

static inline Masks classify(const u8* p) {
    Masks m;
#if defined(__AVX512BW__) && !defined(SS_NO_AVX512)
    __m512i v = _mm512_loadu_si512((const void*)p);
    m.comma = _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8(','));
    m.nl = _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('\n'));
    m.quote = _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('"'));
#elif defined(__AVX2__)
    __m256i a = _mm256_loadu_si256((const __m256i*)p), b = _mm256_loadu_si256((const __m256i*)(p + 32));
    __m256i c = _mm256_set1_epi8(','), n = _mm256_set1_epi8('\n'), q = _mm256_set1_epi8('"');
    m.comma = u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, c))) | (u64(u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(b, c)))) << 32);
    m.nl = u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, n))) | (u64(u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(b, n)))) << 32);
    m.quote = u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, q))) | (u64(u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(b, q)))) << 32);
#elif defined(__ARM_NEON)
    const uint8x16_t bits = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    uint8x16_t v0 = vld1q_u8(p), v1 = vld1q_u8(p + 16), v2 = vld1q_u8(p + 32), v3 = vld1q_u8(p + 48);
    auto mask = [&](u8 ch) -> u64 {
        uint8x16_t c = vdupq_n_u8(ch);
        uint8x16_t s0 = vpaddq_u8(vandq_u8(vceqq_u8(v0, c), bits), vandq_u8(vceqq_u8(v1, c), bits));
        uint8x16_t s1 = vpaddq_u8(vandq_u8(vceqq_u8(v2, c), bits), vandq_u8(vceqq_u8(v3, c), bits));
        s0 = vpaddq_u8(s0, s1);
        s0 = vpaddq_u8(s0, s0);
        return vgetq_lane_u64(vreinterpretq_u64_u8(s0), 0);
    };
    m.comma = mask(','); m.nl = mask('\n'); m.quote = mask('"');
#else
    m.comma = m.nl = m.quote = 0;
    for (int i = 0; i < 64; i++) {
        m.comma |= u64(p[i] == ',') << i;
        m.nl |= u64(p[i] == '\n') << i;
        m.quote |= u64(p[i] == '"') << i;
    }
#endif
    return m;
}

// Calls row(first_byte, comma_positions, newline_position, quotes_in_line) for
// every line that starts in [start, stop) and has exactly NF fields. buf[start]
// starts a line; a '\n' exists at or after stop - 1; 64 readable bytes follow it.
template <int NF, class Row>
static inline void scan_rows(const u8* buf, size_t start, size_t stop, Row&& row) {
    if (start >= stop) return;
    size_t line = start;
    size_t commas[NF];
    u32 found = 0;
    u32 quotes = 0;
    for (size_t block = start;; block += 64) {
        Masks m = classify(buf + block);
        u64 delims = m.comma | m.nl;
        u64 q = m.quote;
        while (delims) {
            u32 bit = u32(__builtin_ctzll(delims));
            delims &= delims - 1;
            size_t pos = block + bit;
            if (!((m.nl >> bit) & 1)) {
                if (found < NF - 1) commas[found] = pos;
                found++;
                continue;
            }
            u64 below = (2ULL << bit) - 1;
            u32 line_quotes = quotes + u32(__builtin_popcountll(q & below));
            q &= ~below;
            quotes = 0;
            if (found == NF - 1) row(line, commas, pos, line_quotes);
            found = 0;
            line = pos + 1;
            if (line >= stop) return;
        }
        quotes += u32(__builtin_popcountll(q));
    }
}

// A field wrapped in quotes loses them and counts 2 toward the line's quotes.
// If the count does not reach the line's total, a quote sits somewhere else.
#define UNQUOTE(s, e) \
    { u32 w = u32((e) - (s) >= 2) & u32(*(s) == '"') & u32((e)[-1] == '"'); (s) += w; (e) -= w; wrapped += 2 * w; }

// ---------------------------------------------------------------- input chunks
struct InputFile { int fd; size_t size; };
struct Task { u32 file; size_t begin, end; };

// A thread's read buffer. data() has 64 readable bytes before and after.
struct Reader {
    u8* mem = nullptr;
    size_t cap = 0;
    static const size_t LEAD = 64;

    void reserve(size_t n) {
        if (n + 2 * LEAD + 1 <= cap) return;
        size_t want = std::max(n + 2 * LEAD + 1, cap * 2);
        u8* fresh = (u8*)malloc(want);
        if (!fresh) die("out of memory for a read buffer");
        if (mem) { memcpy(fresh, mem, cap); free(mem); }
        else memset(fresh, 0, LEAD);
        mem = fresh;
        cap = want;
    }
    u8* data() { return mem + LEAD; }

    // Loads the lines of `task`. Returns false when no line starts inside it.
    bool load(const InputFile& f, const Task& task, const u8*& buf, size_t& start, size_t& stop) {
        size_t from = task.begin ? task.begin - 1 : 0;
        size_t have = 0;
        size_t slack = 4096;
        size_t need = task.end - 1 - from;                 // a '\n' is needed at or after this index
        for (;;) {
            size_t upto = std::min(f.size, task.end + slack);
            reserve(upto - from);
            while (from + have < upto) {
                ssize_t got = pread(f.fd, data() + have, upto - from - have, off_t(from + have));
                if (got < 0) { if (errno == EINTR) continue; die("pread failed"); }
                if (got == 0) die("input file shrank while reading");
                have += size_t(got);
            }
            if (memchr(data() + need, '\n', have - need)) break;
            if (from + have == f.size) { data()[have++] = '\n'; break; }   // last line without a newline
            slack *= 4;
        }
        memset(data() + have, 0, LEAD);
        buf = data();
        const u8* first = (const u8*)memchr(buf, '\n', have);              // ends the header, or the line before this chunk
        if (!first) return false;
        start = size_t(first - buf) + 1;
        stop = task.end - from;
        return start < stop;
    }
};

// ---------------------------------------------------------------- arena and scatter lists
static const size_t SLAB = 2u << 20;
static const size_t BLOCK = 8192;

static std::mutex g_arena_lock;
static u8* g_arena_cur = nullptr;
static u8* g_arena_end = nullptr;
static size_t g_arena_region = 1ULL << 30;

static u8* slab_alloc() {
    std::lock_guard<std::mutex> guard(g_arena_lock);
    if (g_arena_cur + SLAB > g_arena_end) {
        size_t len = g_arena_region;
        void* p = mmap(nullptr, len + SLAB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS
#ifdef MAP_NORESERVE
                       | MAP_NORESERVE
#endif
                       , -1, 0);
        if (p == MAP_FAILED) die("mmap failed for the record arena");
        u8* base = (u8*)((uintptr_t(p) + SLAB - 1) & ~(uintptr_t(SLAB) - 1));
#ifdef MADV_HUGEPAGE
        madvise(base, len, MADV_HUGEPAGE);
#endif
        g_arena_cur = base;
        g_arena_end = base + len;
    }
    u8* out = g_arena_cur;
    g_arena_cur += SLAB;
    return out;
}

struct BlockAlloc {
    u8* cur = nullptr;
    u8* end = nullptr;
    inline u8* get() {
        if (cur == end) { cur = slab_alloc(); end = cur + SLAB; }
        u8* out = cur;
        cur += BLOCK;
        return out;
    }
};

struct Block { Block* next; u32 n; u32 pad; };

template <class T>
struct List {
    static const u32 CAP = u32((BLOCK - sizeof(Block)) / sizeof(T));
    T* cur = nullptr;
    T* end = nullptr;
    Block* head = nullptr;
    Block* tail = nullptr;

    static T* items(Block* b) { return (T*)(b + 1); }
    inline void push(const T& v, BlockAlloc& alloc) {
        if (cur == end) grow(alloc);
        *cur++ = v;
    }
    void grow(BlockAlloc& alloc) {
        Block* b = (Block*)alloc.get();
        b->next = nullptr;
        b->n = CAP;
        if (tail) tail->next = b; else head = b;
        tail = b;
        cur = items(b);
        end = cur + CAP;
    }
    void finish() { if (tail) tail->n = u32(cur - items(tail)); }
};

struct RecA { u64 ekey; u64 ukey; i64 ingest; u32 ts; u32 payload; };      // one event copy, 32 bytes
struct RecB { u64 person; u32 ts; u32 payload; };                          // one surviving event, 16 bytes
struct RecC { u64 person; u64 ckey; u32 ts; u32 camp; };                   // one valid click, 24 bytes

// ---------------------------------------------------------------- catalogs
// Small files (products, campaigns): the same dialect, parsed the plain way.
static std::vector<std::vector<std::string>> read_small_csv(const std::string& path, size_t width) {
    std::vector<std::vector<std::string>> rows;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return rows;
    std::string data;
    char tmp[1 << 16];
    size_t got;
    while ((got = fread(tmp, 1, sizeof tmp, f)) > 0) data.append(tmp, got);
    fclose(f);
    size_t at = 0;
    bool header = true;
    while (at < data.size()) {
        size_t nl = data.find('\n', at);
        if (nl == std::string::npos) nl = data.size();
        std::string_view line(data.data() + at, nl - at);
        at = nl + 1;
        if (header) { header = false; continue; }
        std::vector<std::string> fields;
        bool ok = !line.empty();
        size_t s = 0;
        while (ok) {
            size_t c = line.find(',', s);
            std::string_view field = line.substr(s, c == std::string_view::npos ? std::string_view::npos : c - s);
            if (field.find('"') != std::string_view::npos) {
                if (field.size() < 2 || field.front() != '"' || field.back() != '"' ||
                    field.substr(1, field.size() - 2).find('"') != std::string_view::npos) { ok = false; break; }
                field = field.substr(1, field.size() - 2);
            }
            fields.emplace_back(field);
            if (c == std::string_view::npos) break;
            s = c + 1;
        }
        if (ok && fields.size() == width) rows.push_back(std::move(fields));
    }
    return rows;
}

// `prefix` then a decimal number with no leading zero -> the number, else -1.
static inline i64 canonical_number(const u8* s, const u8* e, size_t prefix) {
    size_t n = size_t(e - s);
    if (n <= prefix || n > prefix + 8) return -1;
    const u8* p = s + prefix;
    if (*p == '0' && e - p > 1) return -1;
    u32 v = 0;
    for (; p < e; p++) {
        u32 d = *p - '0';
        if (d > 9) return -1;
        v = v * 10 + d;
    }
    return v;
}

struct Catalogs {
    // products
    std::vector<std::string> categories;       // byte-wise sorted, `unknown` included
    u32 unknown = 0;
    std::vector<u8> product_dense;             // p<N> -> category + 1, 0 = not in the file
    std::unordered_map<std::string, u8> product_other;
    // campaigns
    std::vector<std::string> campaign_ids, channels;
    std::vector<i32> campaign_dense;           // c_<N> -> index, -1 = not in the file
    std::unordered_map<std::string, i32> campaign_other;

    void load(const std::string& dir) {
        auto products = read_small_csv(dir + "/products.csv", 2);
        std::unordered_map<std::string, std::string> category_of;
        for (auto& row : products) category_of[row[0]] = row[1];      // a repeated ID keeps its last row
        categories.push_back("unknown");
        for (auto& kv : category_of) categories.push_back(kv.second);
        std::sort(categories.begin(), categories.end());
        categories.erase(std::unique(categories.begin(), categories.end()), categories.end());
        if (categories.size() > 255) die("more than 255 categories");
        std::unordered_map<std::string, u8> index;
        for (size_t i = 0; i < categories.size(); i++) index[categories[i]] = u8(i);
        unknown = index["unknown"];
        i64 largest = -1;
        for (auto& kv : category_of) {
            const u8* s = (const u8*)kv.first.data();
            if (kv.first.size() >= 2 && s[0] == 'p') largest = std::max(largest, canonical_number(s, s + kv.first.size(), 1));
        }
        product_dense.assign(size_t(largest + 1), 0);
        for (auto& kv : category_of) {
            const u8* s = (const u8*)kv.first.data();
            i64 number = (kv.first.size() >= 2 && s[0] == 'p') ? canonical_number(s, s + kv.first.size(), 1) : -1;
            if (number >= 0) product_dense[size_t(number)] = u8(index[kv.second] + 1);
            else product_other[kv.first] = index[kv.second];
        }

        auto campaigns = read_small_csv(dir + "/campaigns.csv", 2);
        std::unordered_map<std::string, i32> seen;
        for (auto& row : campaigns) {
            auto it = seen.find(row[0]);
            if (it != seen.end()) { channels[size_t(it->second)] = row[1]; continue; }
            seen[row[0]] = i32(campaign_ids.size());
            campaign_ids.push_back(row[0]);
            channels.push_back(row[1]);
        }
        largest = -1;
        auto number_of = [](const std::string& id) -> i64 {
            const u8* s = (const u8*)id.data();
            return (id.size() >= 3 && s[0] == 'c' && s[1] == '_') ? canonical_number(s, s + id.size(), 2) : -1;
        };
        for (auto& id : campaign_ids) largest = std::max(largest, number_of(id));
        campaign_dense.assign(size_t(largest + 1), -1);
        for (size_t i = 0; i < campaign_ids.size(); i++) {
            i64 number = number_of(campaign_ids[i]);
            if (number >= 0) campaign_dense[size_t(number)] = i32(i);
            else campaign_other[campaign_ids[i]] = i32(i);
        }
    }

    inline u32 category(const u8* s, const u8* e) const {
#ifndef SS_FORCE_SLOW
        if (*s == 'p') {
            i64 number = canonical_number(s, e, 1);
            if (number >= 0) {
                if (size_t(number) < product_dense.size() && product_dense[size_t(number)]) return product_dense[size_t(number)] - 1u;
                return unknown;
            }
        }
        if (product_other.empty()) return unknown;
#else
        if (*s == 'p') {
            i64 number = canonical_number(s, e, 1);
            if (number >= 0) return (size_t(number) < product_dense.size() && product_dense[size_t(number)]) ? product_dense[size_t(number)] - 1u : unknown;
        }
#endif
        auto it = product_other.find(std::string((const char*)s, size_t(e - s)));
        return it == product_other.end() ? unknown : it->second;
    }

    inline i32 campaign(const u8* s, const u8* e) const {
        if (e - s >= 3 && s[0] == 'c' && s[1] == '_') {
            i64 number = canonical_number(s, e, 2);
            if (number >= 0) return size_t(number) < campaign_dense.size() ? campaign_dense[size_t(number)] : -1;
        }
        if (campaign_other.empty()) return -1;
        auto it = campaign_other.find(std::string((const char*)s, size_t(e - s)));
        return it == campaign_other.end() ? -1 : it->second;
    }
};

// ---------------------------------------------------------------- linked IDs
// Maps every ID that is not its person's name to that name. An ID absent
// from the table is its own person.
struct AliasMap {
    struct Slot { u64 key, name; };
    Slot* slots = nullptr;
    u64 mask = 0;
    int shift = 64;

    void build(const std::vector<std::pair<u64, u64>>& entries) {
        size_t cap = 16;
        while (cap < entries.size() * 2) cap <<= 1;
        size_t bytes = cap * sizeof(Slot);
        void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) die("mmap failed for the alias map");
#ifdef MADV_HUGEPAGE
        madvise(p, bytes, MADV_HUGEPAGE);
#endif
        slots = (Slot*)p;
        mask = cap - 1;
        shift = 64 - __builtin_ctzll(cap);
        for (auto& entry : entries) {
            u64 at = mix64(entry.first) >> shift;
            while (slots[at].key) at = (at + 1) & mask;
            slots[at] = {entry.first, entry.second};
        }
    }
    inline u64 home(u64 key) const { return mix64(key) >> shift; }
    inline void prefetch(u64 key) const { __builtin_prefetch(&slots[home(key)]); }
    inline u64 person(u64 key) const {
        u64 at = home(key);
        for (;;) {
            u64 k = slots[at].key;
            if (k == key) return slots[at].name;
            if (!k) return key;
            at = (at + 1) & mask;
        }
    }
};

struct Links {
    std::vector<std::pair<u64, u64>> pairs;

    void add(u64 a, u64 b) { pairs.emplace_back(a, b); }

    void solve(AliasMap& out) {
        size_t cap = 16;
        while (cap < pairs.size() * 4) cap <<= 1;
        std::vector<u32> table(cap, 0);            // index + 1 into ids
        std::vector<u64> ids;
        std::vector<u32> parent;
        ids.reserve(pairs.size() * 2);
        u64 mask = cap - 1;
        auto index_of = [&](u64 key) -> u32 {
            u64 at = mix64(key) & mask;
            for (;;) {
                u32 slot = table[at];
                if (!slot) {
                    ids.push_back(key);
                    parent.push_back(u32(ids.size() - 1));
                    table[at] = u32(ids.size());
                    return u32(ids.size() - 1);
                }
                if (ids[slot - 1] == key) return slot - 1;
                at = (at + 1) & mask;
            }
        };
        auto find = [&](u32 x) -> u32 {
            while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
            return x;
        };
        for (auto& pair : pairs) {
            u32 a = find(index_of(pair.first)), b = find(index_of(pair.second));
            if (a == b) continue;
            // The root is always the byte-wise smallest ID, so it is the person's name.
            if (id_less(ids[b], ids[a])) std::swap(a, b);
            parent[b] = a;
        }
        std::vector<std::pair<u64, u64>> entries;
        for (u32 i = 0; i < ids.size(); i++) {
            u32 root = find(i);
            if (root != i) entries.emplace_back(ids[i], ids[root]);
        }
        out.build(entries);
    }
};

// ---------------------------------------------------------------- shared state
struct Config {
    std::string data, out;
    u32 threads = 8;
    size_t chunk = 256 << 10;
};

struct Shared {
    Config cfg;
    Catalogs cat;
    i64 ws = 0, we = 0;                     // event window, epoch ms
    i64 day0 = 0;                           // first session day (days since the epoch)
    u32 ndays = 0;
    u32 abits = 6, bbits = 6;               // log2 of event buckets and person buckets
    std::vector<InputFile> files;
    std::vector<Task> event_tasks, click_tasks, link_tasks;
    std::atomic<size_t> next_event{0}, next_click{0}, next_bucket{0}, next_person{0};
    std::atomic<bool> links_ready{false};
    AliasMap alias;
    // [thread][bucket]
    std::vector<std::vector<List<RecA>>> a;
    std::vector<std::vector<List<RecB>>> b;
    std::vector<std::vector<List<RecC>>> c;
    std::vector<BlockAlloc> alloc;
    std::vector<u32> person_order;
    // per thread: [(campaign * ndays + day) * 4 + {sessions, purchasing, revenue, assisted}]
    std::vector<std::vector<i64>> campaign_totals;
    std::atomic<u64> user_rows{0};
};

static void list_csv(Shared& sh, const std::string& folder, std::vector<Task>& tasks) {
    std::string dir = sh.cfg.data + "/" + folder;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* entry = readdir(d)) {
        std::string name = entry->d_name;
        if (name.size() < 4 || name[0] == '.' || name.compare(name.size() - 4, 4, ".csv") != 0) continue;
        std::string path = dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) continue;
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) die("cannot open an input file");
        u32 index = u32(sh.files.size());
        sh.files.push_back({fd, size_t(st.st_size)});
        for (size_t at = 0; at < size_t(st.st_size); at += sh.cfg.chunk)
            tasks.push_back({index, at, std::min(size_t(st.st_size), at + sh.cfg.chunk)});
    }
    closedir(d);
}

// ---------------------------------------------------------------- phase 1: parse
static void parse_links(Shared& sh, Reader& reader) {
    Links links;
    for (const Task& task : sh.link_tasks) {
        const u8* buf; size_t start, stop;
        if (!reader.load(sh.files[task.file], task, buf, start, stop)) continue;
        scan_rows<2>(buf, start, stop, [&](size_t line, const size_t* commas, size_t nl, u32 quotes) {
            const u8 *s0 = buf + line, *e0 = buf + commas[0], *s1 = e0 + 1, *e1 = buf + nl;
            u32 wrapped = 0;
            UNQUOTE(s0, e0) UNQUOTE(s1, e1)
            if (wrapped != quotes || s0 == e0 || s1 == e1) return;
            links.add(pack_id(s0, size_t(e0 - s0)), pack_id(s1, size_t(e1 - s1)));
        });
    }
    links.solve(sh.alias);
    sh.links_ready.store(true, std::memory_order_release);
}

static void parse_events(Shared& sh, u32 tid, Reader& reader) {
    const u32 VIEW = rd32((const u8*)"view"), CART = rd32((const u8*)"cart");
    const u64 PURCHASE = rd64((const u8*)"purchase");
    const i64 ws = sh.ws, we = sh.we;
    const int shift = 64 - int(sh.abits);
    const Catalogs& cat = sh.cat;
    List<RecA>* lists = sh.a[tid].data();
    BlockAlloc& alloc = sh.alloc[tid];
    DateCache dates;
    u64 sink = 0;
    for (;;) {
        size_t at = sh.next_event.fetch_add(1, std::memory_order_relaxed);
        if (at >= sh.event_tasks.size()) break;
        const Task& task = sh.event_tasks[at];
        const u8* buf; size_t start, stop;
        if (!reader.load(sh.files[task.file], task, buf, start, stop)) continue;
#if SS_PROBE == 1
        continue;
#endif
        scan_rows<7>(buf, start, stop, [&](size_t line, const size_t* commas, size_t nl, u32 quotes) {
#if SS_PROBE == 2
            lists[0].cur += (commas[0] + nl + quotes + line) & 0; return;
#endif
            const u8 *s0 = buf + line, *e0 = buf + commas[0];
            const u8 *s1 = e0 + 1, *e1 = buf + commas[1];
            const u8 *s2 = e1 + 1, *e2 = buf + commas[2];
            const u8 *s3 = e2 + 1, *e3 = buf + commas[3];
            const u8 *s4 = e3 + 1, *e4 = buf + commas[4];
            const u8 *s5 = e4 + 1, *e5 = buf + commas[5];
            const u8 *s6 = e5 + 1, *e6 = buf + nl;
            u32 wrapped = 0;
            UNQUOTE(s0, e0) UNQUOTE(s1, e1) UNQUOTE(s2, e2) UNQUOTE(s3, e3) UNQUOTE(s4, e4) UNQUOTE(s5, e5) UNQUOTE(s6, e6)
            if (wrapped != quotes || s0 == e0) return;          // malformed, or no event_id

            RecA rec;
            rec.ekey = pack_id(s0, size_t(e0 - s0));
            // rules.md: ingest_ts is always valid F1. If it is not, the copy can never be kept.
            i64 ingest = (e3 - s3 == 20) ? parse_f1(s3, dates) : TS_BAD;
            rec.ingest = ingest == TS_BAD ? INT64_MAX : ingest;
            rec.ukey = 0;
            rec.ts = DEAD;
            rec.payload = 0;
            if (ingest != TS_BAD && s1 != e1 && s5 != e5) {
                i64 ts = parse_ts(s2, size_t(e2 - s2), dates);
                if (ts != TS_BAD && ts >= ws && ts < we && ingest - ts <= DAY) {
                    size_t kind = size_t(e4 - s4);
                    bool ok = false;
                    if (kind == 4) {
                        u32 word = rd32(s4);
                        ok = (word == VIEW) | (word == CART);
                    } else if (kind == 8 && rd64(s4) == PURCHASE) {
                        const u8* p = s6;
                        while (p < e6 && *p == '0') p++;
                        u32 price = 0;
                        bool digits = e6 > s6 && e6 - p <= 8;
                        for (; digits && p < e6; p++) {
                            u32 d = *p - '0';
                            if (d > 9) digits = false;
                            price = price * 10 + d;
                        }
                        if (digits && price >= 1 && price <= 10'000'000) {
                            ok = true;
                            rec.payload = (price << 8) | cat.category(s5, e5);
                        }
                    }
                    if (ok) {
                        rec.ts = u32(ts - ws);
                        rec.ukey = pack_id(s1, size_t(e1 - s1));
                    }
                }
            }
#if SS_PROBE == 3
            sink += rec.ekey + rec.ukey + rec.ts + rec.payload + u64(rec.ingest); return;
#endif
            lists[mix64(rec.ekey) >> shift].push(rec, alloc);
        });
    }
    if (sink == 1) fprintf(stderr, "sink\n");
}

static void parse_clicks(Shared& sh, u32 tid, Reader& reader) {
    while (!sh.links_ready.load(std::memory_order_acquire)) std::this_thread::yield();
    const i64 lo = sh.ws - REACH, hi = sh.we;
    const int shift = 64 - int(sh.bbits);
    const Catalogs& cat = sh.cat;
    const AliasMap& alias = sh.alias;
    List<RecC>* lists = sh.c[tid].data();
    BlockAlloc& alloc = sh.alloc[tid];
    DateCache dates;
    for (;;) {
        size_t at = sh.next_click.fetch_add(1, std::memory_order_relaxed);
        if (at >= sh.click_tasks.size()) break;
        const Task& task = sh.click_tasks[at];
        const u8* buf; size_t start, stop;
        if (!reader.load(sh.files[task.file], task, buf, start, stop)) continue;
        scan_rows<4>(buf, start, stop, [&](size_t line, const size_t* commas, size_t nl, u32 quotes) {
            const u8 *s0 = buf + line, *e0 = buf + commas[0];
            const u8 *s1 = e0 + 1, *e1 = buf + commas[1];
            const u8 *s2 = e1 + 1, *e2 = buf + commas[2];
            const u8 *s3 = e2 + 1, *e3 = buf + nl;
            u32 wrapped = 0;
            UNQUOTE(s0, e0) UNQUOTE(s1, e1) UNQUOTE(s2, e2) UNQUOTE(s3, e3)
            if (wrapped != quotes || s0 == e0 || s1 == e1) return;
            i64 ts = parse_ts(s3, size_t(e3 - s3), dates);
            if (ts == TS_BAD || ts < lo || ts >= hi) return;
            i32 campaign = cat.campaign(s2, e2);
            if (campaign < 0) return;
            // Copies of a click are identical (rules.md), so they change neither
            // the credited campaign nor the set of assisting campaigns: no dedup.
            RecC rec;
            rec.person = alias.person(pack_id(s1, size_t(e1 - s1)));
            rec.ckey = pack_id(s0, size_t(e0 - s0));
            rec.ts = u32(ts - lo);
            rec.camp = u32(campaign);
            lists[mix64(rec.person) >> shift].push(rec, alloc);
        });
    }
}

// ---------------------------------------------------------------- phase 2: dedup
static void dedup_buckets(Shared& sh, u32 tid) {
    const u32 nbuckets = 1u << sh.abits;
    const int shift = 64 - int(sh.bbits);
    const AliasMap& alias = sh.alias;
    List<RecB>* out = sh.b[tid].data();
    BlockAlloc& alloc = sh.alloc[tid];
    std::vector<Block*> blocks;
    std::vector<u32> table;
    static const u32 BATCH = 32;
    RecA* pending[BATCH];
    for (;;) {
        u32 bucket = u32(sh.next_bucket.fetch_add(1, std::memory_order_relaxed));
        if (bucket >= nbuckets) break;
        blocks.clear();
        size_t total = 0;
        for (u32 t = 0; t < sh.cfg.threads; t++)
            for (Block* b = sh.a[t][bucket].head; b; b = b->next) { blocks.push_back(b); total += b->n; }
        if (!total) continue;
        size_t cap = 16;
        while (cap < total * 2) cap <<= 1;
        table.assign(cap, 0);
        const u64 mask = cap - 1;
        // Smallest ingest_ts wins; equal ingest_ts means identical copies.
        for (u32 bi = 0; bi < blocks.size(); bi++) {
            RecA* recs = List<RecA>::items(blocks[bi]);
            for (u32 i = 0; i < blocks[bi]->n; i++) {
                RecA& rec = recs[i];
                u64 at = mix64(rec.ekey) & mask;
                for (;;) {
                    u32 slot = table[at];
                    if (!slot) { table[at] = (bi << 8 | i) + 1; break; }
                    RecA& best = List<RecA>::items(blocks[(slot - 1) >> 8])[(slot - 1) & 255];
                    if (best.ekey == rec.ekey) {
                        if (rec.ingest < best.ingest) { best.ts = DEAD; table[at] = (bi << 8 | i) + 1; }
                        else rec.ts = DEAD;
                        break;
                    }
                    at = (at + 1) & mask;
                }
            }
        }
        // Survivors that are valid get their person and move to a person bucket.
        u32 waiting = 0;
        auto flush = [&]() {
            for (u32 k = 0; k < waiting; k++) {
                RecA* rec = pending[k];
                RecB row{alias.person(rec->ukey), rec->ts, rec->payload};
                out[mix64(row.person) >> shift].push(row, alloc);
            }
            waiting = 0;
        };
        for (Block* b : blocks) {
            RecA* recs = List<RecA>::items(b);
            for (u32 i = 0; i < b->n; i++) {
                if (recs[i].ts == DEAD) continue;
                alias.prefetch(recs[i].ukey);
                pending[waiting++] = &recs[i];
                if (waiting == BATCH) flush();
            }
        }
        flush();
    }
}

// ---------------------------------------------------------------- phase 3: people
struct Click { u32 ts; u32 camp; u64 key; };

struct UserWriter {
    static const u32 GROUP = 1u << 17;
    pq::File file;
    bool opened = false;
    std::string path;
    u32 n = 0;
    std::vector<u8> ids, cats, nulls;
    std::vector<i32> days, sessions, events;
    std::vector<i64> revenue;
    size_t ids_len = 0, cats_len = 0;

    void init(const std::string& p) {
        path = p;
        ids.resize(size_t(GROUP) * 16 + 4096);
        cats.resize(1 << 16);
        nulls.assign(GROUP / 8 + 8, 0);
        days.resize(GROUP); sessions.resize(GROUP); events.resize(GROUP); revenue.resize(GROUP);
    }
    static std::vector<pq::Col> schema() {
        return {{"user_id", pq::BYTE_ARRAY, pq::UTF8, false}, {"day", pq::INT32, pq::DATE, false},
                {"sessions", pq::INT32, pq::NONE, false}, {"events", pq::INT32, pq::NONE, false},
                {"revenue_cents", pq::INT64, pq::NONE, false}, {"top_category", pq::BYTE_ARRAY, pq::UTF8, true}};
    }
    inline void add(const char* id, u32 id_len, i32 day, i32 nsessions, i32 nevents, i64 cents, const std::string* category) {
        if (ids_len + id_len + 4 > ids.size()) ids.resize(std::max(ids.size() * 2, ids_len + id_len + 4));
        memcpy(&ids[ids_len], &id_len, 4);
        memcpy(&ids[ids_len + 4], id, id_len);
        ids_len += id_len + 4;
        days[n] = day; sessions[n] = nsessions; events[n] = nevents; revenue[n] = cents;
        if (category) {
            u32 len = u32(category->size());
            if (cats_len + len + 4 > cats.size()) cats.resize(std::max(cats.size() * 2, cats_len + len + 4));
            memcpy(&cats[cats_len], &len, 4);
            memcpy(&cats[cats_len + 4], category->data(), len);
            cats_len += len + 4;
            nulls[n >> 3] |= u8(1u << (n & 7));
        }
        if (++n == GROUP) flush();
    }
    void flush() {
        if (!n) return;
        if (!opened) { file.open(path, schema()); opened = true; }
        // Definition levels: one bit-packed run, 1 bit a row.
        u32 groups = (n + 7) / 8;
        std::vector<u8> levels;
        u64 header = (u64(groups) << 1) | 1;
        while (header >= 0x80) { levels.push_back(u8((header & 0x7F) | 0x80)); header >>= 7; }
        levels.push_back(u8(header));
        levels.insert(levels.end(), nulls.begin(), nulls.begin() + groups);
        pq::Part parts[6];
        parts[0].values = ids.data(); parts[0].values_len = ids_len;
        parts[1].values = days.data(); parts[1].values_len = size_t(n) * 4;
        parts[2].values = sessions.data(); parts[2].values_len = size_t(n) * 4;
        parts[3].values = events.data(); parts[3].values_len = size_t(n) * 4;
        parts[4].values = revenue.data(); parts[4].values_len = size_t(n) * 8;
        parts[5].levels = levels.data(); parts[5].levels_len = levels.size();
        parts[5].values = cats.data(); parts[5].values_len = cats_len;
        file.row_group(n, parts);
        memset(nulls.data(), 0, groups);
        n = 0; ids_len = 0; cats_len = 0;
    }
    void finish() {
        flush();
        if (opened) file.close();
    }
};

static void process_people(Shared& sh, u32 tid) {
    const u32 nthreads = sh.cfg.threads;
    const u32 ncamp = u32(sh.cat.campaign_ids.size());
    const u32 organic = ncamp;
    const u32 ndays = sh.ndays;
    const i64 ws = sh.ws, day0 = sh.day0;
    const u32 ncat = u32(sh.cat.categories.size());
    std::vector<i64>& totals = sh.campaign_totals[tid];
    totals.assign(size_t(ncamp + 1) * ndays * 4, 0);

    UserWriter writer;
    char name[64];
    snprintf(name, sizeof name, "/user_daily/part-%03u.parquet", tid);
    writer.init(sh.cfg.out + name);

    std::vector<u32> table, local, counts, click_counts, click_local;
    std::vector<u64> persons, events;
    std::vector<Click> clicks;
    std::vector<i64> cat_cents(ncat, 0);
    std::vector<u32> cat_touched;
    std::vector<u32> camp_count(ncamp + 1, 0), camp_slot(ncamp + 1, 0), active;
    u64 rows = 0;

    for (;;) {
        size_t turn = sh.next_person.fetch_add(1, std::memory_order_relaxed);
        if (turn >= sh.person_order.size()) break;
        u32 bucket = sh.person_order[turn];

        size_t nev = 0;
        for (u32 t = 0; t < nthreads; t++)
            for (Block* b = sh.b[t][bucket].head; b; b = b->next) nev += b->n;
        if (!nev) continue;

        // Number the bucket's people and count their events.
        size_t cap = 64;
        while (cap < nev / 2) cap <<= 1;
        table.assign(cap, 0);
        u64 mask = cap - 1;
        persons.clear();
        counts.clear();
        local.resize(nev);
        size_t at_event = 0;
        auto number = [&](u64 person) -> u32 {
            u64 at = mix64(person) & mask;
            for (;;) {
                u32 slot = table[at];
                if (!slot) break;
                if (persons[slot - 1] == person) return slot - 1;
                at = (at + 1) & mask;
            }
            if ((persons.size() + 1) * 2 > cap) {                 // keep the table at most half full
                cap <<= 1;
                mask = cap - 1;
                table.assign(cap, 0);
                for (u32 i = 0; i < persons.size(); i++) {
                    u64 h = mix64(persons[i]) & mask;
                    while (table[h]) h = (h + 1) & mask;
                    table[h] = i + 1;
                }
                at = mix64(person) & mask;
                while (table[at]) at = (at + 1) & mask;
            }
            persons.push_back(person);
            counts.push_back(0);
            table[at] = u32(persons.size());
            return u32(persons.size() - 1);
        };
        for (u32 t = 0; t < nthreads; t++)
            for (Block* b = sh.b[t][bucket].head; b; b = b->next) {
                RecB* recs = List<RecB>::items(b);
                for (u32 i = 0; i < b->n; i++) {
                    u32 id = number(recs[i].person);
                    counts[id]++;
                    local[at_event++] = id;
                }
            }
        const u32 npeople = u32(persons.size());

        // Events grouped by person, then ordered by time.
        std::vector<u32>& offset = click_local;                   // reused as scratch for offsets
        offset.assign(npeople + 1, 0);
        for (u32 i = 0; i < npeople; i++) offset[i + 1] = offset[i] + counts[i];
        std::vector<u32> cursor(offset.begin(), offset.end() - 1);
        events.resize(nev);
        at_event = 0;
        for (u32 t = 0; t < nthreads; t++)
            for (Block* b = sh.b[t][bucket].head; b; b = b->next) {
                RecB* recs = List<RecB>::items(b);
                for (u32 i = 0; i < b->n; i++)
                    events[cursor[local[at_event++]]++] = (u64(recs[i].ts) << 32) | recs[i].payload;
            }
        std::vector<u32> event_offset(offset);

        // Clicks of people who have events, grouped the same way.
        click_counts.assign(npeople + 1, 0);
        size_t nclicks = 0;
        auto lookup = [&](u64 person) -> i64 {
            u64 at = mix64(person) & mask;
            for (;;) {
                u32 slot = table[at];
                if (!slot) return -1;
                if (persons[slot - 1] == person) return slot - 1;
                at = (at + 1) & mask;
            }
        };
        for (u32 t = 0; t < nthreads; t++)
            for (Block* b = sh.c[t][bucket].head; b; b = b->next) {
                RecC* recs = List<RecC>::items(b);
                for (u32 i = 0; i < b->n; i++) {
                    i64 id = lookup(recs[i].person);
                    if (id >= 0) { click_counts[size_t(id)]++; nclicks++; }
                }
            }
        std::vector<u32> click_offset(npeople + 1, 0);
        if (nclicks) {
            for (u32 i = 0; i < npeople; i++) click_offset[i + 1] = click_offset[i] + click_counts[i];
            std::vector<u32> at(click_offset.begin(), click_offset.end() - 1);
            clicks.resize(nclicks);
            for (u32 t = 0; t < nthreads; t++)
                for (Block* b = sh.c[t][bucket].head; b; b = b->next) {
                    RecC* recs = List<RecC>::items(b);
                    for (u32 i = 0; i < b->n; i++) {
                        i64 id = lookup(recs[i].person);
                        if (id >= 0) clicks[at[size_t(id)]++] = {recs[i].ts, recs[i].camp, recs[i].ckey};
                    }
                }
        }

        for (u32 p = 0; p < npeople; p++) {
            u64* ev = events.data() + event_offset[p];
            const u32 n = event_offset[p + 1] - event_offset[p];
            if (n <= 24) {
                for (u32 i = 1; i < n; i++) {
                    u64 v = ev[i];
                    u32 j = i;
                    while (j > 0 && ev[j - 1] > v) { ev[j] = ev[j - 1]; j--; }
                    ev[j] = v;
                }
            } else {
                std::sort(ev, ev + n);
            }
            Click* ck = nclicks ? clicks.data() + click_offset[p] : nullptr;
            const u32 m = nclicks ? click_offset[p + 1] - click_offset[p] : 0;
            if (m > 1)
                // Time ascending; at one instant the smallest click_id comes last, so
                // the last click at or before a session start is the one to credit.
                std::sort(ck, ck + m, [](const Click& x, const Click& y) {
                    return x.ts < y.ts || (x.ts == y.ts && id_less(y.key, x.key));
                });

            std::string person_name = id_string(persons[p]);
            i64 cur_day = -1;
            i32 d_sessions = 0, d_events = 0;
            i64 d_revenue = 0;
            auto emit = [&]() {
                const std::string* top = nullptr;
                if (!cat_touched.empty()) {
                    std::sort(cat_touched.begin(), cat_touched.end());   // categories are numbered in byte order
                    i64 best = 0;
                    for (u32 c : cat_touched) {
                        if (cat_cents[c] > best) { best = cat_cents[c]; top = &sh.cat.categories[c]; }
                        cat_cents[c] = 0;
                    }
                    cat_touched.clear();
                }
                writer.add(person_name.data(), u32(person_name.size()), i32(day0 + cur_day), d_sessions, d_events, d_revenue, top);
                rows++;
            };
            u32 left = 0, right = 0;
            u32 i = 0;
            while (i < n) {
                const u32 start = u32(ev[i] >> 32);
                const i64 day = floor_div(ws + start, DAY) - day0;
                if (day != cur_day) {
                    if (cur_day >= 0) emit();
                    cur_day = day; d_sessions = 0; d_events = 0; d_revenue = 0;
                }
                i64 cents = 0;
                u32 prev = start;
                u32 j = i;
                for (; j < n; j++) {
                    u32 ts = u32(ev[j] >> 32);
                    if (i64(ts) - i64(prev) > SESSION_GAP) break;
                    prev = ts;
                    u32 payload = u32(ev[j]);
                    if (payload) {
                        u32 c = payload & 0xFF;
                        i64 price = payload >> 8;
                        if (!cat_cents[c]) cat_touched.push_back(c);
                        cat_cents[c] += price;
                        cents += price;
                    }
                }
                // purchases have a price of at least 1, so cents > 0 means "has a purchase"
                d_sessions++; d_events += i32(j - i); d_revenue += cents;

                u32 credited = organic;
                if (m) {
                    // Click times count from window_start - 7 days, event times from
                    // window_start, so the clicks in reach are those in [start, start + 7 days].
                    const u64 hi = u64(start) + u64(REACH);
                    while (right < m && ck[right].ts <= hi) {
                        u32 c = ck[right++].camp;
                        if (camp_count[c]++ == 0) { camp_slot[c] = u32(active.size()); active.push_back(c); }
                    }
                    while (left < right && ck[left].ts < start) {
                        u32 c = ck[left++].camp;
                        if (--camp_count[c] == 0) {
                            u32 last = active.back();
                            active[camp_slot[c]] = last;
                            camp_slot[last] = camp_slot[c];
                            active.pop_back();
                        }
                    }
                    if (right > left) credited = ck[right - 1].camp;
                    for (u32 c : active) totals[(size_t(c) * ndays + size_t(day)) * 4 + 3]++;
                }
                i64* cell = &totals[(size_t(credited) * ndays + size_t(day)) * 4];
                cell[0]++;
                cell[1] += cents > 0;
                cell[2] += cents;
                i = j;
            }
            if (cur_day >= 0) emit();
            for (u32 c : active) camp_count[c] = 0;
            active.clear();
        }
    }
    writer.finish();
    sh.user_rows.fetch_add(rows);
}

// ---------------------------------------------------------------- main
template <class F>
static void run_threads(u32 n, F&& body) {
    std::vector<std::thread> threads;
    for (u32 t = 1; t < n; t++) threads.emplace_back([&body, t] { body(t); });
    body(0);
    for (auto& th : threads) th.join();
}

static i64 manifest_time(const std::string& text, const char* key) {
    size_t at = text.find(key);
    if (at == std::string::npos) die("manifest.json lacks a window bound");
    at = text.find(':', at);
    size_t open = text.find('"', at);
    size_t close = text.find('"', open + 1);
    if (at == std::string::npos || open == std::string::npos || close == std::string::npos) die("manifest.json is not in the expected form");
    std::string value = text.substr(open + 1, close - open - 1);
    value.append(32, '\0');
    DateCache cache;
    i64 ts = parse_ts((const u8*)value.data(), close - open - 1, cache);
    if (ts == TS_BAD) die("manifest.json has an unreadable window bound");
    return ts;
}

static void write_campaigns(Shared& sh) {
    const u32 ncamp = u32(sh.cat.campaign_ids.size());
    const u32 ndays = sh.ndays;
    std::vector<i64> total(size_t(ncamp + 1) * ndays * 4, 0);
    for (auto& part : sh.campaign_totals)
        for (size_t i = 0; i < part.size(); i++) total[i] += part[i];
    std::vector<u8> ids, channels;
    std::vector<i32> days;
    std::vector<i64> sessions, purchasing, revenue, assisted;
    auto put = [](std::vector<u8>& out, const std::string& s) {
        u32 len = u32(s.size());
        out.insert(out.end(), (u8*)&len, (u8*)&len + 4);
        out.insert(out.end(), s.begin(), s.end());
    };
    const std::string organic = "organic";
    for (u32 c = 0; c <= ncamp; c++)
        for (u32 d = 0; d < ndays; d++) {
            const i64* cell = &total[(size_t(c) * ndays + d) * 4];
            if (!cell[0] && !cell[3]) continue;
            put(ids, c == ncamp ? organic : sh.cat.campaign_ids[c]);
            put(channels, c == ncamp ? organic : sh.cat.channels[c]);
            days.push_back(i32(sh.day0 + d));
            sessions.push_back(cell[0]); purchasing.push_back(cell[1]); revenue.push_back(cell[2]);
            assisted.push_back(c == ncamp ? 0 : cell[3]);
        }
    pq::File file;
    file.open(sh.cfg.out + "/campaign_daily/part-000.parquet",
              {{"campaign_id", pq::BYTE_ARRAY, pq::UTF8, false}, {"channel", pq::BYTE_ARRAY, pq::UTF8, false},
               {"day", pq::INT32, pq::DATE, false}, {"sessions", pq::INT64, pq::NONE, false},
               {"purchasing_sessions", pq::INT64, pq::NONE, false}, {"revenue_cents", pq::INT64, pq::NONE, false},
               {"assisted_sessions", pq::INT64, pq::NONE, false}});
    size_t n = days.size();
    if (n) {
        pq::Part parts[7];
        parts[0].values = ids.data(); parts[0].values_len = ids.size();
        parts[1].values = channels.data(); parts[1].values_len = channels.size();
        parts[2].values = days.data(); parts[2].values_len = n * 4;
        parts[3].values = sessions.data(); parts[3].values_len = n * 8;
        parts[4].values = purchasing.data(); parts[4].values_len = n * 8;
        parts[5].values = revenue.data(); parts[5].values_len = n * 8;
        parts[6].values = assisted.data(); parts[6].values_len = n * 8;
        file.row_group(n, parts);
    }
    file.close();
}

int main() {
    double t0 = now();
    Shared sh;
    Config& cfg = sh.cfg;
    auto env = [](const char* name, const char* fallback) { const char* v = getenv(name); return std::string(v && *v ? v : fallback); };
    cfg.data = env("DATA_DIR", "/data");
    cfg.out = env("OUTPUT_DIR", "/output");
    std::string threads = env("SS_THREADS", env("NCPU", "").c_str());
    cfg.threads = threads.empty() ? std::thread::hardware_concurrency() : u32(atoi(threads.c_str()));
    if (cfg.threads < 1) cfg.threads = 1;
    if (cfg.threads > 256) cfg.threads = 256;
    std::string chunk = env("SS_CHUNK", "");
    if (!chunk.empty()) cfg.chunk = size_t(std::max(1LL, atoll(chunk.c_str())));

    {
        std::string text;
        FILE* f = fopen((cfg.data + "/manifest.json").c_str(), "rb");
        if (!f) die("cannot open manifest.json");
        char tmp[4096];
        size_t got;
        while ((got = fread(tmp, 1, sizeof tmp, f)) > 0) text.append(tmp, got);
        fclose(f);
        sh.ws = manifest_time(text, "\"window_start\"");
        sh.we = manifest_time(text, "\"window_end\"");
    }
    if (sh.we - sh.ws + REACH >= i64(0xFFFFFFF0u)) die("window longer than this build supports");
    sh.day0 = floor_div(sh.ws, DAY);
    sh.ndays = sh.we > sh.ws ? u32(floor_div(sh.we - 1, DAY) - sh.day0 + 1) : 1;
    sh.cat.load(cfg.data);

    list_csv(sh, "links", sh.link_tasks);
    list_csv(sh, "events", sh.event_tasks);
    list_csv(sh, "clicks", sh.click_tasks);
    size_t event_bytes = 0;
    for (const Task& task : sh.event_tasks) event_bytes += task.end - task.begin;
    // About 16K events a bucket, so a bucket's dedup table stays in cache.
    size_t rows = event_bytes / 64;
    while (sh.abits < 12 && (rows >> sh.abits) > 16384) sh.abits++;
    sh.bbits = sh.abits;
    g_arena_region = std::max<size_t>(64u << 20, std::min<size_t>(4ULL << 30, event_bytes / 2 + (64u << 20)));
    g_arena_region = (g_arena_region + SLAB - 1) & ~(SLAB - 1);

    const u32 T = cfg.threads;
    sh.a.assign(T, std::vector<List<RecA>>(size_t(1) << sh.abits));
    sh.b.assign(T, std::vector<List<RecB>>(size_t(1) << sh.bbits));
    sh.c.assign(T, std::vector<List<RecC>>(size_t(1) << sh.bbits));
    sh.alloc.resize(T);
    sh.campaign_totals.resize(T);
    mkdir(cfg.out.c_str(), 0777);
    mkdir((cfg.out + "/user_daily").c_str(), 0777);
    mkdir((cfg.out + "/campaign_daily").c_str(), 0777);
    double t1 = now();

    run_threads(T, [&](u32 tid) {
        Reader reader;
        if (tid == 0) parse_links(sh, reader);
        parse_events(sh, tid, reader);
        parse_clicks(sh, tid, reader);
        for (auto& list : sh.a[tid]) list.finish();
        for (auto& list : sh.c[tid]) list.finish();
    });
    double t2 = now();

    run_threads(T, [&](u32 tid) {
        dedup_buckets(sh, tid);
        for (auto& list : sh.b[tid]) list.finish();
    });
    double t3 = now();

    // Largest person buckets first, so a heavy one never ends up running alone at the end.
    {
        const u32 nb = 1u << sh.bbits;
        std::vector<u64> size(nb, 0);
        for (u32 t = 0; t < T; t++)
            for (u32 k = 0; k < nb; k++)
                for (Block* b = sh.b[t][k].head; b; b = b->next) size[k] += b->n;
        sh.person_order.resize(nb);
        for (u32 k = 0; k < nb; k++) sh.person_order[k] = k;
        std::sort(sh.person_order.begin(), sh.person_order.end(), [&](u32 x, u32 y) { return size[x] > size[y]; });
    }
    run_threads(T, [&](u32 tid) { process_people(sh, tid); });
    double t4 = now();

    write_campaigns(sh);
    if (sh.user_rows.load() == 0) {                       // an empty table still needs a file with the schema
        pq::File file;
        file.open(cfg.out + "/user_daily/part-000.parquet", UserWriter::schema());
        file.close();
    }
    double t5 = now();
    fprintf(stderr, "threads %u buckets %u | setup %.3f parse %.3f dedup %.3f people %.3f write %.3f | total %.3f s | user rows %llu\n",
            T, 1u << sh.abits, t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4, t5 - t0, (unsigned long long)sh.user_rows.load());
    _exit(0);
}
