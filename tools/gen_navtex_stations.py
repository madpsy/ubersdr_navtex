#!/usr/bin/env python3
"""Generate src/navtex_stations.h from Wikipedia's "List of Navtex stations"
merged with the YaND decoder's station list.

    python3 -I tools/gen_navtex_stations.py tools/navtex_stations.wiki tools/YAND_STATIONS.TXT \\
        [-o src/navtex_stations.h] [--report]

The build runs this (src/CMakeLists.txt); -o rewrites the file only when its
content changes, so an unchanged table does not trigger a rebuild.  --report
prints, on stderr, every station the merge renamed, added, dropped or moved.

Refresh the Wikipedia snapshot with
    curl -sf 'https://en.wikipedia.org/w/index.php?title=List_of_Navtex_stations&action=raw' \\
        > tools/navtex_stations.wiki

How the two sources are merged (see merge()):
  * 490 and 518 kHz: YaND decides which transmitters exist, their B1 letter,
    site name, position, NAVAREA and callsign, and supplies the coastline
    keywords and originator IDs.  It is newer than the Wikipedia page (India's
    rebuilt network, Canadian sites named by transmitter rather than by MCTS
    centre, ...).
  * Range and active come from the Wikipedia row for the same transmitter
    (same channel and letter within MATCH_KM), including the evidence-backed
    CORRECTIONS below.  A transmitter YaND lists — trial services included —
    is active unless that evidence says it is off air.
  * Schedules come from the Wikipedia row: every YaND schedule is the plain
    ITU letter pattern, so it carries no information of its own, while the
    page's irregular ones are real (and corrected below where they are not).
  * Names keep the page's accented spelling when YaND's is the same name
    without accents.
  * A Wikipedia station YaND does not list stays, active "not stated" — unless
    YaND lists the same site on the same channel under another letter, which
    is a letter change and replaces it.
  * 424 and 4209.5 kHz: Wikipedia only; YaND does not cover them.

The tables on that page do not share a column layout (some lead with Area and
kHz columns, some have a State column, some no Range), so cells are recognised
by their content rather than by position: a lone capital letter is the B1
identity, {{coord}} is the position, HH:MM lists are the schedule, and so on.
Rows without a usable B1 letter or position are dropped — they cannot be
matched against a decode anyway.

Source text is CC BY-SA 4.0; the generated header carries the attribution.
"""
import argparse
import math
import os
import re
import sys
import unicodedata

WIKI_URL = "https://en.wikipedia.org/wiki/List_of_Navtex_stations"

HEAD_RE  = re.compile(r"^==\s*(.*?)\s*==\s*$")
FREQ_RE  = re.compile(r"^(\d+(?:\.\d+)?)&nbsp;kHz")
AREA_RE  = re.compile(r"Navarea\s+(\d+)", re.I)
TIME_RE  = re.compile(r"\b([0-2]?\d):([0-5]\d)\b")
COORD_RE = re.compile(r"\{\{coord\|([^}]*)\}\}", re.I)
LINK_RE  = re.compile(r"\[\[(?:[^|\]]*\|)?([^\]]*)\]\]")
REF_RE   = re.compile(r"<ref[^>]*/>|<ref[^>]*>.*?</ref>", re.S)


def parse_coord(body):
    """{{coord|d|m|s|N|d|m|s|E|...}} in any of its d / d|m / d|m|s forms."""
    parts = [p.strip() for p in body.split("|")]
    pos = []
    for p in parts:
        if ":" in p or "=" in p:
            break
        pos.append(p)
    try:
        h1 = next(i for i, p in enumerate(pos) if p in ("N", "S"))
        h2 = next(i for i, p in enumerate(pos) if p in ("E", "W"))
    except StopIteration:
        return None

    def dms(fields):
        v, scale = 0.0, 1.0
        for f in fields:
            if f:
                v += float(f) / scale
            scale *= 60.0
        return v

    lat = dms(pos[:h1]) * (-1 if pos[h1] == "S" else 1)
    lon = dms(pos[h1 + 1:h2]) * (-1 if pos[h2] == "W" else 1)
    if not (-90 <= lat <= 90 and -180 <= lon <= 180):
        return None
    return lat, lon


def clean(s):
    s = REF_RE.sub("", s)
    s = LINK_RE.sub(r"\1", s)
    s = re.sub(r"'''?|<[^>]+>", "", s)
    return s.replace("&nbsp;", " ").strip()


def split_cells(row_lines):
    cells = []
    for ln in row_lines:
        if not ln.startswith("|"):
            if cells:                      # continuation of previous cell
                cells[-1] += " " + ln
            continue
        # {{coord|02|31||S|...}} leaves a seconds field empty, and that "||"
        # is not a cell separator — hide templates while splitting.
        tmpl = []
        masked = re.sub(r"\{\{.*?\}\}", lambda m: tmpl.append(m.group(0)) or f"\0{len(tmpl) - 1}\0",
                        ln[1:])
        for c in masked.split("||"):
            c = re.sub(r"\0(\d+)\0", lambda m: tmpl[int(m.group(1))], c)
            # Drop cell attributes such as 'style="..." | value'.
            if "|" in c and "=" in c.split("|", 1)[0] and "{{" not in c.split("|", 1)[0]:
                c = c.split("|", 1)[1]
            cells.append(c.strip())
    return cells


def parse_row(cells, sect_freq, area):
    freq = sect_freq
    coord = None
    ident = None
    name = None
    country = ""
    minutes = []
    rng = 0
    active = None
    for i, c in enumerate(cells):
        m = COORD_RE.search(c)
        if m:
            coord = parse_coord(m.group(1))
            continue
        cc = clean(c)
        if ident is None and re.fullmatch(r"[A-Z]", cc):
            ident = cc
            if i + 1 < len(cells):
                name = clean(cells[i + 1])
            if i + 2 < len(cells) and re.fullmatch(r"[A-Z]{3}", clean(cells[i + 2])):
                country = clean(cells[i + 2])
            continue
        if re.fullmatch(r"\d{3,4}(\.\d+)?", cc) and float(cc) in (424, 490, 518, 4209.5):
            freq = float(cc)
            continue
        if TIME_RE.search(cc) and not minutes:
            minutes = sorted({int(h) * 60 + int(mm) for h, mm in TIME_RE.findall(cc)
                              if int(h) < 24})
            continue
        low = cc.lower().rstrip("?")
        if low in ("yes", "no", "y", "n"):
            active = low.startswith("y")
            continue
        if re.fullmatch(r"\d{2,4}", cc) and coord is not None and not rng:
            rng = int(cc)
    if ident is None or coord is None or freq is None:
        return None
    return dict(freq=freq, id=ident, name=name or "?", country=country,
                area=area, lat=coord[0], lon=coord[1], minutes=minutes,
                range_nm=rng, active=active)


def parse(text):
    stations = []
    area = 0
    sect_freq = None
    in_table = False
    row = []

    def flush():
        if row:
            st = parse_row(split_cells(row), sect_freq, area)
            if st:
                stations.append(st)
        row.clear()

    for ln in text.splitlines():
        s = ln.strip()
        h = HEAD_RE.match(s)
        if h:
            title = h.group(1)
            m = AREA_RE.search(title)
            area = int(m.group(1)) if m else 0
            sect_freq = 4209.5 if "4209.5" in title else None
            continue
        f = FREQ_RE.match(s)
        if f and not in_table:
            sect_freq = float(f.group(1))
            continue
        if s.startswith("{|"):
            in_table = True
            row.clear()
            continue
        if not in_table:
            continue
        if s.startswith("|}"):
            flush()
            in_table = False
            continue
        if s.startswith("|-"):
            flush()
            continue
        if s.startswith("!"):
            continue
        if s:
            row.append(s)
    return stations


def canon(letter):
    """The ITU-R M.540 slot pattern: letter A at 00:00, B at 00:10 ... every 4 h."""
    off = (ord(letter) - ord("A")) * 10
    return [off + 240 * k for k in range(6)]


# Evidence sources cited below:
#   DB   the Dalgety Bay receiver's own logs (56.04N 3.36W, Aug-Oct 2026,
#        32,651 messages), attributed from message TEXT, not from the table
#   SV   the Svelvik (Norway) receiver archive on navtex.net, 518 kHz
#   NN   navtex.net station notices and blog (2012-2013)
#   RXX  the RXX signal database, rxx.classaxe.com, signal id in brackets;
#        positions there are 6-character locator centres (about +-3 km)
# Only facts are taken from the latter two — neither carries a data licence.
# A correction needs two independent sources, or one direct observation of
# a closure.

# Known errors in the source page, keyed (freq_khz, B1, name). Each entry says
# what is wrong and how we know; prefer fixing Wikipedia too.
CORRECTIONS = {
    # 490 kHz row gives 01°27'E — the same Cullercoats site on 518 kHz is
    # 01°27'W, and east of the meridian at that latitude is the North Sea.
    (490.0, "U", "Cullercoats"): {"lon": -1.4632},

    # Odesa and Haifa carry each other's schedules on the page. Odesa text is
    # received at x:20 (DB, SV "ODESA-NAVTEX" ending 00:23-00:25); RXX [5537]
    # and [5524] give the canonical C and P slots.
    (518.0, "C", "Odesa"): {"minutes": canon("C")},
    (518.0, "P", "Haifa"): {"minutes": canon("P")},

    # Arkhangelsk moved from F to L, and both it and Murmansk (C -> K) are
    # NAVAREA XX, not XIII. DB/SV: "COASTAL WARNING ARKHANGELSK" under L; NN
    # 2012 notice; RXX F [5679] and C [5678] "SLOT CHANGE TO L/K", inactive,
    # L [12430] (Iles station) and K [12656] active.
    (518.0, "F", "Arkhangelsk"): {"id": "L", "area": 20, "lat": 64.3542, "lon": 40.6250,
                                  "minutes": canon("L")},
    (518.0, "C", "Murmansk"): {"id": "K", "area": 20, "minutes": canon("K")},

    # The Azores letters are transmitted from Sao Miguel, 281 km east of the
    # Horta (Faial) position on the page, and 490 J is in service: RXX [5471]
    # [5467] active 2026-09; DB hears 490 J.
    (518.0, "F", "Horta"): {"name": "São Miguel", "lat": 37.8125, "lon": -25.5417},
    (490.0, "J", "Horta"): {"name": "São Miguel", "lat": 37.8125, "lon": -25.5417, "active": 1},

    # Marked inactive on the page, but heard: DB; RXX [13019] [5468] logged
    # 2026-09-26.
    (518.0, "P", "Porto Santo"): {"active": 1},
    (490.0, "M", "Porto Santo"): {"active": 1},

    # 12:20 is a typo for 12:40 (canonical E): DB, NN, RXX [10005].
    (490.0, "E", "Saudanes"): {"minutes": canon("E")},
    # The page's 02:55 pattern is the old 518 kHz schedule; 490 J runs on the
    # canonical J slot: DB observes x:30, RXX [5548] 0130/0530/...
    (490.0, "J", "Sydney (Nova Scotia)"): {"minutes": canon("J")},
    # Canonical A and B slots: RXX [5540] [8098]; the page's 03:00-based and
    # three-slot schedules match neither receiver nor any notice.
    (518.0, "A", "Novorossijsk"): {"minutes": canon("A")},
    (518.0, "B", "Algiers"): {"minutes": canon("B")},

    # Oostende: the page puts T 40 km inland. T, V and 490 B share the coastal
    # site in RXX [5461] [12567] [11614]; NN also differs from the page.
    (518.0, "T", "Oostende"): {"lat": 51.1875, "lon": 2.7917},

    # Off air: Guam V since 2021 (RXX [5611]; USCG schedule is canonical V per
    # NN), Kerch B since 2019 (RXX [5528]), Wiarton H and Thunder Bay P closed
    # July 2026 (RXX [5557] [5560]).
    (518.0, "V", "Guam"): {"active": 0, "minutes": canon("V")},
    (518.0, "B", "Kerch"): {"active": 0},
    (518.0, "H", "Wiarton"): {"active": 0},
    (518.0, "P", "Thunder Bay"): {"active": 0},
}

# Rows to drop. Cabo de la Nao appears in both NAVAREA II and III on the page;
# it is one NAVAREA III station (NN, RXX [5545] [7755]).
REMOVALS = {
    (518.0, "X", "Cabo La Nao"),
    (490.0, "M", "Cabo La Nao"),
}

# Transmitters missing from the page. Each is heard at DB with text that
# names the service, and is listed active in RXX.
ADDITIONS = [
    # Dutch-language Belgian national service; DB 1,699 messages. RXX [11614].
    dict(freq=490.0, id="B", name="Oostende", country="BEL", area=1,
         lat=51.1875, lon=2.7917, range_nm=150, active=1, minutes=canon("B")),
    # Irish national service; DB 311, NN Dec 2012, RXX [11000]. Same site as
    # Malin Head 518 Q.
    dict(freq=490.0, id="A", name="Malin Head", country="IRL", area=1,
         lat=55.3633, lon=-7.3392, range_nm=400, active=1, minutes=canon("A")),
    # Croatian national service; DB 223, RXX [16927]. Same site as Split 518 Q.
    dict(freq=490.0, id="F", name="Split", country="HRV", area=3,
         lat=43.1819, lon=16.4223, range_nm=200, active=1, minutes=canon("F")),
    # Ukrainian national service; DB 181, RXX [9328]. Same site as Odesa 518 C.
    dict(freq=490.0, id="X", name="Odesa", country="UKR", area=3,
         lat=46.3776, lon=30.7482, range_nm=250, active=1, minutes=canon("X")),
    # Italian national service; DB 81, NN 2013, RXX [14921].
    dict(freq=490.0, id="N", name="Piombino", country="ITA", area=3,
         lat=42.9375, lon=10.5417, range_nm=320, active=1, minutes=canon("N")),
    # Azov Sea coastal warnings; DB 440 (night only — about 3,000 km), RXX
    # [17494] gives the canonical O slot and this locator.
    dict(freq=490.0, id="O", name="Taganrog", country="RUS", area=3,
         lat=47.1458, lon=38.2917, range_nm=240, active=1, minutes=canon("O")),
]


def apply_corrections(stations):
    # Keys as on the page, before any correction renames a letter.
    page_keys = {(s["freq"], s["id"], s["name"]) for s in stations}
    out = []
    for s in stations:
        key = (s["freq"], s["id"], s["name"])
        if key in REMOVALS:
            continue
        fix = CORRECTIONS.get(key)
        if fix:
            s.update(fix)
        out.append(s)
    known = {(s["freq"], s["id"], s["name"]) for s in out}
    for a in ADDITIONS:
        if (a["freq"], a["id"], a["name"]) in known:
            sys.exit(f"addition {a['name']} {a['freq']} {a['id']} is already on the page — drop it")
        out.append(dict(a))
    unused = (set(CORRECTIONS) | REMOVALS) - page_keys
    if unused:
        sys.exit(f"corrections no longer match the page: {sorted(unused)}")
    return out


# ------------------------------------------------------------------ YaND

# Same transmitter: same channel and letter, positions within this distance.
# The two sources' positions for one site differ by up to ~140 km (Las Palmas,
# Buenos Aires), while the nearest distinct same-letter pair is far further.
MATCH_KM = 150
# A Wikipedia-only row whose site YaND lists on the same channel under another
# letter: the letter changed.  Tighter, because sites on one channel can be
# close (Niton E/K, Oostende T/V).
MOVED_KM = 30

# YaND's operator codes for territories and home nations -> the codes the
# country table (tools/gen_navtex_countries.py) uses.
YAND_COUNTRY = {
    "ENG": "GBR", "SCT": "GBR",
    "HOL": "NLD", "ATN": "NLD",
    "CNR": "ESP",
    "AZR": "POR", "MDR": "POR",
    "RSE": "RUS",
    "SAR": "ITA",
    "SVB": "NOR",
    "GRL": "DNK",
    "PTR": "USA",
    "RYI": "JPN",
    "ANI": "IND",
}

TRIAL_RE = re.compile(r"\s*\{trial\}\s*", re.I)


def great_circle_km(a, b):
    la1, lo1, la2, lo2 = map(math.radians, (a["lat"], a["lon"], b["lat"], b["lon"]))
    c = (math.sin(la1) * math.sin(la2) +
         math.cos(la1) * math.cos(la2) * math.cos(lo1 - lo2))
    return 6371.0 * math.acos(max(-1.0, min(1.0, c)))


def off_air(s):
    """active is True/False from the page, 1/0 from CORRECTIONS, None if unstated."""
    return s["active"] is not None and not s["active"]


# Letters NFKD does not decompose into a base letter plus an accent.
FOLD_EXTRA = str.maketrans({"ø": "o", "Ø": "O", "æ": "ae", "Æ": "AE", "ı": "i",
                            "ł": "l", "Ł": "L", "đ": "d", "Đ": "D", "ß": "ss"})


def fold(s):
    """Lower case, accents and punctuation removed: for comparing names."""
    s = unicodedata.normalize("NFKD", s.translate(FOLD_EXTRA))
    s = "".join(ch for ch in s if not unicodedata.combining(ch))
    return re.sub(r"[^a-z0-9]+", " ", s.lower()).strip()


def split_list(s):
    return [x.strip() for x in s.split(",") if x.strip()]


def parse_yand(path):
    """AREA#kHz#B1#CALL#NAME#CODE#COUNTRY#LAT#LON#HH:MM x6#KEYWORDS#ORIGINATORS#"""
    rows = open(path, encoding="ascii").read().splitlines()
    if not rows or not rows[0].startswith("VERSION"):
        sys.exit(f"{path}: not a YaND station list (no VERSION line)")
    out = []
    for n, ln in enumerate(rows[1:], 2):
        if not ln.strip():
            continue
        p = ln.split("#")
        if len(p) < 16:
            sys.exit(f"{path}:{n}: {len(p)} fields")
        times = [t for t in p[9:15] if t]
        if not all(re.fullmatch(r"\d\d:\d\d", t) for t in times):
            sys.exit(f"{path}:{n}: bad schedule {times}")
        name = p[4].strip()
        trial = bool(TRIAL_RE.search(name))
        out.append(dict(
            area=int(p[0]), freq=float(p[1]), id=p[2], call=p[3].strip("_ "),
            name=TRIAL_RE.sub(" ", name).strip(), trial=trial,
            country=YAND_COUNTRY.get(p[5], p[5]),
            lat=float(p[7]), lon=float(p[8]),
            minutes=sorted(int(t[:2]) * 60 + int(t[3:]) for t in times),
            keywords=split_list(p[15]),
            originators=split_list(p[16]) if len(p) > 16 else [],
        ))
    return out


def merged_name(wiki_name, yand_name):
    """YaND's name, in the page's spelling where it is the same name."""
    fw, fy = fold(wiki_name), fold(yand_name)
    if fw == fy:
        return wiki_name
    # "São Miguel" + "Sao Miguel (CENCOMARACORES)" -> "São Miguel (CENCOMARACORES)"
    # "Jeløy" + "Jeloya" -> "Jeløya"
    if fy.startswith(fw) and fold(yand_name[:len(wiki_name)]) == fw:
        return wiki_name + yand_name[len(wiki_name):]
    return yand_name


def merge(wiki, yand, report):
    """See the module docstring.  Returns the merged list; logs to report()."""
    mf = (490.0, 518.0)
    keep = [s for s in wiki if s["freq"] not in mf]
    wmf = [s for s in wiki if s["freq"] in mf]

    # Pair each YaND row with the nearest unclaimed Wikipedia row on the same
    # channel and letter, closest pairs first.
    pairs = sorted((great_circle_km(w, y), wi, yi)
                   for wi, w in enumerate(wmf) for yi, y in enumerate(yand)
                   if w["freq"] == y["freq"] and w["id"] == y["id"])
    w_of, y_of = {}, {}
    for d, wi, yi in pairs:
        if d <= MATCH_KM and wi not in y_of and yi not in w_of:
            w_of[yi], y_of[wi] = wi, yi

    out = []
    for yi, y in enumerate(yand):
        s = dict(freq=y["freq"], id=y["id"], name=y["name"], country=y["country"],
                 area=y["area"], lat=y["lat"], lon=y["lon"], call=y["call"],
                 keywords=y["keywords"], originators=y["originators"],
                 minutes=y["minutes"], range_nm=0, active=True)
        tag = " (trial)" if y["trial"] else ""
        wi = w_of.get(yi)
        if wi is None:
            report(f"  added    {y['freq']:g} {y['id']}  {y['name']}{tag}")
        else:
            w = wmf[wi]
            s["name"] = merged_name(w["name"], y["name"])
            s["range_nm"] = w["range_nm"]
            if off_air(w):
                s["active"] = False
            if w["minutes"]:
                s["minutes"] = w["minutes"]
                if w["minutes"] != y["minutes"]:
                    report(f"  schedule {y['freq']:g} {y['id']}  {s['name']}: keeping "
                           f"{fmt_mins(w['minutes'])} over YaND's {fmt_mins(y['minutes'])}")
            if s["name"] != w["name"]:
                report(f"  renamed  {y['freq']:g} {y['id']}  {w['name']} -> {s['name']}{tag}")
            d = great_circle_km(w, y)
            if d > 20:
                report(f"  moved    {y['freq']:g} {y['id']}  {s['name']}: {d:.0f} km")
        out.append(s)

    for wi, w in enumerate(wmf):
        if wi in y_of:
            continue
        moved = [y for y in yand if y["freq"] == w["freq"] and great_circle_km(w, y) <= MOVED_KM]
        if moved:
            report(f"  letter   {w['freq']:g} {w['id']} -> {moved[0]['id']}  {w['name']} "
                   f"(YaND: {moved[0]['name']}); old row dropped")
            continue
        s = dict(w, call="", keywords=[], originators=[])
        s["active"] = False if off_air(w) else None
        report(f"  kept     {w['freq']:g} {w['id']}  {w['name']}: not in YaND, active "
               + ("no" if off_air(w) else "not stated"))
        out.append(s)

    for s in keep:
        out.append(dict(s, call="", keywords=[], originators=[]))
    out.sort(key=lambda s: (s["area"], -s["freq"], s["id"], s["name"]))
    return out


def fmt_mins(m):
    return "/".join(f"{x // 60:02d}{x % 60:02d}" for x in m[:2]) + ("/..." if len(m) > 2 else "")


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit(stations, out):
    out.write(f"""/* GENERATED by tools/gen_navtex_stations.py — do not edit by hand.
 * The build regenerates it from tools/navtex_stations.wiki and
 * tools/YAND_STATIONS.TXT; change those or the generator instead.
 *
 * NAVTEX transmitter table: Wikipedia "List of Navtex stations"
 * ({WIKI_URL}), licensed CC BY-SA 4.0, merged with the station list of the
 * YaND decoder.
 * {len(stations)} stations.
 *
 * minutes: scheduled transmission starts, minutes after 00:00 UTC.
 * active:  1 yes, 0 no, -1 not stated by either source.
 */
#pragma once

struct NavtexStation {{
    int         freq_hz;
    char        id;
    const char *name;
    const char *country;
    int         navarea;
    double      lat;
    double      lon;
    int         range_nm;
    int         active;
    int         n_minutes;
    short       minutes[12];
}};

static const NavtexStation NAVTEX_STATIONS[] = {{
""")
    for s in stations:
        mins = s["minutes"][:12]
        act = -1 if s["active"] is None else int(s["active"])
        out.write("    {{ {:>7}, '{}', {}, {}, {:>2}, {:9.4f}, {:9.4f}, {:>4}, {:>2}, {:>2}, {{{}}} }},\n".format(
            int(round(s["freq"] * 1000)), s["id"], c_str(s["name"]), c_str(s["country"]),
            s["area"], s["lat"], s["lon"], s["range_nm"], act, len(mins),
            ", ".join(str(m) for m in mins)))
    out.write("};\n\nstatic const int NAVTEX_STATION_COUNT =\n"
              "    (int)(sizeof(NAVTEX_STATIONS) / sizeof(NAVTEX_STATIONS[0]));\n")
    out.write("""
/* Parallel to NAVTEX_STATIONS (same index), from YaND; empty where YaND does
 * not list the station.  keywords: place and service names that appear in the
 * station's message text; originators: how the station signs its messages.
 * Both comma separated. */
struct NavtexStationText {
    const char *callsign;
    const char *keywords;
    const char *originators;
};

static const NavtexStationText NAVTEX_STATION_TEXT[] = {
""")
    for s in stations:
        out.write("    {{ {}, {}, {} }},\n".format(
            c_str(s.get("call", "")), c_str(",".join(s.get("keywords", []))),
            c_str(",".join(s.get("originators", [])))))
    out.write("};\n\nstatic_assert(sizeof(NAVTEX_STATION_TEXT) / sizeof(NAVTEX_STATION_TEXT[0]) ==\n"
              "              sizeof(NAVTEX_STATIONS) / sizeof(NAVTEX_STATIONS[0]),\n"
              "              \"NAVTEX_STATION_TEXT must parallel NAVTEX_STATIONS\");\n")


def write_if_changed(path, text):
    try:
        if open(path, encoding="utf-8").read() == text:
            return
    except OSError:
        pass
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("wiki", help="tools/navtex_stations.wiki")
    ap.add_argument("yand", help="tools/YAND_STATIONS.TXT")
    ap.add_argument("-o", "--out", help="write here (only if changed) instead of stdout")
    ap.add_argument("--report", action="store_true", help="list merge decisions on stderr")
    a = ap.parse_args()

    wiki = apply_corrections(parse(open(a.wiki, encoding="utf-8").read()))
    if len(wiki) < 100:
        sys.exit(f"only {len(wiki)} stations parsed — page layout changed?")
    yand = parse_yand(a.yand)
    if len(yand) < 100:
        sys.exit(f"only {len(yand)} YaND stations parsed")

    log = []
    stations = merge(wiki, yand, log.append)
    if a.report:
        order = ["added", "renamed", "letter", "kept", "moved", "schedule"]
        log.sort(key=lambda l: (order.index(l.split()[0]), l))
        sys.stderr.write(f"{len(wiki)} Wikipedia + {len(yand)} YaND -> {len(stations)} stations\n")
        sys.stderr.write("\n".join(log) + "\n")

    keys = [(s["freq"], s["id"], s["name"]) for s in stations]
    dup = {k for k in keys if keys.count(k) > 1}
    if dup:
        sys.exit(f"duplicate station keys (channel, letter, name): {sorted(dup)}")

    import io
    buf = io.StringIO()
    emit(stations, buf)
    if a.out:
        write_if_changed(a.out, buf.getvalue())
    else:
        sys.stdout.write(buf.getvalue())


if __name__ == "__main__":
    main()
