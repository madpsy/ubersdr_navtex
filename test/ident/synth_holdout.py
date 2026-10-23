#!/usr/bin/env python3
"""Independent, adversarial HELD-OUT generator for the NAVTEX transmitter-identification harness.

Written separately from synth.py (shares nothing but the CSV format) so that an algorithm
tuned against synth.py's world cannot pass here by having learnt that generator's quirks.
The world is modelled differently on purpose, while staying realistic:

  * Propagation: groundwave in a P.368-like form (sea curve with Millington mixed-path
    correction for land at the receiver end and land crossed on the way, per-receiver
    ground constants); night skywave in a P.1147-like form (slant distance, geomagnetic-
    latitude loss factor, D-layer absorption from the solar elevation at the control
    points, seasonal term, night-to-night and per-path variability); atmospheric noise
    rising at night and in the tropics; geomagnetic-storm days that depress high-latitude
    skywave for 1-3 days; sporadic very-long-distance nights; Rician message fading;
    co-channel interference as true SINR (power sum), not a capture threshold.
  * Three seasons: northern-winter, northern-summer and autumn-equinox epochs, same receivers
    (train is a single spring window).
  * Timing: per-station clock offsets up to +-60 s, early/late stations, adjacent-slot
    schedule errors, sequential "chains" (as the Swedish H/I/J really do), late runs,
    long overruns, vital warnings broadcast on receipt AND repeated off-slot, stations
    without a schedule (Taganrog-style coastal warnings).
  * Table errors different from synth.py's: a few stations transmitting with a different
    letter than the table (Arkhangelsk really sends L), latitude sign flips, wrong site,
    the real schedule errors (Haifa, Odesa, Irish Q/W, Saudanes 490, Sydney NS 490),
    decommissioned stations and stations that appear mid-run; ~25 transmitters absent
    from the table (the 490 kHz national services seen at Dalgety Bay plus adversarial
    ones whose letter matches a distant table station).
  * Receivers: none shared with train; many deliberately placed on the bisector of two
    same-freq same-letter table stations, far-inland receivers, ships on passage
    (position moves between messages), unknown locations, misconfigured locations,
    a receiver reporting the legacy (+30 dB) SNR scale, receivers without metrics,
    cross-channel leakage.
  * Decoder: B1 corruption calibrated to the real logs (1.7% below 0 dB, ~0.4% at
    0-5 dB, ~0 above), 40% of it to the letter whose slot is adjacent in time.

    python3 -I test/ident/synth_holdout.py --out test/ident/data/holdout.csv
    python3 -I test/ident/synth_holdout.py --calib      # print Dalgety-Bay-like statistics only

Columns as synth.py / real_to_csv.py.  Tags: synth.py's vocabulary plus
  ship, storm, sporadic_dx, equinox, clock_offset, late_start, chain, adj_shift, letter_diff,
  garble_adjacent, vital_repeat, long_overrun, rx_pos_wrong, snr_legacy, leakage,
  equidistant, winter, summer, unsched, decommissioned, new_station, wrong_site.
"""
import argparse
import csv
import datetime as dt
import json
import math
import os
import re
import sys
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, '..', '..', 'src', 'navtex_stations.h')
CALIB = os.path.join(os.environ.get('NAVTEX_IDENT_WORK', os.path.join(HERE, 'work')), 'ident', 'calib.json')
R_E = 6371.0
D2R = math.pi / 180

# ---------------------------------------------------------------- table
_RX = re.compile(r"\{\s*(\d+),\s*'(.)',\s*\"([^\"]*)\",\s*\"([^\"]*)\",\s*(-?\d+),\s*(-?[\d.]+),\s*(-?[\d.]+),"
                 r"\s*(-?\d+),\s*(-?\d+),\s*(\d+),\s*\{([^}]*)\}\s*\}")


def load_table(path):
    out = []
    for m in _RX.finditer(open(path, encoding='utf-8').read()):
        g = m.groups()
        out.append(dict(idx=len(out), freq=int(g[0]), id=g[1], name=g[2], country=g[3], navarea=int(g[4]),
                        lat=float(g[5]), lon=float(g[6]), range_nm=int(g[7]), active=int(g[8]),
                        minutes=[int(x) for x in g[10].split(',') if x.strip()]))
    return out


def hav(la1, lo1, la2, lo2):
    la1, lo1, la2, lo2 = (np.asarray(x, float) * D2R for x in (la1, lo1, la2, lo2))
    a = np.sin((la2 - la1) / 2) ** 2 + np.cos(la1) * np.cos(la2) * np.sin((lo2 - lo1) / 2) ** 2
    return 2 * R_E * np.arcsin(np.sqrt(np.clip(a, 0, 1)))


def hav1(la1, lo1, la2, lo2):
    return float(hav(la1, lo1, la2, lo2))


def gc_point(la1, lo1, la2, lo2, f):
    """Point a fraction f along the great circle (vectorised)."""
    la1, lo1, la2, lo2 = (np.asarray(x, float) * D2R for x in (la1, lo1, la2, lo2))
    d = 2 * np.arcsin(np.sqrt(np.clip(np.sin((la2 - la1) / 2) ** 2 +
                                      np.cos(la1) * np.cos(la2) * np.sin((lo2 - lo1) / 2) ** 2, 0, 1)))
    sd = np.where(d < 1e-9, 1.0, np.sin(d))
    A = np.where(d < 1e-9, 1 - f, np.sin((1 - f) * d) / sd)
    B = np.where(d < 1e-9, f, np.sin(f * d) / sd)
    x = A * np.cos(la1) * np.cos(lo1) + B * np.cos(la2) * np.cos(lo2)
    y = A * np.cos(la1) * np.sin(lo1) + B * np.cos(la2) * np.sin(lo2)
    z = A * np.sin(la1) + B * np.sin(la2)
    return np.arctan2(z, np.hypot(x, y)) / D2R, np.arctan2(y, x) / D2R


def offset_point(lat, lon, km, brg_deg):
    la, lo, b, dd = lat * D2R, lon * D2R, brg_deg * D2R, km / R_E
    la2 = math.asin(math.sin(la) * math.cos(dd) + math.cos(la) * math.sin(dd) * math.cos(b))
    lo2 = lo + math.atan2(math.sin(b) * math.sin(dd) * math.cos(la), math.cos(dd) - math.sin(la) * math.sin(la2))
    return la2 / D2R, ((lo2 / D2R + 540) % 360) - 180


def solar(t, lat, lon):
    """Solar elevation (deg) and declination (deg); t unix seconds (arrays ok)."""
    n = (np.asarray(t, float) - 946728000.0) / 86400.0
    L = (280.46 + 0.9856474 * n) % 360
    g = (357.528 + 0.9856003 * n) * D2R
    lam = (L + 1.915 * np.sin(g) + 0.020 * np.sin(2 * g)) * D2R
    eps = (23.439 - 4e-7 * n) * D2R
    dec = np.arcsin(np.sin(eps) * np.sin(lam))
    ra = np.arctan2(np.cos(eps) * np.sin(lam), np.cos(lam))
    gmst = (18.697374558 + 24.06570982441908 * n) % 24
    ha = (gmst * 15 + np.asarray(lon, float)) * D2R - ra
    la = np.asarray(lat, float) * D2R
    el = np.arcsin(np.clip(np.sin(la) * np.sin(dec) + np.cos(la) * np.cos(dec) * np.cos(ha), -1, 1))
    return el / D2R, dec / D2R


GM_LAT, GM_LON = 80.65, -72.68


def geomag_lat(lat, lon):
    la, lo = np.asarray(lat, float) * D2R, np.asarray(lon, float) * D2R
    s = np.sin(la) * math.sin(GM_LAT * D2R) + np.cos(la) * math.cos(GM_LAT * D2R) * np.cos(lo - GM_LON * D2R)
    return np.arcsin(np.clip(s, -1, 1)) / D2R


def sig(x):
    return 1.0 / (1.0 + np.exp(-np.asarray(x, float)))


def stable_seed(*parts):
    return zlib.crc32('|'.join(str(p) for p in parts).encode()) & 0x7fffffff


# ---------------------------------------------------------------- receivers
# (name, lat, lon, inland_km, flags)  flags: U=location unknown, 5=518 only, H=+4209.5, J=+424,
#   W=noisy urban site, L=cross-channel leakage, G=legacy SNR scale, N=no metrics,
#   F=full-time (24/7), Pname=configured position differs (see POS_WRONG)
RECEIVERS = [
    # NW Europe, coasts
    ('aberdeen',        57.150,  -2.090,    0, 'FL'),
    ('lerwick',         60.155,  -1.145,    0, 'G'),
    ('stockholm',       59.330,  18.070,    5, 'U'),
    ('riga',            56.950,  24.100,   10, ''),
    ('oulu',            65.010,  25.470,    3, '5'),
    ('tromso',          69.650,  18.960,    0, 'F'),
    ('kirkenes',        69.730,  30.050,    0, ''),
    ('bordeaux',        44.840,  -0.580,   60, 'W'),
    ('bilbao',          43.260,  -2.930,    5, ''),
    ('turku_cfg',       60.450,  22.270,    0, 'P'),
    # far inland Europe / Asia / America
    ('warsaw',          52.230,  21.000,  250, 'W'),
    ('prague',          50.080,  14.430,  350, ''),
    ('vienna',          48.200,  16.370,  450, '5'),
    ('zurich',          47.370,   8.540,  500, 'W'),
    ('moscow',          55.750,  37.620,  600, 'W'),
    ('ulaanbaatar',     47.900, 106.900, 1500, ''),
    ('kansas_city',     39.100, -94.580, 1000, 'U'),
    ('quebec',          46.810, -71.210,   60, ''),
    # Mediterranean / Black Sea / Middle East
    ('palma',           39.570,   2.650,    0, 'F'),
    ('tunis',           36.800,  10.180,    5, ''),
    ('tangier',         35.770,  -5.800,    0, ''),
    ('dubrovnik',       42.650,  18.090,    0, 'L'),
    ('bari_cfg',        41.120,  16.870,    0, 'P'),
    ('thessaloniki',    40.640,  22.940,    0, 'H'),
    ('bodrum',          37.030,  27.430,    0, 'UH'),
    ('batumi',          41.640,  41.630,    0, ''),
    ('beirut',          33.900,  35.500,    0, 'N'),
    ('port_said',       31.260,  32.300,    0, 'UH'),
    ('doha',            25.290,  51.530,    0, ''),
    ('port_sudan',      19.600,  37.220,    0, '5'),
    # Atlantic islands, Americas
    ('funchal',         32.650, -16.900,    0, ''),
    ('ponta_delgada',   37.740, -25.670,    0, 'F'),
    ('tasiilaq',        65.610, -37.640,    0, 'N'),
    ('st_pierre',       46.780, -56.180,    0, ''),
    ('chicago',         41.880, -87.630,    0, 'W'),
    ('nassau',          25.060, -77.340,    0, ''),
    ('wilmington_nc',   34.210, -77.890,   15, 'H'),
    ('kingston_jm',     17.970, -76.790,    0, ''),
    ('vancouver',       49.280,-123.120,    0, ''),
    ('los_angeles',     33.740,-118.270,    0, 'W'),
    ('anchorage',       61.220,-149.900,    0, ''),
    ('arica',          -18.480, -70.320,    0, 'N'),
    ('montevideo',     -34.900, -56.160,    0, 'U'),
    ('puerto_madryn',  -42.770, -65.040,    0, ''),
    # Indian Ocean, Asia, Oceania
    ('colombo',          6.930,  79.850,    0, ''),
    ('maputo',         -25.970,  32.570,    0, ''),
    ('jeju',            33.500, 126.530,    0, 'J'),
    ('osaka',           34.690, 135.500,    0, 'UJ'),
    ('cebu',            10.300, 123.900,    0, ''),
    ('xiamen',          24.480, 118.090,    0, 'H'),
    ('hanoi',           21.030, 105.850,   90, 'W'),
    ('surabaya',        -7.250, 112.750,    0, ''),
    ('perth',          -31.950, 115.860,   10, ''),
    # on the bisector of two same-freq same-letter table stations (collision by construction)
    ('eq_tipperary',    52.690,  -7.800,   60, ''),     # 518 D Coruna / Torshavn
    ('eq_luxembourg',   49.880,   6.040,  300, ''),     # 518 Q Malin Head / Split
    ('eq_kosice',       49.470,  21.910,  450, ''),     # 518 J Gislovshammar / Varna
    ('eq_lviv',         49.910,  24.310,  600, ''),     # 518 H Bjuroklubb / Iraklio
    ('eq_santander',    43.420,  -3.720,    3, 'F'),    # 490 T Niton / Tarifa
    ('eq_nis',          43.850,  22.400,  300, ''),     # 518 P Netherlands / Haifa
    ('eq_nantes',       47.810,  -1.490,   40, ''),     # 518 W Valentia / Cross La Garde
    ('eq_youghal',      51.900,  -7.900,    2, ''),     # 518 X Grindavik / Cabo de la Nao
    ('eq_olomouc',      49.680,  17.290,  500, 'U'),    # 518 L Rogaland / Limnos
    ('eq_macon',        46.030,   4.520,  300, ''),     # 490 I Niton / La Maddalena
    ('eq_chisinau',     47.880,  29.320,  120, ''),     # 518 F Antalya / Tallinn
    ('eq_amami',        28.380, 129.500,    0, 'J'),    # 518 J Manila / Otaru
    ('eq_taichung',     24.150, 120.680,   20, ''),     # 518 P Chilung / Kaohsiung
    ('eq_garda',        45.580,  10.350,  120, ''),     # 518 K Niton / Kerkyra
    ('eq_annecy',       45.740,   6.380,  200, 'U'),    # 518 O Portpatrick / Malta
]
POS_WRONG = {'turku_cfg': (22.27, 60.45),        # lat/lon swapped in the config
             'bari_cfg': (50.11, 8.68)}           # owner's home (Frankfurt) left in the config

# ships: name -> (waypoints, speed km/day, flags)
SHIPS = {
    'ship_channel_med': ([(51.0, 1.6), (49.9, -3.0), (48.6, -6.0), (43.8, -9.8), (38.5, -9.6), (36.0, -6.2),
                          (36.4, -2.0), (37.8, 4.0), (37.5, 10.0), (35.9, 14.6), (34.5, 24.0), (31.4, 32.3)], 600, ''),
    'ship_baltic':      ([(51.95, 3.9), (53.8, 5.0), (55.5, 7.3), (57.7, 10.7), (57.2, 11.5), (55.5, 12.9),
                          (55.1, 14.6), (55.8, 18.0), (57.5, 20.0), (59.5, 22.5), (60.0, 25.0)], 450, ''),
    'ship_transat':     ([(49.9, -5.5), (47.0, -15.0), (43.0, -25.0), (41.0, -40.0), (42.5, -55.0),
                          (44.5, -63.0), (40.4, -73.5)], 650, ''),
    'ship_suez_gulf':   ([(31.3, 32.3), (29.9, 32.6), (27.0, 34.5), (20.0, 38.5), (13.0, 43.0), (12.5, 47.0),
                          (14.5, 53.0), (22.0, 60.0), (26.4, 56.6), (26.5, 52.0), (25.3, 51.6)], 600, 'H'),
    'ship_china':       ([(1.25, 103.9), (5.0, 105.5), (10.0, 110.0), (16.0, 113.0), (22.0, 114.3), (25.0, 120.0),
                          (30.5, 123.0), (33.0, 126.0), (35.0, 129.2), (34.0, 132.5), (34.5, 138.5), (35.3, 139.8)],
                         650, 'UJ'),
    'ship_chile':       ([(-12.1, -77.3), (-18.0, -72.5), (-23.6, -70.6), (-33.0, -71.8), (-37.0, -73.5),
                          (-41.8, -73.8), (-50.0, -75.5), (-53.2, -70.9)], 550, ''),
    'ship_north_atl':   ([(60.0, -4.0), (62.0, -6.8), (64.0, -14.0), (66.5, -18.0), (66.6, -24.0), (63.5, -23.0),
                          (61.0, -14.0), (60.0, -4.0)], 350, ''),
    'rig_biscay':       ([(45.58, -3.86)], 0, 'F'),     # 518 G Cullercoats / Tarifa bisector, at sea
    'rig_hebrides':     ([(57.49, -10.14)], 0, 'F'),    # 490 E Saudanes / Cross Corsen bisector
    'rig_norwegian_sea': ([(63.49, -0.65)], 0, ''),     # 518 A Svalbard / Cross Corsen bisector
    'ship_mozambique':  ([(-28.29, 39.31), (-26.0, 35.0), (-29.9, 31.2), (-34.0, 25.7), (-34.2, 18.4)], 400, ''),
}

# missing (not-in-table) transmitters seen in the real Dalgety Bay logs: name, freq, letter, lat, lon, schedule
REAL_MISSING = [
    ('Oostende 490 (NL language)', 490000, 'B', 51.1823, 2.8065, 'canon', 1),
    ('Malin Head 490', 490000, 'A', 55.3633, -7.3392, 'early5', 0),
    ('Taganrog coastal 490', 490000, 'O', 47.2, 38.9, 'unsched', 0),
    ('Odesa 490', 490000, 'X', 46.3776, 30.7482, 'canon', 0),
    ('Split 490', 490000, 'F', 43.5, 16.44, 'canon', 0),
    ('Piombino 490', 490000, 'N', 42.93, 10.53, 'canon', 0),
]

SUBJ_OTHER = 'CFGHJKVZ'
JUNK = '-9ZY(2?Z'

# tx flag bits for events
F_OFFSLOT, F_VREP, F_LATESTART, F_UNSCHED = 1, 2, 4, 8


class Tx(object):
    pass


class World(object):
    pass


# ---------------------------------------------------------------- world
def build_world(table, a, rng):
    W = World()
    W.table = table
    # duplicate rows (same freq, letter, site within 5 km) are one transmitter
    canon = {}
    for s in table:
        for c in canon.values():
            if c['freq'] == s['freq'] and c['id'] == s['id'] and hav1(c['lat'], c['lon'], s['lat'], s['lon']) < 5:
                s['dup_of'] = c['idx']
                break
        else:
            canon[s['idx']] = s
    W.dup_of = {s['idx']: s.get('dup_of', s['idx']) for s in table}
    txs = []
    # Real schedule quirks the station table does not carry, keyed by station
    # (never by row number: rows move when the table is corrected).  Haifa P
    # and Odesa C used to be here too; the table now has their true slots.
    by_key = {(s['freq'], s['id'], s['name']): s['idx'] for s in table}
    special_sched = {by_key[k]: off for k, off in (
        ((518000, 'Q', 'Malin Head'), -240), ((518000, 'W', 'Valentia'), -240)) if k in by_key}
    swedish = [by_key[k] for k in ((518000, 'H', 'Bjuröklubb'), (518000, 'I', 'Grimeton'),
                                   (518000, 'J', 'Gislövshammar')) if k in by_key]
    for s in canon.values():
        t = Tx()
        t.truth, t.freq, t.letter, t.tab_letter = s['idx'], s['freq'], s['id'], s['id']
        t.name, t.country, t.navarea = s['name'], s['country'], s['navarea']
        t.lat, t.lon, t.tab_lat, t.tab_lon = s['lat'], s['lon'], s['lat'], s['lon']
        t.range_nm, t.active = s['range_nm'], s['active']
        t.tags = set()
        t.unknown = False
        t.hf = s['freq'] > 1000000
        L = ord(s['id']) - 65
        canon_min = [((L * 10) + 240 * k) % 1440 for k in range(6)]
        mins = s['minutes'] if s['minutes'] else (canon_min if rng.random() < 0.8 else
                                                  [((L * 10 + rng.choice([-10, 10, 20])) + 240 * k) % 1440 for k in range(6)])
        if not s['minutes'] and mins != canon_min:
            t.tags.add('sched_diff')
        t.nominal_s = sorted(m * 60 for m in mins)
        t.offset_s = 0.0
        t.chain = None
        # real, observed schedule errors
        if s['idx'] in special_sched:
            t.offset_s = special_sched[s['idx']]
            t.tags.add('sched_diff')
            if abs(t.offset_s) >= 600:
                t.tags.add('adj_shift')
                t.nominal_s = sorted(((x + t.offset_s) % 86400) for x in t.nominal_s)
                t.offset_s = 0
        elif s['idx'] == 24:          # Saudanes 490 E: table 740 should be 760
            t.nominal_s = sorted(m * 60 for m in [40, 280, 520, 760, 1000, 1240])
            t.tags.add('sched_diff')
        elif s['idx'] == 104:         # Sydney NS 490 J: table x:55, observed canonical J
            t.nominal_s = [((9 * 10) + 240 * k) % 1440 * 60 for k in range(6)]
            t.tags.add('sched_diff')
        elif s['idx'] in swedish:
            pass
        else:
            u = rng.random()
            if u < a.adj_shift_frac:
                sh = rng.choice([-600, 600])
                t.nominal_s = sorted(((x + sh) % 86400) for x in t.nominal_s)
                t.tags.update(('sched_diff', 'adj_shift'))
            elif u < a.adj_shift_frac + a.early_frac:
                t.offset_s = -rng.uniform(120, 420)
                t.tags.add('sched_diff')
            elif u < a.adj_shift_frac + a.early_frac + a.late_frac:
                t.offset_s = rng.uniform(120, 480)
                t.tags.update(('sched_diff', 'late_start'))
            elif u < a.adj_shift_frac + a.early_frac + a.late_frac + 0.03:
                t.nominal_s = t.nominal_s[::2] if len(t.nominal_s) >= 4 else t.nominal_s
                t.tags.add('sched_diff')
            elif u < a.adj_shift_frac + a.early_frac + a.late_frac + 0.05:
                t.nominal_s = sorted(set(t.nominal_s + [(x + 7200) % 86400 for x in t.nominal_s[:2]]))
                t.tags.add('sched_diff')
        # clock offset
        t.clock_s = rng.uniform(-60, 60) if rng.random() < 0.15 else float(np.clip(rng.normal(0, 15), -60, 60))
        if abs(t.clock_s) >= 30:
            t.tags.add('clock_offset')
        # position errors (true position differs from the table)
        u = rng.random()
        if u < 0.04:
            t.lat, t.lon = offset_point(t.lat, t.lon, rng.uniform(20, 150), rng.uniform(0, 360))
            t.tags.add('pos_error')
        elif u < 0.055:
            t.lat, t.lon = offset_point(t.lat, t.lon, rng.uniform(300, 900), rng.uniform(0, 360))
            t.tags.update(('pos_error', 'wrong_site'))
        # activity
        if t.active == 1:
            t.on_air = rng.random() > 0.04
        elif t.active == 0:
            t.on_air = rng.random() < 0.6
            if t.on_air:
                t.tags.add('inactive_on_air')
        else:
            t.on_air = rng.random() < 0.75
            t.tags.add('unknown_active')
        t.power_db = rng.normal(0, 3.0) + (2 if t.range_nm >= 400 else 0) - (3 if 0 < t.range_nm <= 150 else 0) \
            + (6 if t.hf else 0)
        t.land_km = rng.uniform(0, 3) if rng.random() < 0.8 else rng.uniform(5, 60)
        t.busy = float(np.clip(rng.lognormal(math.log(3.0), 0.6), 0.3, 16)) * (1.3 if t.navarea == 1 else 1.0)
        t.skip_p = 0.3 if rng.random() < 0.05 else 0.02
        t.letter_alias = None
        txs.append(t)
    byidx = {t.truth: t for t in txs}
    # Swedish chain (real): H at its slot, I and J back to back after it
    for k, i in enumerate(swedish):
        if k:
            byidx[i].chain = (byidx[swedish[k - 1]], k)
            byidx[i].nominal_s = list(byidx[swedish[0]].nominal_s)
            byidx[i].offset_s = 0
            byidx[i].tags.update(('sched_diff', 'chain'))
    # random chains among same-country same-freq stations (2 groups)
    groups = {}
    for t in txs:
        if t.chain is None and t.truth not in swedish and len(t.nominal_s) >= 3 and not t.hf:
            groups.setdefault((t.country, t.freq), []).append(t)
    cands = [g for g in groups.values() if len(g) >= 2]
    rng.shuffle(cands)
    for g in cands[:2]:
        g = sorted(g, key=lambda x: x.nominal_s[0])[:3]
        for k in range(1, len(g)):
            g[k].chain = (g[k - 1], k)
            g[k].nominal_s = list(g[0].nominal_s)
            g[k].offset_s = 0
            g[k].tags.update(('sched_diff', 'chain'))
    # stations that transmit with another letter than the table (Arkhangelsk 518 'F' really sends 'L')
    def reletter(t, new):
        t.letter = new
        L = ord(new) - 65
        t.nominal_s = [((L * 10) + 240 * k) % 1440 * 60 for k in range(6)]
        t.offset_s = 240 if t.truth == 188 else t.offset_s
        t.chain = None
        t.tags.update(('letter_diff', 'sched_diff'))
    reletter(byidx[188], 'L')
    pool = [t for t in txs if t.active == 1 and not t.hf and t.chain is None and t.truth not in swedish
            and 'adj_shift' not in t.tags and t.freq != 424000]
    for t in rng.choice(pool, 2, replace=False):
        nl = chr(ord(t.letter) + (1 if t.letter < 'X' else -1))
        reletter(t, nl)
    # latitude sign flips in the table (true position is the mirror)
    flip_pool = [t for t in txs if abs(t.lat) > 8 and 'pos_error' not in t.tags]
    for t in rng.choice(flip_pool, 2, replace=False):
        t.lat = -t.lat
        t.tags.add('sign_flip')
    # unknown (not-in-table) transmitters
    def mk_unknown(name, freq, letter, lat, lon, sched, early=0.0):
        t = Tx()
        t.truth, t.freq, t.letter, t.tab_letter, t.name = -1, freq, letter, None, name
        t.country, t.navarea = 'UNK', 0
        t.lat, t.lon, t.tab_lat, t.tab_lon = lat, lon, None, None
        t.range_nm, t.active, t.on_air, t.unknown, t.hf = 0, 1, True, True, freq > 1000000
        t.tags = {'unknown_tx'}
        L = ord(letter) - 65
        t.nominal_s = [((L * 10) + 240 * k) % 1440 * 60 for k in range(6)]
        t.offset_s = early
        t.unsched = sched == 'unsched'
        if t.unsched:
            t.tags.add('unsched')
        t.chain = None
        t.clock_s = float(np.clip(rng.normal(0, 15), -60, 60))
        t.power_db = rng.normal(0, 3.0)
        t.land_km = rng.uniform(0, 3)
        t.busy = float(np.clip(rng.lognormal(math.log(2.5), 0.6), 0.3, 14))
        t.skip_p = 0.02
        t.letter_alias = None
        return t
    for (nm, f, L, la, lo, sch, _) in REAL_MISSING:
        # The table may since have added it (tools/gen_navtex_stations.py
        # ADDITIONS): then it is a listed transmitter, not an unknown one.
        if any(s['freq'] == f and s['id'] == L and hav1(s['lat'], s['lon'], la, lo) < 100 for s in table):
            continue
        u = mk_unknown(nm, f, L, la, lo, sch, -300.0 if sch == 'early5' else 0.0)
        if nm.startswith('Malin'):
            u.letter_alias = 'Q'          # its gale warnings go out as B1 = Q (real)
        txs.append(u)
    sites518 = [t for t in txs if t.freq == 518000 and not t.unknown]
    used490 = {(t.letter, round(t.lat), round(t.lon)) for t in txs if t.freq == 490000}
    n_extra = a.unknown_tx
    for k in range(n_extra):
        if k < int(n_extra * 0.6):          # national 490 service at a 518 site
            for _ in range(50):
                s = sites518[rng.integers(len(sites518))]
                if not any(abs(s.lat - x.lat) < 0.5 and abs(s.lon - x.lon) < 0.5 for x in txs if x.freq == 490000):
                    break
            far = [x for x in table if x['freq'] == 490000 and 1000 < hav1(x['lat'], x['lon'], s.lat, s.lon) < 4000]
            if far and rng.random() < 0.5:
                L = far[rng.integers(len(far))]['id']
            else:
                L = s.letter if rng.random() < 0.5 else chr(65 + rng.integers(24))
            txs.append(mk_unknown('UNK490@' + s.name, 490000, L, s.lat, s.lon, 'canon'))
        elif k < int(n_extra * 0.85):        # extra 518 site whose letter matches a distant table station
            s = sites518[rng.integers(len(sites518))]
            la, lo = offset_point(s.lat, s.lon, rng.uniform(80, 400), rng.uniform(0, 360))
            far = [x for x in table if x['freq'] == 518000 and 800 < hav1(x['lat'], x['lon'], la, lo) < 3000]
            L = far[rng.integers(len(far))]['id'] if far else s.letter
            txs.append(mk_unknown('UNK518@near ' + s.name, 518000, L, la, lo, 'canon'))
        else:                                # unscheduled coastal-warning service
            s = sites518[rng.integers(len(sites518))]
            la, lo = offset_point(s.lat, s.lon, rng.uniform(50, 300), rng.uniform(0, 360))
            txs.append(mk_unknown('UNKcoastal@near ' + s.name, rng.choice([490000, 518000]),
                                  chr(65 + rng.integers(24)), la, lo, 'unsched'))
    for t in txs:
        if not hasattr(t, 'unsched'):
            t.unsched = False
    W.txs = txs
    W.byidx = byidx
    return W


def epoch_mods(W, ep_idx, ep_start, days, rng):
    """Per-epoch on-air changes: outages, decommissioning, new stations."""
    T0, T1 = ep_start, ep_start + days * 86400
    for t in W.txs:
        t.ep_off = []
        if rng.random() < 0.08:
            a0 = rng.uniform(T0 - 5 * 86400, T1)
            t.ep_off.append((a0, a0 + rng.uniform(1, 12) * 86400))
    known = [t for t in W.txs if not t.unknown and t.on_air and t.active == 1]
    unk = [t for t in W.txs if t.unknown]
    if ep_idx == 1:
        for t in rng.choice(known, 2, replace=False):
            a0 = rng.uniform(T0 + 0.3 * (T1 - T0), T0 + 0.7 * (T1 - T0))
            t.ep_off.append((a0, T1 + 1))
            t.tags.add('decommissioned')
    if ep_idx == 0:
        for t in rng.choice(unk, 2, replace=False):
            a0 = rng.uniform(T0 + 0.3 * (T1 - T0), T0 + 0.7 * (T1 - T0))
            t.ep_off.append((T0 - 30 * 86400, a0))
            t.tags.add('new_station')


def gen_events(t, T0, T1, rng, chain_ends, month_winter):
    """All messages transmitter t sends in [T0, T1). Returns dict of numpy arrays."""
    ev = []          # (t0, dur, b2, serial, flags, nominal)
    bursts_end = {}
    if not t.on_air:
        return ev, bursts_end
    off = list(t.ep_off)

    def offair(x):
        for a0, a1 in off:
            if a0 <= x < a1:
                return True
        return False

    serial = {c: int(rng.integers(1, 100)) for c in 'ABCDEFGHJKLVZ'}

    def nxt(c):
        serial[c] = serial[c] % 99 + 1
        return serial[c]

    def dur(c):
        if c == 'E':
            return float(np.clip(rng.lognormal(math.log(200), 0.6), 30, 1200))
        if c in 'BD':
            return float(np.clip(rng.lognormal(math.log(55), 0.6), 15, 600))
        return float(np.clip(rng.lognormal(math.log(36), 1.15), 13, 1200))

    burn = T0 - 8 * 86400
    winter = month_winter(t.lat)          # +1 local winter, -1 local summer, 0 tropics
    rateA = t.busy * 0.8 / 3.0
    rateB = 0.25 * (1.5 if abs(t.lat) > 45 else 0.6) * (1 + 0.6 * winter)
    arrivals = []
    for c, rate, life in (('A', rateA, None), ('L', t.busy * 0.01, None), ('B', rateB, 'B'),
                          ('D', 0.02, 'D'), ('O', 0.15, None)):
        n = rng.poisson(rate * (T1 - burn) / 86400)
        for x in np.sort(rng.uniform(burn, T1, n)):
            cc = SUBJ_OTHER[rng.integers(len(SUBJ_OTHER))] if c == 'O' else c
            if cc == 'A':
                lf = rng.exponential(3.0) * 86400
            elif cc == 'B':
                lf = rng.uniform(6, 24) * 3600
            elif cc == 'D':
                lf = rng.uniform(4, 24) * 3600
            else:
                lf = rng.uniform(1, 3) * 86400
            arrivals.append([float(x), cc, 0, dur(cc), float(x) + lf])
    arrivals.sort(key=lambda r: r[0])
    for r in arrivals:
        r[2] = nxt(r[1])
    # unscheduled services: random night-time bursts only
    if t.unsched:
        for day in range(int((T1 - T0) / 86400) + 1):
            for _ in range(rng.poisson(3)):
                # local night: hours 18..06 local
                lh = (rng.uniform(18, 30)) % 24
                x = T0 + day * 86400 + ((lh - t.lon / 15) % 24) * 3600
                if not (T0 <= x < T1) or offair(x):
                    continue
                cur = x
                for _ in range(int(rng.integers(1, 4))):
                    d = dur('A')
                    ev.append((cur, d, 'A', nxt('A'), F_UNSCHED, float('nan')))
                    cur += d + 6
        return ev, bursts_end
    # vital / on-receipt broadcasts
    p_rx = {'B': 0.55, 'D': 0.9, 'A': 0.03, 'L': 0.05}
    for r in arrivals:
        if r[1] in p_rx and T0 <= r[0] < T1 and rng.random() < p_rx[r[1]]:
            x = r[0] + rng.uniform(30, 600)
            if not offair(x):
                ev.append((x, r[3], r[1], r[2], F_OFFSLOT, float('nan')))
            if rng.random() < 0.45:
                x2 = x + rng.uniform(20, 90) * 60
                if x2 < T1 and not offair(x2):
                    ev.append((x2, r[3], r[1], r[2], F_OFFSLOT | F_VREP, float('nan')))
    e_slots = set(rng.choice(len(t.nominal_s), min(2, len(t.nominal_s)), replace=False).tolist())
    ai = 0
    active = []
    nd = int(math.ceil((T1 - T0) / 86400)) + 1
    for day in range(-1, nd):
        for si, ns in enumerate(t.nominal_s):
            nominal = T0 + day * 86400 + ns
            if nominal < T0 - 3600 or nominal >= T1:
                continue
            while ai < len(arrivals) and arrivals[ai][0] <= nominal:
                active.append(arrivals[ai])
                ai += 1
            active = [w for w in active if w[4] > nominal]
            if offair(nominal) or rng.random() < t.skip_p:
                continue
            msgs = sorted(active, key=lambda w: ('0' if w[1] in 'BD' else '1' if w[1] in 'AL' else '2', w[2]))
            msgs = [(w[1], w[2], w[3]) for w in msgs]
            if si in e_slots and rng.random() < 0.9:
                for _ in range(1 + int(rng.random() < 0.3)):
                    msgs.append(('E', nxt('E'), dur('E')))
            msgs = msgs[:20]
            flags = 0
            if t.chain is not None:
                lead_end = chain_ends.get((id(t.chain[0]), nominal))
                start = (lead_end if lead_end is not None else nominal + rng.uniform(11, 20)) + rng.uniform(6, 20)
            else:
                start = nominal + t.offset_s + t.clock_s + rng.uniform(11, 20)
                u = rng.random()
                if u < 0.04:
                    start += rng.uniform(60, 600)
                    flags |= F_LATESTART
                elif u < 0.048:
                    start += rng.uniform(600, 2400)
                    flags |= F_LATESTART
            cur = start
            for (c, sn, d) in msgs:
                ev.append((cur, d, c, sn, flags, nominal))
                g = 6.0 if rng.random() < 0.62 else 7.0 if rng.random() < 0.55 else rng.uniform(8, 12) \
                    if rng.random() < 0.9 else rng.uniform(30, 120)
                cur += d + g
            bursts_end[(id(t), nominal)] = cur
    ev = [e for e in ev if T0 <= e[0] < T1]
    ev.sort(key=lambda e: e[0])
    return ev, bursts_end


# ---------------------------------------------------------------- propagation (SNR domain, dB re noise)
def prop(a, t, rxlat, rxlon, rx, tx, pair_rng, ep, storms, sporadic_nights, T0):
    """Burst-level mean groundwave and skywave SNR (dB) for times t (array)."""
    d = np.maximum(hav(rxlat, rxlon, tx.lat, tx.lon), 3.0)
    hf = tx.hf
    # --- groundwave, P.368-like sea curve + Millington two-section land correction
    a_s = 0.04 if hf else a.gw_sea_att
    gsea = a.gw_c - 20 * np.log10(d) - a_s * d
    cross = pair_rng.choice([0.0, 1.0, 2.0], p=[0.45, 0.35, 0.20])
    cf = 0.0 if cross == 0 else pair_rng.uniform(0, 0.35) if cross == 1 else pair_rng.uniform(0.35, 0.95)
    Lnd = np.minimum(d, rx['inland'] + tx.land_km + cf * d)
    gfac = rx['ground'] * (2.5 if hf else 1.0)
    Ll = lambda x: gfac * 30 * np.log10(1 + np.asarray(x) / 40.0)
    gw = gsea - 0.5 * (Ll(d) - Ll(d - Lnd) + Ll(Lnd))
    # --- skywave, P.1147-like
    p = np.sqrt(d * d + 40000.0)
    mla, mlo = gc_point(rxlat, rxlon, tx.lat, tx.lon, 0.5)
    phi = np.abs(geomag_lat(mla, mlo))
    kR = (0.5 if hf else a.sw_kr0) + (0.25 if hf else a.sw_kr1) * np.tan(np.minimum(phi + 3, 72) * D2R) ** 2
    sw = (a.sw_a + (10 if hf else 0)) - 20 * np.log10(p) - kR * p / 1000.0
    el, dec = solar(t, mla, mlo)
    long_path = d > 2000
    if np.any(long_path):
        q1 = gc_point(rxlat, rxlon, tx.lat, tx.lon, 0.2)
        q3 = gc_point(rxlat, rxlon, tx.lat, tx.lon, 0.8)
        e1, _ = solar(t, *q1)
        e3, _ = solar(t, *q3)
        el = np.where(long_path, np.maximum(el, np.maximum(e1, e3)), el)
    s = np.sin(np.maximum(el, 0) * D2R) ** 0.6
    D0, Dt = (14.0, 4.0) if hf else (a.day_absorb, 10.0)
    absorb = np.where(el > 0, D0 * s + Dt, np.where(el > -6, Dt * (el + 6) / 6, 0.0))
    if hf:
        absorb = absorb * np.minimum(1.0, d / 1500.0)
    summer = np.where(np.abs(mla) > 20, np.sin(dec * D2R) * np.sign(mla) / math.sin(23.44 * D2R), 0.0)
    seasonal = -3.0 * summer
    pair_c = pair_rng.normal(0, 3.5)
    nkey = np.floor((t - T0 + mlo / 15 * 3600) / 86400 + 0.5).astype(int) + 2
    nightly = pair_rng.normal(0, 5.0, size=int(nkey.max()) + 3)[nkey]
    pairboost = np.where(pair_rng.random(size=int(nkey.max()) + 3) < 0.004,
                         pair_rng.uniform(12, 22, size=int(nkey.max()) + 3), 0.0)[nkey]
    storm = np.zeros_like(t, dtype=float)
    for (ts, M, tau) in storms:
        storm += np.where(t > ts, M * np.exp(-(t - ts) / tau), 0.0)
    storm_db = storm * np.clip((phi - 30) / 25.0, 0.15, 1.2)
    spor = np.where((sporadic_nights[np.clip(nkey, 0, len(sporadic_nights) - 1)] > 0) & (d > 1200),
                    sporadic_nights[np.clip(nkey, 0, len(sporadic_nights) - 1)], 0.0)
    boost = np.maximum(pairboost, spor)
    sw = sw - absorb + seasonal + pair_c + nightly - storm_db + boost
    # noise: atmospheric noise up at night, more in the tropics and in local summer
    rel, rdec = solar(t, rxlat, rxlon)
    rsummer = np.sin(rdec * D2R) * np.sign(rxlat) / math.sin(23.44 * D2R)
    night = rel < -6
    atm = np.where(night, np.where(np.abs(rxlat) < 30, 5.0, 2.0) + 2.0 * np.maximum(rsummer, 0), 0.0)
    noise = rx['noise'] + atm
    return d, gw + tx.power_db - noise, sw + tx.power_db - noise, storm_db, boost


# ---------------------------------------------------------------- receivers
def make_receivers(a, rng):
    rxs = []
    for (nm, la, lo, inland, fl) in RECEIVERS:
        rxs.append(dict(name=nm, lat=la, lon=lo, inland=inland, flags=fl, track=None, speed=0))
    for nm, (wp, sp, fl) in SHIPS.items():
        rxs.append(dict(name=nm, lat=wp[0][0], lon=wp[0][1], inland=0, flags=fl + ('S' if sp > 0 else ''),
                        track=wp, speed=sp))
    for r in rxs:
        fl = r['flags']
        r['valid'] = 'U' not in fl
        r['freqs'] = [518000] if '5' in fl else [518000, 490000]
        if 'H' in fl:
            r['freqs'].append(4209500)
        if 'J' in fl:
            r['freqs'].append(424000)
        r['noise'] = rng.normal(0, 2.0) + (rng.uniform(5, 10) if 'W' in fl else 0) + \
            (min(6.0, r['inland'] / 150.0) if r['inland'] > 100 else 0) + (3 if r['track'] and r['speed'] else 0)
        r['ground'] = rng.uniform(0.7, 1.4)
        r['cal'] = rng.normal(0, 2.0)
        r['has_snr'] = 'N' not in fl
        r['legacy'] = 'G' in fl
        r['leak'] = 'L' in fl
        r['fulltime'] = 'F' in fl or (r['track'] is not None) or rng.random() < 0.35
        r['duty'] = 0.95 if r['fulltime'] else rng.uniform(0.25, 0.7)
        r['pos_wrong'] = POS_WRONG.get(r['name'])
        r['ship'] = r['track'] is not None and r['speed'] > 0
    return rxs


def ship_pos(r, t, T0):
    wp = r['track']
    if len(wp) == 1 or r['speed'] == 0:
        return np.full_like(t, wp[0][0], dtype=float), np.full_like(t, wp[0][1], dtype=float)
    seg = [hav1(wp[i][0], wp[i][1], wp[i + 1][0], wp[i + 1][1]) for i in range(len(wp) - 1)]
    cum = np.concatenate([[0], np.cumsum(seg)])
    tot = cum[-1]
    s = (t - T0) / 86400.0 * r['speed']
    s = np.mod(s, 2 * tot)
    s = np.where(s > tot, 2 * tot - s, s)            # ping-pong along the route
    i = np.clip(np.searchsorted(cum, s, side='right') - 1, 0, len(seg) - 1)
    f = (s - cum[i]) / np.maximum(np.asarray(seg)[i], 1e-6)
    la = np.array([p[0] for p in wp]); lo = np.array([p[1] for p in wp])
    return la[i] + f * (la[i + 1] - la[i]), lo[i] + f * (lo[i + 1] - lo[i])


def sessions(r, T0, T1, rng):
    """Online intervals."""
    out = []
    if r['fulltime']:
        cur = T0
        while cur < T1:
            up = rng.exponential(6 * 86400)
            out.append((cur, min(T1, cur + up)))
            cur += up + rng.uniform(1, 10) * 3600
        return out
    cur = T0 + rng.uniform(0, 12) * 3600
    T = 16 * 3600.0
    while cur < T1:
        up = rng.exponential(r['duty'] * T)
        out.append((cur, min(T1, cur + up)))
        cur += up + rng.exponential((1 - r['duty']) * T)
    return out


# ---------------------------------------------------------------- simulate
def simulate(a):
    rng = np.random.default_rng(a.seed)
    table = load_table(a.table)
    W = build_world(table, a, rng)
    rxs = make_receivers(a, rng)
    if a.calib:
        rxs = [dict(name='dalgety_bay_virtual', lat=56.0403, lon=-3.3554, inland=0, flags='F', track=None, speed=0,
                    valid=True, freqs=[518000, 490000], noise=0.0, ground=1.0, cal=0.0, has_snr=True, legacy=False,
                    leak=False, fulltime=True, duty=0.97, pos_wrong=None, ship=False)]
    elif a.receivers:
        rxs = rxs[:a.receivers]
    st_by_freq_letter = {}
    for s in table:
        if W.dup_of[s['idx']] == s['idx']:
            st_by_freq_letter.setdefault((s['freq'], s['id']), []).append(s)
    rows = []
    epochs = [e for e in a.epochs.split(',') if e]
    world_dump = []
    for ep, ep_s in enumerate(epochs):
        T0 = int(dt.datetime.strptime(ep_s, '%Y-%m-%d').replace(tzinfo=dt.timezone.utc).timestamp())
        T1 = T0 + a.days * 86400
        ep_rng = np.random.default_rng(stable_seed(a.seed, 'epoch', ep))
        epoch_mods(W, ep, T0, a.days, ep_rng)
        mid_dec = float(solar(T0 + a.days * 43200, 0, 0)[1])

        def month_winter(lat):
            if abs(lat) < 23 or abs(mid_dec) < 8:
                return 0
            return 1 if (lat > 0) == (mid_dec < 0) else -1
        # geomagnetic storms: (start, magnitude dB, decay s)
        storms = [(ep_rng.uniform(T0, T1), ep_rng.uniform(10, 25), ep_rng.uniform(0.5, 1.5) * 86400)
                  for _ in range(a.storms)]
        # events per transmitter (chain leaders first)
        order = sorted(W.txs, key=lambda t: (t.chain[1] if t.chain else 0))
        chain_ends = {}
        EV = {}
        for t in order:
            trng = np.random.default_rng(stable_seed(a.seed, 'tx', t.name, t.freq, t.letter, ep))
            ev, be = gen_events(t, T0, T1, trng, chain_ends, month_winter)
            chain_ends.update(be)
            EV[id(t)] = ev
        world_dump.append(dict(epoch=ep_s, storms=[(int(s), round(m, 1), round(tau / 86400, 2)) for s, m, tau in storms],
                               n_events=sum(len(v) for v in EV.values())))
        storm_t = np.linspace(T0, T1, 2000)
        storm_level = np.zeros_like(storm_t)
        for (ts, M, tau) in storms:
            storm_level += np.where(storm_t > ts, M * np.exp(-(storm_t - ts) / tau), 0.0)
        for ri, r in enumerate(rxs):
            rrng = np.random.default_rng(stable_seed(a.seed, 'rx', r['name'], ep))
            sess = sessions(r, T0, T1, rrng)
            s_a = np.array([x[0] for x in sess]); s_b = np.array([x[1] for x in sess])

            def online(x):
                i = np.searchsorted(s_a, x, side='right') - 1
                return (i >= 0) & (x < s_b[np.maximum(i, 0)])
            nn = a.days + 6
            spor = np.where(rrng.random(nn) < a.sporadic_p, rrng.uniform(10, 20, nn), 0.0)
            per_freq = {}
            for t in W.txs:
                if t.freq not in r['freqs']:
                    continue
                ev = EV[id(t)]
                if not ev:
                    continue
                if not r['track'] and hav1(r['lat'], r['lon'], t.lat, t.lon) > (14000 if t.hf else 10000):
                    continue
                et = np.array([e[0] for e in ev])
                if r['track']:
                    rla, rlo = ship_pos(r, et, T0)
                else:
                    rla, rlo = np.full_like(et, r['lat']), np.full_like(et, r['lon'])
                prng = np.random.default_rng(stable_seed(a.seed, 'pair', r['name'], t.name, t.freq, t.letter, ep))
                d, gw, sw, stdb, boost = prop(a, et, rla, rlo, r, t, prng, ep, storms, spor, T0)
                keep = np.maximum(gw, sw + 6) > -14
                if not keep.any():
                    continue
                idx = np.nonzero(keep)[0]
                gl = 10 ** (gw[idx] / 10)
                sl = 10 ** (sw[idx] / 10)
                S = gl * 10 ** (prng.normal(0, 1.2, len(idx)) / 10) + sl * prng.exponential(1.0, len(idx))
                lst = per_freq.setdefault(t.freq, [])
                for k, i in enumerate(idx):
                    e = ev[i]
                    lst.append((e[0], e[0] + e[1], S[k], t, e, float(d[i]), float(rla[i]), float(rlo[i]),
                                bool(sl[k] > gl[k]), float(stdb[i]), float(boost[i])))
            for f, lst in per_freq.items():
                lst.sort(key=lambda x: x[0])
                st = np.array([x[0] for x in lst]); en = np.array([x[1] for x in lst])
                Sv = np.array([x[2] for x in lst])
                maxdur = float((en - st).max()) if len(lst) else 0
                for i, x in enumerate(lst):
                    t0, t1 = x[0], x[1]
                    if not (online(t0) and online(t1)):
                        continue
                    lo_i = np.searchsorted(st, t0 - maxdur - 1)
                    hi_i = np.searchsorted(st, t1)
                    Ih = Ib = 0.0
                    cap = False
                    for j in range(lo_i, hi_i):
                        if j == i or lst[j][3] is x[3] or en[j] <= t0:
                            continue
                        ov = min(t1, en[j]) - max(t0, st[j])
                        if ov <= 0:
                            continue
                        Ib += Sv[j] * ov / (t1 - t0)
                        if st[j] < t0 + 12:
                            Ih += Sv[j]
                        if Sv[j] > 0.5:
                            cap = True
                    sh = 10 * math.log10(x[2] / (1 + Ih))
                    sb = 10 * math.log10(x[2] / (1 + Ib))
                    pdec = sig((sh + 3.0) / 0.9) * sig((sb + 3.5) / 1.5) ** (1 + (t1 - t0) / 300.0)
                    if rrng.random() >= pdec:
                        continue
                    rows.append(emit(a, r, x, sb, cap, rrng, st_by_freq_letter, W, storm_t, storm_level, mid_dec,
                                     f, None))
                    if r['leak'] and f == 518000 and sb > 15 and rrng.random() < 0.04 and 490000 in r['freqs'] \
                            and online(t0):
                        rows.append(emit(a, r, x, sb - 22, False, rrng, st_by_freq_letter, W, storm_t, storm_level,
                                         mid_dec, 490000, 'leakage'))
            # noise headers
            for f in r['freqs']:
                n = rrng.poisson(a.noise_per_day * a.days)
                for x in rrng.uniform(T0, T1, n):
                    if not online(x):
                        continue
                    rows.append(noise_row(r, x, f, rrng, T0, st_by_freq_letter, storm_t, storm_level, mid_dec))
    rows = [r for r in rows if r is not None]
    return rows, W, world_dump, rxs


def season_tag(lat, mid_dec):
    if abs(lat) < 23:
        return None
    if abs(mid_dec) < 8:
        return 'equinox'
    return 'winter' if (lat > 0) == (mid_dec < 0) else 'summer'


def rx_tags(r, lat, mid_dec):
    tg = set()
    if not r['valid']:
        tg.add('rx_unknown')
    if r['inland'] >= 40:
        tg.add('inland')
    if r['ship']:
        tg.add('ship')
    if r['pos_wrong']:
        tg.add('rx_pos_wrong')
    if r['legacy']:
        tg.add('snr_legacy')
    s = season_tag(lat, mid_dec)
    if s:
        tg.add(s)
    return tg


def cand_tags(f, b1, rlat, rlon, st_by_freq_letter):
    tg = set()
    c = st_by_freq_letter.get((f, b1), [])
    ds = sorted(hav1(rlat, rlon, s['lat'], s['lon']) for s in c)
    ds = [d for d in ds if d < 3000]
    if len(ds) >= 2:
        tg.add('collision')
        if ds[1] < 1.3 * max(ds[0], 50):
            tg.add('equidistant')
    return tg


def rx_report_pos(r, rlat, rlon):
    if not r['valid']:
        return 0, '', ''
    if r['pos_wrong']:
        return 1, '%.4f' % r['pos_wrong'][0], '%.4f' % r['pos_wrong'][1]
    return 1, '%.4f' % rlat, '%.4f' % rlon


def report_snr(a, r, snr_true, rng):
    if not r['has_snr']:
        return '', ''
    s = snr_true + r['cal'] + rng.normal(0, 1.0) + (a.legacy_db if r['legacy'] else 0)
    if snr_true > 6:
        ch = 0.0 if rng.random() < 0.85 else rng.exponential(0.3)
    else:
        ch = rng.exponential(0.5 + 0.8 * (6 - snr_true))
    return '%.2f' % s, '%.1f' % min(100.0, ch)


def emit(a, r, x, sinr, cap, rng, sfl, W, storm_t, storm_level, mid_dec, f, extra):
    t0, t1, S, tx, e, d, rlat, rlon, sky, stdb, boost = x
    _, dur, b2, serial, flags, nominal = e
    letter = tx.letter
    if tx.letter_alias and b2 == 'B':
        letter = tx.letter_alias
    tg = set(tx.tags) | rx_tags(r, rlat, mid_dec)
    if extra:
        tg.add(extra)
    if f == 4209500:
        tg.add('hf')
    if f == 424000:
        tg.add('jp424')
    # decoder: header corruption calibrated to the real logs
    b1 = letter
    pg = 0.0002 + 0.02 * float(sig(-sinr / 1.5))
    if rng.random() < pg:
        u = rng.random()
        if u < 0.40:
            o = ord(letter) + (1 if rng.random() < 0.5 else -1)
            o = 66 if o < 65 else 87 if o > 88 else o
            b1 = chr(o)
            tg.update(('garbled', 'garble_adjacent'))
        elif u < 0.75:
            b1 = chr(65 + rng.integers(24))
            tg.add('garbled') if b1 != letter else None
        elif u < 0.90:
            b1 = JUNK[rng.integers(len(JUNK))]
            tg.update(('garbled', 'b1_nonletter'))
        else:
            b1 = ''
            tg.update(('garbled', 'b1_nonletter'))
    if rng.random() < 0.002 + 0.01 * float(sig(-sinr)):
        b2 = chr(65 + rng.integers(26))
    ser = serial if rng.random() > (0.012 + 0.05 * float(sig(-sinr))) else -1
    duration = dur + (rng.uniform(60, 900) if rng.random() < 0.015 else 0)
    # timing tags
    if flags & F_OFFSLOT:
        tg.add('offslot')
    if flags & F_VREP:
        tg.add('vital_repeat')
    if flags & F_LATESTART:
        tg.add('late_start')
    if not math.isnan(nominal):
        into = t0 - nominal
        if into >= 300:
            tg.add('late_slot')
        if into >= 600:
            tg.add('overrun')
        if into >= 1200:
            tg.add('long_overrun')
    rng_km = (tx.range_nm if tx.range_nm > 0 else 300) * 1.852
    if d > rng_km:
        tg.add('dx')
        if sky:
            tg.add('night_dx')
            if boost > 0 and sinr - boost < -2:
                tg.update(('sporadic_dx', 'anomalous_dx'))
    if float(np.interp(t0, storm_t, storm_level)) > 6:
        tg.add('storm')
    if cap:
        tg.add('capture')
    if len(b1) == 1:
        tg |= cand_tags(f, b1, rlat, rlon, sfl)
    snr, ch = report_snr(a, r, sinr, rng)
    rv, la, lo = rx_report_pos(r, rlat, rlon)
    return (r['name'], rv, la, lo, f, b1, b2, ser, int(round(t0)), int(round(duration)), snr, ch,
            tx.truth, '|'.join(sorted(tg)), '', int(round(d)), tx.name)


def noise_row(r, x, f, rng, T0, sfl, storm_t, storm_level, mid_dec):
    if r['track']:
        rla, rlo = ship_pos(r, np.array([x]), T0)
        rlat, rlon = float(rla[0]), float(rlo[0])
    else:
        rlat, rlon = r['lat'], r['lon']
    tg = rx_tags(r, rlat, mid_dec) | {'noise'}
    if rng.random() < 0.75:
        b1 = chr(65 + rng.integers(24))
        tg |= cand_tags(f, b1, rlat, rlon, sfl)
    else:
        b1 = JUNK[rng.integers(len(JUNK))]
        tg.add('b1_nonletter')
    b2 = chr(65 + rng.integers(26))
    ser = int(rng.integers(0, 100)) if rng.random() < 0.6 else -1
    sinr = rng.uniform(-6, 1)

    class _A:
        legacy_db = 30.0
    snr, ch = report_snr(_A, r, sinr, rng)
    if snr:
        ch = '%.1f' % min(100.0, rng.uniform(2, 40))
    rv, la, lo = rx_report_pos(r, rlat, rlon)
    if f == 4209500:
        tg.add('hf')
    if f == 424000:
        tg.add('jp424')
    return (r['name'], rv, la, lo, f, b1, b2, ser, int(round(x)), int(rng.uniform(12, 120)), snr, ch,
            -1, '|'.join(sorted(tg)), '', '', 'noise')


# ---------------------------------------------------------------- calibration print-out
def calib_report(rows):
    """Statistics of the virtual Dalgety Bay receiver, comparable with calib.json."""
    import collections
    real = None
    try:
        real = json.load(open(CALIB))
    except Exception:
        pass
    n = len(rows)
    days = (max(r[8] for r in rows) - min(r[8] for r in rows)) / 86400.0
    print('virtual Dalgety Bay: %d msgs, %.0f/day' % (n, n / max(days, 1)))
    snr = np.array([float(r[10]) for r in rows if r[10] != ''])
    print('SNR quantiles 1/5/50/95/99: %s' % np.round(np.percentile(snr, [1, 5, 50, 95, 99]), 1))
    dist = np.array([r[15] for r in rows if r[15] != '' and r[10] != ''], float)
    s2 = np.array([float(r[10]) for r in rows if r[15] != '' and r[10] != ''])
    m = dist > 30
    b, a0 = np.polyfit(np.log10(dist[m]), s2[m], 1)
    print('SNR fit: %.1f %+.1f*log10(km)   (real 50.1 -15.0*log10)' % (a0, b))
    sub = collections.Counter(r[6] for r in rows)
    print('subjects', {k: round(v / n, 3) for k, v in sub.most_common(6)})
    bands = [(0, 300), (300, 700), (700, 1200), (1200, 2000), (2000, 3000), (3000, 5000)]
    print('msgs by distance band:', {('%d-%d' % b_): int(((dist >= b_[0]) & (dist < b_[1])).sum()) for b_ in bands})
    tg = collections.Counter(t for r in rows for t in r[13].split('|'))
    print('tags', dict(tg.most_common()))
    per = collections.defaultdict(list)
    for r in rows:
        if r[10] != '' and r[15] != '':
            per[(r[16], r[4])].append((r[15], float(r[10])))
    for k, v in sorted(per.items(), key=lambda kv: kv[1][0][0]):
        if len(v) >= 10 and v[0][0] < 4500:
            print('  %-34s %7d %5d km n=%5d med %5.1f' % (k[0][:34], k[1], v[0][0], len(v), np.median([x[1] for x in v])))
    if real:
        print('real reception by band (heard stations):',
              {k: v['p_heard'] for k, v in real['recommended_parameters']['reception_by_band'].items()})


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--seed', type=int, default=90210)
    ap.add_argument('--epochs', default='2026-12-02,2027-06-08,2027-09-25',
                    help='comma-separated epoch start dates (UTC): northern winter, summer, autumn equinox')
    ap.add_argument('--days', type=int, default=10, help='days per epoch')
    ap.add_argument('--receivers', type=int, default=0, help='limit the receiver count (0 = all)')
    ap.add_argument('--out', default=os.path.join(HERE, 'data', 'holdout.csv'))
    ap.add_argument('--table', default=TABLE)
    ap.add_argument('--calib', action='store_true', help='simulate a virtual Dalgety Bay only and print statistics')
    ap.add_argument('--unknown-tx', dest='unknown_tx', type=int, default=20)
    ap.add_argument('--adj-shift-frac', dest='adj_shift_frac', type=float, default=0.05)
    ap.add_argument('--early-frac', dest='early_frac', type=float, default=0.06)
    ap.add_argument('--late-frac', dest='late_frac', type=float, default=0.06)
    ap.add_argument('--storms', type=int, default=1, help='geomagnetic storms per epoch')
    ap.add_argument('--sporadic-p', dest='sporadic_p', type=float, default=0.025, help='P(sporadic DX night) per rx')
    ap.add_argument('--noise-per-day', dest='noise_per_day', type=float, default=0.3)
    ap.add_argument('--legacy-db', dest='legacy_db', type=float, default=30.0)
    ap.add_argument('--gw-c', dest='gw_c', type=float, default=66.5, help='groundwave SNR at 1 km, 1 kW, sea')
    ap.add_argument('--gw-sea-att', dest='gw_sea_att', type=float, default=0.010)
    ap.add_argument('--sw-a', dest='sw_a', type=float, default=69.0)
    ap.add_argument('--sw-kr0', dest='sw_kr0', type=float, default=1.0)
    ap.add_argument('--sw-kr1', dest='sw_kr1', type=float, default=0.6)
    ap.add_argument('--day-absorb', dest='day_absorb', type=float, default=32.0)
    a = ap.parse_args()
    rows, W, wd, rxs = simulate(a)
    rows.sort(key=lambda r: (r[0], r[8]))
    if a.calib:
        calib_report(rows)
        return
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w', newline='', encoding='utf-8') as fh:
        w = csv.writer(fh)
        w.writerow(['rx', 'rx_valid', 'rx_lat', 'rx_lon', 'freq_hz', 'b1', 'b2', 'serial', 'start_utc',
                    'duration_s', 'snr_db', 'chars_failed_pct', 'truth', 'tags', 'proxy_truth', 'dist_km', 'tx_name'])
        for r in rows:
            w.writerow(r)
    world = dict(args=vars(a), epochs=wd,
                 receivers=[dict(name=r['name'], lat=r['lat'], lon=r['lon'], inland_km=r['inland'], valid=r['valid'],
                                 ship=r['ship'], freqs=r['freqs'], fulltime=r['fulltime'], duty=round(r['duty'], 2),
                                 pos_wrong=r['pos_wrong'], legacy_snr=r['legacy'], leak=r['leak'])
                            for r in rxs],
                 transmitters=[dict(truth=t.truth, freq=t.freq, letter=t.letter, tab_letter=t.tab_letter, name=t.name,
                                    lat=round(t.lat, 4), lon=round(t.lon, 4), tab_lat=t.tab_lat, tab_lon=t.tab_lon,
                                    nominal_min=[round(x / 60.0, 1) for x in t.nominal_s], offset_s=round(t.offset_s),
                                    clock_s=round(t.clock_s, 1), on_air=t.on_air, power_db=round(t.power_db, 1),
                                    chain=(t.chain[0].name if t.chain else None), tags=sorted(t.tags))
                               for t in W.txs])
    with open(os.path.splitext(a.out)[0] + '.world.json', 'w', encoding='utf-8') as fh:
        json.dump(world, fh, indent=1, ensure_ascii=False,
                  default=lambda o: o.item() if hasattr(o, 'item') else str(o))
    import collections
    tg = collections.Counter(t for r in rows for t in r[13].split('|') if t)
    print('%d rows, %d receivers, truth -1: %d' % (len(rows), len(set(r[0] for r in rows)),
                                                    sum(1 for r in rows if r[12] == -1)), file=sys.stderr)
    print('tags: ' + ', '.join('%s %d' % kv for kv in tg.most_common()), file=sys.stderr)


if __name__ == '__main__':
    main()
