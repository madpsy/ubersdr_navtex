/* -*- c++ -*- */
/*
 * navtex_ident.h — which transmitter sent this NAVTEX message?
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Header-only, C++11, no I/O, no mutable globals (all state lives inside
 * NavtexIdentifier).  Include this file only; it pulls in the two member
 * methods (navtex_ident_bayes.h, navtex_ident_rules.h) and the shared types
 * (navtex_ident_common.h).
 */

/*
 * THE PROBLEM
 * -----------
 * A NAVTEX header says "ZCZC B1B2nn".  B1 is the transmitter's identity letter,
 * but there are only 24 letters and 21 NAVAREAs, so every letter is in use many
 * times over: 'E' on 518 kHz is Niton in the UK, and also a station in the
 * Mediterranean, one in the Far East, one in South America...  The decoder
 * also garbles B1 now and then, and some transmitters on the air are not in
 * our table at all (the 490 kHz national services Dalgety Bay hears from
 * Oostende and Malin Head, for instance).  So "which transmitter?" has to be
 * worked out from what else is known about the message:
 *
 *   - the channel, and the letter as decoded;
 *   - WHEN it started, against each station's 10-minute schedule slots;
 *   - WHERE the receiver is: how far each candidate is, whether the path is
 *     in daylight (MF groundwave reaches ~250-400 nm) or darkness (skywave can
 *     carry 518/490 kHz 1000-3000+ km), and whether the SNR fits the distance;
 *   - what this receiver has heard before.
 *
 * The message BODY is never read.  The answer must stand on the header, the
 * clock, the decoder's metrics and the receiver position alone.
 *
 * TWO METHODS, AND WHY
 * --------------------
 * Two identification methods run side by side, built differently on purpose,
 * and their answers are combined.  When two independently built methods agree
 * that is the strongest available guard against a confident mistake; when they
 * disagree that is itself the "be careful" signal, and a disagreement never
 * produces a "high" answer.
 *
 *  1. bayes (navtex_ident_bayes.h): a full probabilistic model.  Every table
 *     station on the channel, plus "a transmitter not in the table" and "a
 *     false header", is scored as rate x P(letter) x P(time) x P(SNR); the rate
 *     comes from distance and day/night propagation and is corrected per
 *     receiver by slot-level learning (scheduled slots where a station was
 *     expected but not heard lower its rate).  Its strength is coverage and
 *     ranking; its weakness is that its confidence is only as honest as its
 *     model of the world, and on an unseen synthetic world its confident
 *     answers were wrong 2.5% of the time, mostly on transmitters not in the
 *     table and on misconfigured receiver positions.
 *
 *  2. rules (navtex_ident_rules.h): transparent, readable factors (letter,
 *     reach, slot, SNR) per candidate, explicit "unlisted service at a known
 *     site" candidates, and an operator gate: it claims >= 0.90 only for an
 *     in-slot, matching-letter, reachable station whose fitted reliability is
 *     >= 0.985.  It is the safest member (0.86% wrong at >= 0.90 on the unseen
 *     world) but covers far less.
 *
 * THE COMBINER
 * ------------
 * Each message falls into one class, and the class decides both the answer and
 * which calibration curve gives its confidence:
 *
 *   AGREE_HIGH   both methods name the same transmitter, AND bayes is at
 *                >= 0.90, AND the rules member's own evidence passes its
 *                structural gate (letter matches, reach >= 0.05, and the burst
 *                the message belongs to started between 15 min before and 35 min
 *                after one of the station's slot starts, table or canonical
 *                schedule: early clocks, long bursts and overruns.  Narrower
 *                windows were measured on holdout and real: no fewer wrong
 *                answers, 6-12% fewer "high" ones.  The result's in_slot and
 *                burst_offset_s report exactly this test) with a fitted
 *                reliability of at least 0.95 (0.985, the rules member's own solo
 *                gate, when the station is beyond 1200 km: skywave-only reception
 *                can deliver an unlisted transmitter from anywhere just as well)
 *                and a "not in the table" posterior of at most 3%, AND the
 *                receiver position is configured, has been checked against what
 *                is heard (100 messages, see below) and is not suspect.
 *                Only this class (and AGREE_HIGH_424) can reach level "high".
 *   AGREE_HIGH_424  on 424 kHz (Japan only) without a receiver position, when the
 *                letter is used by ONE transmitter: agreement + bayes >= 0.90 +
 *                matching letter + in the slot window.
 *                HF (4209.5 kHz) is never "high".  The table's HF list is known to
 *                be incomplete (several administrations run 4209.5 kHz services
 *                that reuse table letters at the canonical slots), HF reaches
 *                around the world at night, and none of the synthetic worlds
 *                contained an unlisted HF transmitter, so the old "unique letter"
 *                rule was never tested: on an adversarial set that plants seven
 *                (test/ident verify_stats adv_gen.py) it was wrong 7-8% of the time
 *                at >= 0.90 (20% without a position).
 *   AGREE        both name the same transmitter but something above is
 *                missing.  Confidence from a calibration map, capped at 0.895.
 *   AGREE_NO_RX  as AGREE, but the receiver position is unknown.  Without a
 *                position, a co-channel or unlisted transmitter can never be
 *                ruled out, so this never reaches "high".
 *   AGREE_LETTER both name the same transmitter, but B1 is unreadable (anything
 *                outside A..X, the valid NAVTEX range) or is another station's
 *                letter, so the letter was recovered from timing and propagation
 *                alone: capped at 0.5 ("low").
 *   DISAGREE     both attribute, to different transmitters.  The rules
 *                member's answer is reported (it was right more often on the
 *                unseen synthetic world AND on the real Dalgety Bay data),
 *                bayes's answer is the first alternative, and confidence is
 *                capped at 0.5.
 *   BAYES_ONLY   the rules member says "not in the table" but bayes names a
 *                transmitter with confidence >= 0.3: bayes's answer at its
 *                calibrated (never high) confidence.  On 490/518 kHz with a known
 *                position only for a station within 1200 km or on a dark path:
 *                on the real data bayes otherwise named Istanbul (2,900 km, by
 *                day) for the unlisted Oostende 490 B service, right 1 time in 71.
 *   UNLISTED_LOCAL  this receiver hears the letter BY DAY on this MF channel
 *                although no table station using it can be heard here by day
 *                (day reach < 0.2%, on fully daylit paths), so a transmitter missing from the table is
 *                close by; a station that cannot reach here by day is then not
 *                told apart from it at night either: confidence capped at 0.3.
 *                (Real: Oostende 490 B at night had been called Istanbul at up to
 *                0.73.)
 *   TOO_FAR      490/518 kHz answer more than 4000 km away on a daylit path:
 *                not attributed.
 *   UNLISTED     otherwise, when either method concludes "not a transmitter in
 *                the table": not attributed.
 *   RX_POS_SUSPECT  see below: confidence capped at 0.5.
 *
 * The AGREE / AGREE_HIGH confidence is a logistic score of both members'
 * outputs (logit of each confidence, receiver position known, the rules
 * member's unlisted posterior, in-slot-and-letter flag) mapped through a
 * monotone (isotonic) curve per class.  Score weights and curves were fitted
 * on the synthetic train set blended with the real Dalgety Bay messages that
 * carry a text-derived proxy label (single-transmitter labels weighted twice
 * the shared-service ones, which are partly circular), so the real-data
 * calibration figures below are in-sample (an out-of-time split of the real
 * data gave ECE 0.065 against 0.062 in-sample on the same half).  The
 * thresholds of the AGREE_HIGH gate were chosen on train and checked on a
 * held-out world.
 *
 * Learning in observe() never lifts a station the table lists as inactive
 * (both members): its own wrong attributions would otherwise feed back into a
 * higher rate (real: Sydney NS 490 J called "Horta", inactive, ever more
 * confidently).
 *
 * Measured (test/ident harness, NAVTEX_IDENT_VERSION 2), wrong at >= 0.90 /
 * share of all messages that are high AND right:
 *
 *                                 version 2         version 1
 *   synthetic train               0.60% / 24.0%     0.53% / 27.8%
 *   unseen world, holdout         0.33% / 10.7%     0.31% / 11.6%
 *   real Dalgety Bay proxy labels 0 of 8,280 / 26.9%   0 of 8,296 / 26.9%
 *   adversarial sets (seed 5 / 6) 1.13% / 0.93%     2.6% / 2.6%
 *
 * Version 2 lost coverage mostly to HF never being "high" (train has much HF
 * traffic) and to the 100-message position warm-up.  Holdout: seed 90210, used
 * to check design choices.  Version 1 was also run once on two sealed worlds:
 * holdout2 (seed 31337) FAILED the gate at 1.22% before the 1200 km rule above
 * existed; holdout3 (seed 4711) gave 0.62%, but the version without the rule
 * also passes holdout3 (0.76%), so the swing is mostly world-to-world
 * variance, and all three holdouts share the same 79 receivers and 6 of their
 * unlisted transmitters: they test new station errors and traffic, not new
 * geography.  Version 2 was not run on them.
 *
 * What "high" assumes.  The pooled figure hides concentration: on holdout
 * (version 2) a receiver-clustered bootstrap gives a 95% interval of
 * 0.17%-0.52%, three receivers hold 45% of the errors and the worst receiver
 * (Jeju) is at 1.8%.  Nearly all confident errors are transmitters missing
 * from the table: "high" assumes the table is complete near the receiver.
 * An unlisted transmitter 80-200 km away that uses the letter and slot of a
 * listed station 450-1100 km away is taken for that station (adversarial
 * "unl_local": 4.2% wrong at >= 0.90).  Real: of the 8,280 high answers, about
 * 6,000 carry proxy labels that were themselves chosen by B1 and slot timing
 * (partly circular); the ~2,300 that rest on single-transmitter text alone
 * give a 95% upper bound of about 0.13% from 0 errors.
 *
 * RECEIVER POSITION SANITY (rx_pos_suspect)
 * -----------------------------------------
 * A wrongly configured receiver position (lat/lon swapped, the owner's home
 * town left in the config...) makes every distance wrong, and both methods
 * then agree confidently on the wrong station.  So the configured position is
 * checked against what is heard, with two independent signs:
 *
 *   (a) reach likelihood: the rules member keeps, for ~150 candidate positions
 *       (every known transmitter site) and for the configured position, a
 *       decayed log-likelihood of "these stations were decoded at these
 *       times".  Per message, the best site out-scoring the configured
 *       position by >= 0.9 nats is sign (a).
 *   (b) impossible by day: the share of messages whose nearest in-slot
 *       candidate with the decoded letter has reach < 3% from the configured
 *       position on a DAYLIGHT path (MF does not travel far by day).  >= 8% is
 *       sign (b).
 *
 * Until 100 messages have been heard at the configured position nothing is
 * "high" (rx_pos_checked is false: "position not yet verified"); a grossly
 * wrong position used to produce confident wrong answers in that window.
 * Both signs together then flag the position as suspect, and the flag is
 * sticky: both signs must stay below their "off" thresholds ((a) < 0.6, (b) <
 * 5%) for 300 messages in a row to clear it.  Both signs swing with the day
 * (b only builds by day), so the old "either sign below" rule switched the
 * flag on and off ~100 times over the real data with the position set to
 * Istanbul and let 151 wrong "high" answers through; now 0 (Halifax and
 * Valparaiso settings: 0, were 5 and 9).  A configured position that jumps
 * more than 100 km faster than 60 km/h (a configuration change, not a ship)
 * resets the check and the learned per-receiver rates.  Correctly configured
 * receivers that are simply far from any listed transmitter fit (a) badly
 * too, which is why (b) is needed.  The real Dalgety Bay receiver is never
 * flagged.  A position about 1,700 km off (Marseille for Dalgety Bay) is not
 * caught: it fits what is heard nearly as well (5 wrong of 443 "high").
 * While suspect, nothing is "high" and confidence is capped at 0.5; the UI
 * should warn the operator.
 *
 * Inputs are sanitised first: a non-finite or out-of-range position means
 * "position unknown", a non-finite SNR or character-error figure means "not
 * measured", and lower-case B1/B2 are upper-cased (a single NaN position used
 * to poison the learned rates for good).
 *
 * LEVELS (navtex_level_for)
 * -------------------------
 * Only "high" is a contract (wrong < 1%).  Medium and low are approximate:
 * their accuracy moves from world to world by several points either way.
 * Measured accuracy (mean confidence) of each level, version 2:
 *
 *            synthetic train   unseen world (holdout)   real Dalgety Bay proxy
 *   high     99.4% (0.994)     99.7% (0.994)            100%  (0.994)
 *   medium   89.0% (0.855)     83.6% (0.834)            96.1% (0.869)
 *   low      42.5% (0.450)     50.9% (0.429)            51.9% (0.412)
 *
 * Version 1 measured medium at 76.8% (mean confidence 0.832) on holdout2 and
 * 82.0% on holdout3, and within single (channel, letter, day/night) groups of
 * the real data medium can be far worse than its average (night 518 D called
 * Torshavn was 26% right: the Spanish Coruna service shares the slot).  Read
 * medium as "probably", not as a calibrated probability.
 *
 * REPLAY
 * ------
 * State is learned per receiver in observe(), with bounded memory.  The
 * caller serialises calls and feeds messages oldest first: identify() then
 * observe() with that result.  Replaying the stored history the same way at
 * startup reproduces the live state exactly (nothing depends on the wall
 * clock or on call timing).
 */
#pragma once

#include <cmath>
#include <cstring>

#include "navtex_ident_common.h"
#include "navtex_ident_bayes.h"
#include "navtex_ident_rules.h"

/* Stored in each message's sidecar beside the attribution.  Bump whenever a
 * change could alter a result, so stored attributions are recomputed. */
static const int NAVTEX_IDENT_VERSION = 2;

struct NavtexIdentMember {
    const char *name;         /* "bayes" or "rules" */
    int         station;      /* index into NAVTEX_STATIONS, -1 = not attributed */
    double      confidence;   /* that member's own calibrated confidence */
    const char *reason;       /* that member's reason code */
};

struct NavtexIdentResult {
    int         station;        /* index into NAVTEX_STATIONS, or -1 = cannot attribute */
    double      confidence;     /* calibrated P(correct); 0 when station == -1 */
    const char *level;          /* "high" | "medium" | "low" | "none" */
    bool        agree;          /* both members picked the same transmitter */
    bool        rx_pos_suspect; /* configured rx location contradicts what is heard */
    NavtexIdentMember member[2];/* [0] bayes, [1] rules */
    double      distance_km, bearing_deg;  /* receiver -> station; -1 when unknown */
    bool        night_path;     /* path mostly dark (solar elevation along it below ~-3 deg) */
    bool        rx_pos_checked; /* enough messages heard at the configured position to check it */
    bool        in_slot;        /* the burst this message belongs to started within [-15, +35) min
                                   of one of the station's slots (table, or canonical letter slot);
                                   the same test the "high" gate applies (GATE_SLOT_LO/HI) */
    int         slot_offset_s;  /* this message's start relative to the station's nearest table slot start */
    int         burst_offset_s; /* the burst's start relative to the slot in_slot was judged on */
    int         n_candidates;   /* transmitters on this freq with this letter (duplicates merged) */
    char        expected_b1;    /* letter whose nominal table slot is on air at the message start on
                                   this freq near rx, 0 if none (informational, see expected_letter) */
    const char *reason;         /* short machine-readable reason code (see the classes above) */
    int         alt[2];         /* runner-up transmitters (never the answer, never each other) or -1 */
    double      alt_p[2];       /* rough probability of each, on the confidence scale:
                                   the member's own estimate, never above the answer's confidence,
                                   and confidence + alt_p[0] + alt_p[1] <= 1 */
};

/* Level thresholds (see LEVELS in the comment at the top). */
static const double NAVTEX_IDENT_HIGH = 0.90;
static const double NAVTEX_IDENT_MEDIUM = 0.60;   /* see LEVELS above for the measured accuracy */

inline const char *navtex_level_for(double confidence) {
    if (confidence >= NAVTEX_IDENT_HIGH) return "high";
    if (confidence >= NAVTEX_IDENT_MEDIUM) return "medium";
    if (confidence > 0) return "low";
    return "none";
}

namespace navtex_ident_detail {

/* ---- combiner calibration (fitted by the harness; see test/ident) -------- */
/* score z = CAL_BETA . (1, logit bayes conf, logit rules conf, rx position known,
 *                       logit rules P(not in table), rules in slot & letter ok)  */
static const double CAL_BETA[6] = {0.6761, 0.8569, 0.5599, -0.3478, -0.0373, -0.8163};
/* AGREE (not high-eligible): z -> P(correct), then capped at CAP_AGREE */
static const double CAL_A_Z[16] = {-4.980, -0.350, 0.302, 0.795, 1.000, 1.454, 1.714, 1.992, 2.165, 2.349, 2.539, 2.836, 4.274, 4.736, 5.639, 9.020};
static const double CAL_A_P[16] = {0.0000, 0.4611, 0.5698, 0.6484, 0.6578, 0.8107, 0.8301, 0.8505, 0.8610, 0.8943, 0.9221, 0.9546, 0.9872, 0.9948, 0.9966, 1.0000};
/* AGREE_HIGH: z -> P(correct), floored at 0.90, capped at CAP_HIGH */
static const double CAL_H_Z[6] = {4.049, 4.704, 5.061, 6.378, 8.033, 8.789};
static const double CAL_H_P[6] = {0.9867, 0.9920, 0.9934, 0.9963, 0.9998, 1.0000};
/* AGREE_LETTER (B1 unreadable or another station's letter): z -> P(correct), capped at CAP_LETTER */
static const double CAL_L_Z[9] = {-4.685, -0.823, 0.809, 1.674, 2.059, 2.147, 3.149, 4.443, 6.280};
static const double CAL_L_P[9] = {0.0000, 0.4364, 0.4640, 0.6000, 0.6552, 0.7711, 0.7865, 0.8767, 1.0000};
/* DISAGREE: rules confidence -> P(rules is right), capped at CAP_DISAGREE */
static const double CAL_D_X[5] = {0.140, 0.516, 0.589, 0.745, 0.991};
static const double CAL_D_P[5] = {0.0000, 0.3472, 0.3531, 0.3664, 1.0000};
/* BAYES_ONLY (rules says "not in the table"): bayes confidence -> P(bayes is right), capped at CAP_AGREE */
static const double CAL_B_X[6] = {0.300, 0.390, 0.479, 0.512, 0.611, 0.989};
static const double CAL_B_P[6] = {0.0000, 0.0722, 0.0959, 0.1853, 0.3376, 1.0000};

static const double CAP_AGREE = 0.895, CAP_HIGH = 0.995, CAP_DISAGREE = 0.5, CAP_SUSPECT = 0.5, CAP_LETTER = 0.5;
static const double BAYES_ONLY_MIN = 0.3;  /* below this a rules "not in the table" stands */
static const double CONF_FLOOR = 0.01;     /* an attribution is never reported at 0 */

/* AGREE_HIGH gate */
static const double GATE_BAYES = 0.90, GATE_RULES_REL = 0.95, GATE_RULES_UNLISTED = 0.03, GATE_REACH = 0.05;
/* a claim beyond groundwave range (skywave only) needs the rules member's own solo gate */
static const double GATE_FAR_KM = 1200, GATE_FAR_RULES_REL = 0.985;

/* receiver-position check */
static const int    POS_MIN_OBS = 100;
static const double POS_DECAY = 0.995;            /* = the rules member's position-likelihood decay */
static const double POS_LLR_ON = 0.9, POS_LLR_OFF = 0.6;     /* nats per message */
static const double POS_IMP_ON = 0.08, POS_IMP_OFF = 0.05;   /* share impossible by day */
static const double POS_IMP_REACH = 0.03, POS_IMP_NIGHT = 0.2;

/* the "high" gate's slot window: burst start, minutes from slot start */
static const double GATE_SLOT_LO = -15, GATE_SLOT_HI = 35;
/* MF (490/518 kHz) answers this far away are not attributed at all */
static const double MF_NONE_KM = 4000;
/* a position jump bigger than this, faster than POS_JUMP_KMH, resets the position evidence */
static const double POS_JUMP_KM = 100, POS_JUMP_KMH = 60;
/* once suspect, the position must look right for this many messages in a row to clear */
static const int    POS_CLEAR_RUN = 300;
/* a letter heard by day that no listed station could deliver by day (local_unlisted):
 * at least LU_MIN such messages (decayed by LU_DECAY per daylight message of that
 * letter) making up LU_SHARE of them */
static const double LU_NIGHT = 0.05;   /* only fully daylit paths count */
static const double LU_REACH = 0.002, LU_MIN = 3, LU_SHARE = 0.25, LU_DECAY = 0.99, LU_CAP = 0.3;

template <int K>
inline double interp_k(const double (&X)[K], const double (&Y)[K], double x) {
    if (!(x > X[0])) return Y[0];            /* also NaN: the low end */
    for (int k = 1; k < K; k++)
        if (x <= X[k]) return Y[k - 1] + (Y[k] - Y[k - 1]) * (x - X[k - 1]) / (X[k] - X[k - 1]);
    return Y[K - 1];
}
inline double logit_c(double p) {
    if (p < 1e-4) p = 1e-4;
    if (p > 1 - 1e-4) p = 1 - 1e-4;
    return std::log(p / (1 - p));
}
inline bool is_letter(char c) { return c >= 'A' && c <= 'X'; }   /* the valid NAVTEX B1 range, as the members use it */
inline bool is_mf(long f) { return std::labs(f - 518000) <= 500 || std::labs(f - 490000) <= 500; }

}  // namespace navtex_ident_detail

class NavtexIdentifier {
public:
    NavtexIdentifier() : imp_num_(0), imp_den_(0), suspect_(false), clear_run_(0), pend_valid_(false), pend_reach_(-1), pend_night_(0),
                         have_pos_(false), pos_lat_(0), pos_lon_(0), pos_t_(0), pend_lu_c_(-1), pend_lu_l_(0), pend_lu_x_(0) {
        reset_local_unlisted();
    }

    NavtexIdentResult identify(const NavtexIdentQuery &q0) {
        using namespace navtex_ident_detail;
        const NavtexIdentQuery q = sanitize(q0);
        note_position(q);
        NavtexIdentResult r;
        std::memset(&r, 0, sizeof(r));
        r.station = -1; r.alt[0] = r.alt[1] = -1; r.distance_km = r.bearing_deg = -1;
        r.level = "none"; r.reason = "NO_CHANNEL";

        MemberResult rb = bayes_.identify(q);
        MemberResult rr = rules_.identify(q);
        const navtex_ident_rules::RCFeatures &F = rules_.last;
        r.member[0].name = "bayes"; r.member[0].station = rb.station; r.member[0].confidence = rb.confidence;
        r.member[0].reason = rb.station < 0 ? "NOT_IN_TABLE" : (rb.confidence >= 0.9 ? "POSTERIOR_HIGH" : "POSTERIOR");
        r.member[1].name = "rules"; r.member[1].station = rr.station; r.member[1].confidence = rr.confidence;
        r.member[1].reason = F.reason ? F.reason : "NONE";
        pend_valid_ = q.rx_valid; pend_reach_ = F.cfg_reach; pend_night_ = F.cfg_night;
        pend_lu_c_ = -1;
        if (q.rx_valid && is_mf(q.freq_hz) && is_letter(q.b1) && F.cfg_reach >= 0 && F.cfg_night < LU_NIGHT) {
            pend_lu_c_ = std::labs(q.freq_hz - 518000) <= 500 ? 0 : 1; pend_lu_l_ = q.b1 - 'A';
            pend_lu_x_ = F.cfg_reach < LU_REACH ? 1.0 : 0.0;
        }

        bool checked = update_suspect(q);
        r.rx_pos_suspect = suspect_;
        r.rx_pos_checked = q.rx_valid && checked;
        r.n_candidates = count_candidates(q.freq_hz, q.b1);
        r.expected_b1 = expected_letter(q);

        const bool mf = is_mf(q.freq_hz);
        bool agree = rb.station >= 0 && same_transmitter(rb.station, rr.station);
        r.agree = agree;
        int st = -1;
        double conf = 0;
        /* candidate runner-ups, best first; pick_alts() drops the answer and duplicates */
        int ca[5] = {-1, -1, -1, -1, -1};
        double cp[5] = {0, 0, 0, 0, 0};
        /* bayes alone, against the rules member's "not in the table": on MF only for a
         * station a groundwave/short-skywave path could deliver (the unlisted national
         * 490 kHz services are otherwise handed to whatever far station shares the letter) */
        bool bayes_only_ok = rb.station >= 0 && rr.station < 0 && rb.confidence >= BAYES_ONLY_MIN &&
                             (!mf || !q.rx_valid || dist_to(q, rb.station) <= GATE_FAR_KM || night_to(q, rb.station));
        if (bayes_only_ok) {
            st = rb.station;
            conf = interp_k(CAL_B_X, CAL_B_P, rb.confidence);
            if (conf > CAP_AGREE) conf = CAP_AGREE;
            r.reason = "BAYES_ONLY";
            ca[0] = rb.alt[0]; cp[0] = rb.alt_p[0]; ca[1] = rb.alt[1]; cp[1] = rb.alt_p[1];
        } else if (rb.station < 0 || rr.station < 0) {
            r.reason = (rb.station < 0 && rr.station < 0 && F.reason && std::strcmp(F.reason, "NONE") == 0) ? "NO_CANDIDATE" : "UNLISTED";
        } else if (!agree) {
            st = rr.station;
            conf = interp_k(CAL_D_X, CAL_D_P, rr.confidence);
            if (conf > CAP_DISAGREE) conf = CAP_DISAGREE;
            r.reason = "DISAGREE";
            ca[0] = rb.station; cp[0] = rb.confidence;
            ca[1] = rr.alt[0]; cp[1] = rr.alt_p[0]; ca[2] = rr.alt[1]; cp[2] = rr.alt_p[1];
            ca[3] = rb.alt[0]; cp[3] = rb.alt_p[0];
        } else {
            st = rr.station;
            double z = CAL_BETA[0] + CAL_BETA[1] * logit_c(rb.confidence) + CAL_BETA[2] * logit_c(rr.confidence)
                     + CAL_BETA[3] * (q.rx_valid ? 1 : 0) + CAL_BETA[4] * logit_c(F.p_unk)
                     + CAL_BETA[5] * ((F.in_slot && F.letter_ok) ? 1 : 0);
            bool hf = std::labs(q.freq_hz - 4209500) <= 500;
            bool j424 = std::labs(q.freq_hz - 424000) <= 500;
            bool slot_ok = F.delta_top >= GATE_SLOT_LO && F.delta_top < GATE_SLOT_HI;
            /* HF: never "high" (see AGREE_HIGH_424 above: the HF table is known to be incomplete) */
            bool gate = !hf && (slot_ok && F.letter_ok && F.heard_top >= GATE_REACH &&
                              F.conf_cal >= GATE_RULES_REL && F.p_unk <= GATE_RULES_UNLISTED &&
                              !(F.dist_top > GATE_FAR_KM && F.conf_cal < GATE_FAR_RULES_REL));
            bool high = q.rx_valid && checked && !suspect_ && rb.confidence >= GATE_BAYES && gate;
            /* 424 kHz (Japan only) without a receiver position: when the letter is used by a
             * single transmitter, the position is not needed to tell the stations apart */
            if (!q.rx_valid && j424 && r.n_candidates == 1 && F.letter_ok && slot_ok && rb.confidence >= GATE_BAYES) high = true;
            if (!F.letter_ok) {
                conf = interp_k(CAL_L_Z, CAL_L_P, z);
                if (conf > CAP_LETTER) conf = CAP_LETTER;
                r.reason = "AGREE_LETTER";
            } else if (high) {
                conf = interp_k(CAL_H_Z, CAL_H_P, z);
                if (conf < NAVTEX_IDENT_HIGH) conf = NAVTEX_IDENT_HIGH;
                if (conf > CAP_HIGH) conf = CAP_HIGH;
                r.reason = j424 && !q.rx_valid ? "AGREE_HIGH_424" : "AGREE_HIGH";
            } else {
                conf = interp_k(CAL_A_Z, CAL_A_P, z);
                if (conf > CAP_AGREE) conf = CAP_AGREE;
                r.reason = q.rx_valid ? "AGREE" : "AGREE_NO_RX";
            }
            ca[0] = rr.alt[0]; cp[0] = rr.alt_p[0]; ca[1] = rr.alt[1]; cp[1] = rr.alt_p[1];
            ca[2] = rb.alt[0]; cp[2] = rb.alt_p[0]; ca[3] = rb.alt[1]; cp[3] = rb.alt_p[1];
        }
        /* A letter this receiver hears BY DAY although no table station using it can be
         * heard here by day is served by a transmitter missing from the table, close by.
         * By night a far table station with that letter cannot be told apart from it. */
        if (st >= 0 && mf && q.rx_valid && is_letter(q.b1) && NAVTEX_STATIONS[st].id == q.b1 &&
            local_unlisted(q.freq_hz, q.b1) && !day_reachable(q, st)) {
            if (conf > LU_CAP) { conf = LU_CAP; r.reason = "UNLISTED_LOCAL"; }
        }
        /* MF does not carry NAVTEX this far, by day or night: not attributed */
                if (st >= 0 && mf && q.rx_valid && dist_to(q, st) > MF_NONE_KM && !night_to(q, st)) {
            st = -1; conf = 0; r.reason = "TOO_FAR";
        }
        if (st >= 0) {
            if (!(conf >= CONF_FLOOR)) conf = CONF_FLOOR;
            if (suspect_ && conf > CAP_SUSPECT) { conf = CAP_SUSPECT; r.reason = "RX_POS_SUSPECT"; }
        }
        r.station = st;
        r.confidence = st >= 0 ? conf : 0.0;
        r.level = navtex_level_for(r.confidence);
        if (st >= 0) {
            pick_alts(st, r.confidence, ca, cp, r);
            describe(q, st, F.burst_start, r);
        }
        return r;
    }

    void observe(const NavtexIdentQuery &q0, const NavtexIdentResult &) {
        using namespace navtex_ident_detail;
        const NavtexIdentQuery q = sanitize(q0);
        bayes_.observe(q);
        rules_.observe(q);
        if (pend_valid_ && q.rx_valid && pend_reach_ >= 0) {
            double x = (pend_reach_ < POS_IMP_REACH && pend_night_ < POS_IMP_NIGHT) ? 1.0 : 0.0;
            imp_num_ = imp_num_ * POS_DECAY + x;
            imp_den_ = imp_den_ * POS_DECAY + 1.0;
        }
        pend_valid_ = false; pend_reach_ = -1;
        if (pend_lu_c_ >= 0 && q.rx_valid) {
            double &a = lu_imp_[pend_lu_c_][pend_lu_l_], &b = lu_n_[pend_lu_c_][pend_lu_l_];
            a = a * LU_DECAY + pend_lu_x_; b = b * LU_DECAY + 1.0;
        }
        pend_lu_c_ = -1;
    }

    /* Inputs the identifier cannot use are treated as missing: a non-finite or
     * out-of-range receiver position means "position unknown", a non-finite SNR or
     * character-error figure means "not measured", and B1/B2 are upper-cased. */
    static NavtexIdentQuery sanitize(const NavtexIdentQuery &in) {
        NavtexIdentQuery q = in;
        if (q.rx_valid && !(std::isfinite(q.rx_lat) && std::isfinite(q.rx_lon) &&
                            std::fabs(q.rx_lat) <= 90 && std::fabs(q.rx_lon) <= 180)) {
            q.rx_valid = false; q.rx_lat = q.rx_lon = 0;
        }
        if (q.has_snr && !std::isfinite(q.snr_db)) { q.has_snr = false; q.snr_db = 0; }
        if (q.has_chars && !std::isfinite(q.chars_failed_pct)) { q.has_chars = false; q.chars_failed_pct = 0; }
        if (q.b1 >= 'a' && q.b1 <= 'z') q.b1 = (char)(q.b1 - 'a' + 'A');
        if (q.b2 >= 'a' && q.b2 <= 'z') q.b2 = (char)(q.b2 - 'a' + 'A');
        return q;
    }

private:
    /* Copyable (members hold only values and std::vectors): the server
     * identifies an in-progress message on a copy, so a provisional answer
     * never touches the learned state.  A copy is ~5 MB, under 1 ms. */

    navtex_ident_bayes::BayesPhysical bayes_;
    navtex_ident_rules::RulesCalibrated rules_;
    double imp_num_, imp_den_;
    bool suspect_;
    int clear_run_;
    bool pend_valid_;
    double pend_reach_, pend_night_;
    bool have_pos_;
    double pos_lat_, pos_lon_;
    int64_t pos_t_;
    /* per MF channel and letter: decayed counts of daylight-path messages, and of those no
     * table station with that letter could deliver by day (see local_unlisted) */
    double lu_imp_[2][24], lu_n_[2][24];
    int pend_lu_c_, pend_lu_l_; double pend_lu_x_;

    void reset_local_unlisted() {
        for (int c = 0; c < 2; c++) for (int l = 0; l < 24; l++) lu_imp_[c][l] = lu_n_[c][l] = 0;
    }
    bool local_unlisted(long f, char b1) const {
        using namespace navtex_ident_detail;
        int c = std::labs(f - 518000) <= 500 ? 0 : 1, l = b1 - 'A';
        return lu_imp_[c][l] >= LU_MIN && lu_imp_[c][l] >= LU_SHARE * lu_n_[c][l];
    }
    static bool day_reachable(const NavtexIdentQuery &q, int st) {
        using namespace navtex_ident_detail;
        return navtex_ident_rules::rc::p_heard(dist_to(q, st), 0.0, NAVTEX_STATIONS[st].range_nm) >= LU_REACH;
    }

    /* path to station st mostly dark (the rules member's night weight >= 0.5) */
    static bool night_to(const NavtexIdentQuery &q, int st) {
        const NavtexStation &s = NAVTEX_STATIONS[st];
        return navtex_ident_rules::rc::path_night(q.start_utc, q.rx_lat, q.rx_lon, s.lat, s.lon, dist_to(q, st)) >= 0.5;
    }
    static double dist_to(const NavtexIdentQuery &q, int st) {
        const NavtexStation &s = NAVTEX_STATIONS[st];
        return navtex_ident_detail::gc_km(q.rx_lat, q.rx_lon, s.lat, s.lon);
    }

    /* A configured position that jumps (further than POS_JUMP_KM, faster than a ship
     * could move) is a configuration change: what was learned about the old position
     * no longer applies, so the position check and the learned rates start again. */
    void note_position(const NavtexIdentQuery &q) {
        using namespace navtex_ident_detail;
        if (!q.rx_valid) return;
        if (have_pos_) {
            double d = gc_km(pos_lat_, pos_lon_, q.rx_lat, q.rx_lon);
            double hours = (double)(q.start_utc - pos_t_) / 3600.0;
            if (hours < 0) hours = 0;
            if (d > POS_JUMP_KM && d > POS_JUMP_KMH * hours) {
                suspect_ = false; clear_run_ = 0; imp_num_ = imp_den_ = 0;
                rules_.reset_position_evidence();
                reset_local_unlisted();
                bayes_.reset_learning();
            }
        }
        have_pos_ = true; pos_lat_ = q.rx_lat; pos_lon_ = q.rx_lon; pos_t_ = q.start_utc;
    }

    /* Returns true once the configured position has been checked (POS_MIN_OBS messages).
     * The flag is sticky: raised when both signs agree, cleared only after POS_CLEAR_RUN
     * messages in a row on which both signs are below their "off" thresholds (or when the
     * configured position changes, see note_position). */
    bool update_suspect(const NavtexIdentQuery &q) {
        using namespace navtex_ident_detail;
        if (!q.rx_valid) return false;
        int n = 0; double bl, bo;
        double llr = rules_.pos_llr_reach(n, bl, bo);
        if (n < POS_MIN_OBS || imp_den_ <= 0) return false;
        double W = (1 - std::pow(POS_DECAY, (double)n)) / (1 - POS_DECAY);
        double per = llr / W, imp = imp_num_ / imp_den_;
        if (!suspect_) {
            if (per >= POS_LLR_ON && imp >= POS_IMP_ON) { suspect_ = true; clear_run_ = 0; }
        } else if (per < POS_LLR_OFF && imp < POS_IMP_OFF) {
            if (++clear_run_ >= POS_CLEAR_RUN) { suspect_ = false; clear_run_ = 0; }
        } else clear_run_ = 0;
        return true;
    }

    /* alternatives: the first two candidates that are neither the answer nor each other,
     * probabilities on the confidence scale (see NavtexIdentResult::alt_p) */
    static void pick_alts(int st, double conf, const int *ca, const double *cp, NavtexIdentResult &r) {
        using navtex_ident_detail::same_transmitter;
        double left = 1 - conf;
        int n = 0;
        for (int k = 0; k < 5 && n < 2; k++) {
            int a = ca[k];
            if (a < 0 || same_transmitter(a, st) || (n == 1 && same_transmitter(a, r.alt[0]))) continue;
            double p = cp[k];
            if (!(p >= 0)) p = 0;
            if (p > conf) p = conf;
            if (p > left) p = left;
            r.alt[n] = a; r.alt_p[n] = p; left -= p; n++;
        }
    }

    static int count_candidates(long f, char b1) {
        using namespace navtex_ident_detail;
        if (!is_letter(b1)) return 0;
        int n = 0;
        for (int i = 0; i < NAVTEX_STATION_COUNT; i++) {
            const NavtexStation &s = NAVTEX_STATIONS[i];
            if (std::labs((long)s.freq_hz - f) > 500 || s.id != b1) continue;
            bool dup = false;
            for (int j = 0; j < i && !dup; j++) dup = same_transmitter(i, j);
            if (!dup) n++;
        }
        return n;
    }

    /* schedule of station i (table, or the canonical (letter-A)*10 min + k*4 h when empty) */
    static int schedule(int i, short *mins) {
        const NavtexStation &s = NAVTEX_STATIONS[i];
        if (s.n_minutes > 0) {
            for (int k = 0; k < s.n_minutes; k++) mins[k] = s.minutes[k];
            return s.n_minutes;
        }
        int base = ((s.id - 'A') * 10) % 240;
        for (int k = 0; k < 6; k++) mins[k] = (short)(base + 240 * k);
        return 6;
    }
    /* seconds from the nearest scheduled slot start to t, in [-12 h, +12 h) */
    static int slot_offset(int i, int64_t t) {
        short mins[12];
        int n = schedule(i, mins);
        long sod = (long)(((t % 86400) + 86400) % 86400);
        long best = 1L << 30;
        for (int k = 0; k < n; k++) {
            long d = sod - (long)mins[k] * 60;
            while (d < -43200) d += 86400;
            while (d >= 43200) d -= 86400;
            if (std::labs(d) < std::labs(best)) best = d;
        }
        return (int)best;
    }
    static double minute_of_day(int64_t t) { return (double)(((t % 86400) + 86400) % 86400) / 60.0; }
    /* burst start of station i relative to its slot, minutes (the gate's measure), 1e9 if in no window */
    static double burst_delta(int i, int64_t burst_start) {
        return navtex_ident_rules::rc::slot_delta(NAVTEX_STATIONS[i], minute_of_day(burst_start));
    }

    /* The letter whose nominal slot [start, start + 10 min) of the table schedule is on
     * air at the message start, on this channel near the receiver: of those stations,
     * the one most likely heard here (reach), or any one if the position is unknown.
     * Informational: a "high" answer can name another letter when the message is a
     * burst continuation or its station starts early or overruns (see in_slot). */
    static char expected_letter(const NavtexIdentQuery &q) {
        using namespace navtex_ident_rules::rc;
        char best = 0; double bw = 0;
        for (int i = 0; i < NAVTEX_STATION_COUNT; i++) {
            const NavtexStation &s = NAVTEX_STATIONS[i];
            if (std::labs((long)s.freq_hz - q.freq_hz) > 500) continue;
            int off = slot_offset(i, q.start_utc);
            if (off < 0 || off >= 600) continue;
            double w = 1e-6;
            if (q.rx_valid) {
                double d = navtex_ident_detail::gc_km(q.rx_lat, q.rx_lon, s.lat, s.lon);
                double nw = path_night(q.start_utc, q.rx_lat, q.rx_lon, s.lat, s.lon, d);
                w = p_heard(d, nw, s.range_nm);
            }
            if (w > bw) { bw = w; best = s.id; }
        }
        return best;
    }

    static void describe(const NavtexIdentQuery &q, int st, int64_t burst_start, NavtexIdentResult &r) {
        using navtex_ident_detail::GATE_SLOT_LO;
        using navtex_ident_detail::GATE_SLOT_HI;
        const NavtexStation &s = NAVTEX_STATIONS[st];
        r.slot_offset_s = slot_offset(st, q.start_utc);
        double dl = burst_delta(st, burst_start);
        r.in_slot = dl >= GATE_SLOT_LO && dl < GATE_SLOT_HI;
        r.burst_offset_s = dl < 1e8 ? (int)std::floor(dl * 60 + 0.5) : slot_offset(st, burst_start);
        if (q.rx_valid) {
            r.distance_km = navtex_great_circle_km(q.rx_lat, q.rx_lon, s.lat, s.lon);
            r.bearing_deg = navtex_initial_bearing_deg(q.rx_lat, q.rx_lon, s.lat, s.lon);
            /* the rules member's path-darkness weight: 0 when the sun is above +2 deg
             * anywhere along the path, 1 below -8 deg */
            r.night_path = navtex_ident_rules::rc::path_night(q.start_utc, q.rx_lat, q.rx_lon, s.lat, s.lon,
                                                              r.distance_km) >= 0.5;
        }
    }
};
