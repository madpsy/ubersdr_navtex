/* ident_facts_test.cpp — unit tests for navtex_ident_facts().
 *
 * The facts are the only explanation an operator ever sees for an
 * identification, and the UI, MQTT and the API all repeat them verbatim — so a
 * fact that claims more than the result supports is a bug everywhere at once.
 * These tests build queries and results by hand rather than running the
 * identifier, so they pin the WORDING RULES independently of whatever the
 * algorithm decides: each line appears exactly when its support is present,
 * and never otherwise.
 *
 * Table-derived numbers (how many stations share a letter) are computed from
 * navtex_stations.h rather than hard-coded, so regenerating the table does not
 * break the test.
 *
 *   cmake --build build --target ident_facts_test
 *   ctest --test-dir build -R ident_facts --output-on-failure
 */

#include "navtex_ident_facts.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

static void check(bool ok, const std::string &what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

static bool any_contains(const std::vector<std::string> &f, const std::string &needle)
{
    for (const auto &s : f) if (s.find(needle) != std::string::npos) return true;
    return false;
}

static void dump(const std::vector<std::string> &f)
{
    for (const auto &s : f) printf("         | %s\n", s.c_str());
}

static int find_station(long freq_hz, char id, const char *name)
{
    for (int i = 0; i < NAVTEX_STATION_COUNT; i++)
        if (NAVTEX_STATIONS[i].freq_hz == freq_hz && NAVTEX_STATIONS[i].id == id &&
            strcmp(NAVTEX_STATIONS[i].name, name) == 0)
            return i;
    return -1;
}

/* Dalgety Bay, Scotland — the receiver the real logs came from. */
static NavtexIdentQuery base_query(long freq, char b1, char b2)
{
    NavtexIdentQuery q {};
    q.freq_hz  = freq;
    q.b1 = b1; q.b2 = b2; q.serial = 42;
    q.start_utc = 1792800000;   /* 2026-10-24 00:00:00 UTC */
    q.duration_s = 60;
    q.rx_valid = true; q.rx_lat = 56.0403; q.rx_lon = -3.3554;
    q.has_snr = true; q.snr_db = 12;
    return q;
}

static NavtexIdentResult empty_result()
{
    NavtexIdentResult r {};
    r.station = -1; r.level = "none"; r.reason = "";
    r.distance_km = r.bearing_deg = -1;
    r.member[0] = { "bayes", -1, 0, "" };
    r.member[1] = { "rules", -1, 0, "" };
    r.alt[0] = r.alt[1] = -1;
    return r;
}

static NavtexIdentResult chose(int st, double c0, double c1)
{
    NavtexIdentResult r = empty_result();
    r.station = st; r.confidence = (c0 + c1) / 2; r.level = navtex_level_for(r.confidence);
    r.agree = true;
    r.rx_pos_checked = true;
    r.member[0].station = st; r.member[0].confidence = c0;
    r.member[1].station = st; r.member[1].confidence = c1;
    return r;
}

static void test_helpers()
{
    printf("helpers\n");
    int niton = find_station(518000, 'E', "Niton");
    check(niton >= 0, "Niton E is in the table");
    check(navtex_station_key(niton) == "518:E:Niton", "station key is freq_khz:letter:name");
    check(navtex_freq_khz_str(4209500) == "4209.5", "HF carrier keeps its half kHz");
    check(navtex_freq_khz_str(490000) == "490", "MF carrier has no .0");
    check(navtex_fmt_km(1140.4) == "1,140", "thousands separator");
    check(navtex_fmt_km(612.6) == "613", "rounds to whole km");
    check(navtex_fmt_km(1234567) == "1,234,567", "two separators");
    check(navtex_station_key(-1).empty() && navtex_station_key(NAVTEX_STATION_COUNT).empty(),
          "out-of-range index has no key");
}

static void test_identified_in_slot()
{
    printf("identified, in slot, methods agree\n");
    int niton = find_station(518000, 'E', "Niton");
    NavtexIdentQuery q = base_query(518000, 'E', 'A');
    q.start_utc += 40 * 60 + 102;                 /* 00:41:42 */
    NavtexIdentResult r = chose(niton, 0.98, 0.95);
    r.in_slot = true; r.slot_offset_s = 102; r.burst_offset_s = 102;

    auto f = navtex_ident_facts(q, r);
    dump(f);
    size_t n = navtex_letter_candidates(518000, 'E').size();
    check(any_contains(f, std::to_string(n) + " stations use letter E on 518 kHz"),
          "candidate count comes from the table");
    check(any_contains(f, "Started 1 min 42 s into E's 00:40 slot"), "slot offset and slot time");
    check(any_contains(f, "Both methods agreed (98% / 95%)"), "agreement with both confidences");
    check(any_contains(f, "Nearest is Niton"), "Niton is the nearest E on 518 to Dalgety Bay");
    check(!any_contains(f, "Night path"), "no night line when the result does not say so");
    check(!any_contains(f, "Receiver location unknown"), "location is known");
    check(f.size() >= 3 && f.size() <= NAVTEX_IDENT_MAX_FACTS, "3..6 lines");
}

static void test_before_slot_and_night()
{
    printf("before the slot, night path\n");
    int niton = find_station(518000, 'E', "Niton");
    NavtexIdentQuery q = base_query(518000, 'E', 'E');
    q.start_utc += 4 * 3600 + 40 * 60 - 20;       /* 04:39:40 */
    NavtexIdentResult r = chose(niton, 0.9, 0.9);
    r.in_slot = true; r.slot_offset_s = -20; r.burst_offset_s = -20; r.night_path = true;

    auto f = navtex_ident_facts(q, r);
    dump(f);
    check(any_contains(f, "Started 20 s before E's 04:40 slot"), "negative offset reads as before");
    check(any_contains(f, "Night path"), "night line present when the result says night");
}

static void test_outside_slot()
{
    printf("outside the slot\n");
    int niton = find_station(518000, 'E', "Niton");
    NavtexIdentQuery q = base_query(518000, 'E', 'B');
    NavtexIdentResult r = chose(niton, 0.7, 0.7);
    r.in_slot = false;
    auto f = navtex_ident_facts(q, r);
    dump(f);
    check(any_contains(f, "Outside E's scheduled slots — subject B (meteorological warnings) "
                          "may be sent on receipt"),
          "subject B explains an off-slot start");

    q.b2 = 'E';
    f = navtex_ident_facts(q, r);
    check(any_contains(f, "Outside E's scheduled slots") && !any_contains(f, "on receipt"),
          "a forecast gets no on-receipt excuse");
}

static void test_disagreement()
{
    printf("methods disagree\n");
    int niton = find_station(518000, 'E', "Niton");
    int samsun = find_station(518000, 'E', "Samsun");
    check(samsun >= 0, "Samsun E is in the table");
    NavtexIdentQuery q = base_query(518000, 'E', 'A');
    NavtexIdentResult r = chose(niton, 0.6, 0.0);
    r.agree = false;
    r.member[1].station = samsun; r.member[1].confidence = 0.55;
    auto f = navtex_ident_facts(q, r);
    dump(f);
    check(any_contains(f, "Methods disagreed: Niton vs Samsun"), "names both verdicts");
    check(!any_contains(f, "Both methods agreed"), "no agreement claim");

    r.member[1].station = -1;
    f = navtex_ident_facts(q, r);
    check(any_contains(f, "Only one method named a station"), "abstention is reported");
}

static void test_rx_unknown()
{
    printf("receiver location unknown\n");
    int niton = find_station(518000, 'E', "Niton");
    NavtexIdentQuery q = base_query(518000, 'E', 'A');
    q.rx_valid = false;
    NavtexIdentResult r = chose(niton, 0.4, 0.4);
    auto f = navtex_ident_facts(q, r);
    dump(f);
    check(any_contains(f, "Receiver location unknown — confidence limited"), "says so");
    check(!any_contains(f, " km"), "no distance is claimed without a location");
}

static void test_letter_not_in_list()
{
    printf("letter not in the list\n");
    NavtexIdentQuery q = base_query(490000, 'Z', 'A');
    check(navtex_letter_candidates(490000, 'Z').empty(), "no Z on 490 kHz in the table");
    NavtexIdentResult r = empty_result();
    auto f = navtex_ident_facts(q, r);
    dump(f);
    check(any_contains(f, "Letter Z not in the station list for 490 kHz — transmitter not identified"),
          "explains the non-identification");
    check(!any_contains(f, "Both methods"), "no method line when nothing was chosen");
    check(!any_contains(f, "slot"), "no slot line without a station");
    check(any_contains(f, "Nearest stations on 490 kHz: "), "names the letters used nearby");
    q.rx_valid = false;
    check(!any_contains(navtex_ident_facts(q, r), "Nearest stations"),
          "but not without a receiver location");
    q.rx_valid = true;

    q.freq_hz = 12345000;
    f = navtex_ident_facts(q, r);
    check(any_contains(f, "No station list for 12345 kHz"), "unknown frequency");
}

static void test_unattributed_with_candidates()
{
    printf("candidates but no match\n");
    NavtexIdentQuery q = base_query(518000, 'E', 'A');
    NavtexIdentResult r = empty_result();
    r.expected_b1 = 'F';
    auto f = navtex_ident_facts(q, r);
    dump(f);
    check(any_contains(f, "No confident match among the"), "says no candidate was chosen");
    check(any_contains(f, "Nearest candidate is Niton"), "nearest candidate is still a fact");
    check(any_contains(f, "Timing fits letter F's slot, not E's"), "a mistimed letter is flagged");
}

static void test_suspect_and_range()
{
    printf("warnings\n");
    int samsun = find_station(518000, 'E', "Samsun");
    NavtexIdentQuery q = base_query(518000, 'E', 'A');
    NavtexIdentResult r = chose(samsun, 0.9, 0.9);
    r.rx_pos_suspect = true;
    auto f = navtex_ident_facts(q, r);
    dump(f);
    check(any_contains(f, "Receiver location looks inconsistent"), "suspect location");
    check(any_contains(f, "nearest candidate is Niton"), "a far choice names the nearer candidate");
    check(f.size() <= NAVTEX_IDENT_MAX_FACTS, "still capped");

    /* A day-path station far outside its range gets a range line. */
    int valentia = find_station(518000, 'W', "Valentia");
    if (valentia >= 0) {
        NavtexIdentQuery q2 = base_query(518000, 'W', 'A');
        q2.rx_lat = 30.0; q2.rx_lon = -3.3554;
        NavtexIdentResult r2 = chose(valentia, 0.9, 0.9);
        auto f2 = navtex_ident_facts(q2, r2);
        dump(f2);
        check(any_contains(f2, "nominal range"), "out-of-range day path is noted");
    }
}

static void test_null_strings_are_not_read()
{
    printf("stored results\n");
    int niton = find_station(518000, 'E', "Niton");
    NavtexIdentQuery q = base_query(518000, 'E', 'A');
    NavtexIdentResult r = chose(niton, 0.9, 0.9);
    r.level = nullptr; r.reason = nullptr;
    r.member[0].name = r.member[1].name = nullptr;
    r.member[0].reason = r.member[1].reason = nullptr;
    auto f = navtex_ident_facts(q, r);   /* must not crash */
    check(!f.empty(), "a result with nulled strings still yields facts");
}

/* From the UX review: percentages that disagree with the page, a night line
 * on a 160 km path, an unexplained rejection and an unexplained alternative. */
static void test_review_fixes()
{
    printf("review fixes: rounding, short night path, out-of-range, other-letter alternative\n");
    check(navtex_fmt_pct(0.445) == "45%" && navtex_fmt_pct(0.4444) == "44%",
          "rounds via 3 dp like the page");
    check(navtex_fmt_pct(0.995) == "99%" && navtex_fmt_pct(1.0) == "100%", "never 100% short of 1");

    int niton = find_station(518000, 'E', "Niton");
    NavtexIdentQuery q = base_query(518000, 'E', 'E');
    q.rx_lat = 51.0; q.rx_lon = -1.5;              /* ~60 km from Niton */
    NavtexIdentResult r = chose(niton, 0.9, 0.9);
    r.night_path = true;
    auto f = navtex_ident_facts(q, r);
    dump(f);
    check(!any_contains(f, "Night path"), "no 'long-distance' line for a short path");

    int other = -1;
    for (int i = 0; i < NAVTEX_STATION_COUNT; i++)
        if (NAVTEX_STATIONS[i].freq_hz == 518000 && NAVTEX_STATIONS[i].id == 'K') { other = i; break; }
    check(other >= 0, "a 518 kHz K station exists");
    if (other >= 0) {
        r = chose(niton, 0.6, 0.6);
        r.alt[0] = other; r.alt_p[0] = 0.2;
        f = navtex_ident_facts(q, r);
        dump(f);
        check(any_contains(f, "uses letter K"), "other-letter alternative explained");
        r.alt_p[0] = 0.001;
        f = navtex_ident_facts(q, r);
        check(!any_contains(f, "uses letter K"), "...but not for a negligible one");
    }

    /* A lone candidate on the far side of the world, rejected. */
    bool tried = false;
    for (int i = 0; i < NAVTEX_STATION_COUNT && !tried; i++) {
        const NavtexStation &s = NAVTEX_STATIONS[i];
        if (s.range_nm <= 0) continue;
        if (navtex_letter_candidates(s.freq_hz, s.id).size() != 1) continue;
        if (navtex_great_circle_km(56.0403, -3.3554, s.lat, s.lon) < s.range_nm * 1.852 * 3) continue;
        tried = true;
        NavtexIdentQuery q2 = base_query(s.freq_hz, s.id, 'A');
        f = navtex_ident_facts(q2, empty_result());
        dump(f);
        check(any_contains(f, "far beyond its"),
              std::string("out-of-range sole candidate explained: ") + s.name);
    }
    check(tried, "found a far sole candidate to test with");
}

/* v2: a later message in a run is judged by the run's start, and the facts
 * must say so rather than look off-schedule; reason codes that cap or withhold
 * an answer are explained; an unchecked receiver position is mentioned. */
static void test_run_and_reasons()
{
    printf("transmission runs and reason codes\n");
    int niton = find_station(518000, 'E', "Niton");
    NavtexIdentQuery q = base_query(518000, 'E', 'A');
    NavtexIdentResult r = chose(niton, 0.98, 0.95);
    /* fifth message of the 00:40 run, starting 14 min in; the run began 40 s in */
    q.start_utc += 54 * 60;                       /* 00:54:00 */
    r.in_slot = true; r.slot_offset_s = 14 * 60; r.burst_offset_s = 40;
    std::vector<std::string> f = navtex_ident_facts(q, r);
    for (auto &l : f) printf("         | %s\n", l.c_str());
    check(any_contains(f, "Part of E's 00:40 transmission run, which started 40 s into the slot"),
          "a later message names the run, not its own late start");
    check(!any_contains(f, "Outside"), "a message within an on-schedule run is not 'outside'");

    r.reason = "UNLISTED_LOCAL";
    check(any_contains(navtex_ident_facts(q, r), "possibly an unlisted local service"), "UNLISTED_LOCAL explained");
    r.reason = "AGREE_LETTER";
    check(any_contains(navtex_ident_facts(q, r), "identified from timing and propagation alone"), "AGREE_LETTER explained");
    r.reason = "";
    r.rx_pos_checked = false;
    check(any_contains(navtex_ident_facts(q, r), "Receiver location not yet confirmed"), "unchecked position mentioned");
}

int main()
{
    printf("navtex ident facts test\n\n");
    test_helpers();
    test_identified_in_slot();
    test_before_slot_and_night();
    test_outside_slot();
    test_disagreement();
    test_rx_unknown();
    test_review_fixes();
    test_letter_not_in_list();
    test_unattributed_with_candidates();
    test_suspect_and_range();
    test_null_strings_are_not_read();
    test_run_and_reasons();
    printf("\n%s\n", g_failures == 0 ? "all checks passed" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
