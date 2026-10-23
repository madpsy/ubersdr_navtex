// navtex_ident_rules.h — member 2 of the identification ensemble (src/navtex_ident.h).
// Include navtex_ident.h, not this file.
//
// This is the "rules_calibrated" variant of the test/ident harness.  Changes made for
// production, each measured in the harness:
//   * B1 coordination (grafted from the letter_recovery variant): an unlisted service
//     at a known site is not considered for letter L when a listed L on the same
//     channel is within 700 km (administrations coordinate B1 letters).  Ensemble
//     score on the unseen synthetic world -0.218 -> -0.211, real proxy 0.082 -> 0.103,
//     no new confident errors.
//   * Position check: with a configured receiver position the reach-only position
//     likelihood over the known transmitter sites is still accumulated, together with
//     that of the configured position (pos_llr_reach()), and the reach / path darkness
//     of the best in-slot candidate is exposed (cfg_reach, cfg_night); the ensemble
//     uses both to flag a wrongly configured position.
//   * Speed: the sun position is computed once per message and the sample points of
//     each path once per geometry (identical results), 64 -> 36 us per message.
//   * The fitted reliability before the operator gate is exposed (conf_cal).
//
// rules_calibrated: a transparent, rule/feature-based NAVTEX transmitter identifier.
//
// 1. EVIDENCE.  For every table station on the channel four plain-language factors are
//    multiplied (each one a number an operator can print and check):
//      letter  does the decoded B1 equal the station's letter?  (header corruption is
//              ~0.5% overall, ~2% below 0 dB SNR, so a mismatch costs ~1/1000)
//      reach   how often is a station heard at this distance, day vs night?  (curve from
//              the Dalgety Bay per-slot reception table; "night" from the solar elevation
//              along the path; day reach scaled by the stated range, floored at 200 nm)
//      slot    does the START OF THE BURST this message belongs to fall in one of the
//              station's 10-minute slots?  (table schedule mixed 85/15 with the canonical
//              (letter-A)*10 min schedule, because the table has known errors; early
//              starts and overruns get reduced density; the off-slot share depends on
//              the subject: gale warnings B / UK V are sent on receipt)
//      snr     is the SNR consistent with the distance law 50.1 - 15 log10 km (from the
//              real logs), conditioned on being above the -3 dB decode floor?
//    plus co-channel capture (a same-letter station >10 dB stronger usually masks a weak
//    one) and a "not in the table" pseudo-candidate: a generic remote transmitter, and
//    an unlisted service at any known transmitter site (the real logs show Oostende,
//    Malin Head, Split, Odesa 490 kHz services missing from the table).
// 2. PER-RECEIVER LEARNING (observe(), bounded memory, never sees truth):
//      - per (channel, letter) a decayed sequential log-likelihood ratio "unlisted vs
//        table"; a letter that keeps being unexplained gets a larger unlisted prior
//        (x1..x30, never below the base prior);
//      - per station a decayed soft count, redistributing weight among the stations
//        that share a letter (never between "table" and "unlisted");
//      - when the receiver position is unknown, a decayed likelihood over candidate
//        positions (the known transmitter sites) learned from what it hears.
// 3. CALIBRATION.  The normalised product is a raw posterior.  Each decision falls into
//    a rule class (in slot / no position / off slot / letter mismatch) whose small
//    logistic score + monotone (isotonic) map, fitted on synthetic train data, gives
//    the confidence.
// 4. OPERATOR GATE.  A confidence >= 0.90 is only reported for an in-slot,
//    matching-letter, reachable station at a known (or well-learned) position whose
//    fitted reliability is >= HI_MIN; otherwise it is reported as 0.89.  Abstain
//    (station -1) only when "not in the table" has posterior > ABSTAIN_PUNK.
// Every decision carries a reason code in last.reason, e.g. "SLOT_RANKED", "OFFSLOT",
// "SLOT_RANKED:CAP" (would have been >= 0.9 but a gate rule failed), "UNLISTED".
#pragma once
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "navtex_ident_common.h"

namespace navtex_ident_rules {

using navtex_ident_detail::MemberResult;

namespace rc {

static const double PI = 3.14159265358979323846;
static const int NSTA = (int)(sizeof(NAVTEX_STATIONS) / sizeof(NAVTEX_STATIONS[0]));

inline double dist_km(double la1, double lo1, double la2, double lo2) {
    const double k = PI / 180.0;
    double a = std::sin((la2 - la1) * k / 2), b = std::sin((lo2 - lo1) * k / 2);
    double h = a * a + std::cos(la1 * k) * std::cos(la2 * k) * b * b;
    return 2 * 6371.0 * std::asin(std::sqrt(h < 1 ? h : 1));
}

// point at fraction f along the great circle
inline void gc_point(double la1, double lo1, double la2, double lo2, double f, double &la, double &lo) {
    const double k = PI / 180.0;
    double p1 = la1 * k, l1 = lo1 * k, p2 = la2 * k, l2 = lo2 * k;
    double x1 = std::cos(p1) * std::cos(l1), y1 = std::cos(p1) * std::sin(l1), z1 = std::sin(p1);
    double x2 = std::cos(p2) * std::cos(l2), y2 = std::cos(p2) * std::sin(l2), z2 = std::sin(p2);
    double dot = x1 * x2 + y1 * y2 + z1 * z2;
    if (dot > 1) dot = 1;
    if (dot < -1) dot = -1;
    double om = std::acos(dot);
    double a, b;
    if (om < 1e-9) { a = 1 - f; b = f; }
    else { a = std::sin((1 - f) * om) / std::sin(om); b = std::sin(f * om) / std::sin(om); }
    double x = a * x1 + b * x2, y = a * y1 + b * y2, z = a * z1 + b * z2;
    la = std::atan2(z, std::sqrt(x * x + y * y)) / k;
    lo = std::atan2(y, x) / k;
}

// sun position at time t (low-precision almanac), so that many elevations at one
// moment cost one cos + one asin each
struct SunPos { double sindec, cosdec, ra, gmst; };
inline SunPos sun_at(int64_t t) {
    const double k = PI / 180.0;
    double days = (double)t / 86400.0 - 10957.5;            // since J2000.0
    double g = std::fmod(357.529 + 0.98560028 * days, 360.0) * k;
    double q = std::fmod(280.459 + 0.98564736 * days, 360.0);
    double L = (q + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g)) * k;
    double e = (23.439 - 0.00000036 * days) * k;
    double dec = std::asin(std::sin(e) * std::sin(L));
    SunPos s;
    s.sindec = std::sin(dec); s.cosdec = std::cos(dec);
    s.ra = std::atan2(std::cos(e) * std::sin(L), std::cos(L)) / k;
    s.gmst = std::fmod(280.46061837 + 360.98564736629 * days, 360.0);
    return s;
}
inline double sun_elev(const SunPos &s, double sinlat, double coslat, double lon) {
    const double k = PI / 180.0;
    double ha = (s.gmst + lon - s.ra) * k;
    return std::asin(sinlat * s.sindec + coslat * s.cosdec * std::cos(ha)) / k;
}
// solar elevation (degrees), low-precision (good to ~1 deg)
inline double solar_elev(int64_t t, double lat, double lon) {
    const double k = PI / 180.0;
    return sun_elev(sun_at(t), std::sin(lat * k), std::cos(lat * k), lon);
}

// piecewise-linear interpolation in log10(distance)
inline double interp_logd(const double *xs, const double *ys, int n, double d) {
    double x = std::log10(d < 1 ? 1 : d);
    if (x <= xs[0]) return ys[0];
    for (int i = 1; i < n; i++)
        if (x <= xs[i]) { double f = (x - xs[i - 1]) / (xs[i] - xs[i - 1]); return ys[i - 1] + f * (ys[i] - ys[i - 1]); }
    return ys[n - 1];
}

// P(a scheduled message is decoded) vs distance; log10 km knots.  From the
// Dalgety Bay per-slot reception table (calib), smoothed and floored.
inline double p_heard(double d_km, double night, int range_nm) {
    static const double X[] = {1.0, 2.477, 2.699, 2.845, 3.0, 3.204, 3.301, 3.477, 3.699, 3.845, 4.0, 4.301};
    //                           10    300    500    700   1000  1600   2000   3000   5000   7000  10000 20000
    static const double DAY[]   = {0.92, 0.90, 0.55, 0.32, 0.25, 0.03, 0.004, 0.0008, 0.0004, 0.0003, 0.0002, 0.0001};
    static const double NIGHT[] = {0.90, 0.87, 0.80, 0.77, 0.72, 0.50, 0.40, 0.18, 0.15, 0.05, 0.01, 0.002};
    // groundwave (day) reach scales with the stated range, floored: real "55 nm"
    // Oostende is heard at 700 km.
    double R = (range_nm > 0 ? range_nm : 300) * 1.852;
    if (R < 370) R = 370;
    if (R > 1000) R = 1000;
    double dday = d_km * (556.0 / R);
    double pd = interp_logd(X, DAY, 12, dday);
    double pn = interp_logd(X, NIGHT, 12, d_km);
    double p = night * pn + (1 - night) * pd;
    return p > pd ? p : pd;
}

// night weight of a path: 1 when the whole path is dark (sun < -8 deg), 0 in daylight
inline double path_night(int64_t t, double la1, double lo1, double la2, double lo2, double d) {
    double emax = -90;
    int n = d > 1500 ? 3 : 1;
    for (int i = 0; i < n; i++) {
        double f = n == 1 ? 0.5 : 0.25 * (i + 1), la, lo;
        gc_point(la1, lo1, la2, lo2, f, la, lo);
        double e = solar_elev(t, la, lo);
        if (e > emax) emax = e;
    }
    double w = (2.0 - emax) / 10.0;          // +2 deg -> 0, -8 deg -> 1
    return w < 0 ? 0 : (w > 1 ? 1 : w);
}

// time density (per minute) for an in-slot message, delta = minutes after slot start
inline double slot_shape(double delta) {
    if (delta >= -1.5 && delta < 10.0) return 0.80 / 11.5;
    if (delta >= -15.0 && delta < -1.5) return 0.10 / 13.5;
    if (delta >= 10.0 && delta < 35.0) return 0.10 / 25.0;
    return 0.0;
}

inline double wrap_min(double x) {                         // -> [-720, 720)
    x = std::fmod(x, 1440.0);
    if (x < -720) x += 1440;
    if (x >= 720) x -= 1440;
    return x;
}

// density over minute-of-day given a slot list; also returns best |delta| info
inline double sched_density(const short *mins, int n, double tmin, double &best_delta) {
    double s = 0; best_delta = 1e9;
    for (int i = 0; i < n; i++) {
        double dl = wrap_min(tmin - mins[i]);
        s += slot_shape(dl);
        if (dl >= -15 && dl < 35 && std::fabs(dl - 4) < std::fabs(best_delta - 4)) best_delta = dl;
    }
    return n > 0 ? s / n : 0.0;
}

inline double canon_density(char letter, double tmin, double &best_delta) {
    short m[6]; int base = (letter - 'A') * 10;
    for (int k = 0; k < 6; k++) m[k] = (short)(base + 240 * k);
    return sched_density(m, 6, tmin, best_delta);
}

inline bool is_letter(char c) { return c >= 'A' && c <= 'X'; }

// Minutes from station s's slot start to tmin (minutes of day), as the rules use it:
// the table schedule or the canonical (letter-A)*10 + k*4 h one, whichever puts tmin
// nearer to +4 min into a slot; 1e9 when tmin is in no slot window [-15, +35).
inline double slot_delta(const NavtexStation &s, double tmin) {
    double bd1 = 1e9, bd2 = 1e9;
    if (is_letter(s.id)) canon_density(s.id, tmin, bd2);
    if (s.n_minutes > 0) sched_density(s.minutes, s.n_minutes, tmin, bd1);
    return std::fabs(bd1 - 4) < std::fabs(bd2 - 4) ? bd1 : bd2;
}

inline double offslot_rate(char b2) {
    switch (b2) {
        case 'B': return 0.30;   // gale warnings on receipt
        case 'V': return 0.25;   // UK 490 actuals / special
        case 'A': case 'D': case 'L': return 0.05;
        default:  return 0.05;
    }
}

} // namespace rc

static const double PRIOR_UNK = 0.03;
static const double PRIOR_UNK_NOLOC = 0.10;
// classes: 0 in slot, 1 no receiver position, 2 off slot, 3 letter mismatch
static const double HI_MIN = 0.985;
// BEGIN FITTED (fit3.py)
static const double CALW[4][8] = {
    { -0.8508, 0.7127, -0.1592, -0.1905, -0.2869, -0.1509, 0.3200, 0.0000 },
    { -0.4431, 0.5239, 0.0418, -0.1196, -0.0788, 0.0000, 0.0241, 0.6318 },
    { 0.4508, 0.7864, 0.5999, -0.2871, 1.4930, -0.0270, 2.1985, 0.0000 },
    { 0.8320, 0.8470, 0.2232, -0.1232, 1.2295, 0.3114, 0.0000, -0.5555 } };
static const double ISO_Z[4][24] = {
    { -0.126, 0.354, 0.714, 1.084, 1.433, 1.755, 2.056, 2.360, 2.644, 2.919, 3.179, 3.418, 3.646, 3.877, 4.120, 4.415, 4.746, 5.164, 5.628, 6.207, 6.934, 8.146, 8.967, 9.455 },
    { -0.206, 0.138, 0.313, 0.540, 0.793, 1.089, 1.380, 1.612, 1.837, 2.057, 2.290, 2.518, 2.739, 2.949, 3.136, 3.326, 3.623, 3.879, 4.107, 4.493, 4.808, 5.028, 5.204, 5.341 },
    { -2.005, -1.051, -0.656, -0.365, -0.093, 0.168, 0.427, 0.658, 0.896, 1.116, 1.339, 1.561, 1.825, 2.086, 2.318, 2.593, 2.847, 3.079, 3.236, 3.236, 3.236, 3.236, 3.236, 3.236 },
    { -0.198, -0.198, -0.198, -0.198, -0.198, -0.198, -0.198, -0.198, -0.198, -0.198, -0.198, -0.198, -0.198, -0.198, -0.198, -0.198, -0.198, -0.198, -0.198, -0.198, -0.198, -0.198, -0.198, -0.198 } };
static const double ISO_P[4][24] = {
    { 0.4447, 0.5439, 0.6731, 0.7503, 0.8489, 0.9039, 0.9258, 0.9271, 0.9271, 0.9322, 0.9498, 0.9586, 0.9586, 0.9586, 0.9652, 0.9888, 0.9907, 0.9907, 0.9995, 0.9995, 0.9998, 0.9999, 0.9999, 0.9999 },
    { 0.4091, 0.5946, 0.5946, 0.6314, 0.6314, 0.7428, 0.7527, 0.8400, 0.9239, 0.9239, 0.9239, 0.9239, 0.9239, 0.9239, 0.9239, 0.9239, 0.9923, 0.9992, 0.9992, 0.9992, 0.9992, 0.9992, 0.9992, 0.9992 },
    { 0.1404, 0.3475, 0.3475, 0.3604, 0.4309, 0.4985, 0.5188, 0.6737, 0.7720, 0.7975, 0.7975, 0.8153, 0.8153, 0.8532, 0.9122, 0.9493, 0.9725, 0.9725, 0.9725, 0.9725, 0.9725, 0.9725, 0.9725, 0.9725 },
    { 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597, 0.4597 } };
// END FITTED

static const double SNR_FLOOR = -3.0;
static const double CAPTURE_DB = 10.0;
static const double CAPTURE_P = 0.5;
static const double ABSTAIN_PUNK = 0.93;
static const double SNR_OUTLIER = 0.04;   // share of readings far off the distance law (anomalous propagation)
static const double POS_DECAY = 0.995;
static const double LP_GAIN = 0.5;
static const double LP_DECAY = 0.995;
static const int POS_MIN_OBS = 20;
static const double POS_HI_CONC = 0.8;
static const double LEARN_GAIN = 0.5;
static const double LEARN_MAXLOG = 3.4;     // at most x30
static const double LEARN_DECAY = 0.98;
static const double LEARN_MINLOG = 0.0;
static const double PU_SAME = 0.03;
static const double PU_OTHER = 0.12;
static const bool POS_CHECK = true;
static const double B1_COORD_KM = 700.0;   // accumulate the position likelihood with a configured position too

struct RCFeatures {
    double p_top, p_second, p_unk;
    double heard_top, timing_top, snr_lr_top;
    double delta_top;          // minutes after slot start (burst start), 1e9 if out of window
    int    n_cand, n_plaus;    // letter candidates on channel, candidates with p > 1%
    int    letter_ok, in_slot, burst_cont, rx_valid, has_snr, nonletter;
    double dist_top;
    double conf_raw, conf_cal, unk_mult, llr, pos_conc;
    double cfg_reach, cfg_night;   // best reach of a same-letter in-slot station at the configured position, its path night weight
    int64_t burst_start;       // start of the burst this message belongs to (its own start if none)
    const char *reason;
};

class RulesCalibrated {
public:
    RCFeatures last;

    RulesCalibrated() {
        std::memset(&last, 0, sizeof(last));
        geo_lat = geo_lon = 1e9;
        npos_obs = 0; pend_np = 0;
        sun_t = -4611686018427387904LL; std::memset(&sun_cur, 0, sizeof(sun_cur));
        ncfg_obs = 0; cfgL2 = 0; pend_cfg_ll2 = 0;
        for (int k = 0; k < MAXSITE; k++) posL2[k] = pos_ll2[k] = 0;
        pend_pos = false;
        for (int i = 0; i < rc::NSTA; i++) scount[i] = 0;
        for (int k = 0; k < MAXSITE; k++) posL[k] = pos_ll[k] = 0;
        pend_c = -1; pend_l = -1; pend_cont = false; pend_pu = pend_pt = pend_llr = 0;
        for (int c = 0; c < 4; c++) for (int l = 0; l < 24; l++) lS[c][l] = 0;
        for (int i = 0; i < rc::NSTA + MAXSITE; i++) { dcache[i] = -1; ncache[i] = 0; }
        for (int c = 0; c < 4; c++) { ch_end[c] = -1000000; ch_b1[c] = 0; ch_burst[c] = 0; }
        for (int i = 0; i < rc::NSTA; i++) {
            eqv[i] = i;
            for (int j = 0; j < i; j++) {
                const NavtexStation &a = NAVTEX_STATIONS[i], &b = NAVTEX_STATIONS[j];
                if (a.freq_hz == b.freq_hz && a.id == b.id && rc::dist_km(a.lat, a.lon, b.lat, b.lon) < 5) { eqv[i] = eqv[j]; break; }
            }
        }
        // distinct transmitter SITES (any channel, within 30 km): candidate homes of
        // services missing from the table (real: Oostende/Malin Head/Split/Odesa 490)
        nsite = 0;
        for (int i = 0; i < rc::NSTA; i++) {
            const NavtexStation &a = NAVTEX_STATIONS[i];
            int ch = chan(a.freq_hz);
            int k = 0;
            for (; k < nsite; k++) if (rc::dist_km(a.lat, a.lon, site_lat[k], site_lon[k]) < 30) break;
            if (k == nsite) { site_lat[k] = a.lat; site_lon[k] = a.lon; site_mask[k] = 0; site_range[k] = 0; nsite++; }
            if (ch >= 0) site_mask[k] |= 1 << ch;
            if (a.range_nm > site_range[k]) site_range[k] = a.range_nm;
        }
        for (int i = 0; i < rc::NSTA + MAXSITE; i++) night_t[i] = -1000000000LL;
        for (int c = 0; c < 2; c++) for (int k = 0; k < MAXSITE; k++) coord_mask[c][k] = 0;
        for (int i = 0; i < rc::NSTA; i++) {
            int c = chan(NAVTEX_STATIONS[i].freq_hz);
            if (c != 0 && c != 1) continue;
            for (int k = 0; k < nsite; k++)
                if (rc::dist_km(site_lat[k], site_lon[k], NAVTEX_STATIONS[i].lat, NAVTEX_STATIONS[i].lon) < B1_COORD_KM)
                    coord_mask[c][k] |= 1u << (NAVTEX_STATIONS[i].id - 'A');
        }
        for (int c = 0; c < 4; c++) {
            umass[c] = PRIOR_UNK * 0.3;
            if (c < 2) for (int k = 0; k < nsite; k++) umass[c] += ((site_mask[k] & (1 << c)) ? PU_SAME : PU_OTHER) / 24.0;
        }
    }

    MemberResult identify(const NavtexIdentQuery &q) {
        MemberResult r;
        r.station = -1; r.confidence = 0.0; r.alt[0] = r.alt[1] = -1; r.alt_p[0] = r.alt_p[1] = 0.0;
        std::memset(&last, 0, sizeof(last));
        last.reason = "NONE";

        int c = chan(q.freq_hz);
        bool letter = rc::is_letter(q.b1);
        // burst continuation: same channel, same B1, starts within 90 s of the previous end
        int64_t tb = q.start_utc;
        bool cont = false;
        if (c >= 0 && letter && q.b1 == ch_b1[c] && q.start_utc - ch_end[c] <= 90 && q.start_utc - ch_end[c] >= -30) {
            tb = ch_burst[c]; cont = true;
        }
        double tmin = (double)(((tb % 86400) + 86400) % 86400) / 60.0;
        last.burst_start = tb;

        double eps;                                             // P(B1 corrupted into a given wrong letter)
        if (!q.has_snr) eps = 0.006; else if (q.snr_db < 0) eps = 0.02; else if (q.snr_db < 5) eps = 0.006; else eps = 0.0015;
        eps /= 23.0;

        double off = rc::offslot_rate(q.b2);
        double total = 0;
        int ncand = 0;
        for (int i = 0; i < rc::NSTA; i++) {
            w[i] = 0; bw[i] = 0;
            const NavtexStation &s = NAVTEX_STATIONS[i];
            if (std::labs((long)s.freq_hz - q.freq_hz) > 500 || eqv[i] != i) continue;
            double lw = letter ? (s.id == q.b1 ? 1.0 : eps) : 1.0 / 24.0;
            if (letter && s.id == q.b1) ncand++;
            // reach
            double act = s.active == 1 ? 1.0 : (s.active == 0 ? 0.5 : 0.8);
            // timing: table schedule mixed with the canonical one
            double bd1 = 1e9, bd2 = 1e9, tt;
            double dc = rc::canon_density(s.id, tmin, bd2);
            if (s.n_minutes > 0) {
                double dt = rc::sched_density(s.minutes, s.n_minutes, tmin, bd1);
                tt = 0.85 * dt + 0.15 * dc;
            } else tt = dc;
            double bd = std::fabs(bd1 - 4) < std::fabs(bd2 - 4) ? bd1 : bd2;
            // a different letter that is not even in its slot cannot compete: skip the
            // (expensive) propagation terms
            if (letter && s.id != q.b1 && tt <= 0) continue;
            tt = (1 - off) * tt + off / 1440.0;
            double h = 0.5, d = -1, sl = 1.0, nw = 0.5;
            if (q.rx_valid) {
                geo(q, i, s.lat, s.lon, d, nw);
                h = rc::p_heard(d, nw, s.range_nm);
                if (q.has_snr) sl = snr_lik(q.snr_db, d) / snr_lik_unknown(q.snr_db);
            }
            w[i] = lw * h * act * tt * sl;
            bw[i] = lw * act * tt; hraw[i] = h; nwv[i] = nw;
            heard[i] = h; timing[i] = tt * 1440.0; snrl[i] = sl; dl[i] = bd; dk[i] = d;
            total += w[i];
        }
        last.cfg_reach = -1; last.cfg_night = 0;
        if (q.rx_valid && letter) {
            for (int i = 0; i < rc::NSTA; i++)
                if (bw[i] > 0 && NAVTEX_STATIONS[i].id == q.b1 && timing[i] > 0.5 && hraw[i] > last.cfg_reach) {
                    last.cfg_reach = hraw[i]; last.cfg_night = nwv[i];
                }
        }
        // co-channel capture: stations sharing a letter share the slot, so a candidate
        // can only be decoded when every much stronger (>10 dB by the distance law)
        // same-slot station is silent or fades out.
        if (q.rx_valid && letter) {
            int ci[64], nci = 0;
            for (int i = 0; i < rc::NSTA && nci < 64; i++)
                if (w[i] > 0 && NAVTEX_STATIONS[i].id == q.b1) ci[nci++] = i;
            if (nci > 1) {
                double fac[64];
                for (int a = 0; a < nci; a++) {
                    double pa = pred_snr(dk[ci[a]]), m = 0;
                    for (int b = 0; b < nci; b++)
                        if (b != a && pred_snr(dk[ci[b]]) - pa > CAPTURE_DB && heard[ci[b]] > m) m = heard[ci[b]];
                    fac[a] = 1 - CAPTURE_P * m;
                }
                for (int a = 0; a < nci; a++) { total -= w[ci[a]]; w[ci[a]] *= fac[a]; total += w[ci[a]]; heard[ci[a]] *= fac[a]; }
            }
        }
        // learned share among the stations sharing this letter: redistributes weight
        // between them, never between "table" and "not in the table"
        if (letter && LP_GAIN > 0) {
            double s0 = 0, s1 = 0, fi[rc::NSTA];
            for (int i = 0; i < rc::NSTA; i++) if (w[i] > 0 && NAVTEX_STATIONS[i].id == q.b1) {
                fi[i] = learned_prior(i, q.b1); s0 += w[i]; s1 += w[i] * fi[i];
            }
            if (s1 > 0) for (int i = 0; i < rc::NSTA; i++) if (w[i] > 0 && NAVTEX_STATIONS[i].id == q.b1) {
                double nwi = w[i] * fi[i] * s0 / s1; total += nwi - w[i]; w[i] = nwi;
            }
        }
        // "not in the table": a transmitter we know nothing about.  It usually keeps a
        // canonical slot for its letter; its reach and SNR are uninformative.
        double tu;
        {
            double bd;
            tu = letter ? 0.6 * rc::canon_density(q.b1, tmin, bd) + 0.4 / 1440.0 : 1.0 / 1440.0;
            tu = (1 - off) * tu + off / 1440.0;
        }
        // (a) a remote transmitter we know nothing about (reach/SNR uninformative);
        // (b) an unlisted service at a known site (letter unknown -> 1/24), with that
        //     site's reach and SNR.  Unlisted at a site already on this channel is rarer.
        double wu = (q.rx_valid ? PRIOR_UNK * 0.3 : PRIOR_UNK_NOLOC * 0.5) * tu * (letter ? 1.0 : 3.0);
        int myc = chan(q.freq_hz);
        if (q.rx_valid && (myc == 0 || myc == 1)) {
            for (int k = 0; k < nsite; k++) {
                double pu = (site_mask[k] & (1 << myc)) ? PU_SAME : PU_OTHER;
                double d, nw;
                if (dcache[rc::NSTA + k] > 6000) continue;      // negligible reach
                // B1 coordination (graft from letter_recovery): administrations do not give
                // a letter to a transmitter within ~700 km of another using it on the channel
                if (letter && B1_COORD_KM > 0 && (coord_mask[myc][k] >> (q.b1 - 'A') & 1)) continue;
                geo(q, rc::NSTA + k, site_lat[k], site_lon[k], d, nw);
                double h = rc::p_heard(d, nw, site_range[k]);
                double sl = q.has_snr ? snr_lik(q.snr_db, d) / snr_lik_unknown(q.snr_db) : 1.0;
                wu += pu / 24.0 * h * sl * tu * (letter ? 1.0 : 3.0);
            }
        }
        // receiver position unknown: the set of stations a receiver hears pins down its
        // region (MF reach is ~1000 km).  Keep a decayed log-likelihood over candidate
        // positions (the known transmitter sites) and use the posterior mixture.
        // With a CONFIGURED position the same likelihood is still accumulated, together
        // with the likelihood of the configured position itself (cfg_ll), so that
        // pos_llr() can tell when what is heard contradicts the configuration.
        pend_pos = false;
        if (letter && (!q.rx_valid || POS_CHECK)) {
            double mx = -1e300;
            for (int k = 0; k < nsite; k++) if (posL[k] > mx) mx = posL[k];
            double pk[MAXSITE], zs = 0;
            for (int k = 0; k < nsite; k++) { pk[k] = std::exp(posL[k] - mx); zs += pk[k]; }
            int ci[64], nci = 0;
            double base[64], mix[64];
            for (int i = 0; i < rc::NSTA && nci < 64; i++)
                if ((q.rx_valid ? bw[i] : w[i]) > 0 && NAVTEX_STATIONS[i].id == q.b1) {
                    ci[nci] = i; base[nci] = q.rx_valid ? bw[i] : w[i] / 0.5; mix[nci] = 0; nci++;
                }
            double wgen = PRIOR_UNK * 0.3 * tu;
            // With a configured position only the reach-only likelihood (posL2) is kept, for
            // the position check; the SNR-weighted one (posL) places a receiver that has no
            // position, and skipping it otherwise halves the cost.
            const bool snr_on = !q.rx_valid;
            if (q.rx_valid) {                     // the configured position, same formula
                double sk2 = wgen;
                for (int a = 0; a < nci; a++) sk2 += base[a] * hraw[ci[a]];
                pend_cfg_ll2 = std::log(sk2);
            }
            double pmax = 0;
            double llmin = 1e300, llmin2 = 1e300, mx2 = -1e300;
            for (int k = 0; k < nsite; k++) if (posL2[k] > mx2) mx2 = posL2[k];
            bool prune = npos_obs >= 50;          // long-excluded positions: skip the physics
            const rc::SunPos sun = rc::sun_at(q.start_utc);
            for (int k = 0; k < nsite; k++) {
                if (prune && (snr_on ? posL[k] < mx - 40 : posL2[k] < mx2 - 40)) { pos_ll[k] = 1e300; pos_ll2[k] = 1e300; continue; }
                double sk = wgen, sk2 = wgen;
                double pkk = pk[k] / zs;
                if (pkk > pmax) pmax = pkk;
                for (int a = 0; a < nci; a++) {
                    const NavtexStation &st = NAVTEX_STATIONS[ci[a]];
                    double d = sdist(k, ci[a]);
                    double nw = snight(k, ci[a], q.start_utc, d, sun);
                    double hh = base[a] * rc::p_heard(d, nw, st.range_nm);
                    sk2 += hh;
                    if (!snr_on) continue;
                    double t = hh * (q.has_snr ? snr_lik(q.snr_db, d) / snr_lik_unknown(q.snr_db) : 1.0);
                    sk += t;
                    if (pkk > 1e-4) mix[a] += pkk * t;
                }
                pos_ll[k] = snr_on ? std::log(sk) : 0.0; pos_ll2[k] = std::log(sk2);
                if (pos_ll[k] < llmin) llmin = pos_ll[k];
                if (pos_ll2[k] < llmin2) llmin2 = pos_ll2[k];
            }
            for (int k = 0; k < nsite; k++) {
                if (pos_ll[k] > 1e299) pos_ll[k] = snr_on ? llmin : 0.0;
                if (pos_ll2[k] > 1e299) pos_ll2[k] = llmin2;
            }
            pend_pos = true;
            if (!q.rx_valid && npos_obs >= POS_MIN_OBS) {   // enough evidence: replace the flat reach
                for (int a = 0; a < nci; a++) { total -= w[ci[a]]; w[ci[a]] = mix[a]; total += mix[a]; heard[ci[a]] = mix[a] / base[a]; }
                last.pos_conc = pmax;
            }
        }
        // learned per receiver: if this (channel, letter) keeps producing messages that
        // no table station explains, raise the "not in the table" prior for it.
        double tot_t = total;                       // table part
        double pu_base = (tot_t + wu) > 0 ? wu / (tot_t + wu) : 0;
        double mult = 1.0;
        int li = letter ? q.b1 - 'A' : -1;
        if (c >= 0 && li >= 0) {
            double e = LEARN_GAIN * lS[c][li];
            if (e < LEARN_MINLOG) e = LEARN_MINLOG;
            if (e > LEARN_MAXLOG) e = LEARN_MAXLOG;
            mult = std::exp(e);
        }
        wu *= mult;
        total += wu;
        if (total <= 0) return r;

        int b0 = -1, b1i = -1, b2i = -1;
        for (int i = 0; i < rc::NSTA; i++) {
            if (w[i] <= 0) continue;
            if (b0 < 0 || w[i] > w[b0]) { b2i = b1i; b1i = b0; b0 = i; }
            else if (b1i < 0 || w[i] > w[b1i]) { b2i = b1i; b1i = i; }
            else if (b2i < 0 || w[i] > w[b2i]) b2i = i;
        }
        int nplaus = 0;
        for (int i = 0; i < rc::NSTA; i++) if (w[i] > 0.01 * total) nplaus++;

        RCFeatures &f = last;
        pend_c = c; pend_l = li; pend_pu = pu_base; pend_cont = cont;
        pend_np = 0;
        if (letter && LP_GAIN > 0)
            for (int i = 0; i < rc::NSTA && pend_np < 16; i++)
                if (w[i] > 0 && NAVTEX_STATIONS[i].id == q.b1) { pend_si[pend_np] = i; pend_sp[pend_np] = w[i] / total; pend_np++; }
        pend_pt = b0 >= 0 ? w[b0] / (tot_t + wu / mult) : 0;
        {   // per-message log likelihood ratio "unlisted transmitter" vs "table station(s)"
            double mass = q.rx_valid ? umass[c >= 0 ? c : 0] : PRIOR_UNK_NOLOC * 0.5;
            double lu_ = (wu / mult) / mass, lt_ = tot_t > 1e-300 ? tot_t : 1e-300;
            double llr = std::log((lu_ > 1e-300 ? lu_ : 1e-300) / lt_);
            pend_llr = llr < -3 ? -3 : (llr > 3 ? 3 : llr);
            f.llr = pend_llr;
        }
        f.unk_mult = mult;
        f.p_unk = wu / total;
        f.p_top = b0 >= 0 ? w[b0] / total : 0;
        f.p_second = b1i >= 0 ? w[b1i] / total : 0;
        f.n_cand = ncand; f.n_plaus = nplaus;
        f.rx_valid = q.rx_valid; f.has_snr = q.has_snr; f.nonletter = !letter;
        f.burst_cont = cont;
        if (b0 >= 0) {
            f.heard_top = heard[b0]; f.timing_top = timing[b0]; f.snr_lr_top = snrl[b0];
            f.delta_top = dl[b0]; f.dist_top = dk[b0];
            f.letter_ok = letter && NAVTEX_STATIONS[b0].id == q.b1;
            f.in_slot = dl[b0] >= -15 && dl[b0] < 35;
        }

        if (b0 < 0 || f.p_unk > ABSTAIN_PUNK) { f.reason = "UNLISTED"; return r; }

        // rule class -> calibration map; the class is also the reason code
        int cls; const char *why;
        static const char *const WHY[5][2] = {
            { "SLOT_SOLE", "SLOT_SOLE:CAP" }, { "SLOT_RANKED", "SLOT_RANKED:CAP" },
            { "NOLOC", "NOLOC:CAP" }, { "OFFSLOT", "OFFSLOT:CAP" }, { "LETTER", "LETTER:CAP" } };
        int wi;
        if (!f.letter_ok)      { cls = 3; wi = 4; }   // B1 is another station's letter: corrupted header
        else if (!q.rx_valid)  { cls = 1; wi = 2; }   // no receiver position: learned position / timing
        else if (!f.in_slot)   { cls = 2; wi = 3; }   // burst start outside every slot of the station
        else                   { cls = 0; wi = ncand <= 1 ? 0 : 1; }
        why = WHY[wi][0];
        double conf = calibrate(f, cls);
        f.conf_cal = conf;
        // operator gate: claim >= 0.90 only for an in-slot, matching-letter, reachable
        // station at a known receiver position whose fitted reliability is at least
        // HI_MIN.  Without a position (given or learned) an unlisted or co-channel
        // transmitter can never be excluded, so NOLOC is capped unless the learned
        // position is concentrated.
        bool pos_ok = q.rx_valid || f.pos_conc >= POS_HI_CONC;    // learned position well pinned
        if (conf >= 0.9 && (conf < HI_MIN || cls >= 2 || !pos_ok || f.heard_top < 0.05)) { conf = 0.89; why = WHY[wi][1]; }
        if (conf > 0.995) conf = 0.995;
        f.conf_raw = conf;
        f.reason = why;

        r.station = b0; r.confidence = conf;
        r.alt[0] = b1i; r.alt[1] = b2i;
        r.alt_p[0] = b1i >= 0 ? w[b1i] / total : 0;
        r.alt_p[1] = b2i >= 0 ? w[b2i] / total : 0;
        return r;
    }

    // Position consistency (receiver position configured): decayed log-likelihood
    // (reach only, no SNR) of the best candidate site minus that of the configured
    // position, over the n_obs messages that went into it, and the best site.  A large
    // positive value means the stations being heard fit some other place much better
    // than the configured one.
    double pos_llr_reach(int &n_obs, double &best_lat, double &best_lon) const {
        n_obs = ncfg_obs;
        int kb = -1;
        for (int k = 0; k < nsite; k++) if (kb < 0 || posL2[k] > posL2[kb]) kb = k;
        if (kb < 0 || ncfg_obs == 0) { best_lat = best_lon = 0; return 0; }
        best_lat = site_lat[kb]; best_lon = site_lon[kb];
        return posL2[kb] - cfgL2;
    }

    // Forget the evidence about the configured receiver position (called by the
    // ensemble when the configured position jumps, e.g. the operator fixed the config).
    void reset_position_evidence() {
        ncfg_obs = 0; cfgL2 = 0; pend_cfg_ll2 = 0;
        for (int k = 0; k < MAXSITE; k++) posL2[k] = pos_ll2[k] = 0;
        pend_pos = false;
    }

    void observe(const NavtexIdentQuery &q) {
        int c = chan(q.freq_hz);
        if (c < 0) return;
        if (pend_c == c && pend_l >= 0) {
            // a burst is one observation of the propagation path: weight its later
            // messages down so one long night burst cannot flip the letter's prior
            lS[c][pend_l] = lS[c][pend_l] * LEARN_DECAY + (pend_cont ? 0.25 : 1.0) * pend_llr;
        }
        pend_c = -1;
        for (int a = 0; a < pend_np; a++) {          // per-station soft attribution counts
            double &cc = scount[pend_si[a]];
            cc = cc * LP_DECAY + pend_sp[a] * (pend_cont ? 0.25 : 1.0);
        }
        pend_np = 0;
        if (pend_pos) {
            for (int k = 0; k < nsite; k++) { posL[k] = posL[k] * POS_DECAY + pos_ll[k]; posL2[k] = posL2[k] * POS_DECAY + pos_ll2[k]; }
            if (q.rx_valid) { cfgL2 = cfgL2 * POS_DECAY + pend_cfg_ll2; ncfg_obs++; }
            npos_obs++;
            pend_pos = false;
        }
        bool cont = rc::is_letter(q.b1) && q.b1 == ch_b1[c] && q.start_utc - ch_end[c] <= 90 && q.start_utc - ch_end[c] >= -30;
        if (!cont) ch_burst[c] = q.start_utc;
        ch_b1[c] = q.b1;
        ch_end[c] = q.start_utc + (q.duration_s > 0 ? q.duration_s : 30);
    }

private:
    enum { NC = 4, NF = 8, NK = 24 };
    // learned per (channel, letter): decayed sum of per-message LLR "unlisted vs table"
    double lS[4][24], umass[4];
    int pend_c, pend_l; bool pend_cont; double pend_pu, pend_pt, pend_llr;
    enum { MAXSITE = 256 };
    // learned per receiver: which of the stations sharing a letter it actually hears
    double scount[rc::NSTA]; int pend_si[16]; double pend_sp[16]; int pend_np;
    double learned_prior(int i, char b1) const {
        if (LP_GAIN <= 0 || NAVTEX_STATIONS[i].id != b1) return 1.0;
        double tot = 0; int n = 0;
        for (int j = 0; j < rc::NSTA; j++)
            if (NAVTEX_STATIONS[j].id == b1 && NAVTEX_STATIONS[j].freq_hz == NAVTEX_STATIONS[i].freq_hz && eqv[j] == j) { tot += scount[j]; n++; }
        if (n <= 1) return 1.0;
        double share = (scount[i] + 2.0) / (tot + 2.0 * n) * n;   // 1 = no information
        double f = std::pow(share, LP_GAIN);
        double fmax = NAVTEX_STATIONS[i].active == 0 ? 1.0 : 3.0;   // never lift a station listed inactive
        return f < 0.3 ? 0.3 : (f > fmax ? fmax : f);
    }
    // learned receiver position (only used when rx_valid is false)
    double posL[MAXSITE], pos_ll[MAXSITE];
    int npos_obs; bool pend_pos;
    int ncfg_obs;                                   // messages in the configured-position likelihood
    double posL2[MAXSITE], pos_ll2[MAXSITE], cfgL2, pend_cfg_ll2;   // reach-only position likelihoods
    double bw[rc::NSTA], hraw[rc::NSTA], nwv[rc::NSTA];
    // The caches below are std::vector so the identifier copies correctly: the
    // server identifies an in-progress message on a COPY, to leave the learned
    // state untouched until the message is final.
    std::vector<float> sd;                         // site x station distance table, lazily built
    double sdist(int k, int i) {
        if (sd.empty()) {
            sd.assign(MAXSITE * rc::NSTA, 0.0f);
            for (int a = 0; a < nsite; a++) for (int b = 0; b < rc::NSTA; b++)
                sd[a * rc::NSTA + b] = (float)rc::dist_km(site_lat[a], site_lon[a], NAVTEX_STATIONS[b].lat, NAVTEX_STATIONS[b].lon);
        }
        return sd[k * rc::NSTA + i];
    }
    // night weight site k -> station i, cached for 15 minutes
    // (the sample points along each site->station path are fixed, so they are worked
    //  out once; the sun position comes from the caller, once per message)
    std::vector<float> sn; std::vector<int> snt;
    struct PathPts { int n; double sl[3], cl[3], lo[3]; };
    std::vector<PathPts> spp;
    double snight(int k, int i, int64_t t, double d, const rc::SunPos &sun) {
        if (sn.empty()) {
            sn.assign(MAXSITE * rc::NSTA, 0.0f); snt.assign(MAXSITE * rc::NSTA, 0); spp.assign(MAXSITE * rc::NSTA, PathPts());
            for (int j = 0; j < MAXSITE * rc::NSTA; j++) { snt[j] = -1000000; spp[j].n = 0; }
        }
        int j = k * rc::NSTA + i, tm = (int)(t / 60);
        if (std::abs(tm - snt[j]) > 15) {
            PathPts &pp = spp[j];
            if (pp.n == 0) {                       // same points as rc::path_night
                const NavtexStation &st = NAVTEX_STATIONS[i];
                const double kk = rc::PI / 180.0;
                pp.n = d > 1500 ? 3 : 1;
                for (int a = 0; a < pp.n; a++) {
                    double f = pp.n == 1 ? 0.5 : 0.25 * (a + 1), la, lo;
                    rc::gc_point(site_lat[k], site_lon[k], st.lat, st.lon, f, la, lo);
                    pp.sl[a] = std::sin(la * kk); pp.cl[a] = std::cos(la * kk); pp.lo[a] = lo;
                }
            }
            double emax = -90;
            for (int a = 0; a < pp.n; a++) {
                double e = rc::sun_elev(sun, pp.sl[a], pp.cl[a], pp.lo[a]);
                if (e > emax) emax = e;
            }
            double w = (2.0 - emax) / 10.0;
            sn[j] = (float)(w < 0 ? 0 : (w > 1 ? 1 : w)); snt[j] = tm;
        }
        return sn[j];
    }
    int eqv[rc::NSTA];
    int nsite, site_mask[MAXSITE], site_range[MAXSITE];
    unsigned coord_mask[2][MAXSITE];                 // letters listed within B1_COORD_KM of each site, per MF channel
    double site_lat[MAXSITE], site_lon[MAXSITE];
    // geometry cache (per receiver position) and night-weight cache (10 min)
    double geo_lat, geo_lon, dcache[rc::NSTA + MAXSITE], ncache[rc::NSTA + MAXSITE];
    int64_t night_t[rc::NSTA + MAXSITE];
    double w[rc::NSTA], heard[rc::NSTA], timing[rc::NSTA], snrl[rc::NSTA], dl[rc::NSTA], dk[rc::NSTA];

    // distance and night weight from the receiver to station/site k
    // (the path sample points are worked out once per receiver position, the sun
    //  position once per message; the result equals rc::path_night)
    PathPts gpp[rc::NSTA + MAXSITE];
    int64_t sun_t; rc::SunPos sun_cur;
    void geo(const NavtexIdentQuery &q, int k, double la, double lo, double &d, double &nw) {
        if (q.rx_lat != geo_lat || q.rx_lon != geo_lon) {
            geo_lat = q.rx_lat; geo_lon = q.rx_lon;
            for (int i = 0; i < rc::NSTA + MAXSITE; i++) { dcache[i] = -1; night_t[i] = -1000000000LL; }
        }
        if (dcache[k] < 0) {
            dcache[k] = rc::dist_km(q.rx_lat, q.rx_lon, la, lo);
            PathPts &pp = gpp[k];
            const double kk = rc::PI / 180.0;
            pp.n = dcache[k] > 1500 ? 3 : 1;
            for (int a = 0; a < pp.n; a++) {
                double f = pp.n == 1 ? 0.5 : 0.25 * (a + 1), pla, plo;
                rc::gc_point(q.rx_lat, q.rx_lon, la, lo, f, pla, plo);
                pp.sl[a] = std::sin(pla * kk); pp.cl[a] = std::cos(pla * kk); pp.lo[a] = plo;
            }
        }
        d = dcache[k];
        if (std::llabs(q.start_utc - night_t[k]) > 600) {
            if (sun_t != q.start_utc) { sun_cur = rc::sun_at(q.start_utc); sun_t = q.start_utc; }
            const PathPts &pp = gpp[k];
            double emax = -90;
            for (int a = 0; a < pp.n; a++) {
                double e = rc::sun_elev(sun_cur, pp.sl[a], pp.cl[a], pp.lo[a]);
                if (e > emax) emax = e;
            }
            double w = (2.0 - emax) / 10.0;
            ncache[k] = w < 0 ? 0 : (w > 1 ? 1 : w);
            night_t[k] = q.start_utc;
        }
        nw = ncache[k];
    }
    int64_t ch_end[4], ch_burst[4];
    char ch_b1[4];

    static int chan(long f) {
        if (std::labs(f - 518000) <= 500) return 0;
        if (std::labs(f - 490000) <= 500) return 1;
        if (std::labs(f - 424000) <= 500) return 2;
        if (std::labs(f - 4209500) <= 500) return 3;
        return -1;
    }
    // SNR given distance: median 50.1 - 15 log10 km, asymmetric spread (fading makes
    // weak readings common, readings far above the prediction rare), plus a broad floor.
    // Groundwave at short range is steady (spread ~5 dB in the real data); skywave
    // fades more.  The density is conditioned on the message having been decoded
    // (SNR above the ~-3 dB floor): reach already accounts for P(decoded), and a far
    // station is only ever decoded on its strong moments.
    static double snr_lik(double snr, double d) {
        double pred = 50.1 - 15.0 * std::log10(d < 30 ? 30 : d);
        double f = d < 300 ? 0 : (d > 700 ? 1 : (d - 300) / 400.0);
        double sl = 7.0 + 3.0 * f, sh = 6.0 + 2.0 * f;
        double r = snr - pred;
        double sg = r > 0 ? sh : sl;
        double dens = 2.0 / (sl + sh) * std::exp(-0.5 * r * r / (sg * sg)) / 2.5066283;
        double rf = SNR_FLOOR - pred, cdf;                      // P(snr < floor)
        if (rf < 0) cdf = 2 * sl / (sl + sh) * phi(rf / sl);
        else cdf = sl / (sl + sh) + 2 * sh / (sl + sh) * (phi(rf / sh) - 0.5);
        double above = 1 - cdf; if (above < 0.02) above = 0.02;
        if (snr >= SNR_FLOOR) dens /= above;
        return (1 - SNR_OUTLIER) * dens + SNR_OUTLIER / 60.0;
    }
    static double pred_snr(double d) { return 50.1 - 15.0 * std::log10(d < 30 ? 30 : d); }
    static double phi(double x) { return 0.5 * std::erfc(-x / 1.41421356); }
    static double snr_lik_unknown(double) { return 1.0 / 50.0; }

    static double lgt(double p) {
        if (p < 1e-6) p = 1e-6;
        if (p > 1 - 1e-6) p = 1 - 1e-6;
        return std::log(p / (1 - p));
    }
    // per class: score z = w . features (logistic, fitted on train), then a monotone
    // piecewise-linear (isotonic) map z -> P(correct), also fitted on train (fit3.py)
    static void cal_feats(const RCFeatures &f, double *x) {
        x[0] = 1; x[1] = lgt(f.p_top); x[2] = lgt(f.p_unk); x[3] = lgt(f.p_second);
        x[4] = std::log10(f.heard_top > 1e-4 ? f.heard_top : 1e-4);
        x[5] = f.has_snr ? std::log(f.snr_lr_top > 1e-3 ? f.snr_lr_top : 1e-3) : 0; x[6] = f.burst_cont;
        x[7] = f.rx_valid ? 0 : f.pos_conc;           // learned-position concentration (NOLOC only)
    }
    double calibrate(const RCFeatures &f, int cls) const {
        double x[NF], z = 0;
        cal_feats(f, x);
        for (int i = 0; i < NF; i++) z += CALW[cls][i] * x[i];
        const double *kz = ISO_Z[cls], *kp = ISO_P[cls];
        if (!(z > kz[0])) return kp[0];                         // also NaN: the low end
        for (int i = 1; i < NK; i++) {
            if (kz[i] <= kz[i - 1]) return kp[i - 1];          // unused knots
            if (z <= kz[i]) return kp[i - 1] + (z - kz[i - 1]) / (kz[i] - kz[i - 1]) * (kp[i] - kp[i - 1]);
        }
        return kp[NK - 1];
    }
};

}  // namespace navtex_ident_rules
