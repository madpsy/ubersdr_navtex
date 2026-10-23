// production: thin adapter so the harness evaluates the production identifier itself,
// src/navtex_ident.h (an ensemble of the bayes_physical and rules_calibrated methods).
#pragma once
#include "../ident_api.h"
#include "../../../src/navtex_ident.h"

class ProductionIdent : public Identifier {
public:
    IdentResult identify(const IdentQuery &q) {
        NavtexIdentQuery p;
        p.freq_hz = q.freq_hz; p.b1 = q.b1; p.b2 = q.b2; p.serial = q.serial;
        p.start_utc = q.start_utc; p.duration_s = q.duration_s;
        p.rx_valid = q.rx_valid; p.rx_lat = q.rx_lat; p.rx_lon = q.rx_lon;
        p.has_snr = q.has_snr; p.snr_db = q.snr_db; p.has_chars = q.has_chars; p.chars_failed_pct = q.chars_failed_pct;
        last_q = p;
        last = id.identify(p);
        IdentResult r;
        r.station = last.station; r.confidence = last.confidence;
        r.alt[0] = last.alt[0]; r.alt[1] = last.alt[1]; r.alt_p[0] = last.alt_p[0]; r.alt_p[1] = last.alt_p[1];
        return r;
    }
    void observe(const IdentQuery &, const IdentResult &) { id.observe(last_q, last); }
    NavtexIdentResult last;
private:
    NavtexIdentifier id;
    NavtexIdentQuery last_q;
};

Identifier *make_identifier() { return new ProductionIdent(); }
