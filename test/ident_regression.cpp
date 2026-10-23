/*
 * ident_regression.cpp — regression test for src/navtex_ident.h, the transmitter
 * identification ensemble.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 *   ident_regression <path to test/testdata/ident_fixture.csv>
 *
 * Two parts:
 *
 *  (a) hand-written golden cases, each a situation an operator would recognise
 *      (Niton heard at Dalgety Bay in its slot, the same header with no receiver
 *      position, an unreadable B1, an off-slot gale warning, two Mediterranean
 *      stations sharing 'X', the unlisted Oostende 490 kHz service, a receiver
 *      whose configured lat/lon are swapped), checking the answer AND the
 *      "how this was worked out" fields the UI prints;
 *
 *  (b) the production header replayed over a small committed sample of the
 *      held-out synthetic set (test/ident/make_fixture.py), asserting the
 *      operator contract (wrong at most 1% of the time at level "high"), an
 *      accuracy floor about 5 points under the measured value, the invariants
 *      of the result (no "high" on a disagreement or a suspect position,
 *      confidence 0 exactly when not attributed, level == navtex_level_for),
 *      and that a replay reproduces the same results (the server re-identifies
 *      its stored history at startup and must land in the same state).
 *
 * Exit status 0 = pass.  Needs nothing but a C++11 compiler.
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <chrono>

#include "../src/navtex_ident.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

/* --------------------------------------------------------------- helpers */
static int64_t utc(int y, int mo, int d, int h, int mi, int s) {
    /* days from civil (Howard Hinnant), so the test does not depend on timegm() */
    y -= mo <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400 + h * 3600 + mi * 60 + s;
}
static NavtexIdentQuery query(long f, char b1, char b2, int64_t t, bool rxv, double la, double lo, double snr) {
    NavtexIdentQuery q;
    std::memset(&q, 0, sizeof(q));
    q.freq_hz = f; q.b1 = b1; q.b2 = b2; q.serial = 1; q.start_utc = t; q.duration_s = 60;
    q.rx_valid = rxv; q.rx_lat = la; q.rx_lon = lo; q.has_snr = true; q.snr_db = snr;
    return q;
}
static int find_station(long f, char id, const char *name) {
    for (int i = 0; i < NAVTEX_STATION_COUNT; i++)
        if (NAVTEX_STATIONS[i].freq_hz == f && NAVTEX_STATIONS[i].id == id && std::strcmp(NAVTEX_STATIONS[i].name, name) == 0) return i;
    return -1;
}
static bool same_tx(int a, int b) { return navtex_ident_detail::same_transmitter(a, b); }
static const char *nm(int i) { return i >= 0 ? NAVTEX_STATIONS[i].name : "-"; }
static void show(const char *what, const NavtexIdentResult &r) {
    std::printf("  %-34s %-22s conf %.3f %-6s %-15s agree %d d %5.0f km brg %3.0f night %d slot %+5d s (%s) cand %d exp %c\n",
                what, nm(r.station), r.confidence, r.level, r.reason, r.agree, r.distance_km, r.bearing_deg,
                r.night_path, r.slot_offset_s, r.in_slot ? "in" : "out", r.n_candidates, r.expected_b1 ? r.expected_b1 : '-');
}

/* A receiver that has been running for a few days: 518 kHz E (Niton), G (Cullercoats),
 * O (Portpatrick), S, P and T heard in every one of their slots, two or three messages
 * back to back, at SNRs like the real Dalgety Bay logs. */
static void warm_dalgety(NavtexIdentifier &id, double la, double lo, int days, int *n_suspect, int *n_high,
                         int first_day = 0) {
    static const char L[] = "EGOSPT";
    static const int M0[] = {40, 60, 140, 180, 150, 190};
    static const double S[] = {12, 30, 25, 18, 22, 16};
    int64_t t0 = utc(2026, 9, 1, 0, 0, 0) + (int64_t)first_day * 86400;
    for (int day = 0; day < days; day++)
        for (int slot = 0; slot < 6; slot++)
            for (int k = 0; k < 6; k++)
                for (int m = 0; m < 3; m++) {
                    int64_t t = t0 + day * 86400 + (M0[k] + 240 * slot + 1 + 2 * m) * 60 + 20;
                    NavtexIdentQuery q = query(518000, L[k], 'A', t, true, la, lo, S[k]);
                    NavtexIdentResult r = id.identify(q);
                    id.observe(q, r);
                    if (n_suspect) *n_suspect += r.rx_pos_suspect;
                    if (n_high) *n_high += r.confidence >= 0.9;
                }
}

/* ---------------------------------------------------------- golden cases */
static void golden() {
    const double LA = 56.0403, LO = -3.3554;                  /* Dalgety Bay, IO86ha */
    const int niton = find_station(518000, 'E', "Niton");
    const int nao = find_station(518000, 'X', "Cabo de la Nao");
    const int serapeum = find_station(518000, 'X', "Serapeum (Ismailia)");
    CHECK(niton >= 0 && nao >= 0 && serapeum >= 0, "table rows missing");
    std::printf("golden cases:\n");

    /* 1. Dalgety Bay, 518 kHz 'E' 1:42 into Niton's 04:40 slot, after three days of
     *    ordinary reception: both methods agree on Niton and the gate passes -> high. */
    {
        NavtexIdentifier id;
        int sus = 0;
        warm_dalgety(id, LA, LO, 3, &sus, 0);
        NavtexIdentResult r = id.identify(query(518000, 'E', 'A', utc(2026, 9, 4, 4, 41, 42), true, LA, LO, 12));
        show("518 E in slot, Dalgety Bay", r);
        CHECK(same_tx(r.station, niton), "got %s", nm(r.station));
        CHECK(std::strcmp(r.level, "high") == 0 && r.confidence >= 0.9, "level %s conf %.3f", r.level, r.confidence);
        CHECK(r.agree && std::strcmp(r.reason, "AGREE_HIGH") == 0, "reason %s", r.reason);
        CHECK(r.distance_km > 600 && r.distance_km < 630, "distance %.1f", r.distance_km);   /* Niton is 612-622 km */
        CHECK(r.bearing_deg > 160 && r.bearing_deg < 172, "bearing %.1f", r.bearing_deg);
        CHECK(r.in_slot && r.slot_offset_s == 102, "slot offset %d", r.slot_offset_s);
        CHECK(r.n_candidates >= 2, "n_candidates %d", r.n_candidates);              /* 'E' is reused worldwide */
        CHECK(r.expected_b1 == 'E', "expected_b1 %c", r.expected_b1);
        CHECK(!r.rx_pos_suspect && sus == 0, "correct position flagged suspect (%d)", sus);
        CHECK(std::strcmp(r.member[0].name, "bayes") == 0 && std::strcmp(r.member[1].name, "rules") == 0, "member names");
    }
    /* 2. The same header with no receiver position: never high. */
    {
        NavtexIdentifier id;
        NavtexIdentResult r = id.identify(query(518000, 'E', 'A', utc(2026, 9, 4, 4, 41, 42), false, 0, 0, 12));
        show("518 E, receiver position unknown", r);
        CHECK(std::strcmp(r.level, "high") != 0, "level %s", r.level);
        CHECK(r.distance_km == -1 && r.bearing_deg == -1, "distance without a position");
    }
    /* 3. Unreadable B1 ('9') in Niton's slot.  Abstaining was measured to be worse than
     *    a low-confidence attribution by timing (Brier 819 vs 247 on train), so it may
     *    be attributed, but never above "low". */
    {
        NavtexIdentifier id;
        NavtexIdentResult r = id.identify(query(518000, '9', 'A', utc(2026, 9, 4, 4, 41, 42), true, LA, LO, 12));
        show("518 B1 '9' in E's slot", r);
        CHECK(std::strcmp(r.level, "low") == 0 || std::strcmp(r.level, "none") == 0, "level %s", r.level);
        CHECK(r.n_candidates == 0, "n_candidates %d", r.n_candidates);
    }
    /* 4. Off-slot subject D (vital, sent on receipt), on the same running receiver as
     *    case 1: still Niton, but lower confidence and out of slot. */
    {
        NavtexIdentifier a, b;
        warm_dalgety(a, LA, LO, 3, 0, 0);
        warm_dalgety(b, LA, LO, 3, 0, 0);
        NavtexIdentResult in = a.identify(query(518000, 'E', 'A', utc(2026, 9, 4, 4, 41, 42), true, LA, LO, 12));
        NavtexIdentResult off = b.identify(query(518000, 'E', 'D', utc(2026, 9, 4, 6, 17, 0), true, LA, LO, 12));
        show("518 E subject D, off slot", off);
        CHECK(same_tx(off.station, niton), "got %s", nm(off.station));
        CHECK(off.confidence < in.confidence, "off-slot %.3f >= in-slot %.3f", off.confidence, in.confidence);
        CHECK(!off.in_slot && std::strcmp(off.level, "high") != 0, "in_slot %d level %s", off.in_slot, off.level);
    }
    /* 5. Mediterranean collision on 518 kHz 'X': Cabo La Nao (Spain) and Serapeum (Egypt)
     *    share the letter and the slot.  By day each is heard only near home; at night
     *    from Malta, in between, both are possible and the answer must say so. */
    {
        NavtexIdentifier a, b, c;
        NavtexIdentResult palma = a.identify(query(518000, 'X', 'A', utc(2026, 9, 4, 11, 51, 0), true, 39.57, 2.65, 25));
        NavtexIdentResult crete = b.identify(query(518000, 'X', 'A', utc(2026, 9, 4, 11, 51, 0), true, 35.34, 25.13, 15));
        NavtexIdentResult malta = c.identify(query(518000, 'X', 'A', utc(2026, 9, 4, 23, 51, 0), true, 35.90, 14.51, 5));
        show("518 X by day, Palma", palma);
        show("518 X by day, Crete", crete);
        show("518 X at night, Malta", malta);
        CHECK(same_tx(palma.station, nao), "Palma got %s", nm(palma.station));
        CHECK(same_tx(crete.station, serapeum), "Crete got %s", nm(crete.station));
        CHECK(palma.n_candidates >= 2 && !palma.night_path && malta.night_path, "candidates/night flags");
        CHECK(std::strcmp(malta.level, "high") != 0, "Malta level %s", malta.level);
        bool other = (same_tx(malta.station, nao) && (same_tx(malta.alt[0], serapeum) || same_tx(malta.alt[1], serapeum))) ||
                     (same_tx(malta.station, serapeum) && (same_tx(malta.alt[0], nao) || same_tx(malta.alt[1], nao)));
        CHECK(other, "Malta: the other 'X' station is not offered as an alternative");
    }
    /* 6. 490 kHz 'B' at Dalgety Bay is the Oostende national service.  It was missing
     *    from Wikipedia's list (the only listed 490 'B' was Istanbul, 2,861 km away) and is
     *    now one of the evidence-backed ADDITIONS in tools/gen_navtex_stations.py: heard
     *    in 1,699 real messages, never Istanbul.  It must be named, and never Istanbul. */
    {
        NavtexIdentifier id;
        const int oostende = find_station(490000, 'B', "Oostende");
        NavtexIdentResult r = id.identify(query(490000, 'B', 'A', utc(2026, 9, 4, 12, 11, 0), true, LA, LO, 10));
        show("490 B (Oostende)", r);
        CHECK(oostende >= 0, "Oostende 490 B missing from the table");
        CHECK(same_tx(r.station, oostende), "got %s %.3f", nm(r.station), r.confidence);
    }
    /* 7. Receiver position configured with lat and lon swapped (56.04 E, 3.36 S puts it in
     *    East Africa): what is heard contradicts it, rx_pos_suspect is raised and nothing
     *    is high; the same traffic at the right position is never flagged (case 1). */
    {
        NavtexIdentifier id;
        int sus = 0, hi = 0;
        warm_dalgety(id, LO, LA, 3, &sus, &hi);
        NavtexIdentResult r = id.identify(query(518000, 'G', 'A', utc(2026, 9, 4, 5, 1, 20), true, LO, LA, 30));
        show("lat/lon swapped in the config", r);
        CHECK(r.rx_pos_suspect, "swapped position not flagged");
        CHECK(sus > 100, "flagged on only %d messages", sus);
        CHECK(hi == 0 && std::strcmp(r.level, "high") != 0, "%d high answers at a wrong position", hi);
        /* ... and when the operator fixes the config in the running process, the old
         * position's evidence is dropped: no longer suspect, and high again once the new
         * position has been checked (100 messages, about a day of this traffic). */
        int sus2 = 0, hi2 = 0;
        warm_dalgety(id, LA, LO, 2, &sus2, &hi2, 4);
        NavtexIdentResult g = id.identify(query(518000, 'E', 'A', utc(2026, 9, 7, 4, 41, 42), true, LA, LO, 12));
        show("config fixed, two days later", g);
        CHECK(sus2 == 0 && !g.rx_pos_suspect, "still suspect on %d messages after the fix", sus2);
        CHECK(hi2 > 0 && std::strcmp(g.level, "high") == 0, "high %d, level %s after the fix", hi2, g.level);
    }
    /* 8. Inputs the identifier cannot use.  One message with a NaN position (rx_valid set)
     *    or a NaN SNR must not change the answer to anything else, nor poison the learned
     *    state; lower-case B1 is the same letter. */
    {
        NavtexIdentifier a, b, c;
        warm_dalgety(a, LA, LO, 3, 0, 0);
        warm_dalgety(b, LA, LO, 3, 0, 0);
        warm_dalgety(c, LA, LO, 3, 0, 0);
        NavtexIdentQuery bad = query(518000, 'E', 'A', utc(2026, 9, 4, 4, 41, 42), true, NAN, NAN, 12);
        NavtexIdentResult rb = a.identify(bad);
        a.observe(bad, rb);
        CHECK(rb.station < 0 || same_tx(rb.station, niton), "NaN position: got %s", nm(rb.station));
        CHECK(rb.distance_km == -1 && std::strcmp(rb.level, "high") != 0, "NaN position treated as known");
        warm_dalgety(a, LA, LO, 2, 0, 0, 4);
        NavtexIdentResult after = a.identify(query(518000, 'E', 'A', utc(2026, 9, 6, 4, 41, 42), true, LA, LO, 12));
        show("after one NaN-position message", after);
        CHECK(same_tx(after.station, niton) && std::strcmp(after.level, "high") == 0 && after.agree,
              "poisoned: %s %.3f %s", nm(after.station), after.confidence, after.reason);
        CHECK(same_tx(after.member[0].station, niton), "bayes member names %s", nm(after.member[0].station));
        NavtexIdentResult ns = b.identify(query(518000, 'E', 'A', utc(2026, 9, 4, 4, 41, 42), true, LA, LO, NAN));
        show("SNR NaN", ns);
        CHECK(same_tx(ns.station, niton) && same_tx(ns.member[0].station, niton), "NaN SNR: %s (bayes %s)",
              nm(ns.station), nm(ns.member[0].station));
        NavtexIdentResult lc = c.identify(query(518000, 'e', 'A', utc(2026, 9, 4, 4, 41, 42), true, LA, LO, 12));
        CHECK(same_tx(lc.station, niton) && std::strcmp(lc.level, "high") == 0, "lower-case e: %s %s", nm(lc.station), lc.level);
        NavtexIdentifier d, e;
        NavtexIdentResult y = d.identify(query(518000, 'Y', 'A', utc(2026, 9, 4, 4, 41, 42), true, LA, LO, 12));
        NavtexIdentResult nine = e.identify(query(518000, '9', 'A', utc(2026, 9, 4, 4, 41, 42), true, LA, LO, 12));
        CHECK(y.station == nine.station && std::strcmp(y.reason, nine.reason) == 0, "'Y' %s/%s vs '9' %s/%s",
              nm(y.station), y.reason, nm(nine.station), nine.reason);
    }
    /* 9. A fresh receiver: nothing is high until its configured position has been checked. */
    {
        NavtexIdentifier id;
        NavtexIdentResult r = id.identify(query(518000, 'E', 'A', utc(2026, 9, 4, 4, 41, 42), true, LA, LO, 12));
        CHECK(!r.rx_pos_checked && std::strcmp(r.level, "high") != 0, "cold receiver: level %s", r.level);
    }
    /* 10. HF is never high (the table's 4209.5 kHz list is incomplete); 424 kHz far from
     *     Japan with a known position is not high either. */
    {
        const int naha = find_station(424000, 'G', "Naha");
        NavtexIdentifier a;
        warm_dalgety(a, LA, LO, 3, 0, 0);
        NavtexIdentResult hf = a.identify(query(4209500, 'M', 'A', utc(2026, 9, 4, 2, 1, 0), true, LA, LO, 20));
        show("4209.5 M in slot", hf);
        CHECK(std::strcmp(hf.level, "high") != 0, "HF level %s", hf.level);
        NavtexIdentifier b;
        for (int k = 0; k < 150; k++) {                    /* Sydney AU, checked position */
            NavtexIdentQuery q = query(518000, 'Q', 'A', utc(2026, 9, 1, 0, 0, 0) + k * 1200, true, -33.86, 151.2, 10);
            NavtexIdentResult r = b.identify(q);
            b.observe(q, r);
        }
        NavtexIdentResult j = b.identify(query(424000, 'G', 'A', utc(2026, 9, 4, 1, 1, 10), true, -33.86, 151.2, 10));
        show("424 G in slot, Sydney AU", j);
        CHECK(naha >= 0 && std::strcmp(j.level, "high") != 0, "424 at 7,000 km: level %s", j.level);
    }
}

/* ------------------------------------------------------------ fixture */
struct FRow { std::string rx; NavtexIdentQuery q; int truth; };

static bool load_fixture(const char *path, std::vector<FRow> &rows) {
    FILE *f = std::fopen(path, "r");
    if (!f) return false;
    char line[1024];
    bool header = false;
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        if (!header) { header = true; continue; }
        std::vector<std::string> v;
        std::string cur;
        for (char *p = line; *p && *p != '\n' && *p != '\r'; p++) {
            if (*p == ',') { v.push_back(cur); cur.clear(); } else cur += *p;
        }
        v.push_back(cur);
        if (v.size() < 13) continue;
        FRow r;
        std::memset(&r.q, 0, sizeof(r.q));
        r.rx = v[0];
        r.q.rx_valid = std::atoi(v[1].c_str()) != 0;
        r.q.rx_lat = std::atof(v[2].c_str()); r.q.rx_lon = std::atof(v[3].c_str());
        r.q.freq_hz = std::atol(v[4].c_str());
        r.q.b1 = (char)std::atoi(v[5].c_str()); r.q.b2 = (char)std::atoi(v[6].c_str());
        r.q.serial = v[7].empty() ? -1 : std::atoi(v[7].c_str());
        r.q.start_utc = std::atoll(v[8].c_str()); r.q.duration_s = std::atoi(v[9].c_str());
        r.q.has_snr = !v[10].empty(); r.q.snr_db = std::atof(v[10].c_str());
        r.q.has_chars = !v[11].empty(); r.q.chars_failed_pct = std::atof(v[11].c_str());
        r.truth = std::atoi(v[12].c_str());
        rows.push_back(r);
    }
    std::fclose(f);
    return true;
}

static void run_fixture(const std::vector<FRow> &rows, std::vector<NavtexIdentResult> &out, double *us_per_msg) {
    out.clear();
    out.reserve(rows.size());
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    size_t i = 0;
    while (i < rows.size()) {
        size_t j = i;
        while (j < rows.size() && rows[j].rx == rows[i].rx) j++;
        NavtexIdentifier *id = new NavtexIdentifier();          /* one fresh identifier per receiver */
        for (size_t k = i; k < j; k++) {
            NavtexIdentResult r = id->identify(rows[k].q);
            id->observe(rows[k].q, r);
            out.push_back(r);
        }
        delete id;
        i = j;
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (us_per_msg) *us_per_msg = rows.empty() ? 0 : 1e6 * s / rows.size();
}

/* Measured on this fixture when the floor was set (NAVTEX_IDENT_VERSION 2, 20 receivers
 * x 200 messages, sampled from a holdout world generated from the CORRECTED station
 * table): acc 0.735.  The floor sits about 5 points under it.  (On the fixture from the
 * uncorrected table it was 0.813; the drop is that world's random table-error draws —
 * e.g. Mondolfo 490 sending as 'F' beside the newly listed Split 490 F — not the
 * corrections, which on the real Dalgety Bay logs name ~2,900 more messages correctly.) */
static const double ACC_FLOOR = 0.69;

static void fixture(const char *path) {
    std::vector<FRow> rows;
    if (!load_fixture(path, rows) || rows.size() < 1000) { CHECK(false, "cannot read fixture %s", path); return; }
    std::vector<NavtexIdentResult> res, res2;
    double us = 0;
    run_fixture(rows, res, &us);
    long n_in = 0, ok_in = 0, n_hi = 0, bad_hi = 0, bad_inv = 0;
    for (size_t k = 0; k < rows.size(); k++) {
        const NavtexIdentResult &r = res[k];
        int t = rows[k].truth;
        bool correct = r.station >= 0 ? (t >= 0 && same_tx(r.station, t)) : t == -1;
        if (t >= 0) { n_in++; ok_in += correct; }
        if (r.station >= 0 && r.confidence >= 0.9) { n_hi++; bad_hi += !correct; }
        bool inv = std::strcmp(r.level, navtex_level_for(r.confidence)) == 0 &&
                   (r.station >= 0) == (r.confidence > 0) && r.confidence >= 0 && r.confidence <= 1 &&
                   !(std::strcmp(r.level, "high") == 0 && (!r.agree || r.rx_pos_suspect)) &&
                   (r.station < 0 || !r.agree || same_tx(r.member[0].station, r.member[1].station)) &&
                   /* "high" passed the slot test the result reports */
                   !(r.confidence >= 0.9 && !r.in_slot) &&
                   /* alternatives: never the answer, never each other, on the confidence scale */
                   !(r.alt[0] >= 0 && same_tx(r.alt[0], r.station)) && !(r.alt[1] >= 0 && same_tx(r.alt[1], r.station)) &&
                   !(r.alt[0] >= 0 && r.alt[1] >= 0 && same_tx(r.alt[0], r.alt[1])) &&
                   r.alt_p[0] <= r.confidence && r.alt_p[1] <= r.confidence &&
                   r.confidence + r.alt_p[0] + r.alt_p[1] <= 1 + 1e-9;
        bad_inv += !inv;
    }
    double acc = n_in ? (double)ok_in / n_in : 0, wrong_hi = n_hi ? (double)bad_hi / n_hi : 0;
    std::printf("fixture: %zu rows, acc %.4f (floor %.2f), high %ld wrong %ld = %.4f (gate 0.01), %.1f us/msg\n",
                rows.size(), acc, ACC_FLOOR, n_hi, bad_hi, wrong_hi, us);
    CHECK(wrong_hi <= 0.01, "wrong_hi %.4f", wrong_hi);
    CHECK(n_hi >= 100, "only %ld high answers", n_hi);
    CHECK(acc >= ACC_FLOOR, "acc %.4f below the floor", acc);
    CHECK(bad_inv == 0, "%ld results break an invariant", bad_inv);

    run_fixture(rows, res2, 0);                                   /* replay: same state, same answers */
    long diff = 0;
    for (size_t k = 0; k < rows.size(); k++)
        diff += res[k].station != res2[k].station || res[k].confidence != res2[k].confidence ||
                res[k].rx_pos_suspect != res2[k].rx_pos_suspect;
    CHECK(diff == 0, "%ld results differ on replay", diff);
}

int main(int argc, char **argv) {
    std::printf("navtex_ident version %d, %d stations\n", NAVTEX_IDENT_VERSION, NAVTEX_STATION_COUNT);
    golden();
    fixture(argc > 1 ? argv[1] : "testdata/ident_fixture.csv");
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
