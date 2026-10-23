// Trivial baseline, only to exercise the harness: candidates are the stations on
// the same channel (+-0.5 kHz) whose letter equals B1; pick the nearest to the
// receiver (the first one when the receiver location is unknown);
// confidence = 1 / number of candidates.
#pragma once
#include <cmath>
#include <cstdlib>
#include "../ident_api.h"

class BaselineNearest : public Identifier {
public:
    IdentResult identify(const IdentQuery &q) {
        IdentResult r;
        r.station = -1; r.confidence = 0.0;
        r.alt[0] = r.alt[1] = -1; r.alt_p[0] = r.alt_p[1] = 0.0;
        int n = 0, best = -1;
        double bestd = 1e300;
        int count = (int)(sizeof(NAVTEX_STATIONS) / sizeof(NAVTEX_STATIONS[0]));
        for (int i = 0; i < count; i++) {
            const NavtexStation &s = NAVTEX_STATIONS[i];
            if (std::labs((long)s.freq_hz - q.freq_hz) > 500 || s.id != q.b1 || q.b1 == 0) continue;
            n++;
            double d = q.rx_valid ? dist_km(q.rx_lat, q.rx_lon, s.lat, s.lon) : (double)n;
            if (d < bestd) { bestd = d; best = i; }
        }
        if (best >= 0) { r.station = best; r.confidence = 1.0 / n; }
        return r;
    }
private:
    static double dist_km(double la1, double lo1, double la2, double lo2) {
        const double k = 3.14159265358979323846 / 180.0;
        double a = std::sin((la2 - la1) * k / 2), b = std::sin((lo2 - lo1) * k / 2);
        double h = a * a + std::cos(la1 * k) * std::cos(la2 * k) * b * b;
        return 2 * 6371.0 * std::asin(std::sqrt(h < 1 ? h : 1));
    }
};

Identifier *make_identifier() { return new BaselineNearest(); }
