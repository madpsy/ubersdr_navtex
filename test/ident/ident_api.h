#pragma once
#include <cstdint>
#include "../../src/navtex_stations.h"
struct IdentQuery {
    long    freq_hz;          // channel carrier, e.g. 518000, 490000, 4209500
    char    b1, b2;           // header letters as decoded (0 if missing; may be garbage)
    int     serial;           // -1 if unknown
    int64_t start_utc;        // unix seconds, when ZCZC was seen
    int     duration_s;       // ZCZC..NNNN
    bool    rx_valid;         // receiver location known?
    double  rx_lat, rx_lon;
    bool    has_snr;  double snr_db;
    bool    has_chars; double chars_failed_pct;   // % characters the FEC could not recover
};
struct IdentResult {
    int    station;           // index into NAVTEX_STATIONS, or -1 = "cannot attribute"
    double confidence;        // calibrated P(station is the true transmitter); must be 0 when station == -1
    int    alt[2];            // runner-up indices or -1
    double alt_p[2];
};
class Identifier {
public:
    virtual ~Identifier() {}
    virtual IdentResult identify(const IdentQuery &q) = 0;
    // Called after identify() for every message, in chronological order, with that variant's own result.
    // A variant MAY learn per-receiver state here (it never sees ground truth). Must be bounded memory.
    virtual void observe(const IdentQuery &, const IdentResult &) {}
};
Identifier *make_identifier();   // each variant header defines this
