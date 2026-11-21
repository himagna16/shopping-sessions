// The dense path. Included by main.cpp, after the general people phase.
//
// The generator writes an ID as a letter plus base 36 of scramble(number, key),
// and scramble is a bijection on 0..2^40 (generator/src/text.rs). With the key,
// a canonical ID turns back into its number, and the numbers are small and laid
// out in order: an event's number is its place in (person, time) order, and a
// user ID's number is person * 512 + alias. So events can be put straight into
// an array by number (which is the dedup), and walking the array meets each
// person's events together and in time order: no hash table, no sort.
//
// Nothing here is taken on trust. The keys are recovered from a sample at run
// time; with no key, or for any ID that does not decode, today's general path
// does the work. Decoding is a bijection whatever the key, so copies of one
// event always meet. A person is finished here only when every link of theirs
// stays among their own first 16 IDs; everyone else (shared devices, long chains, odd IDs) is handed to
// the general people phase by name. An event whose own ID does not decode is
// deduplicated the general way and then joins its person here. Each person is
// claimed exactly once: if
// the data ever shows the same person in two places, the whole run starts again
// on the general path.
#ifndef SS_DENSE_H
#define SS_DENSE_H

#if defined(SS_SCAN4) && defined(SS_ING2) && defined(SS_ALIAS1) && defined(SS_ID2) && !defined(SS_PEOPLE1) && !defined(SS_FORCE_SLOW) && !defined(SS_NODENSE)
#define SS_DENSE_BUILD 1
#endif

#ifdef SS_DENSE_BUILD

#ifndef SS_DW
#define SS_DW 14                    // log2 of the event numbers in one partition
#endif
#ifndef SS_RADIXMIN
#define SS_RADIXMIN 4096           // a person with more clicks than this has them ordered by counting passes
#endif
#ifndef SS_CW
#define SS_CW 10                    // log2 of the people in one click partition
#endif

static const u64 MASK40 = (1ULL << 40) - 1;
static const u64 SCR_M1 = 0x9E3779B97F4BULL, SCR_M2 = 0xD6E8FEB86659ULL;
constexpr u64 inverse40(u64 x) {
    u64 inv = x;                                    // right to 3 bits; each step doubles that
    for (int i = 0; i < 6; i++) inv *= 2 - x * inv;
    return inv & ((1ULL << 40) - 1);
}
static constexpr u64 SCR_M1INV = inverse40(SCR_M1), SCR_M2INV = inverse40(SCR_M2);

static inline u64 scramble40(u64 v, u64 key) {
    u64 m = (((v & MASK40) * SCR_M1) ^ key) & MASK40;
    m ^= m >> 17;
    return (m * SCR_M2) & MASK40;
}
// (number * M1) ^ key, from the value an ID spells: the part that needs no key.
static inline SS_HOT u64 scramble_z(u64 value) {
    const u64 y = (value * SCR_M2INV) & MASK40;
    return y ^ (y >> 17) ^ (y >> 34);
}
static inline SS_HOT u64 unscramble40(u64 value, u64 key) { return ((scramble_z(value) ^ key) * SCR_M1INV) & MASK40; }

// The n bytes at p (the part of an ID after its letter) as a base-36 number,
// if they are its canonical spelling: 1 to 8 of 0-9a-z, no leading zero, below
// 2^40. Anything else gives ~0. p must be readable for 8 bytes.
#ifndef SS_B36_SWAR
// One table row per place: a character's digit times that place's power of 36,
// or a value far out of range for a character that is no digit (small enough
// that eight of them cannot wrap round to a valid sum). The eight
// lookups replace the checks and all but two of the multiplications.
struct B36Table {
    u64 row[8][128];
    B36Table() {
        u64 power = 1;
        for (int place = 7; place >= 0; place--, power *= 36)
            for (int c = 0; c < 128; c++) {
                const int digit = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'z') ? c - 'a' + 10 : -1;
                row[place][c] = digit < 0 ? (1ULL << 56) : u64(digit) * power;      // eight of these still do not wrap
            }
    }
};
alignas(64) static const B36Table g_b36;
static inline SS_HOT u64 b36_value(const u8* p, size_t n) {
    if (n - 1 >= 8) return ~0ULL;
    const u64 raw = rd64(p);
    const unsigned s = unsigned(8 * (8 - n));
    const u64 t = (raw << s) | (0x3030303030303030ULL & ((1ULL << s) - 1));    // right-aligned, padded with '0'
    const u64 value = g_b36.row[0][t & 0x7F] + g_b36.row[1][(t >> 8) & 0x7F] + g_b36.row[2][(t >> 16) & 0x7F] + g_b36.row[3][(t >> 24) & 0x7F]
                    + g_b36.row[4][(t >> 32) & 0x7F] + g_b36.row[5][(t >> 40) & 0x7F] + g_b36.row[6][(t >> 48) & 0x7F] + g_b36.row[7][t >> 56 & 0x7F];
    // a byte over 127, a character that is no digit (its entry puts the sum out of range), a value over 2^40, or a leading zero
    if (((t & 0x8080808080808080ULL) != 0) | (value > MASK40) | (((raw & 0xFF) == '0') & (n > 1))) return ~0ULL;
    return value;
}
#else
static inline SS_HOT u64 b36_value(const u8* p, size_t n) {
    if (n - 1 >= 8) return ~0ULL;
    const u64 raw = rd64(p);
    const unsigned s = unsigned(8 * (8 - n));
    const u64 ZERO = 0x3030303030303030ULL, HIGH = 0x8080808080808080ULL;
    const u64 t = (raw << s) | (ZERO & ((1ULL << s) - 1));              // right-aligned, padded with '0'
    const u64 ge30 = t + 0x5050505050505050ULL, ge3a = t + 0x4646464646464646ULL;
    const u64 ge61 = t + 0x1F1F1F1F1F1F1F1FULL, ge7b = t + 0x0505050505050505ULL;
    const u64 good = ((ge30 & ~ge3a) | (ge61 & ~ge7b)) & ~t & HIGH;
    if ((good != HIGH) | (((raw & 0xFF) == '0') & (n > 1))) return ~0ULL;
    const u64 d = t - ZERO - ((ge61 >> 7) & 0x0101010101010101ULL) * 39;   // digit values, most significant in byte 0
    const u64 pair = (d & 0x00FF00FF00FF00FFULL) * 36 + ((d >> 8) & 0x00FF00FF00FF00FFULL);
    const u64 quad = (pair & 0x0000FFFF0000FFFFULL) * 1296 + ((pair >> 16) & 0x0000FFFF0000FFFFULL);
    const u64 value = (quad & 0xFFFFFFFFULL) * 1679616 + (quad >> 32);
    return value > MASK40 ? ~0ULL : value;
}
#endif

// Two IDs at once: the na bytes at pa and the nb bytes at pb, each as b36_value
// reads them. Both fit one 16-byte register: digits by range, then pairs, fours
// and the two halves by multiply-add, with no table and no loop.
static inline SS_HOT void b36_pair(const u8* pa, size_t na, const u8* pb, size_t nb, u64& va, u64& vb) {
#if defined(__SSE4_1__) && !defined(SS_B36_SWAR) && !defined(SS_B36_TABLE)
    const u64 ZERO = 0x3030303030303030ULL;
    const u64 ra = rd64(pa), rb = rd64(pb);
    const unsigned sa = unsigned(8 * (8 - na)) & 63, sb = unsigned(8 * (8 - nb)) & 63;
    const u64 ta = (ra << sa) | (ZERO & ((1ULL << sa) - 1)), tb = (rb << sb) | (ZERO & ((1ULL << sb) - 1));
    const __m128i x = _mm_set_epi64x((long long)tb, (long long)ta);
    const __m128i x1 = _mm_sub_epi8(x, _mm_set1_epi8('0')), x2 = _mm_sub_epi8(x, _mm_set1_epi8('a'));
    const __m128i ok1 = _mm_cmpeq_epi8(_mm_min_epu8(x1, _mm_set1_epi8(9)), x1);
    const __m128i ok2 = _mm_cmpeq_epi8(_mm_min_epu8(x2, _mm_set1_epi8(25)), x2);
    const u32 good = u32(_mm_movemask_epi8(_mm_or_si128(ok1, ok2)));
    const __m128i digit = _mm_sub_epi8(x1, _mm_and_si128(ok2, _mm_set1_epi8(39)));
    const __m128i pairs = _mm_maddubs_epi16(digit, _mm_set1_epi16(0x0124));                 // 36 * first + second
    const __m128i fours = _mm_madd_epi16(pairs, _mm_set1_epi32(0x00010510));                // 1296 * first + second
    const __m128i value = _mm_add_epi64(_mm_mul_epu32(fours, _mm_set1_epi64x(1679616)), _mm_srli_epi64(fours, 32));
    const u64 a = u64(_mm_cvtsi128_si64(value)), b = u64(_mm_extract_epi64(value, 1));
    const bool bad_a = ((good & 0xFF) != 0xFF) | (na - 1 >= 8) | (a > MASK40) | (((ra & 0xFF) == '0') & (na > 1));
    const bool bad_b = ((good >> 8) != 0xFF) | (nb - 1 >= 8) | (b > MASK40) | (((rb & 0xFF) == '0') & (nb > 1));
    va = bad_a ? ~0ULL : a;
    vb = bad_b ? ~0ULL : b;
#else
    va = b36_value(pa, na);
    vb = b36_value(pb, nb);
#endif
}

static inline u32 b36_text(char* out, u64 v) {
    char tmp[16];
    int at = 16;
    do { tmp[--at] = "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36]; v /= 36; } while (v);
    memcpy(out, tmp + at, size_t(16 - at));
    return u32(16 - at);
}

// Orders values as their base-36 spellings order byte-wise: the digits
// left-aligned to 8 places, then the length (a prefix sorts first).
static inline u64 b36_order(u64 v) {
    static const u64 POW[9] = {1ULL, 36ULL, 1296ULL, 46656ULL, 1679616ULL, 60466176ULL, 2176782336ULL, 78364164096ULL, 2821109907456ULL};
    u32 len = 1;
    while (len < 8 && v >= POW[len]) len++;
    return ((v * POW[8 - len]) << 4) | len;
}

// RecD.a = ts << 32 | payload (as RecB); ts == DEAD for a copy that is not valid.
// RecD.b = ingest << ING_SHIFT | number within the partition << 28 | user.
//   ingest: whole seconds from Dense::ing_base, ING_NONE for an unreadable one.
//   user:   person << 4 | alias, or ODD | thread << 21 | index into that thread's odd_users.
// RecK.x = ts << 32 | person << 4 | alias.
// RecK.y = campaign << 48 | click key; the key is the value a canonical click ID
//          spells, or KODD | thread << 40 | index into that thread's odd_clicks.
static const u32 DWIDTH = 1u << SS_DW;
static const u32 ING_SHIFT = 28 + SS_DW;
static const u32 ING_BITS = 64 - ING_SHIFT;
static const u32 ING_NONE = (1u << ING_BITS) - 1;
static const u32 ING_LEAD_DAYS = SS_DW <= 14 ? 12 : 2;      // how far before the window's first day ingest times can be held
static const u32 ODD = 1u << 27, USER_MASK = (1u << 28) - 1;
static const u64 KODD = 1ULL << 47, KEY_MASK = (1ULL << 48) - 1;
static const u64 EMPTY_SLOT = ~0ULL;
static const u64 IDENT = 0xFEDCBA9876543210ULL;             // every alias its own parent
static const u32 CPER = 1u << SS_CW;
static const u32 UNORDERED = 1;

struct DenseFrag { u32 person, n, amask, flags; u64* ev; u8* al; };
struct DenseExtra { u32 person, alias; u64 ev; };      // a surviving event whose own ID did not decode

// The aliases whose 4-bit entry in `parents` is `root`, one bit each.
static inline u32 nibbles_equal(u64 parents, u32 root) {
    const u64 eq = parents ^ (u64(root) * 0x1111111111111111ULL);
    const u64 zero = ~(((eq & 0x7777777777777777ULL) + 0x7777777777777777ULL) | eq) & 0x8888888888888888ULL;
#if defined(__BMI2__)
    return u32(_pext_u64(zero, 0x8888888888888888ULL));
#else
    u32 mask = 0;
    for (u32 a = 0; a < 16; a++) mask |= u32((zero >> (4 * a + 3)) & 1) << a;
    return mask;
#endif
}

struct DenseCtx {
    // filled while parsing
    std::vector<u64> odd_users, odd_clicks;         // IDs that do not decode, as packed keys
    std::vector<RecC> pending;                      // clicks of such users; `person` holds the ID as written
    std::vector<u32> marked;                        // people this thread handed to the general path
    std::vector<DenseExtra> extras;                 // from the general dedup, for people walked here
    // the walk
    UserWriter writer;
    Shared::Totals* totals = nullptr;
    hvec<RecD> slots;
    hvec<u64> ev, tmp_ev, part_ev, sort_ev, sort_keys, more_ev;
    hvec<u8> al, tmp_al, sort_al, more_al;
    bool ready = false;
    struct Run { u32 person, start, amask, flags; };
    hvec<Run> runs;
    hvec<Click> clicks;
    hvec<u64> click_keys, click_tmp;
    hvec<i64> cat_cents;
    hvec<u32> cat_touched, camp_count, camp_slot, stamp, run_day, run_first;
    u32 session_id = 0;
    u32 memo_user = ~0u;
    u64 memo_person = 0;
    u64 rows = 0;
    u64 n_people = 0, n_split = 0, n_sorted = 0, n_handed = 0, n_stitched = 0, n_generic = 0;
    double cpu[4] = {0, 0, 0, 0};
};

struct Dense {
    bool on = false;
    u64 ekey = 0, ukey = 0;
    u64 nidx = 0;                   // event numbers below this are placed by number
    u32 nparts = 0;
    u32 pmax = 0;                   // person numbers below this are known here
    u32 cparts = 0;
    u64* ptab = nullptr;            // per person: the first-linked alias each alias is joined to, 4 bits each, XOR IDENT
    u8* complex = nullptr;          // per person: handed to the general path
    u8* claimed = nullptr;          // per person: 1 already walked, 2 has events waiting in `extras`
    std::vector<DenseExtra> extras; // by person
    u32* cstart = nullptr;          // per click partition: CPER + 1 offsets into its grouped clicks
    RecK** cdata = nullptr;
    DenseFrag* frags = nullptr;     // per event partition: its first and last person's events
    std::vector<u32> gstart, gfrag; // runs of fragments that are one person
    i64 ing_base = 0;               // seconds since the epoch
    std::atomic<u32> bail{0};       // the data broke a rule this path relies on: do the whole run the general way
    std::atomic<u32> next_part{0}, next_cpart{0}, next_group{0};
    std::vector<DenseCtx> ctx;
    double setup_seconds = 0;
    double wall[4] = {0, 0, 0, 0};
    char** argv = nullptr;
};
static Dense g_dense;

static void dense_bail(u32 why) {
    u32 none = 0;
    g_dense.bail.compare_exchange_strong(none, why, std::memory_order_relaxed);
}

static inline void dense_mark(DenseCtx& cx, u32 person) {
    if (!__atomic_load_n(&g_dense.complex[person], __ATOMIC_RELAXED)) {
        __atomic_store_n(&g_dense.complex[person], u8(1), __ATOMIC_RELAXED);
        cx.marked.push_back(person);
    }
}

static inline u32 dense_odd_user(DenseCtx& cx, u32 tid, u64 key) {
    size_t at = cx.odd_users.size();
    if (at >= (1u << 21)) { dense_bail(2); at = 0; }
    else cx.odd_users.push_back(key);
    return ODD | (tid << 21) | u32(at);
}

// A user ID's person and alias when it decodes to one this path knows.
static inline SS_HOT bool dense_user(const u8* s, size_t n, u64 ukey, u64 pmax, u32& code) {
    if (s[0] != 'u') return false;
    const u64 value = b36_value(s + 1, n - 1);
    if (value == ~0ULL) return false;
    const u64 number = unscramble40(value, ukey);
    if ((number >> 9) >= pmax || (number & 511) >= 16) return false;
    code = (u32(number >> 9) << 4) | u32(number & 15);
    return true;
}

static u64 dense_packed(u8 letter, u64 value) {
    char text[32];
    memset(text, 0, sizeof text);
    text[0] = char(letter);
    const u32 len = 1 + b36_text(text + 1, value);
    return pack_id((const u8*)text, len, letter);
}
static u64 dense_user_key(u32 person, u32 alias) { return dense_packed('u', scramble40(u64(person) * 512 + alias, g_dense.ukey)); }
static u64 dense_click_key(u64 keyword) {
    if (keyword & KODD) return g_dense.ctx[(keyword >> 40) & 63].odd_clicks[keyword & MASK40];
    return dense_packed('k', keyword & MASK40);
}
// Byte-wise order of two click IDs.
static inline bool dense_click_less(u64 a, u64 b) {
    if (!((a | b) & KODD)) return b36_order(a) < b36_order(b);
    return id_less(dense_click_key(a), dense_click_key(b), 'k');
}

// ---------------------------------------------------------------- keys
// Reads the start of the largest event file, recovers both keys from it and
// sizes the tables. False leaves the run on the general path.
// Recovers both keys from the start of the largest event file. False if the
// data does not show them plainly.
static bool dense_recover(Shared& sh, u64& ukey_out, u64& ekey_out, double& rows_out) {
    const u32 T = sh.cfg.threads;
    size_t total_bytes = 0, best_size = 0;
    i64 best = -1;
    for (const Task& task : sh.event_tasks) {
        total_bytes += task.end - task.begin;
        if (sh.files[task.file].size > best_size) { best_size = sh.files[task.file].size; best = i64(task.file); }
    }
    if (best < 0) return false;
    const size_t want = std::min<size_t>(best_size, 256u << 10);
    std::vector<u8> text(want + 64, 0);
    for (size_t have = 0; have < want;) {
        const ssize_t got = pread(sh.files[size_t(best)].fd, text.data() + have, want - have, off_t(have));
        if (got < 0) { if (errno == EINTR) continue; return false; }
        if (got == 0) return false;
        have += size_t(got);
    }
    const u8* base = text.data();
    const u8* end = base + want;
    const u8* line = (const u8*)memchr(base, '\n', want);
    if (!line) return false;
    line++;
    const u8* const first = line;
    struct Row { u64 ze, zu; };                     // ~0 where the ID does not decode
    std::vector<Row> rows;
    size_t lines = 0;
    auto zvalue = [](const u8* s, const u8* e, u8 letter) -> u64 {
        if (e - s >= 2 && s[0] == '"' && e[-1] == '"') { s++; e--; }
        if (e - s < 2 || s[0] != letter) return ~0ULL;
        u8 padded[16] = {0};
        memcpy(padded, s + 1, std::min<size_t>(size_t(e - s - 1), 9));
        const u64 value = b36_value(padded, size_t(e - s - 1));
        return value == ~0ULL ? ~0ULL : scramble_z(value);
    };
    while (line < end) {
        const u8* nl = (const u8*)memchr(line, '\n', size_t(end - line));
        if (!nl) break;
        lines++;
        const u8* c0 = (const u8*)memchr(line, ',', size_t(nl - line));
        const u8* c1 = c0 ? (const u8*)memchr(c0 + 1, ',', size_t(nl - c0 - 1)) : nullptr;
        if (c1) rows.push_back({zvalue(line, c0, 'e'), zvalue(c0 + 1, c1, 'u')});
        line = nl + 1;
    }
    if (lines < 16 || line == first) return false;
    const double rows_est = double(total_bytes) * double(lines) / double(line - first);
    if (rows_est > 4.0e9) return false;

    // The user key. The ID met most often belongs to one of the first few people
    // (the generator numbers its heaviest users first), so its number is small:
    // try each, and keep the key under which the other sampled IDs are people too.
    const u64 pmax = std::min<u64>(1u << 23, u64(rows_est / 30 + rows_est / 300) + 256);
    std::vector<u64> zu;
    for (const Row& r : rows) if (r.zu != ~0ULL) zu.push_back(r.zu);
    if (zu.size() < 16) return false;
    std::sort(zu.begin(), zu.end());
    u64 mode = zu[0];
    size_t mode_count = 0, distinct = 0;
    for (size_t i = 0; i < zu.size();) {
        size_t j = i;
        while (j < zu.size() && zu[j] == zu[i]) j++;
        if (j - i > mode_count) { mode_count = j - i; mode = zu[i]; }
        distinct++;
        i = j;
    }
    if (distinct < 4) return false;
    u64 ukey = 0;
    u32 found = 0;
    for (u64 person = 0; person < 256; person++)
        for (u64 alias = 0; alias < 16; alias++) {
            const u64 key = mode ^ (((person * 512 + alias) * SCR_M1) & MASK40);
            size_t good = 0, seen = 0;
            for (u64 z : zu) {
                good += ((((z ^ key) * SCR_M1INV) & MASK40) >> 9) < pmax;
                if (++seen == 8 && good < 6) break;
            }
            if (seen == zu.size() && good * 100 >= zu.size() * 98) { ukey = key; found++; }
        }
    if (found != 1) return false;

    // The event key. A sampled event of one of the first five people has a number
    // below their share of the events (at most 2% each): try each such number, and
    // keep the key under which the sampled event IDs all have numbers in range.
    const u64 nidx = u64(rows_est + rows_est / 32) + 4096;
    std::vector<u64> ze;
    for (const Row& r : rows) if (r.ze != ~0ULL) ze.push_back(r.ze);
    if (ze.size() < 16) return false;
    u64 low_person = ~0ULL, za = 0, zb = ~0ULL;
    for (const Row& r : rows) {
        if (r.ze == ~0ULL || r.zu == ~0ULL) continue;
        const u64 person = (((r.zu ^ ukey) * SCR_M1INV) & MASK40) >> 9;
        if (person < low_person) { low_person = person; za = r.ze; zb = ~0ULL; }
        else if (person == low_person && r.ze != za && zb == ~0ULL) zb = r.ze;
    }
    if (low_person == ~0ULL) return false;
    u64 bound = low_person < 5 ? u64((double(low_person) + 1) * rows_est / 50) + 1024 : nidx;
    if (bound > nidx) bound = nidx;
    if (bound > (1ULL << 26)) return false;
    std::mutex lock;
    std::vector<u64> keys;
    {
        std::vector<std::thread> workers;
        const u64 step = bound / T + 1;
        for (u32 t = 0; t < T; t++)
            workers.emplace_back([&, t] {
                const u64 from = u64(t) * step, to = std::min(bound, from + step);
                u64 w = (from * SCR_M1) & MASK40;
                for (u64 v = from; v < to; v++, w = (w + SCR_M1) & MASK40) {
                    const u64 key = za ^ w;
                    if (zb != ~0ULL && (((zb ^ key) * SCR_M1INV) & MASK40) >= bound) continue;
                    size_t good = 0, seen = 0;
                    for (u64 z : ze) {
                        good += (((z ^ key) * SCR_M1INV) & MASK40) < nidx;
                        if (++seen == 8 && good < 6) break;
                    }
                    if (seen == ze.size() && good * 100 >= ze.size() * 98) {
                        std::lock_guard<std::mutex> guard(lock);
                        keys.push_back(key);
                    }
                }
            });
        for (auto& w : workers) w.join();
    }
    if (keys.size() != 1) return false;

    ukey_out = ukey;
    ekey_out = keys[0];
    rows_out = rows_est;
    return true;
}

// Recovers the keys and sizes the tables. False leaves the run on the general path.
// SS_DENSE=0 switches the dense path off; SS_DENSE=2 (tests) takes it even when
// no key is found, with arbitrary keys: every ID then decodes to some number,
// nearly all of them out of range, and the result must still be exact.
static bool dense_setup(Shared& sh) {
    Dense& D = g_dense;
    const double began = now();
    bool forced = false;
    {
        const char* v = getenv("SS_DENSE");
        if (v && *v == '0') return false;
        forced = v && *v == '2';
    }
    const u32 T = sh.cfg.threads;
    if (T > 64 || sh.cat.campaign_ids.size() + 1 >= 65535 || sh.ndays > 30 || sh.we <= sh.ws) return false;
    if ((u64(ING_LEAD_DAYS) + sh.ndays + 2) * 86400 >= ING_NONE) return false;      // the window's ingest times must fit their field
    u64 ukey = 0, ekey = 0;
    double rows_est = 0;
    if (!dense_recover(sh, ukey, ekey, rows_est)) {
        if (!forced) return false;
        size_t total_bytes = 0;
        for (const Task& task : sh.event_tasks) total_bytes += task.end - task.begin;
        rows_est = double(total_bytes) / 40 + 16;
        if (rows_est > 4.0e9) return false;
        ukey = 0x5DEECE66DULL;
        ekey = 0x2545F4914FULL;
    }
    const u64 pmax = std::min<u64>(1u << 23, u64(rows_est / 30 + rows_est / 300) + 256);
    const u64 nidx = u64(rows_est + rows_est / 32) + 4096;
    D.ukey = ukey;
    D.ekey = ekey;
    D.nidx = nidx;
    D.nparts = u32(nidx >> SS_DW) + 1;
    D.pmax = u32(pmax);
    D.cparts = (D.pmax >> SS_CW) + 1;
    D.ptab = big_alloc<u64>(D.pmax + 1);
    D.complex = big_alloc<u8>(D.pmax + 1);
    D.claimed = big_alloc<u8>(D.pmax + 1);
    D.cstart = big_alloc<u32>(size_t(D.cparts) * (CPER + 1));
    D.cdata = big_alloc<RecK*>(D.cparts);
    D.frags = big_alloc<DenseFrag>(size_t(D.nparts) * 2);
    D.ing_base = (sh.day0 - i64(ING_LEAD_DAYS)) * 86400;
    D.ctx.resize(T + 1);            // one per thread, and a spare for dense_leftovers
    D.on = true;
    g_dense_on = true;
    D.setup_seconds = now() - began;
    return true;
}

// ---------------------------------------------------------------- lines
// The dense parsers read one line at a time. A line of the usual shape is
// settled from the comma, newline and quote bits of one or two 64-byte windows:
// the separators come out by position, and the quote rules are checked with the
// same carry trick as scan_rows (an opening quote directly after a separator, a
// closing one directly before the next, none elsewhere). Any other line (too
// long for its windows, the wrong number of fields, a quote out of place) is
// decided byte by byte, so the result is exact.
template <int NF> struct Fields { const u8* s[NF]; const u8* e[NF]; };      // each field's content, quotes removed

// One bit for every place where the quote rules are broken, given the
// separators S and quotes Q of a window that starts at the start of a field.
// A fault always shows at or before the separator that ends its field.
static inline SS_HOT u64 quote_faults(u64 S, u64 Q) {
    const u64 first = (S << 1) | 1;
    const u64 open = Q & first, other = Q & ~first;
    const u64 u = open << 1, t = other << 1;
    const u64 sum = ~S + open;
    return (t & ~S) | (u & S) | ((sum ^ (t | u)) & S);
}

// Decides a line byte by byte: [a, end) without its newline. False if malformed.
template <int NF>
static bool split_exact(const u8* a, const u8* end, Fields<NF>& f) {
    int n = 0;
    for (;;) {
        const u8* comma = (const u8*)memchr(a, ',', size_t(end - a));
        const u8* stop_at = comma ? comma : end;
        if (n == NF) return false;
        const u8 *s = a, *e = stop_at;
        if (memchr(a, '"', size_t(stop_at - a))) {
            if (stop_at - a < 2 || a[0] != '"' || stop_at[-1] != '"' || memchr(a + 1, '"', size_t(stop_at - a - 2))) return false;
            s = a + 1;
            e = stop_at - 1;
        }
        f.s[n] = s;
        f.e[n] = e;
        n++;
        if (!comma) break;
        a = comma + 1;
    }
    return n == NF;
}

// An event line: the first three fields end within 64 bytes of the line's
// start, the other four and the newline within 64 bytes of the fourth's start.
static inline SS_HOT bool line7(const u8* p, Fields<7>& f, const u8*& next) {
    const Masks A = classify(p);
    u64 ca = A.comma;
    const u32 c1 = tz64(ca); ca &= ca - 1;
    const u32 c2 = tz64(ca); ca &= ca - 1;
    const u32 c3 = tz64(ca);
    if (c3 >= 64 || (A.nl & ((1ULL << c3) - 1))) return false;
    const u8* q = p + c3 + 1;
    const Masks B = classify(q);
    u64 cb = B.comma;
    const u32 c4 = tz64(cb); cb &= cb - 1;
    const u32 c5 = tz64(cb); cb &= cb - 1;
    const u32 c6 = tz64(cb); cb &= cb - 1;
    const u32 c7 = tz64(cb);
    const u32 e = tz64(B.nl);
    const u64 QA = A.quote, QB = B.quote;
    const u64 bad_a = quote_faults(A.comma | A.nl, QA) & (~0ULL >> (63 - c3));
    const u64 bad_b = quote_faults(B.comma | B.nl, QB) & (~0ULL >> ((63 - e) & 63));
    if ((c6 >= e) | (e >= 64) | (c7 <= e) | ((bad_a | bad_b) != 0)) return false;
    const u32 o0 = u32(QA & 1), o1 = u32(QA >> (c1 + 1)) & 1, o2 = u32(QA >> (c2 + 1)) & 1;
    const u32 o3 = u32(QB & 1), o4 = u32(QB >> (c4 + 1)) & 1, o5 = u32(QB >> (c5 + 1)) & 1, o6 = u32(QB >> (c6 + 1)) & 1;
    f.s[0] = p + o0;          f.e[0] = p + c1 - o0;
    f.s[1] = p + c1 + 1 + o1; f.e[1] = p + c2 - o1;
    f.s[2] = p + c2 + 1 + o2; f.e[2] = p + c3 - o2;
    f.s[3] = q + o3;          f.e[3] = q + c4 - o3;
    f.s[4] = q + c4 + 1 + o4; f.e[4] = q + c5 - o4;
    f.s[5] = q + c5 + 1 + o5; f.e[5] = q + c6 - o5;
    f.s[6] = q + c6 + 1 + o6; f.e[6] = q + e - o6;
    next = q + e + 1;
    return true;
}

// A click line: all four fields and the newline within 64 bytes.
static inline SS_HOT bool line4(const u8* p, Fields<4>& f, const u8*& next) {
    const Masks A = classify(p);
    u64 ca = A.comma;
    const u32 c1 = tz64(ca); ca &= ca - 1;
    const u32 c2 = tz64(ca); ca &= ca - 1;
    const u32 c3 = tz64(ca); ca &= ca - 1;
    const u32 c4 = tz64(ca);
    const u32 e = tz64(A.nl);
    const u64 Q = A.quote;
    const u64 bad = quote_faults(A.comma | A.nl, Q) & (~0ULL >> ((63 - e) & 63));
    if ((c3 >= e) | (e >= 64) | (c4 <= e) | (bad != 0)) return false;
    const u32 o0 = u32(Q & 1), o1 = u32(Q >> (c1 + 1)) & 1, o2 = u32(Q >> (c2 + 1)) & 1, o3 = u32(Q >> (c3 + 1)) & 1;
    f.s[0] = p + o0;          f.e[0] = p + c1 - o0;
    f.s[1] = p + c1 + 1 + o1; f.e[1] = p + c2 - o1;
    f.s[2] = p + c2 + 1 + o2; f.e[2] = p + c3 - o2;
    f.s[3] = p + c3 + 1 + o3; f.e[3] = p + e - o3;
    next = p + e + 1;
    return true;
}

// A link line: both fields and the newline within 64 bytes.
static inline SS_HOT bool line2(const u8* p, Fields<2>& f, const u8*& next) {
    const Masks A = classify(p);
    u64 ca = A.comma;
    const u32 c1 = tz64(ca); ca &= ca - 1;
    const u32 c2 = tz64(ca);
    const u32 e = tz64(A.nl);
    const u64 Q = A.quote;
    const u64 bad = quote_faults(A.comma | A.nl, Q) & (~0ULL >> ((63 - e) & 63));
    if ((c1 >= e) | (e >= 64) | (c2 <= e) | (bad != 0)) return false;
    const u32 o0 = u32(Q & 1), o1 = u32(Q >> (c1 + 1)) & 1;
    f.s[0] = p + o0;          f.e[0] = p + c1 - o0;
    f.s[1] = p + c1 + 1 + o1; f.e[1] = p + e - o1;
    next = p + e + 1;
    return true;
}

// The same three readers for a line whose newline is already known (the usual
// way in: line_rows finds the newlines of a batch of lines first, so reading one
// line does not wait on the line before it). Only commas and quotes are looked for.
struct CommaQuote { u64 comma, quote; };
static inline SS_HOT CommaQuote comma_quote(const u8* p) {
#if defined(__AVX2__)
    const __m256i a = _mm256_loadu_si256((const __m256i*)p), b = _mm256_loadu_si256((const __m256i*)(p + 32));
    const __m256i c = _mm256_set1_epi8(','), q = _mm256_set1_epi8('"');
    CommaQuote m;
    m.comma = u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, c))) | (u64(u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(b, c)))) << 32);
    m.quote = u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, q))) | (u64(u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(b, q)))) << 32);
    return m;
#else
    const Masks all = classify(p);
    return {all.comma, all.quote};
#endif
}
static inline SS_HOT u64 newline_mask(const u8* p) {
#if defined(__AVX2__)
    const __m256i n = _mm256_set1_epi8('\n');
    return u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i*)p), n))) |
           (u64(u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i*)(p + 32)), n)))) << 32);
#else
    return classify(p).nl;
#endif
}

// [p, nl) is the line, nl its newline (the first one at or after p).
static inline SS_HOT bool line_known(const u8* p, const u8* nl, Fields<7>& f) {
    const size_t len = size_t(nl - p);
    const CommaQuote A = comma_quote(p);
    u64 ca = A.comma;
    const u32 c1 = tz64(ca); ca &= ca - 1;
    const u32 c2 = tz64(ca); ca &= ca - 1;
    const u32 c3 = tz64(ca);
    if (c3 >= 64 || c3 >= len) return false;                    // the third comma within 64 bytes and within the line
    const u8* q = p + c3 + 1;
    const size_t e = size_t(nl - q);
    if (e >= 64) return false;
    const CommaQuote B = comma_quote(q);
    u64 cb = B.comma;
    const u32 c4 = tz64(cb); cb &= cb - 1;
    const u32 c5 = tz64(cb); cb &= cb - 1;
    const u32 c6 = tz64(cb); cb &= cb - 1;
    const u32 c7 = tz64(cb);
    const u64 QA = A.quote, QB = B.quote;
    const u64 bad_a = quote_faults(A.comma, QA) & (~0ULL >> (63 - c3));
    const u64 bad_b = quote_faults(B.comma | (1ULL << e), QB) & (~0ULL >> (63 - e));
    if ((c6 >= e) | (c7 <= e) | ((bad_a | bad_b) != 0)) return false;
    const u32 o0 = u32(QA & 1), o1 = u32(QA >> (c1 + 1)) & 1, o2 = u32(QA >> (c2 + 1)) & 1;
    const u32 o3 = u32(QB & 1), o4 = u32(QB >> (c4 + 1)) & 1, o5 = u32(QB >> (c5 + 1)) & 1, o6 = u32(QB >> (c6 + 1)) & 1;
    f.s[0] = p + o0;          f.e[0] = p + c1 - o0;
    f.s[1] = p + c1 + 1 + o1; f.e[1] = p + c2 - o1;
    f.s[2] = p + c2 + 1 + o2; f.e[2] = p + c3 - o2;
    f.s[3] = q + o3;          f.e[3] = q + c4 - o3;
    f.s[4] = q + c4 + 1 + o4; f.e[4] = q + c5 - o4;
    f.s[5] = q + c5 + 1 + o5; f.e[5] = q + c6 - o5;
    f.s[6] = q + c6 + 1 + o6; f.e[6] = nl - o6;
    return true;
}
static inline SS_HOT bool line_known(const u8* p, const u8* nl, Fields<4>& f) {
    const size_t e = size_t(nl - p);
    if (e >= 64) return false;
    const CommaQuote A = comma_quote(p);
    u64 ca = A.comma;
    const u32 c1 = tz64(ca); ca &= ca - 1;
    const u32 c2 = tz64(ca); ca &= ca - 1;
    const u32 c3 = tz64(ca); ca &= ca - 1;
    const u32 c4 = tz64(ca);
    const u64 Q = A.quote;
    const u64 bad = quote_faults(A.comma | (1ULL << e), Q) & (~0ULL >> (63 - e));
    if ((c3 >= e) | (c4 <= e) | (bad != 0)) return false;
    const u32 o0 = u32(Q & 1), o1 = u32(Q >> (c1 + 1)) & 1, o2 = u32(Q >> (c2 + 1)) & 1, o3 = u32(Q >> (c3 + 1)) & 1;
    f.s[0] = p + o0;          f.e[0] = p + c1 - o0;
    f.s[1] = p + c1 + 1 + o1; f.e[1] = p + c2 - o1;
    f.s[2] = p + c2 + 1 + o2; f.e[2] = p + c3 - o2;
    f.s[3] = p + c3 + 1 + o3; f.e[3] = nl - o3;
    return true;
}
static inline SS_HOT bool line_known(const u8* p, const u8* nl, Fields<2>& f) {
    const size_t e = size_t(nl - p);
    if (e >= 64) return false;
    const CommaQuote A = comma_quote(p);
    u64 ca = A.comma;
    const u32 c1 = tz64(ca); ca &= ca - 1;
    const u32 c2 = tz64(ca);
    const u64 Q = A.quote;
    const u64 bad = quote_faults(A.comma | (1ULL << e), Q) & (~0ULL >> (63 - e));
    if ((c1 >= e) | (c2 <= e) | (bad != 0)) return false;
    const u32 o0 = u32(Q & 1), o1 = u32(Q >> (c1 + 1)) & 1;
    f.s[0] = p + o0;          f.e[0] = p + c1 - o0;
    f.s[1] = p + c1 + 1 + o1; f.e[1] = nl - o1;
    return true;
}

// Calls row(fields) for every well-formed line of NF fields that starts in
// [start, stop); `last` is the newline that ends the last such line, with 64
// readable bytes after it (as for scan_rows).
template <int NF, class Fast, class Row>
static inline SS_INLINE void line_rows(const u8* buf, size_t start, size_t stop, size_t last, Fast&& fast, Row&& row) {
#ifdef SS_LINE0
    // Measurement: the block scanner underneath, as before.
    (void)fast;
    scan_rows<NF>(buf, start, stop, last, [&](ROW_PARAMS) SS_INLINE {
        Fields<NF> f;
        u32 quoted[NF];
        for (int i = 0; i < NF; i++) quoted[i] = u32(buf[a[i] - 1] == '"');
        for (int i = 0; i < NF; i++) {
            f.s[i] = buf + a[i];
            f.e[i] = i + 1 < NF ? buf + a[i + 1] - quoted[i + 1] - 1 - quoted[i] : buf + nlpos - quoted[i];
        }
        row(f);
    });
#elif !defined(SS_LINE1)
    // The newlines of a batch of lines first, 64 bytes at a time; then each line
    // of the batch on its own, its end already known.
    (void)fast;
    enum { BATCH = 64 };
    u32 ends[BATCH + 72];
    size_t block = start, line = start;
    for (;;) {
        u32 count = 0;
        while (count < BATCH && block <= last) {
            __builtin_prefetch(buf + block + 1024);
            u64 nl = newline_mask(buf + block);
            const u32 found = u32(__builtin_popcountll(nl));
            ends[count] = u32(block) + tz64(nl); nl &= nl - 1;
            ends[count + 1] = u32(block) + tz64(nl); nl &= nl - 1;
            for (u32 i = 2; i < found; i++) { ends[count + i] = u32(block) + tz64(nl); nl &= nl - 1; }
            count += found;
            block += 64;
        }
        for (u32 k = 0; k < count; k++) {
            if (line >= stop) return;
            const u8* p = buf + line;
            const u8* nl = buf + ends[k];
            line = size_t(ends[k]) + 1;
            Fields<NF> f;
            if (__builtin_expect(!line_known(p, nl, f), 0) && !split_exact<NF>(p, nl, f)) continue;
            row(f);
        }
        if (block > last) return;                               // every newline up to `last` has been used
    }
#else
    const u8* p = buf + start;
    const u8* const stop_at = buf + stop;
    const u8* const last_nl = buf + last;
    while (p < stop_at) {
        __builtin_prefetch(p + 1024);
        Fields<NF> f;
        const u8* next;
        if (__builtin_expect(!fast(p, f, next), 0)) {
            const u8* nl = (const u8*)memchr(p, '\n', size_t(last_nl - p) + 1);    // found: `last` is a newline
            next = nl + 1;
            if (!split_exact<NF>(p, nl, f)) { p = next; continue; }
        }
        p = next;
        row(f);
    }
#endif
}

#ifdef SS_SELFTEST
// Test builds only (SS_LINETEST=N): random lines, mostly well-formed, some
// broken; whenever a fast line reader accepts a line, the byte-by-byte reader
// must accept it too and give the same fields and the same next line.
template <int NF, class Fast>
static u64 line_selftest_one(u64& state, u64 lines, Fast&& fast, u64& accepted) {
    auto rnd = [&state]() { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; };
    std::vector<u8> buf;
    u64 failures = 0;
    for (u64 done = 0; done < lines;) {
        buf.clear();
        std::vector<size_t> starts;
        for (int k = 0; k < 64; k++) {
            starts.push_back(buf.size());
            const int nf = rnd() % 10 < 9 ? NF : int(rnd() % 10) + 1;
            for (int i = 0; i < nf; i++) {
                if (i) buf.push_back(',');
                size_t len = rnd() % 14;
                if (rnd() % 40 == 0) len = rnd() % 90;
                const bool quote = rnd() % 4 == 0;
                if (quote) buf.push_back('"');
                for (size_t j = 0; j < len; j++) buf.push_back(u8("abcxyz0189 :-TZ"[rnd() % 15]));
                if (quote) buf.push_back('"');
            }
            const size_t from = starts.back();
            if (rnd() % 6 == 0 && buf.size() > from) {
                const size_t at = from + rnd() % (buf.size() - from);
                switch (rnd() % 5) {
                    case 0: buf[at] = '"'; break;
                    case 1: buf[at] = ','; break;
                    case 2: buf.insert(buf.begin() + long(at), '"'); break;
                    case 3: buf.erase(buf.begin() + long(at)); break;
                    default: buf[at] = '\r'; break;
                }
            }
            buf.push_back('\n');
        }
        const size_t last = buf.size() - 1;
        buf.resize(buf.size() + 256, 'x');
        for (size_t line : starts) {
            const u8* p = buf.data() + line;
            const u8* nl = (const u8*)memchr(p, '\n', last - line + 1);
            Fields<NF> exact, quick;
            const bool exact_ok = split_exact<NF>(p, nl, exact);
            const u8* next = nullptr;
            const bool quick_ok = fast(p, quick, next);
            done++;
            {
                Fields<NF> known;
                if (line_known(p, nl, known)) {
                    bool same = exact_ok;
                    for (int i = 0; same && i < NF; i++) same = exact.s[i] == known.s[i] && exact.e[i] == known.e[i];
                    if (!same && failures++ < 5) fprintf(stderr, "line test (known end): %d fields, mismatch on [%.*s]\n", NF, int(nl - p), (const char*)p);
                    accepted += 1ULL << 32;
                }
            }
            if (!quick_ok) continue;
            accepted++;
            bool same = exact_ok && next == nl + 1;
            for (int i = 0; same && i < NF; i++) same = exact.s[i] == quick.s[i] && exact.e[i] == quick.e[i];
            if (!same) {
                if (failures++ < 5) fprintf(stderr, "line test: %d fields, mismatch on [%.*s]\n", NF, int(nl - p), (const char*)p);
            }
        }
    }
    return failures;
}
static void line_selftest(u64 lines) {
    u64 state = 0x9E3779B97F4A7C15ULL ^ lines, a7 = 0, a4 = 0, a2 = 0;
    {
        // The paired ID decode against the single one, on IDs of every shape.
        auto rnd = [&state]() { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; };
        u64 wrong = 0, valid = 0;
        for (u64 i = 0; i < lines; i++) {
            u8 text[2][32];
            size_t len[2];
            for (int k = 0; k < 2; k++) {
                memset(text[k], 0, sizeof text[k]);
                len[k] = rnd() % 11;
                const u64 kind = rnd() % 8;
                for (size_t j = 0; j < 16; j++) text[k][j] = u8(kind == 0 ? "0123456789abcdefghijklmnopqrstuvwxyzAZ ,\"/:`{"[rnd() % 45] : kind == 1 ? rnd() & 0xFF : kind == 2 ? "AZ/:`{ -"[rnd() % 8] : "0123456789abcdefghijklmnopqrstuvwxyz"[rnd() % 36]);
            }
            u64 a, b;
            b36_pair(text[0], len[0], text[1], len[1], a, b);
            const u64 wa = b36_value(text[0], len[0]), wb = b36_value(text[1], len[1]);
            for (int k = 0; k < 2; k++) {
                // the plain definition: 1 to 8 of 0-9a-z, no leading zero, below 2^40
                u64 plain = 0;
                bool fine = len[k] >= 1 && len[k] <= 8 && !(len[k] > 1 && text[k][0] == '0');
                for (size_t j = 0; fine && j < len[k]; j++) {
                    const u8 c = text[k][j];
                    if (c >= '0' && c <= '9') plain = plain * 36 + (c - '0');
                    else if (c >= 'a' && c <= 'z') plain = plain * 36 + (c - 'a' + 10);
                    else fine = false;
                }
                if (!fine || plain > MASK40) plain = ~0ULL;
                if (plain != (k ? wb : wa)) { if (wrong++ < 5) fprintf(stderr, "single decode mismatch: [%.*s] got %llx want %llx\n", int(len[k]), (const char*)text[k], (unsigned long long)(k ? wb : wa), (unsigned long long)plain); }
            }
            valid += (wa != ~0ULL) + (wb != ~0ULL);
            if (a != wa || b != wb) { if (wrong++ < 5) fprintf(stderr, "pair decode mismatch: [%.*s] [%.*s] got %llx %llx want %llx %llx\n", int(len[0]), (const char*)text[0], int(len[1]), (const char*)text[1], (unsigned long long)a, (unsigned long long)b, (unsigned long long)wa, (unsigned long long)wb); }
        }
        fprintf(stderr, "pair decode: %llu pairs, %llu valid IDs, %llu mismatches\n", (unsigned long long)lines, (unsigned long long)valid, (unsigned long long)wrong);
        if (wrong) _exit(1);
    }
    const u64 bad = line_selftest_one<7>(state, lines, line7, a7) + line_selftest_one<4>(state, lines, line4, a4) + line_selftest_one<2>(state, lines, line2, a2);
    fprintf(stderr, "line test: %llu lines each; accepted %llu / %llu / %llu, with a known end %llu / %llu / %llu; %llu mismatches\n", (unsigned long long)lines,
            (unsigned long long)(a7 & 0xFFFFFFFF), (unsigned long long)(a4 & 0xFFFFFFFF), (unsigned long long)(a2 & 0xFFFFFFFF),
            (unsigned long long)(a7 >> 32), (unsigned long long)(a4 >> 32), (unsigned long long)(a2 >> 32), (unsigned long long)bad);
    _exit(bad ? 1 : 0);
}
#endif

// After the links are read: the links of everyone handed to the general path
// join the general union-find, and the alias map is built from it. Only links
// hand people over, so this runs on the links thread while the others parse.
static void dense_settle(Shared& sh) {
    Dense& D = g_dense;
    Links& links = sh.odd_links;
    for (DenseCtx& cx : D.ctx)
        for (u32 person : cx.marked) {
            if (D.complex[person] == 2) continue;
            D.complex[person] = 2;
            const u64 parents = D.ptab[person] ^ IDENT;
            if (parents == IDENT) continue;
            for (u32 a = 0; a < 16; a++) {
                const u32 parent = u32((parents >> (4 * a)) & 15);
                if (parent != a) links.add(dense_user_key(person, a), dense_user_key(person, parent));
            }
        }
    const double began = now();
    links.solve(sh.alias);
    sh.links_union = now() - began;
}

// ---------------------------------------------------------------- parse
// Links whose two IDs are among one person's first 16 are joined in that
// person's own little table; any other link goes to the general union-find and
// hands the people it touches to the general path.
static void parse_links_dense(Shared& sh, Reader& reader) {
    Dense& D = g_dense;
    DenseCtx& cx = D.ctx[0];
    const double began = now();
    const u64 ukey = D.ukey, pmax = D.pmax;
    Links& links = sh.odd_links;
    struct Local { u32 person; u8 a, b; };
    Local ring[64];
    u32 in = 0, out = 0;
    auto apply = [&](const Local& l) {
        // Kept flat: every alias holds its group's first alias directly.
        u64 parents = D.ptab[l.person] ^ IDENT;
        const u32 a = u32((parents >> (4 * l.a)) & 15), b = u32((parents >> (4 * l.b)) & 15);
        if (a == b) return;
        const u64 ONES = 0x1111111111111111ULL;
        const u64 eq = parents ^ (u64(b) * ONES);
        const u64 zero = ~(((eq & 0x7777777777777777ULL) + 0x7777777777777777ULL) | eq) & 0x8888888888888888ULL;
        const u64 moved = (zero >> 3) * 15;
        parents = (parents & ~moved) | ((u64(a) * ONES) & moved);
        D.ptab[l.person] = parents ^ IDENT;
    };
    for (const Task& task : sh.link_tasks) {
        const u8* buf; size_t start, stop, last;
        if (!reader.load(sh.files[task.file], task, buf, start, stop, last)) { release_chunk(sh.files[task.file], task); continue; }
        line_rows<2>(buf, start, stop, last, line2, [&](const Fields<2>& f) SS_INLINE {
            const u8 *s0 = f.s[0], *e0 = f.e[0], *s1 = f.s[1], *e1 = f.e[1];
            if (s0 == e0 || s1 == e1) return;
            u32 ca = 0, cb = 0;
            const bool da = dense_user(s0, size_t(e0 - s0), ukey, pmax, ca);
            const bool db = dense_user(s1, size_t(e1 - s1), ukey, pmax, cb);
            if (da && db && (ca >> 4) == (cb >> 4)) {
                __builtin_prefetch(&D.ptab[ca >> 4], 1);
                ring[in++ & 63] = {ca >> 4, u8(ca & 15), u8(cb & 15)};
                if (in - out > 24) apply(ring[out++ & 63]);
                return;
            }
            links.add(pack_id(s0, size_t(e0 - s0), 'u'), pack_id(s1, size_t(e1 - s1), 'u'));
            if (da) dense_mark(cx, ca >> 4);
            if (db) dense_mark(cx, cb >> 4);
        });
        release_chunk(sh.files[task.file], task);
    }
    while (out != in) apply(ring[out++ & 63]);
    sh.links_parse = now() - began;
    dense_settle(sh);
    sh.links_seconds = now() - began;
}

static void parse_events_dense(Shared& sh, u32 tid, Reader& reader) {
    Dense& D = g_dense;
    DenseCtx& cx = D.ctx[tid];
    const u32 VIEW = rd32((const u8*)"view"), CART = rd32((const u8*)"cart");
    const u64 PURCHASE = rd64((const u8*)"purchase");
    const i64 ws = sh.ws, we = sh.we;
    const int gshift = 64 - int(sh.abits);
    const Catalogs& cat = sh.cat;
    Scatter<RecA>& general = sh.a[tid];
    Scatter<RecD, 64>& lists = sh.d[tid];
    const u64 ekey = D.ekey, ukey = D.ukey, nidx = D.nidx, pmax = D.pmax;
    const i64 ing_base = D.ing_base;
    const u32 day_lead = ING_LEAD_DAYS * 86400;
    DateCache dates;
#ifndef SS_PL
#define SS_PL 0                      // measurement builds only: stop the event row handler after step N
#endif
    [[maybe_unused]] u64 sink = 0;
    for (;;) {
        size_t at = sh.next_event.fetch_add(1, std::memory_order_relaxed);
        if (at >= sh.event_tasks.size()) break;
        const Task& task = sh.event_tasks[at];
        const u8* buf; size_t start, stop, last;
        if (!reader.load(sh.files[task.file], task, buf, start, stop, last)) { release_chunk(sh.files[task.file], task); continue; }
#if SS_PL == 1
        for (size_t i = start; i < last; i += 64) sink += buf[i];
        release_chunk(sh.files[task.file], task);
        continue;
#endif
        line_rows<7>(buf, start, stop, last, line7, [&](const Fields<7>& f) SS_INLINE {
#if SS_PL == 2
            sink += size_t(f.s[0] - buf) + size_t(f.e[6] - buf); return;
#endif
            const u8 *s0 = f.s[0], *e0 = f.e[0], *s1 = f.s[1], *e1 = f.e[1], *s2 = f.s[2], *e2 = f.e[2], *s3 = f.s[3], *e3 = f.e[3];
            const u8 *s4 = f.s[4], *e4 = f.e[4], *s5 = f.s[5], *e5 = f.e[5], *s6 = f.s[6], *e6 = f.e[6];
            if (s0 == e0) return;                               // no event_id
#if SS_PL == 3
            sink += size_t(e0 - s0) + size_t(e1 - s1) + size_t(e2 - s2) + size_t(e3 - s3) + size_t(e4 - s4) + size_t(e5 - s5) + size_t(e6 - s6); return;
#endif
            // As in parse_events: is the copy valid and on time, and where does its
            // ingest_ts stand among the copies of its event. Here the ingest time is
            // kept as whole seconds from a little before the window.
            const i64 ts = parse_ts(s2, size_t(e2 - s2), dates);
            const bool in_window = ts != TS_BAD && ts >= ws && ts < we;
            bool timely = false, settled = false, far = false;
            u32 ing = ING_NONE;
            if (in_window && e3 - s3 == 20) {
                const u32 d = u32(u64(ts - sh.day_base) / u64(DAY));
                if (u32(rd64(s3) == sh.day_text8[d]) & u32(rd16(s3 + 8) == sh.day_text2[d]) & u32(s3[10] == 'T') & u32(s3[19] == 'Z')) {
                    const i32 seconds = tod_seconds(s3 + 11);
                    if (seconds >= 0) { ing = day_lead + d * 86400 + u32(seconds); timely = true; }
                    settled = true;
                }
            }
            if (!settled) {
                const i64 ingest = (e3 - s3 == 20) ? parse_f1(s3, dates) : TS_BAD;
                if (ingest != TS_BAD) {
                    const i64 rel = floor_div(ingest, 1000) - ing_base;
                    if (rel < 0 || rel >= i64(ING_NONE)) { far = true; ing = rel < 0 ? 0 : ING_NONE - 1; }
                    else ing = u32(rel);
                    timely = in_window && ingest - ts <= DAY;
                }
            }
#if SS_PL == 4
            sink += u64(ts) + ing + u64(timely) + size_t(e6 - s6); return;
#endif
            u32 ts32 = DEAD, payload = 0;
            if (timely && s1 != e1 && s5 != e5) {
                const size_t kind = size_t(e4 - s4);
                bool ok = false;
                if (kind == 4) {
                    const u32 word = rd32(s4);
                    ok = (word == VIEW) | (word == CART);
                } else if (kind == 8 && rd64(s4) == PURCHASE) {
                    const u8* p = s6;
                    while (p < e6 && *p == '0') p++;
                    u32 price = 0;
                    bool digits = e6 > s6 && e6 - p <= 8;
                    for (; digits && p < e6; p++) {
                        const u32 digit = *p - '0';
                        if (digit > 9) digits = false;
                        price = price * 10 + digit;
                    }
                    if (digits && price >= 1 && price <= 10'000'000) {
                        ok = true;
                        payload = (price << 8) | cat.category(s5, e5);
                    }
                }
                if (ok) ts32 = u32(ts - ws);
            }
#if SS_PL == 5
            sink += u64(ts32) + payload + ing; return;
#endif
            const size_t n0 = size_t(e0 - s0), n1 = size_t(e1 - s1);
            // Both IDs are decoded together; the user's number is only used for a valid copy.
            u64 event_value, user_value;
            b36_pair(s0 + 1, n0 - 1, s1 + 1, n1 - 1, event_value, user_value);
            u64 number = ~0ULL;
            if ((s0[0] == 'e') & (event_value != ~0ULL)) number = unscramble40(event_value, ekey);
#if SS_PL == 6
            sink += u64(ts32) + payload + ing + number; return;
#endif
#ifndef SS_PX
#define SS_PX 0                      // measurement builds only: leave one decode of the parse out
#endif
#if SS_PX == 2
            number = (((rd64(s0 + 1) * 0x9E3779B97F4A7C15ULL) >> 32) * nidx) >> 32;
#endif
            if (__builtin_expect(number < nidx, 1)) {
                u32 user = 0;
#if SS_PX == 1
                user = u32(rd64(s1 + 1) & 0xFFFFF0) | 1;
                if (false)
#endif
                if (ts32 != DEAD) {
                    const u64 who = unscramble40(user_value, ukey);
                    if ((s1[0] == 'u') & (user_value != ~0ULL) & ((who >> 9) < pmax) & ((who & 511) < 16)) user = (u32(who >> 9) << 4) | u32(who & 15);
                    else user = dense_odd_user(cx, tid, pack_id(s1, n1, 'u'));
                }
                if (far) dense_bail(1);                         // an ingest time this record cannot hold
#if SS_PL == 7
                sink += u64(ts32) + payload + ing + number + user; return;
#endif
                lists.push(u32(number >> SS_DW), RecD{(u64(ts32) << 32) | payload, (u64(ing) << ING_SHIFT) | ((number & (DWIDTH - 1)) << 28) | user});
                return;
            }
            // An event ID that does not decode: the general dedup, exactly as parse_events
            // does it. If it survives, dense_extra hands it to its person.
            RecA rec;
            rec.ekey = pack_id(s0, n0, 'e');
            const i64 ingest = (e3 - s3 == 20) ? parse_f1(s3, dates) : TS_BAD;
            rec.ingest = ingest == TS_BAD ? INT64_MAX : floor_div(ingest, DAY) * (1LL << 24) + tod_code(s3 + 11);
            rec.ukey = 0;
            rec.ts = ts32;
            rec.payload = payload;
            if (ts32 != DEAD) rec.ukey = pack_id(s1, n1, 'u');
            general.push(u32(hash64(rec.ekey) >> gshift), rec);
        });
        release_chunk(sh.files[task.file], task);
    }
    if (sink == 1) fprintf(stderr, "sink\n");
}

static void parse_clicks_dense(Shared& sh, u32 tid, Reader& reader) {
    Dense& D = g_dense;
    DenseCtx& cx = D.ctx[tid];
    const i64 lo = sh.ws - REACH, hi = sh.we;
    const Catalogs& cat = sh.cat;
    Scatter<RecK, 64>& lists = sh.k[tid];
    const u64 ukey = D.ukey, pmax = D.pmax;
    DateCache dates;
    for (;;) {
        size_t at = sh.next_click.fetch_add(1, std::memory_order_relaxed);
        if (at >= sh.click_tasks.size()) break;
        const Task& task = sh.click_tasks[at];
        const u8* buf; size_t start, stop, last;
        if (!reader.load(sh.files[task.file], task, buf, start, stop, last)) { release_chunk(sh.files[task.file], task); continue; }
        line_rows<4>(buf, start, stop, last, line4, [&](const Fields<4>& f) SS_INLINE {
            const u8 *s0 = f.s[0], *e0 = f.e[0], *s1 = f.s[1], *e1 = f.e[1], *s2 = f.s[2], *e2 = f.e[2], *s3 = f.s[3], *e3 = f.e[3];
            if (s0 == e0 || s1 == e1) return;
            const i64 ts = parse_ts(s3, size_t(e3 - s3), dates);
            if (ts == TS_BAD || ts < lo || ts >= hi) return;
            const i32 campaign = cat.campaign(s2, e2);
            if (campaign < 0) return;
            const size_t n0 = size_t(e0 - s0), n1 = size_t(e1 - s1);
            u64 key, user_value;
            b36_pair(s0 + 1, n0 - 1, s1 + 1, n1 - 1, key, user_value);
            const u64 who = unscramble40(user_value, ukey);
            if ((s1[0] == 'u') & (user_value != ~0ULL) & ((who >> 9) < pmax) & ((who & 511) < 16)) {
                const u32 user = (u32(who >> 9) << 4) | u32(who & 15);
                if (s0[0] != 'k') key = ~0ULL;
                if (key == ~0ULL) {
                    key = KODD | (u64(tid) << 40) | u64(cx.odd_clicks.size());
                    cx.odd_clicks.push_back(pack_id(s0, n0, 'k'));
                }
                lists.push(user >> (4 + SS_CW), RecK{(u64(u32(ts - lo)) << 32) | user, (u64(u32(campaign)) << 48) | key});
            } else {
                cx.pending.push_back(RecC{pack_id(s1, n1, 'u'), pack_id(s0, n0, 'k'), u32(ts - lo), u32(campaign)});
            }
        });
        release_chunk(sh.files[task.file], task);
    }
}

// ---------------------------------------------------------------- between the phases
static inline void dense_hand_event(Shared& sh, DenseCtx& cx, u32 tid, u32 user, u32 ts, u32 payload) {
    u64 person;
    if (user == cx.memo_user) person = cx.memo_person;
    else {
        const u64 key = (user & ODD) ? g_dense.ctx[(user >> 21) & 63].odd_users[user & 0x1FFFFF] : dense_user_key(user >> 4, user & 15);
        person = sh.alias.person(key);
        cx.memo_user = user;
        cx.memo_person = person;
    }
    cx.n_handed++;
    sh.b[tid].push(u32(hash64(person) >> (64 - int(sh.bbits))), RecB{person, ts, payload});
}

// Called by the general dedup for each event it keeps: true when the event's
// user is a person walked here, who then gets the event through `extras`.
static bool dense_extra(u32 tid, u64 ukey, u32 ts, u32 payload) {
    Dense& D = g_dense;
    if (ukey & INTERN_BIT) return false;
    u8 text[16] = {0};
    memcpy(text, &ukey, 8);                                    // the bytes after the letter, as loaded
    size_t len = 0;
    while (len < 8 && text[len]) len++;
    const u64 value = b36_value(text, len);
    if (value == ~0ULL) return false;
    const u64 number = unscramble40(value, D.ukey);
    if ((number >> 9) >= D.pmax || (number & 511) >= 16) return false;
    const u32 person = u32(number >> 9);
    if (D.complex[person]) return false;
    D.ctx[tid].extras.push_back({person, u32(number & 15), (u64(ts) << 32) | payload});
    return true;
}

// After phase 2: every waiting event, by person; their people are flagged.
static void dense_gather_extras() {
    Dense& D = g_dense;
    for (DenseCtx& cx : D.ctx) D.extras.insert(D.extras.end(), cx.extras.begin(), cx.extras.end());
    std::sort(D.extras.begin(), D.extras.end(), [](const DenseExtra& x, const DenseExtra& y) { return x.person < y.person; });
    for (const DenseExtra& e : D.extras) D.claimed[e.person] = 2;
}

// Phase 2: the clicks of each partition of people, grouped by person. Clicks of
// people handed to the general path go there, under the person's name.
static void dense_group_clicks(Shared& sh, u32 tid) {
    Dense& D = g_dense;
    const u32 nthreads = sh.cfg.threads;
    const int shift = 64 - int(sh.bbits);
    u32 count[CPER + 1];
    for (;;) {
        const u32 part = D.next_cpart.fetch_add(1, std::memory_order_relaxed);
        if (part >= D.cparts) break;
        size_t total = 0;
        for (u32 t = 0; t < nthreads; t++)
            for (Block* b = sh.k[t].head[part]; b; b = b->next) total += b->n;
        if (!total) continue;
        memset(count, 0, sizeof count);
        const u32 base = part << SS_CW;
        for (u32 t = 0; t < nthreads; t++)
            for (Block* b = sh.k[t].head[part]; b; b = b->next) {
                const RecK* recs = Scatter<RecK, 64>::items(b);
                for (u32 i = 0; i < b->n; i++) {
                    const u32 user = u32(recs[i].x) & USER_MASK;
                    const u32 person = user >> 4;
                    if (D.complex[person]) {
                        const u64 name = sh.alias.person(dense_user_key(person, user & 15));
                        sh.c[tid].push(u32(hash64(name) >> shift), RecC{name, dense_click_key(recs[i].y & KEY_MASK), u32(recs[i].x >> 32), u32(recs[i].y >> 48)});
                        continue;
                    }
                    count[person - base]++;
                }
            }
        u32* starts = D.cstart + size_t(part) * (CPER + 1);
        u32 sum = 0;
        for (u32 p = 0; p < CPER; p++) { const u32 n = count[p]; starts[p] = sum; count[p] = sum; sum += n; }
        starts[CPER] = sum;
        if (!sum) continue;
        RecK* out = (RecK*)huge_take(size_t(sum) * sizeof(RecK));
        for (u32 t = 0; t < nthreads; t++)
            for (Block* b = sh.k[t].head[part]; b; b = b->next) {
                const RecK* recs = Scatter<RecK, 64>::items(b);
                for (u32 i = 0; i < b->n; i++) {
                    const u32 person = (u32(recs[i].x) & USER_MASK) >> 4;
                    if (D.complex[person]) continue;
                    out[count[person - base]++] = recs[i];
                }
            }
        D.cdata[part] = out;
    }
}

// ---------------------------------------------------------------- the walk
// One person: `n` events in time order, and the clicks of the aliases in `members`.
static void dense_sessions(Shared& sh, DenseCtx& cx, u32 person, u32 name_alias, u32 members, const u64* ev, u32 n) {
    Dense& D = g_dense;
    const u32 ncamp = u32(sh.cat.campaign_ids.size());
    const u32 organic = ncamp;
    const u32 ndays = sh.ndays;
    const i64 day0 = sh.day0;
    const u32 day_shift = u32(sh.ws - sh.day0 * DAY);          // ms from the first day's midnight to window_start
    Shared::Totals& totals = *cx.totals;
    hvec<i64>& cat_cents = cx.cat_cents;
    hvec<u32>& cat_touched = cx.cat_touched;
    hvec<u32>& camp_count = cx.camp_count;
    hvec<u32>& camp_slot = cx.camp_slot;
    hvec<u32>& stamp = cx.stamp;
    hvec<u32>& run_day = cx.run_day;
    hvec<u32>& run_first = cx.run_first;
    UserWriter& writer = cx.writer;
    u64 rows = 0;

    // This person's clicks. A handful are simply scanned for every session. More
    // are put in time order (at one instant the smallest click_id last) for a
    // sliding window: ordered as plain numbers (time, then place), with the rare
    // ties settled afterwards.
    Click* ck = nullptr;
    u32 m = 0;
#if SS_DX == 3
    cx.rows += n; return;
#endif
    {
        const u32 part = person >> SS_CW;
        const u32* starts = D.cstart + size_t(part) * (CPER + 1) + (person & (CPER - 1));
#if SS_DX == 1
        const u32 all = 0;
#else
        const u32 all = starts[1] - starts[0];
#endif
        if (all) {
            if (cx.clicks.size() < all) { cx.clicks.resize(size_t(all) + all / 2 + 64); cx.click_keys.resize(cx.clicks.size()); }
            ck = cx.clicks.data();
            const RecK* src = D.cdata[part] + starts[0];
            if (all <= SS_SCANMAX) {
                for (u32 i = 0; i < all; i++) {
                    ck[m] = Click{u32(src[i].x >> 32), u32(src[i].y >> 48), src[i].y & KEY_MASK};
                    m += (members >> (u32(src[i].x) & 15)) & 1;
                }
            } else {
                u64* keys = cx.click_keys.data();
                for (u32 i = 0; i < all; i++) {
                    keys[m] = (src[i].x & ~0xFFFFFFFFULL) | i;
                    m += (members >> (u32(src[i].x) & 15)) & 1;
                }
                if (m > SS_RADIXMIN) {
                    // A heavy user's clicks: three counting passes over the time (the high half of
                    // the key), which keep the order of equal times and so sort by time, then place.
                    if (cx.click_tmp.size() < m) cx.click_tmp.resize(size_t(m) + m / 2);
                    u64* from = keys;
                    u64* to = cx.click_tmp.data();
                    for (int shift = 32; shift < 64; shift += 11) {
                        u32 count[2048] = {0};
                        for (u32 i = 0; i < m; i++) count[(from[i] >> shift) & 2047]++;
                        u32 sum = 0;
                        for (u32 b = 0; b < 2048; b++) { const u32 c = count[b]; count[b] = sum; sum += c; }
                        for (u32 i = 0; i < m; i++) to[count[(from[i] >> shift) & 2047]++] = from[i];
                        std::swap(from, to);
                    }
                    keys = from;                        // three passes: the result is in the other buffer
                } else if (m > SS_SCANMAX) std::sort(keys, keys + m);
                for (u32 i = 0; i < m; i++) {
                    const RecK& from = src[u32(keys[i])];
                    ck[i] = Click{u32(from.x >> 32), u32(from.y >> 48), from.y & KEY_MASK};
                }
                if (m > SS_SCANMAX)
                    for (u32 i = 1; i < m; i++) {
                        if (ck[i].ts != ck[i - 1].ts) continue;
                        u32 end = i + 1;
                        while (end < m && ck[end].ts == ck[i].ts) end++;
                        std::sort(ck + i - 1, ck + end, [](const Click& x, const Click& y) { return dense_click_less(y.key, x.key); });
                        i = end;
                    }
            }
        }
    }
#if SS_DX == 4
    if (m > SS_SCANMAX) m = 0;
#elif SS_DX == 5
    if (m <= SS_SCANMAX) m = 0;
#endif
    const bool scan_clicks = m <= SS_SCANMAX;
    // Up to 16 clicks: put in time order (at one instant the smallest click_id
    // last), they sit one to a bit. For a session, the clicks in reach are then a
    // run of bits found with one compare; the highest is the click to credit, and
    // a campaign assists once for each click in the run whose previous click of
    // the same campaign lies before the run. Assists are counted per click and
    // added to the totals when the day changes.
    const bool light = scan_clicks && m > 0;
    alignas(32) u32 lts[16];
    alignas(32) i32 lprev[16];
    alignas(32) u32 lacc[16];
    u32 lcamp[17];
#if defined(__AVX2__)
    __m256i lt0 = _mm256_setzero_si256(), lt1 = lt0, lp0 = lt0, lp1 = lt0, acc0 = lt0, acc1 = lt0;
#endif
    if (light) {
        for (u32 i = 1; i < m; i++) {
            const Click v = ck[i];
            u32 j = i;
            while (j > 0 && (v.ts < ck[j - 1].ts || (v.ts == ck[j - 1].ts && dense_click_less(ck[j - 1].key, v.key)))) { ck[j] = ck[j - 1]; j--; }
            ck[j] = v;
        }
        const u32 tag = ++cx.session_id;
        for (u32 k = 0; k < 16; k++) { lts[k] = 0xFFFFFFFFu; lprev[k] = -1; lacc[k] = 0; lcamp[k] = organic; }
        lcamp[16] = organic;
        for (u32 k = 0; k < m; k++) {
            const u32 c = ck[k].camp;
            lts[k] = ck[k].ts;
            lcamp[k] = c;
            lprev[k] = stamp[c] == tag ? i32(camp_slot[c]) : -1;
            stamp[c] = tag;
            camp_slot[c] = k;
        }
#if defined(__AVX2__)
        lt0 = _mm256_load_si256((const __m256i*)lts); lt1 = _mm256_load_si256((const __m256i*)(lts + 8));
        lp0 = _mm256_load_si256((const __m256i*)lprev); lp1 = _mm256_load_si256((const __m256i*)(lprev + 8));
#endif
    }

    char name[24];
    name[0] = 'u';
    const u32 name_len = 1 + b36_text(name + 1, scramble40(u64(person) * 512 + name_alias, D.ukey));

    i64 cur_day = -1;
    i32 d_sessions = 0, d_events = 0;
    i64 d_revenue = 0;
    auto emit = [&]() SS_WALK {
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
#if SS_DX != 2
        writer.add(name, name_len, i32(day0 + cur_day), d_sessions, d_events, d_revenue, top);
#endif
        rows++;
    };
    auto flush_assists = [&]() SS_WALK {
#if defined(__AVX2__)
        _mm256_store_si256((__m256i*)lacc, acc0);
        _mm256_store_si256((__m256i*)(lacc + 8), acc1);
        acc0 = _mm256_setzero_si256();
        acc1 = acc0;
#endif
        for (u32 k = 0; k < m; k++) { totals.assisted[size_t(lcamp[k]) * ndays + size_t(cur_day)] += lacc[k]; lacc[k] = 0; }
    };
    u32 left = 0, right = 0;
    // The same span counting of assists as in process_people.
    u32 ord = 0, nruns = 0;
    auto close_span = [&](u32 c, u32 from) {
        u32 upper = ord;                                  // sessions [from, ord) had this campaign in reach
        for (u32 j = nruns; j-- > 0;) {
            const u32 first = run_first[j];
            const u32 lower = from > first ? from : first;
            if (upper > lower) totals.assisted[size_t(c) * ndays + run_day[j]] += upper - lower;
            upper = first;
            if (first <= from) break;
        }
    };

    u32 i = 0;
    while (i < n) {
        const u32 start = u32(ev[i] >> 32);
        const i64 day = i64((start + day_shift) / u32(DAY));
        if (day != cur_day) {
            if (cur_day >= 0) {
                if (light) flush_assists();
                emit();
            }
            cur_day = day; d_sessions = 0; d_events = 0; d_revenue = 0;
        }
        // The session's events: each within 30 minutes of the one before.
        u32 last = start, nevents = 0;
        i64 cents = 0;
        do {
            const u64 e = ev[i];
            const u32 t = u32(e >> 32);
            if (t - last > u32(SESSION_GAP)) break;
            last = t;
            nevents++;
            if (const u32 payload = u32(e)) {
                const u32 c = payload & 0xFF;
                const i64 price = payload >> 8;
                if (!cat_cents[c]) cat_touched.push_back(c);
                cat_cents[c] += price;
                cents += price;
            }
        } while (++i < n);
        // purchases have a price of at least 1, so cents > 0 means "has a purchase"
        d_sessions++; d_events += i32(nevents); d_revenue += cents;

        u32 credited = organic;
        // Click times count from window_start - 7 days, event times from
        // window_start, so the clicks in reach are those in [start, start + 7 days].
        if (light) {
#if defined(__AVX2__)
            const __m256i from = _mm256_set1_epi32(int(start)), reach = _mm256_set1_epi32(int(REACH));
            const __m256i d0 = _mm256_sub_epi32(lt0, from), d1 = _mm256_sub_epi32(lt1, from);
            const __m256i in0 = _mm256_cmpeq_epi32(_mm256_min_epu32(d0, reach), d0), in1 = _mm256_cmpeq_epi32(_mm256_min_epu32(d1, reach), d1);
            const u32 inmask = u32(_mm256_movemask_ps(_mm256_castsi256_ps(in0))) | (u32(_mm256_movemask_ps(_mm256_castsi256_ps(in1))) << 8);
            const __m256i first = _mm256_set1_epi32(int(__builtin_ctz(inmask | 0x10000)));
            acc0 = _mm256_sub_epi32(acc0, _mm256_and_si256(in0, _mm256_cmpgt_epi32(first, lp0)));
            acc1 = _mm256_sub_epi32(acc1, _mm256_and_si256(in1, _mm256_cmpgt_epi32(first, lp1)));
#else
            u32 inmask = 0;
            for (u32 k = 0; k < m; k++) inmask |= u32(u32(lts[k] - start) <= u32(REACH)) << k;
            const i32 first = i32(__builtin_ctz(inmask | 0x10000));
            for (u32 k = 0; k < m; k++) lacc[k] += ((inmask >> k) & 1) & u32(lprev[k] < first);
#endif
            credited = lcamp[inmask ? 31 - __builtin_clz(inmask) : 16];
        } else if (!scan_clicks) {
            const u64 upto = u64(start) + u64(REACH);
            while (right < m && ck[right].ts <= upto) {
                const u32 c = ck[right++].camp;
                if (camp_count[c]++ == 0) camp_slot[c] = ord;
            }
            while (left < right && ck[left].ts < start) {
                const u32 c = ck[left++].camp;
                if (--camp_count[c] == 0) close_span(c, camp_slot[c]);
            }
            if (right > left) credited = ck[right - 1].camp;
            if (nruns == 0 || run_day[nruns - 1] != u32(day)) { run_day[nruns] = u32(day); run_first[nruns] = ord; nruns++; }
            ord++;
        }
        const size_t slot = size_t(credited) * ndays + size_t(day);
        totals.sessions[slot]++;
        if (cents > 0) { totals.purchasing[slot]++; totals.revenue[slot] += cents; }
    }
    if (cur_day >= 0) {
        if (light) flush_assists();
        emit();
    }
    for (u32 k = left; k < right; k++) {                  // the spans still open at the person's last session
        const u32 c = ck[k].camp;
        if (camp_count[c]) { close_span(c, camp_slot[c]); camp_count[c] = 0; }
    }
    cx.rows += rows;
}

// Every surviving event of one decoded person number: `amask` is the set of
// aliases among them. Their links decide how many people that is (nearly always one).
static void dense_person(Shared& sh, DenseCtx& cx, u32 person, const u64* ev, const u8* al, u32 n, u32 amask, u32 flags) {
    Dense& D = g_dense;
    const u8 before = __atomic_exchange_n(&D.claimed[person], u8(1), __ATOMIC_RELAXED);
    if (__builtin_expect(before != 0, 0)) {
        if (before == 1) { dense_bail(3); return; }
        // Events of this person whose own IDs did not decode: they join the rest.
        auto from = std::lower_bound(D.extras.begin(), D.extras.end(), person, [](const DenseExtra& x, u32 p) { return x.person < p; });
        size_t count = 0;
        while (from + count != D.extras.end() && from[count].person == person) count++;
        cx.more_ev.resize(n + count);
        cx.more_al.resize(n + count);
        std::vector<DenseExtra> extra(from, from + count);
        std::sort(extra.begin(), extra.end(), [](const DenseExtra& x, const DenseExtra& y) { return x.ev < y.ev; });
        for (const DenseExtra& e : extra) amask |= 1u << e.alias;
        // A merge keeps a long ordered stretch ordered (one stray event must not cost a sort of a heavy user).
        size_t i = 0, j = 0, k = 0;
        if (!(flags & UNORDERED))
            while (i < n && j < count) {
                if (u32(extra[j].ev >> 32) < u32(ev[i] >> 32)) { cx.more_ev[k] = extra[j].ev; cx.more_al[k] = u8(extra[j].alias); j++; }
                else { cx.more_ev[k] = ev[i]; cx.more_al[k] = al[i]; i++; }
                k++;
            }
        for (; i < n; i++, k++) { cx.more_ev[k] = ev[i]; cx.more_al[k] = al[i]; }
        for (; j < count; j++, k++) { cx.more_ev[k] = extra[j].ev; cx.more_al[k] = u8(extra[j].alias); }
        n += u32(count);
        ev = cx.more_ev.data();
        al = cx.more_al.data();
    }
    cx.n_people++;
    if (flags & UNORDERED) {
        // Not in time order by number: order by time (then by number), keeping each event's alias.
        cx.n_sorted++;
        cx.sort_keys.resize(n);
        for (u32 i = 0; i < n; i++) cx.sort_keys[i] = (ev[i] & ~0xFFFFFFFFULL) | i;
        std::sort(cx.sort_keys.begin(), cx.sort_keys.end());
        cx.sort_ev.resize(n);
        cx.sort_al.resize(n);
        for (u32 i = 0; i < n; i++) {
            const u32 at = u32(cx.sort_keys[i]);
            cx.sort_ev[i] = ev[at];
            cx.sort_al[i] = al[at];
        }
        ev = cx.sort_ev.data();
        al = cx.sort_al.data();
    }
    const u64 parents = D.ptab[person] ^ IDENT;
    if (parents == IDENT && !(amask & (amask - 1))) {
        // No links and one alias: the usual single-ID person.
        dense_sessions(sh, cx, person, u32(__builtin_ctz(amask)), amask, ev, n);
        return;
    }
    // The groups of linked aliases that have events here: nearly always one.
    u32 present = 0;
    for (u32 m = amask; m; m &= m - 1) present |= 1u << ((parents >> (4 * __builtin_ctz(m))) & 15);
    const bool one = !(present & (present - 1));
    if (!one) cx.n_split++;
    for (u32 left = present; left; left &= left - 1) {
        const u32 c = u32(__builtin_ctz(left));
        // The person's name: the byte-wise smallest of the linked IDs, used or not.
        const u32 members = nibbles_equal(parents, c);
        u32 name_alias = c;
        if (members & (members - 1)) {
            u64 best = ~0ULL;
            for (u32 m = members; m; m &= m - 1) {
                const u32 a = u32(__builtin_ctz(m));
                const u64 order = b36_order(scramble40(u64(person) * 512 + a, D.ukey));
                if (order < best) { best = order; name_alias = a; }
            }
        }
        if (one) { dense_sessions(sh, cx, person, name_alias, members, ev, n); return; }
        if (cx.part_ev.size() < n) cx.part_ev.resize(n);
        u32 count = 0;
        for (u32 i = 0; i < n; i++) if ((members >> al[i]) & 1) cx.part_ev[count++] = ev[i];
        dense_sessions(sh, cx, person, name_alias, members, cx.part_ev.data(), count);
    }
}

static void dense_ctx_init(Shared& sh, DenseCtx& cx, u32 part) {
    const u32 ncamp = u32(sh.cat.campaign_ids.size());
    const u32 ndays = sh.ndays;
    cx.ready = true;
    cx.totals = &sh.campaign_totals[part];
    cx.totals->sessions.assign(size_t(ncamp + 1) * ndays, 0);
    cx.totals->assisted.assign(size_t(ncamp + 1) * ndays, 0);
    cx.totals->purchasing.assign(size_t(ncamp + 1) * ndays, 0);
    cx.totals->revenue.assign(size_t(ncamp + 1) * ndays, 0);
    char name[64];
    snprintf(name, sizeof name, "/user_daily/part-%03u.parquet", part);
    cx.writer.init(sh.cfg.out + name);
    cx.slots.assign(DWIDTH, RecD{0, EMPTY_SLOT});
    cx.ev.resize(DWIDTH);
    cx.al.resize(DWIDTH);
    cx.runs.resize(DWIDTH + 1);
    cx.cat_cents.assign(sh.cat.categories.size(), 0);
    cx.camp_count.assign(ncamp + 1, 0);
    cx.camp_slot.assign(ncamp + 1, 0);
    cx.stamp.assign(ncamp + 1, 0);
    cx.run_day.assign(ndays + 2, 0);
    cx.run_first.assign(ndays + 2, 0);
}

// Phase 3: one partition of event numbers at a time. Copies go to their slot
// (the smallest ingest_ts stays), then the slots are read in order: people
// handed to the general path leave, and each stretch of one person is walked.
// The first and the last stretch may continue in the neighbouring partitions,
// so they are set aside for dense_stitch.
static void dense_walk(Shared& sh, u32 tid) {
    Dense& D = g_dense;
    DenseCtx& cx = D.ctx[tid];
    const u32 nthreads = sh.cfg.threads;
    dense_ctx_init(sh, cx, tid);
    RecD* slots = cx.slots.data();
    u64* ev = cx.ev.data();
    u8* al = cx.al.data();
    DenseCtx::Run* runs = cx.runs.data();
    for (;;) {
        const u32 part = D.next_part.fetch_add(1, std::memory_order_relaxed);
        if (part >= D.nparts || D.bail.load(std::memory_order_relaxed)) break;
        const double step0 = thread_cpu();
        bool any = false;
        for (u32 t = 0; t < nthreads; t++)
            for (Block* b = sh.d[t].head[part]; b; b = b->next) {
                const RecD* recs = Scatter<RecD, 64>::items(b);
                prefetch_block(b->next);
                any = true;
                for (u32 i = 0; i < b->n; i++) {
                    RecD& slot = slots[(recs[i].b >> 28) & (DWIDTH - 1)];
                    // Smallest ingest_ts wins; equal ingest_ts means identical copies.
                    if (slot.b == EMPTY_SLOT || (recs[i].b >> ING_SHIFT) < (slot.b >> ING_SHIFT)) slot = recs[i];
                }
            }
        if (!any) continue;
        const double step1 = thread_cpu();
        // The stretch being read keeps its alias set and its order flag in registers;
        // they are written to its entry when the next stretch begins.
        u32 nruns = 0, count = 0, current = ~0u, last_ts = 0, amask = 0, flags = 0;
        for (u32 i = 0; i < DWIDTH; i++) {
            const u64 b = slots[i].b;
            if (b == EMPTY_SLOT) continue;
            slots[i].b = EMPTY_SLOT;
            const u64 a = slots[i].a;
            const u32 ts = u32(a >> 32);
            if (ts == DEAD) continue;
            const u32 user = u32(b) & USER_MASK;
            const u32 person = user >> 4;
            if (person != current) {
                if ((user & ODD) || D.complex[person]) { dense_hand_event(sh, cx, tid, user, ts, u32(a)); continue; }
                if (nruns) { runs[nruns - 1].amask = amask; runs[nruns - 1].flags = flags; }
                current = person;
                runs[nruns++] = {person, count, 0, 0};
                last_ts = 0; amask = 0; flags = 0;
            }
            amask |= 1u << (user & 15);
            flags |= u32(ts < last_ts);
            last_ts = ts;
            ev[count] = a;
            al[count] = u8(user & 15);
            count++;
        }
        if (nruns) { runs[nruns - 1].amask = amask; runs[nruns - 1].flags = flags; }
        runs[nruns].start = count;
        const double step2 = thread_cpu();
        auto keep = [&](u32 r, DenseFrag& frag) {
            const u32 n = runs[r + 1].start - runs[r].start;
            u8* mem = (u8*)huge_take(size_t(n) * 9);
            frag.ev = (u64*)mem;
            frag.al = mem + size_t(n) * 8;
            memcpy(frag.ev, ev + runs[r].start, size_t(n) * 8);
            memcpy(frag.al, al + runs[r].start, n);
            frag.person = runs[r].person;
            frag.amask = runs[r].amask;
            frag.flags = runs[r].flags;
            frag.n = n;
        };
        if (nruns >= 1) keep(0, D.frags[size_t(part) * 2]);
        if (nruns >= 2) keep(nruns - 1, D.frags[size_t(part) * 2 + 1]);
        for (u32 r = 1; r + 1 < nruns; r++)
            dense_person(sh, cx, runs[r].person, ev + runs[r].start, al + runs[r].start, runs[r + 1].start - runs[r].start, runs[r].amask, runs[r].flags);
        const double step3 = thread_cpu();
        cx.cpu[0] += step1 - step0;
        cx.cpu[1] += step2 - step1;
        cx.cpu[2] += step3 - step2;
    }
}

// Between phases 3 and 4: the set-aside stretches, in number order, joined
// wherever neighbours are the same person. Largest first.
static void dense_groups() {
    Dense& D = g_dense;
    std::vector<std::pair<u64, std::pair<u32, u32>>> groups;       // events, then [first, last] fragment
    const u32 total = D.nparts * 2;
    for (u32 f = 0; f < total;) {
        if (!D.frags[f].n) { f++; continue; }
        u32 last = f;
        u64 events = D.frags[f].n;
        for (u32 g = f + 1; g < total; g++) {
            if (!D.frags[g].n) continue;
            if (D.frags[g].person != D.frags[f].person) break;
            last = g;
            events += D.frags[g].n;
        }
        groups.push_back({events, {f, last}});
        f = last + 1;
    }
    std::sort(groups.begin(), groups.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    D.gstart.clear();
    D.gfrag.clear();
    for (const auto& g : groups) { D.gstart.push_back(g.second.first); D.gfrag.push_back(g.second.second); }
}

// Phase 4: walk each joined stretch.
static void dense_stitch(Shared& sh, u32 tid) {
    Dense& D = g_dense;
    DenseCtx& cx = D.ctx[tid];
    const double began = thread_cpu();
    for (;;) {
        const u32 g = D.next_group.fetch_add(1, std::memory_order_relaxed);
        if (g >= D.gstart.size() || D.bail.load(std::memory_order_relaxed)) break;
        const u32 first = D.gstart[g], last = D.gfrag[g];
        const DenseFrag& head = D.frags[first];
        if (first == last) {
            dense_person(sh, cx, head.person, head.ev, head.al, head.n, head.amask, head.flags);
            continue;
        }
        size_t total = 0;
        for (u32 f = first; f <= last; f++) total += D.frags[f].n;
        if (cx.tmp_ev.size() < total) { cx.tmp_ev.resize(total); cx.tmp_al.resize(total); }
        u32 amask = 0, flags = 0, at = 0, last_ts = 0;
        for (u32 f = first; f <= last; f++) {
            const DenseFrag& frag = D.frags[f];
            if (!frag.n) continue;
            memcpy(cx.tmp_ev.data() + at, frag.ev, size_t(frag.n) * 8);
            memcpy(cx.tmp_al.data() + at, frag.al, frag.n);
            amask |= frag.amask;
            flags |= frag.flags | u32(u32(frag.ev[0] >> 32) < last_ts);
            last_ts = u32(frag.ev[frag.n - 1] >> 32);
            at += frag.n;
        }
        cx.n_stitched += total;
        dense_person(sh, cx, head.person, cx.tmp_ev.data(), cx.tmp_al.data(), u32(total), amask, flags);
    }
    cx.cpu[3] += thread_cpu() - began;
}

// After phase 4: people whose only events are ones whose own IDs did not decode
// were never met by the walk. They are walked here, into a file of their own.
static void dense_leftovers(Shared& sh) {
    Dense& D = g_dense;
    const u32 T = sh.cfg.threads;
    DenseCtx& cx = D.ctx[T];
    for (size_t i = 0; i < D.extras.size(); i++) {
        const u32 person = D.extras[i].person;
        if ((i && D.extras[i - 1].person == person) || D.claimed[person] != 2) continue;
        if (!cx.ready) dense_ctx_init(sh, cx, 2 * T);
        dense_person(sh, cx, person, nullptr, nullptr, 0, 0, 0);
    }
    if (cx.ready) {
        cx.writer.finish();
        sh.user_rows.fetch_add(cx.rows);
    }
}

// Starts the program again with the dense path switched off.
[[noreturn]] static void dense_restart(Shared& sh, bool wrote) {
    Dense& D = g_dense;
    fprintf(stderr, "dense path abandoned (reason %u): running the general path\n", D.bail.load());
    if (wrote)
        for (u32 part = 0; part <= 2 * sh.cfg.threads; part++) {
            char name[64];
            snprintf(name, sizeof name, "/user_daily/part-%03u.parquet", part);
            unlink((sh.cfg.out + name).c_str());
        }
    setenv("SS_DENSE", "0", 1);
    static char fallback[] = "pipeline";
    static char* none[] = {fallback, nullptr};
    char** argv = (D.argv && D.argv[0]) ? D.argv : none;
#ifdef __linux__
    execv("/proc/self/exe", argv);
#endif
    execv(argv[0], argv);
    die("could not restart on the general path");
}

#else  // no dense path in this build
static bool dense_extra(u32, u64, u32, u32) { return false; }
#endif // SS_DENSE_BUILD
#endif // SS_DENSE_H
