/* -*- c++ -*- */
/*
 * Copyright 2024 Contributors.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Plain-English facts behind a station identification.
 *
 * The identifier (navtex_ident.h) answers "which transmitter sent this?" with a
 * station, a confidence and a handful of supporting numbers.  A bare "Niton,
 * 97 %" is not something an operator can check, so every identification is
 * shipped with three to six short sentences saying WHY: how many stations share
 * the letter, how far away they are, whether the message started in the
 * station's scheduled slot, whether the two methods agreed.
 *
 * These lines are produced in exactly one place, here, and travel with the
 * identification everywhere it goes — the sidecar, /api/ident, MQTT and the web
 * UI.  The browser never composes a fact of its own: if two surfaces could
 * describe the same identification in different words, one of them would
 * eventually be wrong.
 *
 * The rule for every line is that it states only what the query and the result
 * actually support.  A fact about distance needs a receiver location; a fact
 * about the slot needs a station with a schedule; agreement needs both members
 * to have named a station.  When the support is missing the line is left out
 * (or replaced by one saying what is missing), never guessed.
 *
 * Pure: no I/O, no shared state, no allocation beyond the returned strings.
 * Only the numeric fields of the result are read — the const char * members
 * (level, reason, member names) may point into the identifier's own buffers,
 * and a caller that keeps a result around is entitled to null them.
 *
 * Unit tests: test/ident_facts_test.cpp (ctest -R ident_facts).
 */

#ifndef NAVTEX_IDENT_FACTS_H
#define NAVTEX_IDENT_FACTS_H

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "navtex_ident.h"
#include "navtex_countries.h"

/* Two table entries are on "the same frequency" when their carriers are within
 * this of each other.  NAVTEX channels are 490, 518 and 4209.5 kHz, so any
 * tolerance below tens of kHz is equivalent; 500 Hz is the identifier's. */
static const long NAVTEX_IDENT_FREQ_TOL_HZ = 500;

/* Facts are capped so the UI tooltip and the MQTT payload stay readable. */
static const size_t NAVTEX_IDENT_MAX_FACTS = 6;

/* "518", "490", "4209.5" — the carrier in kHz without a needless ".0". */
static inline std::string navtex_freq_khz_str(long freq_hz)
{
    char b[32];
    if (freq_hz % 1000 == 0) snprintf(b, sizeof(b), "%ld", freq_hz / 1000);
    else                     snprintf(b, sizeof(b), "%.1f", freq_hz / 1000.0);
    return b;
}

/* The stable key for a table station: "518:E:Niton".
 *
 * Indices into NAVTEX_STATIONS are NOT stable — regenerating the table from a
 * newer copy of the source page inserts and reorders rows — so anything stored
 * (a sidecar, a bookmark in the UI, a Home Assistant history) names a station
 * by frequency, letter and name instead. */
static inline std::string navtex_station_key(int idx)
{
    if (idx < 0 || idx >= NAVTEX_STATION_COUNT) return "";
    const NavtexStation &s = NAVTEX_STATIONS[idx];
    return navtex_freq_khz_str(s.freq_hz) + ":" + std::string(1, s.id) + ":" + s.name;
}

/* Country display name for a station, falling back to its three-letter code
 * so a table row with no country entry still says something. */
static inline std::string navtex_station_country_name(int idx)
{
    if (idx < 0 || idx >= NAVTEX_STATION_COUNT) return "";
    const NavtexCountry *c = navtex_country(NAVTEX_STATIONS[idx].country);
    return c ? c->name : NAVTEX_STATIONS[idx].country;
}

/* 1140.4 -> "1,140".  Whole kilometres: the station coordinates in the table
 * are not good to better than that, and nor is "how far away" as a cue. */
static inline std::string navtex_fmt_km(double km)
{
    long v = std::lround(km < 0 ? 0 : km);
    std::string digits = std::to_string(v), out;
    int n = (int)digits.size();
    for (int i = 0; i < n; i++) {
        if (i > 0 && (n - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

/* 0.983 -> "98%".
 *
 * Rounded the way the page rounds the same number, so one panel never shows
 * "Rules Tallinn 45%" beside "Both methods agreed (49% / 44%)": first to the
 * 3 decimals every JSON payload carries (ident.conf, members[].conf), then to
 * a whole percent, halves away from zero.  And never "100%" for anything
 * short of certainty — 0.995 is "99%". */
static inline std::string navtex_fmt_pct(double frac)
{
    long per_mille = std::lround(frac * 1000.0);
    long pct = (per_mille + 5) / 10;
    if (pct >= 100 && per_mille < 1000) pct = 99;
    char b[16];
    snprintf(b, sizeof(b), "%ld%%", pct);
    return b;
}

/* Below this distance a night path is not worth a line: "long-distance
 * reception likely" said of a station 160 km away explains nothing, since
 * ground wave reaches that far day or night. */
static const double NAVTEX_IDENT_NIGHT_FACT_KM = 500.0;

/* Stations on the query's frequency transmitting the query's letter. */
static inline std::vector<int> navtex_letter_candidates(long freq_hz, char b1)
{
    std::vector<int> out;
    for (int i = 0; i < NAVTEX_STATION_COUNT; i++) {
        const NavtexStation &s = NAVTEX_STATIONS[i];
        if (std::labs((long)s.freq_hz - freq_hz) <= NAVTEX_IDENT_FREQ_TOL_HZ && s.id == b1)
            out.push_back(i);
    }
    return out;
}

/* Is any table station on this frequency at all?  Distinguishes "letter not
 * used here" from "we have no list for this frequency". */
static inline bool navtex_freq_in_table(long freq_hz)
{
    for (int i = 0; i < NAVTEX_STATION_COUNT; i++)
        if (std::labs((long)NAVTEX_STATIONS[i].freq_hz - freq_hz) <= NAVTEX_IDENT_FREQ_TOL_HZ)
            return true;
    return false;
}

/* The subjects a station may broadcast on receipt rather than waiting for its
 * slot (IMO NAVTEX Manual: navigational warnings, meteorological warnings,
 * search-and-rescue information, and additional navigational warnings). */
static inline const char *navtex_on_receipt_subject(char b2)
{
    switch (b2) {
    case 'A': return "navigational warnings";
    case 'B': return "meteorological warnings";
    case 'D': return "search and rescue";
    case 'L': return "navigational warnings";
    default:  return nullptr;
    }
}

static inline std::string navtex_fmt_offset(int s)
{
    int a = s < 0 ? -s : s;
    char b[48];
    if (a >= 60) snprintf(b, sizeof(b), "%d min %d s", a / 60, a % 60);
    else         snprintf(b, sizeof(b), "%d s", a);
    return b;
}

/*
 * The facts for one identification.
 *
 * Ordered most-informative first, and capped at NAVTEX_IDENT_MAX_FACTS:
 *
 *   1. the candidate set — how many stations use this letter here (or that none
 *      does, which is why nothing was identified)
 *   2. distance — the chosen station and the nearest alternative, or that the
 *      receiver location is unknown
 *   3. timing — in or outside the chosen station's scheduled slot
 *   4. method agreement
 *   5. night path
 *   6. warnings — an implausible receiver location, range exceeded
 */
static inline std::vector<std::string>
navtex_ident_facts(const NavtexIdentQuery &q, const NavtexIdentResult &r)
{
    std::vector<std::string> f;
    const std::string khz    = navtex_freq_khz_str(q.freq_hz) + " kHz";
    const std::string letter = (q.b1 > ' ' && q.b1 < 0x7F) ? std::string(1, q.b1) : "?";
    const bool        have   = r.station >= 0 && r.station < NAVTEX_STATION_COUNT;
    const NavtexStation *st  = have ? &NAVTEX_STATIONS[r.station] : nullptr;

    std::vector<int> cand = navtex_letter_candidates(q.freq_hz, q.b1);

    /* 1. Candidate set. */
    if (cand.empty()) {
        if (!navtex_freq_in_table(q.freq_hz))
            f.push_back("No station list for " + khz + " — transmitter not identified");
        else
            f.push_back("Letter " + letter + " not in the station list for " + khz +
                        " — transmitter not identified");
    } else if (cand.size() == 1) {
        f.push_back("Only " + std::string(NAVTEX_STATIONS[cand[0]].name) +
                    " uses letter " + letter + " on " + khz);
    } else {
        f.push_back(std::to_string(cand.size()) + " stations use letter " + letter +
                    " on " + khz);
        if (!have)
            f.push_back("No confident match among the " + std::to_string(cand.size()) +
                        " candidates");
    }

    /* 1b. No candidate at all: the commonest cause is a mis-decoded B1, so
     * name the letters that ARE used nearby on this frequency — the reader can
     * often spot the real one (a corrupted "E" for Niton, say). */
    if (cand.empty() && q.rx_valid && navtex_freq_in_table(q.freq_hz)) {
        std::vector<std::pair<double, int>> near;
        for (int i = 0; i < NAVTEX_STATION_COUNT; i++)
            if (std::labs((long)NAVTEX_STATIONS[i].freq_hz - q.freq_hz) <= NAVTEX_IDENT_FREQ_TOL_HZ)
                near.push_back({ navtex_great_circle_km(q.rx_lat, q.rx_lon, NAVTEX_STATIONS[i].lat,
                                                        NAVTEX_STATIONS[i].lon), i });
        std::sort(near.begin(), near.end());
        std::string line = "Nearest stations on " + khz + ": ";
        for (size_t k = 0; k < near.size() && k < 3; k++) {
            const NavtexStation &n = NAVTEX_STATIONS[near[k].second];
            line += (k ? ", " : "") + std::string(n.name) + " (" + std::string(1, n.id) + ")";
        }
        f.push_back(line);
    }

    /* 2. Distance.  Sorted candidate distances let the line say whether the
     * chosen station is the nearest, which is the single strongest cue a
     * reader has for judging it. */
    if (!q.rx_valid) {
        if (!cand.empty())
            f.push_back("Receiver location unknown — confidence limited");
    } else if (!cand.empty()) {
        std::vector<std::pair<double, int>> byd;
        for (int i : cand)
            byd.push_back({ navtex_great_circle_km(q.rx_lat, q.rx_lon,
                                                   NAVTEX_STATIONS[i].lat,
                                                   NAVTEX_STATIONS[i].lon), i });
        std::sort(byd.begin(), byd.end());

        if (have) {
            double d = navtex_great_circle_km(q.rx_lat, q.rx_lon, st->lat, st->lon);
            double b = navtex_initial_bearing_deg(q.rx_lat, q.rx_lon, st->lat, st->lon);
            char deg[16];
            snprintf(deg, sizeof(deg), "%ld°", std::lround(b) % 360);
            const std::string name = st->name;
            const std::string here = name + ", " + navtex_fmt_km(d) + " km at " + deg;

            /* The nearest OTHER candidate, by table index rather than name:
             * Niton appears twice on 518 kHz (E and K), but never twice under
             * one letter. */
            int other = -1;
            for (auto &p : byd) if (p.second != r.station) { other = p.second; break; }

            if (byd.front().second == r.station) {
                if (other >= 0)
                    f.push_back("Nearest is " + here + "; next " +
                                NAVTEX_STATIONS[other].name + ", " +
                                navtex_station_country_name(other) + ", " +
                                navtex_fmt_km(byd[1].first) + " km");
                else
                    f.push_back(name + " is " + navtex_fmt_km(d) + " km away at " + deg);
            } else {
                f.push_back(name + " is " + navtex_fmt_km(d) + " km at " + deg +
                            "; nearest candidate is " +
                            NAVTEX_STATIONS[byd.front().second].name + ", " +
                            navtex_station_country_name(byd.front().second) + ", " +
                            navtex_fmt_km(byd.front().first) + " km");
            }
        } else {
            /* Nothing chosen.  If even the nearest candidate is far beyond
             * its nominal range, that is why — say so, rather than leave the
             * reader to wonder why the only station with this letter was
             * turned down. */
            int n = byd.front().second;
            const NavtexStation &ns = NAVTEX_STATIONS[n];
            std::string line = "Nearest candidate is " + std::string(ns.name) + ", " +
                               navtex_station_country_name(n) + ", " +
                               navtex_fmt_km(byd.front().first) + " km";
            double rng = ns.range_nm * 1.852;
            if (ns.range_nm > 0 && byd.front().first > rng * 1.5)
                line += " — far beyond its " + std::to_string(ns.range_nm) + " nm (" +
                        navtex_fmt_km(rng) + " km) range";
            f.push_back(line);
        }
    }

    /* 3. Timing.  The identifier judges the start of the RUN of back-to-back
     * messages this one belongs to, not the message itself: a station sends
     * several messages per slot, so the fifth can begin 20 minutes in and
     * still be on schedule.  burst_offset_s is the run's start relative to
     * the slot it was judged on; slot_offset_s is this message's own start
     * relative to its nearest slot, so the slot's clock time is
     * start - slot_offset_s.  The two agree for the message that opens a run. */
    if (have && st->n_minutes > 0) {
        if (r.in_slot) {
            long slot = (long)((q.start_utc - r.slot_offset_s) % 86400);
            if (slot < 0) slot += 86400;
            slot = ((slot + 30) / 60) % 1440;   /* to the nearest minute */
            char hm[8];
            snprintf(hm, sizeof(hm), "%02ld:%02ld", slot / 60, slot % 60);
            auto rel = [&](int off) -> std::string {
                if (off > 0) return navtex_fmt_offset(off) + " into";
                if (off < 0) return navtex_fmt_offset(off) + " before";
                return "on time for";
            };
            if (std::abs(r.burst_offset_s - r.slot_offset_s) <= 60)
                f.push_back("Started " + rel(r.slot_offset_s) + " " + letter + "'s " + hm + " slot");
            else
                f.push_back("Part of " + letter + "'s " + hm + " transmission run, which started " +
                            rel(r.burst_offset_s) + " the slot");
        } else {
            const char *why = navtex_on_receipt_subject(q.b2);
            if (why)
                f.push_back("Outside " + letter + "'s scheduled slots — subject " +
                            std::string(1, q.b2) + " (" + why + ") may be sent on receipt");
            else
                f.push_back("Outside " + letter + "'s scheduled slots");
        }
    } else if (!have && r.expected_b1 && r.expected_b1 != q.b1 && r.expected_b1 > ' ') {
        /* Nothing chosen, but the clock says whose slot this is: worth saying,
         * since a mis-decoded B1 is the commonest reason for no match. */
        f.push_back("Timing fits letter " + std::string(1, r.expected_b1) + "'s slot, not " +
                    letter + "'s");
    }

    /* 4. Agreement between the two methods. */
    if (have) {
        const NavtexIdentMember &m0 = r.member[0], &m1 = r.member[1];
        bool n0 = m0.station >= 0 && m0.station < NAVTEX_STATION_COUNT;
        bool n1 = m1.station >= 0 && m1.station < NAVTEX_STATION_COUNT;
        if (n0 && n1 && m0.station == m1.station)
            f.push_back("Both methods agreed (" + navtex_fmt_pct(m0.confidence) + " / " +
                        navtex_fmt_pct(m1.confidence) + ")");
        else if (n0 && n1)
            f.push_back(std::string("Methods disagreed: ") + NAVTEX_STATIONS[m0.station].name +
                        " vs " + NAVTEX_STATIONS[m1.station].name);
        else if (n0 != n1)
            f.push_back("Only one method named a station");
    }

    /* 4b. An alternative with a DIFFERENT letter: the identifier considered
     * that the B1 itself may have been mis-decoded (E and K, say, differ by
     * few bits).  Without this line such an alternative looks like a bug. */
    for (int i = 0; i < 2; i++) {
        int a = r.alt[i];
        if (a < 0 || a >= NAVTEX_STATION_COUNT || a == r.station) continue;
        if (NAVTEX_STATIONS[a].id == q.b1 || !(r.alt_p[i] >= 0.01)) continue;
        f.push_back("Alternative " + std::string(NAVTEX_STATIONS[a].name) + " uses letter " +
                    std::string(1, NAVTEX_STATIONS[a].id) + " — the letter may have been misread");
        break;
    }

    /* 5. Propagation — only where it explains something: a long path. */
    if (have && r.night_path) {
        double d = q.rx_valid ? navtex_great_circle_km(q.rx_lat, q.rx_lon, st->lat, st->lon) : -1;
        if (d < 0 || d >= NAVTEX_IDENT_NIGHT_FACT_KM)
            f.push_back("Night path — long-distance reception likely");
    }

    /* 5b. Why a result was held back — the identifier's reason codes that a
     * reader would otherwise have to guess at from a low confidence. */
    const std::string why = r.reason ? r.reason : "";
    if (why == "AGREE_LETTER")
        f.push_back("The letter was unreadable or belongs to another station — identified from "
                    "timing and propagation alone, so confidence is capped");
    else if (why == "BAYES_ONLY")
        f.push_back("Only one method named a station; the other thinks the transmitter may not be "
                    "in the station list");
    else if (why == "UNLISTED_LOCAL")
        f.push_back("This letter is heard here by day, but no listed station using it can reach "
                    "here by day — possibly an unlisted local service, so confidence is capped");
    else if (why == "TOO_FAR")
        f.push_back("The only candidates are too far away to be heard on a daylight path — "
                    "not attributed");
    if (q.rx_valid && !r.rx_pos_checked && !r.rx_pos_suspect)
        f.push_back("Receiver location not yet confirmed by enough messages — 'high' is "
                    "unavailable until about 100 have been heard");

    /* 6. Warnings. */
    if (r.rx_pos_suspect)
        f.push_back("Receiver location looks inconsistent with the stations heard — "
                    "check NAVTEX_RX_LAT / NAVTEX_RX_LON or the UberSDR GPS setting");
    if (have && q.rx_valid && st->range_nm > 0 && !r.night_path) {
        double d   = navtex_great_circle_km(q.rx_lat, q.rx_lon, st->lat, st->lon);
        double rng = st->range_nm * 1.852;
        if (d > rng * 1.5)
            f.push_back(navtex_fmt_km(d) + " km is well beyond " + st->name + "'s " +
                        std::to_string(st->range_nm) + " nm (" + navtex_fmt_km(rng) +
                        " km) nominal range");
    }

    if (f.size() > NAVTEX_IDENT_MAX_FACTS) f.resize(NAVTEX_IDENT_MAX_FACTS);
    return f;
}

#endif /* NAVTEX_IDENT_FACTS_H */
