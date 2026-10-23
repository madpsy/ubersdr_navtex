#!/usr/bin/env python3
"""Synthetic NAVTEX reception generator for the transmitter-identification harness.

Builds a TRUE world (transmitters, schedules, traffic) that deliberately differs
from src/navtex_stations.h, simulates propagation to ~40 receivers worldwide and
writes one CSV row per message a receiver's decoder would have produced.

Nothing here knows about any identification algorithm.  The model is driven by
physics-ish propagation, the NAVTEX scheduling rules and the statistics of the
real Dalgety Bay logs (durations, SNR scale, subject mix, schedule offsets).

    python3 -I test/ident/synth.py --seed 1 --days 60 --out test/ident/data/train.csv
    python3 -I test/ident/synth.py --seed 7 --days 3 --receivers 12 --out test/ident/data/smoke.csv

Output columns (shared with real_to_csv.py):
    rx, rx_valid, rx_lat, rx_lon, freq_hz, b1, b2, serial, start_utc, duration_s,
    snr_db, chars_failed_pct, truth, tags, proxy_truth, dist_km, tx_name

  truth       station index into NAVTEX_STATIONS, -1 = transmitter not in the
              table or noise.  (Identical rows in the table - same freq, letter
              and site - are one transmitter; truth is the lowest index and the
              evaluator treats the duplicates as equivalent.)
  tags        '|'-separated scenario tags (see TAGS below)
  dist_km     true rx-tx distance (diagnostic only, never an input)
  tx_name     true transmitter name (diagnostic only)

SNR is on the decoder's audio-protocol-v4 scale (real Dalgety Bay: 1%..99% =
-4..+25 dB, decode floor ~ -4 dB).  A <out>.world.json describing the true
world (positions, offsets, outages) is written next to the CSV.
"""
import argparse
import bisect
import csv
import datetime as dt
import json
import math
import os
import re
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, '..', '..', 'src', 'navtex_stations.h')

TAGS = {
    'dx':              'beyond the transmitter nominal range',
    'night_dx':        'beyond nominal range and carried by night skywave',
    'anomalous_dx':    'rare enhanced long-distance reception',
    'offslot':         'unscheduled vital/important broadcast outside the slot',
    'late_slot':       'message started >= 5 min into its slot',
    'overrun':         'message started after the nominal slot end',
    'garbled':         'B1 as decoded differs from the true letter',
    'b1_nonletter':    'B1 decoded as something outside A-X',
    'unknown_tx':      'transmitter is not in the table (truth -1)',
    'noise':           'decoder produced a header from noise (truth -1)',
    'pos_error':       'true position differs from the table by tens-hundreds km',
    'sign_flip':       'table position has a sign-flipped coordinate',
    'sched_diff':      'true schedule differs from the table',
    'inactive_on_air': 'table says inactive, transmitter is on air',
    'unknown_active':  'table active flag not stated',
    'rx_unknown':      'receiver location unknown to the identifier',
    'collision':       '>=2 table stations with this freq+letter within 3000 km of rx',
    'capture':         'overlapped in time with another decodable co-channel signal',
    'inland':          'receiver is inland',
    'hf':              '4209.5 kHz',
    'jp424':           '424 kHz (Japan)',
}

# ---------------------------------------------------------------- receivers
# name, lat, lon, inland_km, location_known
RECEIVERS = [
    ('dalgety_bay',  56.0403,   -3.3554,    0, True),
    ('plymouth',     50.37,     -4.14,      0, True),
    ('bergen',       60.39,      5.32,      0, True),
    ('hamburg',      53.55,      9.99,     60, True),
    ('rotterdam',    51.92,      4.48,      0, True),
    ('gdansk',       54.35,     18.65,      0, True),
    ('helsinki',     60.17,     24.94,      0, False),
    ('reykjavik',    64.13,    -21.90,      0, True),
    ('brest',        48.39,     -4.49,      0, True),
    ('lisbon',       38.72,     -9.14,      0, True),
    ('marseille',    43.30,      5.37,      0, True),
    ('genoa',        44.41,      8.93,      0, False),
    ('naples',       40.85,     14.27,      0, True),
    ('piraeus',      37.94,     23.65,      0, True),
    ('istanbul',     41.01,     28.98,      0, True),
    ('varna',        43.21,     27.91,      0, True),
    ('odesa',        46.48,     30.73,      0, True),
    ('alexandria',   31.20,     29.92,      0, True),
    ('valletta',     35.90,     14.51,      0, True),
    ('madrid',       40.42,     -3.70,    300, True),
    ('paris',        48.86,      2.35,    170, True),
    ('munich',       48.14,     11.58,    450, False),
    ('dubai',        25.27,     55.30,      0, True),
    ('mumbai',       18.95,     72.83,      0, True),
    ('singapore',     1.29,    103.85,      0, True),
    ('hong_kong',    22.30,    114.17,      0, True),
    ('shanghai',     31.23,    121.47,      0, True),
    ('busan',        35.10,    129.04,      0, False),
    ('tokyo',        35.65,    139.84,      0, True),
    ('vladivostok',  43.12,    131.89,      0, True),
    ('sydney',      -33.86,    151.21,      0, True),
    ('cape_town',   -33.92,     18.42,      0, False),
    ('lagos',         6.45,      3.39,      0, True),
    ('halifax',      44.65,    -63.57,      0, True),
    ('boston',       42.36,    -71.06,      0, True),
    ('norfolk',      36.85,    -76.29,      0, False),
    ('miami',        25.76,    -80.19,      0, True),
    ('galveston',    29.30,    -94.80,      0, True),
    ('san_francisco', 37.77,  -122.42,      0, True),
    ('seattle',      47.61,   -122.33,     10, True),
    ('valparaiso',  -33.05,    -71.62,      0, False),
    ('buenos_aires', -34.60,   -58.38,      0, True),
    ('rio',         -22.91,    -43.17,      0, True),
    ('denver',       39.74,   -104.99,   1500, True),
]
HF_RX = {'istanbul', 'piraeus', 'alexandria', 'galveston', 'miami', 'shanghai', 'hong_kong', 'valletta'}
JP424_RX = {'tokyo', 'busan', 'vladivostok', 'shanghai'}

# Subject mix of the active set (real Dalgety Bay: A 77%, E 11%, B 7%, V 2%, L 1%)
SUBJECTS = [('A', .765, 4.0), ('E', .11, .5), ('B', .07, 1.0), ('V', .02, 7.0),
            ('L', .012, 3.0), ('Z', .006, .3), ('D', .002, .5), ('C', .004, 2.0),
            ('F', .003, 2.0), ('J', .002, 2.0), ('K', .002, 2.0), ('H', .002, 2.0),
            ('G', .002, 2.0)]                       # subject, share, mean lifetime (days)
VITAL_SUBJ = [('A', .45), ('B', .3), ('D', .12), ('L', .13)]
NONLETTER = list('0123456789-.()\'&/?:')
EARTH_KM = 6371.0


# ---------------------------------------------------------------- helpers
def parse_table(path):
    rx = re.compile(r"\s*\{\s*(\d+),\s*'(.)',\s*\"([^\"]*)\",\s*\"([^\"]*)\",\s*(-?\d+),"
                    r"\s*(-?[\d.]+),\s*(-?[\d.]+),\s*(\d+),\s*(-?\d+),\s*(\d+),\s*\{([^}]*)\}")
    out = []
    for line in open(path, encoding='utf-8'):
        m = rx.match(line)
        if not m:
            continue
        g = m.groups()
        mins = [int(x) for x in g[10].split(',') if x.strip()]
        out.append(dict(idx=len(out), freq=int(g[0]), id=g[1], name=g[2], country=g[3],
                        navarea=int(g[4]), lat=float(g[5]), lon=float(g[6]),
                        range_nm=int(g[7]), active=int(g[8]), minutes=mins))
    if not out:
        sys.exit('no stations parsed from ' + path)
    return out


def gc_km(lat1, lon1, lat2, lon2):
    p1, p2 = np.radians(lat1), np.radians(lat2)
    dl = np.radians(np.asarray(lon2) - lon1)
    h = np.sin((p2 - p1) / 2) ** 2 + np.cos(p1) * np.cos(p2) * np.sin(dl / 2) ** 2
    return 2 * EARTH_KM * np.arcsin(np.sqrt(np.minimum(1.0, h)))


def gc_mid(lat1, lon1, lat2, lon2):
    p1, l1, p2, l2 = map(math.radians, (lat1, lon1, lat2, lon2))
    bx = math.cos(p2) * math.cos(l2 - l1)
    by = math.cos(p2) * math.sin(l2 - l1)
    pm = math.atan2(math.sin(p1) + math.sin(p2), math.sqrt((math.cos(p1) + bx) ** 2 + by ** 2))
    lm = l1 + math.atan2(by, math.cos(p1) + bx)
    return math.degrees(pm), (math.degrees(lm) + 540) % 360 - 180


def offset_point(lat, lon, km, bearing_deg):
    d = km / EARTH_KM
    b = math.radians(bearing_deg)
    p1, l1 = math.radians(lat), math.radians(lon)
    p2 = math.asin(math.sin(p1) * math.cos(d) + math.cos(p1) * math.sin(d) * math.cos(b))
    l2 = l1 + math.atan2(math.sin(b) * math.sin(d) * math.cos(p1), math.cos(d) - math.sin(p1) * math.sin(p2))
    return math.degrees(p2), (math.degrees(l2) + 540) % 360 - 180


def solar_elev(t_unix, lat, lon):
    """Solar elevation (deg), vectorised over t_unix.  NOAA low-precision formulae (<0.1 deg)."""
    n = np.asarray(t_unix, dtype=np.float64) / 86400.0 + 2440587.5 - 2451545.0
    L = np.radians((280.460 + 0.9856474 * n) % 360)
    g = np.radians((357.528 + 0.9856003 * n) % 360)
    lam = L + np.radians(1.915) * np.sin(g) + np.radians(0.020) * np.sin(2 * g)
    eps = np.radians(23.439 - 4e-7 * n)
    decl = np.arcsin(np.sin(eps) * np.sin(lam))
    ra = np.arctan2(np.cos(eps) * np.sin(lam), np.cos(lam))
    gmst = np.radians(((18.697374558 + 24.06570982441908 * n) % 24) * 15)
    H = gmst + math.radians(lon) - ra
    p = math.radians(lat)
    return np.degrees(np.arcsin(math.sin(p) * np.sin(decl) + math.cos(p) * np.cos(decl) * np.cos(H)))


def weighted(rng, items):
    tot = sum(w for _, w in items)
    x = rng.random() * tot
    for v, w in items:
        x -= w
        if x <= 0:
            return v
    return items[-1][0]


# ---------------------------------------------------------------- the true world
class Tx:
    __slots__ = ('truth', 'freq', 'letter', 'name', 'lat', 'lon', 'tab_lat', 'tab_lon', 'R_km',
                 'slots', 'slot_len', 'on_air', 'outage', 'busy', 'tags', 'msgs', 'navarea')


def build_world(table, a, rng):
    # merge identical table rows (same freq+letter within 5 km = one physical transmitter)
    canon = {}
    for s in table:
        for c in list(canon.values()):
            if c['freq'] == s['freq'] and c['id'] == s['id'] and \
                    gc_km(c['lat'], c['lon'], s['lat'], s['lon']) < 5:
                break
        else:
            canon[s['idx']] = s
    txs = []
    for s in canon.values():
        t = Tx()
        t.truth, t.freq, t.letter, t.name, t.navarea = s['idx'], s['freq'], s['id'], s['name'], s['navarea']
        t.tab_lat, t.tab_lon = s['lat'], s['lon']
        t.lat, t.lon = s['lat'], s['lon']
        t.tags = set()
        hf = s['freq'] > 1_000_000
        rng_nm = s['range_nm'] or (a.default_range_hf_nm if hf else a.default_range_nm)
        rng_nm = max(rng_nm, a.min_range_nm)                       # stated ranges are conservative
        t.R_km = rng_nm * 1.852 * math.exp(rng.gauss(0, 0.15))      # real power varies around the stated range
        # position errors in the TABLE (true position differs)
        u = rng.random()
        if u < a.sign_flip_frac:
            if rng.random() < 0.8 or abs(t.lat) < 1:
                t.lon = -t.lon
            else:
                t.lat = -t.lat
            t.tags.add('sign_flip')
        elif u < a.sign_flip_frac + a.pos_error_frac:
            km = math.exp(rng.uniform(math.log(25), math.log(400)))
            t.lat, t.lon = offset_point(t.lat, t.lon, km, rng.uniform(0, 360))
            t.tags.add('pos_error')
        # schedule
        mins = list(s['minutes'])
        jp = s['freq'] == 424000
        t.slot_len = 15 * 60 if jp else 10 * 60
        if not mins:
            if rng.random() < 0.75:
                base = (ord(t.letter) - 65) * 10 if 'A' <= t.letter <= 'X' else rng.randrange(24) * 10
            else:
                base = rng.randrange(24) * 10
                t.tags.add('sched_diff')
            mins = [base + 240 * k for k in range(6)]
        shift_s = rng.gauss(0, 15)                         # station clock / start jitter
        u = rng.random()
        if u < a.sched_early_frac:
            shift_s -= rng.uniform(3, 15) * 60
        elif u < a.sched_early_frac + a.sched_late_frac:
            shift_s += rng.uniform(3, 12) * 60
        elif u < a.sched_early_frac + a.sched_late_frac + a.sched_replace_frac:
            off = rng.randrange(1, 24) * 10
            mins = [(m + off) % 1440 for m in mins]
            t.tags.add('sched_diff')
        if abs(shift_s) >= 300:
            t.tags.add('sched_diff')
        if rng.random() < a.sched_drop_frac and len(mins) > 2:   # one slot of the table not used
            mins.pop(rng.randrange(len(mins)))
            t.tags.add('sched_diff')
        t.slots = sorted(((m * 60 + shift_s) % 86400) for m in mins)
        # on air?
        act = s['active']
        if act == 0:
            t.on_air = rng.random() < a.inactive_on_air_frac
            if t.on_air:
                t.tags.add('inactive_on_air')
        elif act == 1:
            t.on_air = rng.random() >= a.active_off_air_frac
        else:
            t.on_air = rng.random() < a.unknown_active_on_frac
            if t.on_air:
                t.tags.add('unknown_active')
        t.outage = None
        if rng.random() < a.outage_frac:
            ln = rng.uniform(2, min(25, max(2, a.days * 0.5)))
            st = rng.uniform(-ln * 0.5, a.days)
            t.outage = (st, st + ln)
        t.busy = min(18.0, max(0.7, math.exp(rng.gauss(math.log(a.msgs_per_slot), 0.65))))
        txs.append(t)

    # transmitters that exist but are not in the table
    mf = [t for t in txs if t.freq in (518000, 490000)]
    for k in range(a.unknown_tx):
        b = rng.choice(mf)
        t = Tx()
        t.truth = -1
        t.letter = chr(65 + rng.randrange(24))
        t.name = 'UNLISTED-%02d' % k
        t.navarea = b.navarea
        if rng.random() < a.unknown_colocated:
            # the common real case (Dalgety Bay proxy labels: Oostende/Malin Head/Split/Odesa 490 kHz):
            # a national-language service at a listed 518 kHz site, on the other channel
            t.freq = 490000 if b.freq == 518000 else 518000
            t.lat, t.lon = offset_point(b.lat, b.lon, rng.uniform(0, 5), rng.uniform(0, 360))
            t.name += '@' + b.name
        else:
            t.freq = 490000 if rng.random() < a.unknown_490 else 518000
            t.lat, t.lon = offset_point(b.lat, b.lon, rng.uniform(200, 1200), rng.uniform(0, 360))
        t.tab_lat = t.tab_lon = None
        t.R_km = a.default_range_nm * 1.852 * math.exp(rng.gauss(0, 0.2))
        t.slot_len = 600
        base = (ord(t.letter) - 65) * 10
        t.slots = sorted((((base + 240 * j) * 60 + rng.gauss(0, 20)) % 86400) for j in range(6))
        t.on_air, t.outage = True, None
        t.busy = min(12.0, max(0.7, math.exp(rng.gauss(math.log(a.msgs_per_slot * 0.7), 0.6))))
        t.tags = {'unknown_tx'}
        txs.append(t)
    return txs


def gen_traffic(t, a, rng, T0):
    """All transmissions of one transmitter: list of (start, dur, b2, serial, flag) flag 0 normal,
    1 offslot vital, 2 late_slot, 3 overrun.  Warnings persist and are repeated every slot."""
    end = T0 + a.days * 86400
    lives = [(sj, sh, lf) for sj, sh, lf in SUBJECTS]
    rates = [(sj, t.busy * sh / lf, lf) for sj, sh, lf in lives]        # arrivals per day
    serial = {sj: rng.randrange(1, 100) for sj, _, _ in SUBJECTS}

    def nxt(sj):
        serial[sj] = serial[sj] % 99 + 1
        return serial[sj]

    def newmsg(sj, t_now, life):
        med = a.dur_median_e if sj == 'E' else a.dur_median
        d = int(min(a.dur_max, max(a.dur_min, math.exp(rng.gauss(math.log(med), a.dur_sigma)))))
        return [t_now + rng.expovariate(1.0 / (life * 86400)), sj, nxt(sj), d]

    active = []
    for sj, r, lf in rates:                                   # warm start at steady state
        n = np.random.default_rng(rng.randrange(1 << 30)).poisson(r * lf)
        for _ in range(n):
            active.append(newmsg(sj, T0 - rng.random() * lf * 86400, lf))
    # event times: slots, arrivals, vitals
    slot_times = []
    d0 = int(T0 // 86400) * 86400
    day = 0
    while d0 + day * 86400 < end + 86400:
        for s in t.slots:
            ts = d0 + day * 86400 + s
            if T0 <= ts < end:
                slot_times.append(ts)
        day += 1
    out = []
    last = T0
    vit_rate = a.vital_per_day / 86400.0
    vit_t = T0 + rng.expovariate(vit_rate) if vit_rate > 0 else end + 1
    for ts in slot_times:
        if t.outage and t.outage[0] * 86400 + T0 <= ts < t.outage[1] * 86400 + T0:
            last = ts
            continue
        # arrivals since last slot
        dtd = (ts - last) / 86400.0
        for sj, r, lf in rates:
            k = 0
            lam = r * dtd
            if lam > 0:
                # poisson by inversion (small lam)
                L, p, k = math.exp(-lam), 1.0, 0
                while True:
                    p *= rng.random()
                    if p < L:
                        break
                    k += 1
            for _ in range(k):
                active.append(newmsg(sj, ts - rng.random() * (ts - last), lf))
        # unscheduled vitals between last and ts
        while vit_t < ts:
            sj = weighted(rng, VITAL_SUBJ)
            m = newmsg(sj, vit_t, 1.0)
            if rng.random() < 0.3:
                m[2] = 0                                      # serial 00 = must print
            out.append((vit_t + rng.uniform(5, 20), m[3], sj, m[2], 1))
            active.append(m)
            vit_t += rng.expovariate(vit_rate)
        last = ts
        active = [m for m in active if m[0] > ts]
        # order: vital/important subjects first then by subject letter, serial
        prio = {'D': 0, 'B': 1, 'L': 2, 'A': 3}
        active.sort(key=lambda m: (prio.get(m[1], 5), m[1], m[2]))
        cap = t.slot_len * (a.overrun_factor if rng.random() < a.overrun_p else 1.0)
        off = rng.uniform(8, 25)
        for m in active:
            if off > cap - 20:
                break
            dur = max(a.dur_min, int(m[3] + rng.gauss(0, 1.5)))
            flag = 3 if off >= t.slot_len else (2 if off >= 300 else 0)
            out.append((ts + off, dur, m[1], m[2], flag))
            off += dur + rng.uniform(2, 12)
    return out


# ---------------------------------------------------------------- propagation and decoding
def ccir_garble(rng, true_ch, nonletter_p):
    if rng.random() < a_glob.b1_missing_frac:
        return ''
    if rng.random() < nonletter_p:
        return rng.choice(NONLETTER)
    c = true_ch
    while c == true_ch:
        c = chr(65 + rng.randrange(26))
    return c


def simulate(a):
    rng = __import__('random').Random(a.seed)
    nrng = np.random.default_rng(a.seed)
    table = parse_table(a.table)
    world = build_world(table, a, rng)
    T0 = int(dt.datetime.strptime(a.start, '%Y-%m-%d').replace(tzinfo=dt.timezone.utc).timestamp())
    for t in world:
        t.msgs = gen_traffic(t, a, rng, T0) if t.on_air else []
        t.msgs = np.array([(m[0], m[1], ord(m[2]), m[3], m[4]) for m in t.msgs], dtype=np.float64) \
            if t.msgs else np.zeros((0, 5))
    rxs = RECEIVERS[:a.receivers] if a.receivers else RECEIVERS
    rows = []
    tab_by_fl = {}
    for s in table:
        tab_by_fl.setdefault((s['freq'], s['id']), []).append(s)
    for ri, (rname, rlat, rlon, inland, known) in enumerate(rxs):
        rr = np.random.default_rng([a.seed, ri])
        gain = -2.0 if rname == 'dalgety_bay' else rr.normal(0, a.rx_gain_sigma)
        cal = 0.0 if rname == 'dalgety_bay' else rr.normal(0, a.rx_cal_sigma)
        thr = a.decode_thr + rr.normal(0, 0.7)
        freqs = {518000}
        if rname == 'dalgety_bay' or rr.random() < a.rx_490_frac:
            freqs.add(490000)
        if rname in HF_RX:
            freqs.add(4209500)
        if rname in JP424_RX:
            freqs.add(424000)
        has_metrics = rname == 'dalgety_bay' or rr.random() >= a.rx_no_metrics_frac
        # receivers are hobby SDRs: online in multi-hour sessions for a fraction of the time
        duty = 0.97 if rname == 'dalgety_bay' else rr.uniform(a.rx_duty_min, a.rx_duty_max)
        start_on = bool(rr.random() < duty)
        edges, tcur, on = [], float(T0) - rr.uniform(0, 86400), start_on
        while tcur < T0 + a.days * 86400:
            edges.append(tcur)               # edges[k] starts run k; runs alternate, run 0 is start_on
            tcur += rr.exponential(a.rx_session_h * 3600 * (duty if on else (1 - duty)) / 0.5)
            on = not on
        edges = np.array(edges + [tcur])

        def online(ts, edges=edges, start_on=start_on):
            k = np.searchsorted(edges, ts, side='right') - 1
            return (k % 2 == 0) == start_on

        rx_tags = set()
        if not known:
            rx_tags.add('rx_unknown')
        if inland:
            rx_tags.add('inland')
        for f in sorted(freqs):
            cands = []          # (start, end, snr, tx, msgrow, tags)
            for t in world:
                if t.freq != f or not len(t.msgs):
                    continue
                d = float(gc_km(rlat, rlon, t.lat, t.lon))
                if d > a.max_km:
                    continue
                hf = f > 1_000_000
                mlat, mlon = gc_mid(rlat, rlon, t.lat, t.lon)
                M = t.msgs
                tm = M[:, 0] + M[:, 1] / 2
                el = solar_elev(tm, mlat, mlon)
                dark = np.clip((3.0 - el) / 15.0, 0, 1)
                dd = max(d, 15.0)
                R = t.R_km
                terrain = rr.exponential(a.path_extra_db) if d > 60 else 0.0   # land/mixed path, unmodelled terrain
                gw = a.gw_ref + 20 * math.log10(R / dd) + a.gw_att * (R - dd) / 100.0 + gain \
                    - inland * a.inland_db_per_km - terrain
                pw = 14 * math.log10(R / 500.0)
                ds = max(d, 400.0)
                if hf:
                    sw0 = a.hf_sw_ref + pw - 20 * math.log10(ds / 1700) - 0.25 * (ds - 1700) / 100 + gain
                    sw = sw0 - a.hf_day_absorb * (1 - dark)
                else:
                    sw0 = a.sw_ref + pw - 20 * math.log10(ds / 1700) - a.sw_att * (ds - 1700) / 100 + gain
                    sw = sw0 - a.day_absorb * (1 - dark)
                day_idx = ((M[:, 0] - T0) // 86400).astype(int)
                iono = rr.normal(0, a.iono_sigma, size=a.days + 2)[np.clip(day_idx, 0, a.days + 1)]
                sw = sw + iono
                p_gw, p_sw = 10 ** (gw / 10), 10 ** (sw / 10)
                tot = 10 * np.log10(p_gw + p_sw)
                swshare = p_sw / (p_gw + p_sw)
                snr = tot + rr.normal(0, 1, len(M)) * (a.fade_gw + (a.fade_sw - a.fade_gw) * swshare)
                anom = np.zeros(len(M), bool)
                if d > 1.5 * R:
                    anom = rr.random(len(M)) < a.anomalous_p
                    snr = np.where(anom, snr + rr.uniform(15, 30, len(M)), snr)
                keep = (snr > thr - 8) & online(M[:, 0])
                if not keep.any():
                    continue
                base_tags = set(t.tags) | rx_tags
                if hf:
                    base_tags.add('hf')
                if f == 424000:
                    base_tags.add('jp424')
                if 'A' <= t.letter <= 'X':
                    near = [s for s in tab_by_fl.get((f, t.letter), [])
                            if gc_km(rlat, rlon, s['lat'], s['lon']) < 3000]
                    if len(near) >= 2 or (t.truth < 0 and near):
                        base_tags.add('collision')
                for i in np.nonzero(keep)[0]:
                    tg = set(base_tags)
                    if d > R:
                        tg.add('dx')
                        if swshare[i] > 0.5 and dark[i] > 0.5:
                            tg.add('night_dx')
                    if anom[i]:
                        tg.add('anomalous_dx')
                    fl = int(M[i, 4])
                    if fl == 1:
                        tg.add('offslot')
                    elif fl == 2:
                        tg.add('late_slot')
                    elif fl == 3:
                        tg.update(('late_slot', 'overrun'))
                    cands.append([M[i, 0], M[i, 0] + M[i, 1], float(snr[i]), t, i, tg, d])
            # co-channel overlap / capture
            cands.sort(key=lambda c: c[0])
            drop = set()
            ends = []   # active (end, idx)
            for ci, c in enumerate(cands):
                ends = [(e, j) for e, j in ends if e > c[0]]
                for e, j in ends:
                    o = cands[j]
                    if o[3] is c[3] or min(c[2], o[2]) < thr - 4:
                        continue                          # an interferer far below the floor does no harm
                    diff = c[2] - o[2]
                    if abs(diff) < a.capture_db:
                        if rr.random() < 0.7:
                            drop.add(ci)
                        if rr.random() < 0.7:
                            drop.add(j)
                        c[5].add('capture'); o[5].add('capture')
                    elif diff > 0:
                        drop.add(j); c[5].add('capture'); c[2] -= 1.0
                    else:
                        drop.add(ci); o[5].add('capture'); o[2] -= 1.0
                ends.append((c[1], ci))
            for ci, c in enumerate(cands):
                if ci in drop:
                    continue
                st, en, snr, t, i, tg, d = c
                pdec = 1 / (1 + math.exp(-(snr - thr) / a.decode_width))
                if rr.random() >= pdec:
                    continue
                lowsnr = 1 / (1 + math.exp(-((thr + 3) - snr) / 1.5))
                p_g = a.garble_base + a.garble_lowsnr * lowsnr
                b1 = t.letter
                if rr.random() < p_g:
                    b1 = ccir_garble(rng, t.letter, a.garble_nonletter)
                b2 = chr(int(t.msgs[i, 2]))
                if rr.random() < p_g * 0.8:
                    b2 = ccir_garble(rng, b2, a.garble_nonletter)
                ser = int(t.msgs[i, 3])
                if rr.random() < a.serial_missing + p_g:
                    ser = -1
                if b1 != t.letter:
                    tg.add('garbled')
                    if not ('A' <= b1 <= 'X'):
                        tg.add('b1_nonletter')
                start = int(round(st + rr.uniform(0, 3)))
                dur = max(5, int(round(en - st + rr.normal(0, 1.5))))
                snr_rep = snr + cal
                if snr_rep > a.snr_knee:                      # reported SNR saturates (real v4 scale tops out ~27 dB)
                    snr_rep = a.snr_knee + (snr_rep - a.snr_knee) * 0.5
                chf = float(rr.exponential(a.chars_failed_base + 1.5 * lowsnr)) if rr.random() < 0.25 + 0.5 * lowsnr else 0.0
                rows.append((ri, rname, known, rlat, rlon, f, b1, b2, ser, start, dur,
                             snr_rep if has_metrics else None, min(100.0, chf) if has_metrics else None,
                             t.truth, tg, d, t.name))
            # noise headers
            nn = rr.poisson(a.noise_per_day * a.days * (1.0 if f in (518000, 490000) else 0.5))
            for _ in range(nn):
                tg = set(rx_tags) | {'noise'}
                b1 = chr(65 + rr.integers(24)) if rr.random() < 0.6 else ccir_garble(rng, '?', 0.7)
                if b1 and not ('A' <= b1 <= 'X'):
                    tg.add('b1_nonletter')
                b2 = chr(65 + rr.integers(26))
                ser = int(rr.integers(0, 100)) if rr.random() < 0.6 else -1
                start = T0 + int(rr.integers(0, a.days * 86400))
                if not online(np.array([start]))[0]:
                    continue
                dur = int(rr.integers(6, 60))
                snr_rep = thr + rr.normal(-1, 2) + cal
                chf = float(min(100, rr.exponential(5.0)))
                rows.append((ri, rname, known, rlat, rlon, f, b1, b2, ser, start, dur,
                             snr_rep if has_metrics else None, chf if has_metrics else None,
                             -1, tg, -1.0, 'NOISE'))
    rows.sort(key=lambda r: (r[0], r[9], r[5]))
    return world, table, rows, T0


def write(a, world, table, rows):
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w', newline='') as fh:
        w = csv.writer(fh, lineterminator='\n')
        w.writerow(['rx', 'rx_valid', 'rx_lat', 'rx_lon', 'freq_hz', 'b1', 'b2', 'serial', 'start_utc',
                    'duration_s', 'snr_db', 'chars_failed_pct', 'truth', 'tags', 'proxy_truth', 'dist_km', 'tx_name'])
        for (ri, rname, known, rlat, rlon, f, b1, b2, ser, start, dur, snr, chf, truth, tg, d, name) in rows:
            w.writerow([rname, 1 if known else 0, '%.4f' % rlat if known else '', '%.4f' % rlon if known else '',
                        f, b1, b2, ser, start, dur,
                        '' if snr is None else '%.2f' % snr, '' if chf is None else '%.1f' % chf,
                        truth, '|'.join(sorted(tg)), '', '%.0f' % d, name])
    wj = []
    for t in world:
        wj.append(dict(truth=t.truth, freq=t.freq, letter=t.letter, name=t.name,
                       lat=round(t.lat, 4), lon=round(t.lon, 4), tab_lat=t.tab_lat, tab_lon=t.tab_lon,
                       range_km=round(t.R_km), slots_s=[round(x) for x in t.slots], on_air=t.on_air,
                       outage_days=t.outage, busy=round(t.busy, 2), tags=sorted(t.tags),
                       n_tx=int(len(t.msgs))))
    with open(a.out.rsplit('.', 1)[0] + '.world.json', 'w') as fh:
        json.dump(dict(args=vars(a), transmitters=wj), fh, indent=0)


def stats(rows):
    from collections import Counter
    c = Counter()
    for r in rows:
        for tg in r[14]:
            c[tg] += 1
    n = len(rows)
    per_rx = Counter(r[1] for r in rows)
    days = max(1, (max(r[9] for r in rows) - min(r[9] for r in rows)) / 86400.0)
    per_rx = Counter({k: int(v / days) for k, v in per_rx.items()})
    sys.stderr.write('rows %d  truth-1 %d\n' % (n, sum(1 for r in rows if r[13] < 0)))
    sys.stderr.write('tags: ' + ', '.join('%s=%.2f%%' % (k, 100.0 * v / n) for k, v in c.most_common()) + '\n')
    sys.stderr.write('per rx/day: ' + ', '.join('%s=%d' % (k, v) for k, v in per_rx.most_common()) + '\n')


DEFAULTS_DOC = """All rates are tunable; defaults are tuned to the real Dalgety Bay logs where they constrain them."""
a_glob = None


def main():
    global a_glob
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog=DEFAULTS_DOC)
    A = p.add_argument
    A('--seed', type=int, default=1)
    A('--days', type=int, default=60, help='simulated days (default 60)')
    A('--start', default='2026-03-01', help='first simulated day, UTC (default 2026-03-01)')
    A('--receivers', type=int, default=0, help='use only the first N receivers (0 = all %d)' % len(RECEIVERS))
    A('--out', default=os.path.join(HERE, 'data', 'train.csv'))
    A('--table', default=TABLE)
    g = p.add_argument_group('true world vs table')
    g.add_argument('--pos-error-frac', type=float, default=0.06, help='tx whose true position is 25-400 km off (0.06)')
    g.add_argument('--sign-flip-frac', type=float, default=0.015, help='tx whose table coordinate has a flipped sign (0.015)')
    g.add_argument('--sched-early-frac', type=float, default=0.15, help='tx starting 3-15 min before the table slot (0.15; real: I,J,Q,W on 518 at Dalgety)')
    g.add_argument('--sched-late-frac', type=float, default=0.05, help='tx starting 3-12 min late (0.05)')
    g.add_argument('--sched-replace-frac', type=float, default=0.03, help='tx whose schedule is a different slot entirely (0.03)')
    g.add_argument('--sched-drop-frac', type=float, default=0.05, help='tx that skips one of its table slots (0.05)')
    g.add_argument('--inactive-on-air-frac', type=float, default=0.5, help='table active=0 stations that do transmit (0.5)')
    g.add_argument('--active-off-air-frac', type=float, default=0.05, help='table active=1 stations that are silent (0.05)')
    g.add_argument('--unknown-active-on-frac', type=float, default=0.8, help='table active=-1 stations that transmit (0.8)')
    g.add_argument('--outage-frac', type=float, default=0.10, help='tx with one 2-25 day outage (0.10)')
    g.add_argument('--unknown-tx', type=int, default=20, help='real transmitters absent from the table (20; ~10%% of real Dalgety Bay messages are proxy-labelled not-in-table)')
    g.add_argument('--unknown-colocated', type=float, default=0.7, help='share of those at a listed site on the other MF channel (0.7)')
    g.add_argument('--unknown-490', type=float, default=0.75, help='share of the remaining ones on 490 kHz (0.75)')
    g.add_argument('--default-range-nm', type=float, default=300, help='MF range when table says 0 (300)')
    g.add_argument('--default-range-hf-nm', type=float, default=900, help='4209.5 kHz range when 0 (900)')
    g = p.add_argument_group('traffic')
    g.add_argument('--msgs-per-slot', type=float, default=6.0, help='median active messages per tx per slot (6; lognormal spread; real Cullercoats ~14)')
    g.add_argument('--vital-per-day', type=float, default=0.5, help='unscheduled A/B/D/L broadcasts per tx per day (0.5)')
    g.add_argument('--overrun-p', type=float, default=0.25, help='P(slot may overrun) (0.25)')
    g.add_argument('--overrun-factor', type=float, default=1.5, help='slot capacity multiplier when overrunning (1.5)')
    g.add_argument('--dur-median', type=float, default=34, help='median message duration s (34; real Dalgety all-subject median 38)')
    g.add_argument('--dur-median-e', type=float, default=220, help='median duration of E forecasts s (220)')
    g.add_argument('--dur-sigma', type=float, default=0.7, help='lognormal sigma of duration (0.7)')
    g.add_argument('--dur-min', type=int, default=15)
    g.add_argument('--dur-max', type=int, default=900)
    g = p.add_argument_group('propagation (dB on the decoder v4 SNR scale)')
    g.add_argument('--gw-ref', type=float, default=8.0, help='groundwave SNR at the nominal range (8; stated ranges are conservative)')
    g.add_argument('--gw-att', type=float, default=1.5, help='extra groundwave loss dB/100 km (1.5)')
    g.add_argument('--path-extra-db', type=float, default=5.0, help='mean of exponential per-path extra groundwave loss (land paths) (5)')
    g.add_argument('--min-range-nm', type=float, default=200, help='floor applied to stated ranges (200; real Oostende 55 nm heard at 700 km)')
    g.add_argument('--inland-db-per-km', type=float, default=0.05, help='groundwave loss per km of inland position (0.05)')
    g.add_argument('--sw-ref', type=float, default=5.0, help='night skywave SNR at 1700 km for a 270 nm station (5)')
    g.add_argument('--sw-att', type=float, default=0.7, help='skywave loss dB/100 km beyond 1700 km (0.7)')
    g.add_argument('--day-absorb', type=float, default=35.0, help='daytime D-layer absorption of MF skywave (35)')
    g.add_argument('--hf-sw-ref', type=float, default=8.0)
    g.add_argument('--hf-day-absorb', type=float, default=8.0)
    g.add_argument('--iono-sigma', type=float, default=4.0, help='day-to-day skywave variation per path (4)')
    g.add_argument('--fade-gw', type=float, default=2.0)
    g.add_argument('--fade-sw', type=float, default=5.0)
    g.add_argument('--anomalous-p', type=float, default=0.0015, help='P(+15..30 dB enhancement) per message beyond 1.5x range (0.0015)')
    g.add_argument('--max-km', type=float, default=7000)
    g.add_argument('--capture-db', type=float, default=8.0, help='co-channel SNR difference below which both are lost (8)')
    g = p.add_argument_group('receiver / decoder')
    g.add_argument('--decode-thr', type=float, default=-3.0, help='SNR at 50%% decode probability (-3)')
    g.add_argument('--decode-width', type=float, default=1.2)
    g.add_argument('--rx-gain-sigma', type=float, default=3.0)
    g.add_argument('--rx-cal-sigma', type=float, default=2.0, help='per-receiver offset of the reported SNR (2)')
    g.add_argument('--rx-duty-min', type=float, default=0.15, help='min fraction of time a receiver is online (0.15; Dalgety Bay 0.97)')
    g.add_argument('--rx-duty-max', type=float, default=0.45, help='max online fraction (0.45)')
    g.add_argument('--rx-session-h', type=float, default=12.0, help='mean on+off cycle length / 2, hours (12)')
    g.add_argument('--rx-490-frac', type=float, default=0.6)
    g.add_argument('--rx-no-metrics-frac', type=float, default=0.1, help='receivers that report no snr/chars (0.1)')
    g.add_argument('--garble-base', type=float, default=0.003, help='P(header char corrupted) at good SNR (0.003)')
    g.add_argument('--garble-lowsnr', type=float, default=0.03, help='extra P(corrupt) near the decode floor (0.03)')
    g.add_argument('--garble-nonletter', type=float, default=0.35, help='share of corruptions giving a non-letter (0.35)')
    g.add_argument('--b1-missing-frac', type=float, default=0.03, help='share of corruptions leaving B1 empty (0.03)')
    g.add_argument('--serial-missing', type=float, default=0.015, help='P(serial unreadable) (0.015; real 1.9%%)')
    g.add_argument('--chars-failed-base', type=float, default=0.2)
    g.add_argument('--snr-knee', type=float, default=18.0, help='reported SNR above this is compressed 2:1 (18)')
    g.add_argument('--noise-per-day', type=float, default=1.0, help='noise headers per rx per freq per day (1.0)')
    a = p.parse_args()
    a_glob = a
    world, table, rows, T0 = simulate(a)
    write(a, world, table, rows)
    stats(rows)
    sys.stderr.write('wrote %s (%d rows)\n' % (a.out, len(rows)))


if __name__ == '__main__':
    main()
