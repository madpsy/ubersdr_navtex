/* -*- c++ -*- */
/*
 * navtex_ident_common.h — types and geometry shared by the two transmitter
 * identification methods (navtex_ident_bayes.h, navtex_ident_rules.h) and by
 * the ensemble that combines them (navtex_ident.h).  Include navtex_ident.h,
 * not this file.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Header-only, C++11, no I/O, no mutable globals.
 */
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "navtex_stations.h"

/* One decoded message, as the identifier sees it.  Nothing here comes from
 * the message body: the identification must stand on the header, the clock,
 * the decoder's metrics and the receiver position alone. */
struct NavtexIdentQuery {
    long    freq_hz;          /* channel carrier, e.g. 518000, 490000, 4209500 */
    char    b1, b2;           /* header letters as decoded (0 if missing; may be garbage) */
    int     serial;           /* -1 if unknown */
    int64_t start_utc;        /* unix seconds, when ZCZC was seen */
    int     duration_s;       /* ZCZC..NNNN */
    bool    rx_valid;         /* receiver location known? */
    double  rx_lat, rx_lon;
    bool    has_snr;  double snr_db;
    bool    has_chars; double chars_failed_pct;   /* % characters the FEC could not recover */
};

namespace navtex_ident_detail {

/* What one member method answers. station is an index into NAVTEX_STATIONS
 * or -1 ("cannot attribute", confidence 0). */
struct MemberResult {
    int    station;
    double confidence;
    int    alt[2];
    double alt_p[2];
};

static const double ID_PI = 3.14159265358979323846;
static const double ID_D2R = ID_PI / 180.0;

inline double gc_km(double la1, double lo1, double la2, double lo2) {
    double a = std::sin((la2 - la1) * ID_D2R / 2), b = std::sin((lo2 - lo1) * ID_D2R / 2);
    double h = a * a + std::cos(la1 * ID_D2R) * std::cos(la2 * ID_D2R) * b * b;
    return 2 * 6371.0 * std::asin(std::sqrt(h < 1 ? h : 1));
}

/* Two table rows are the same physical transmitter when they share channel,
 * letter and site (the table lists Cabo La Nao twice per letter, and so on). */
inline bool same_transmitter(int a, int b) {
    if (a == b) return true;
    if (a < 0 || b < 0) return false;
    const NavtexStation &x = NAVTEX_STATIONS[a], &y = NAVTEX_STATIONS[b];
    return x.freq_hz == y.freq_hz && x.id == y.id && gc_km(x.lat, x.lon, y.lat, y.lon) < 5;
}

}  // namespace navtex_ident_detail

/* Great-circle distance in km (haversine, mean Earth radius 6371 km). */
inline double navtex_great_circle_km(double lat1, double lon1, double lat2, double lon2) {
    return navtex_ident_detail::gc_km(lat1, lon1, lat2, lon2);
}

/* Initial bearing from point 1 towards point 2, degrees clockwise from true
 * north, 0..360. */
inline double navtex_initial_bearing_deg(double lat1, double lon1, double lat2, double lon2) {
    using navtex_ident_detail::ID_D2R;
    double p1 = lat1 * ID_D2R, p2 = lat2 * ID_D2R, dl = (lon2 - lon1) * ID_D2R;
    double y = std::sin(dl) * std::cos(p2);
    double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    double b = std::atan2(y, x) / ID_D2R;
    return b < 0 ? b + 360.0 : b;
}
