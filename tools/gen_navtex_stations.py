#!/usr/bin/env python3
"""Generate src/navtex_stations.h from Wikipedia's "List of Navtex stations".

    curl -sf 'https://en.wikipedia.org/w/index.php?title=List_of_Navtex_stations&action=raw' > navtex.wiki
    python3 -I tools/gen_navtex_stations.py navtex.wiki > src/navtex_stations.h

The tables on that page do not share a column layout (some lead with Area and
kHz columns, some have a State column, some no Range), so cells are recognised
by their content rather than by position: a lone capital letter is the B1
identity, {{coord}} is the position, HH:MM lists are the schedule, and so on.
Rows without a usable B1 letter or position are dropped — they cannot be
matched against a decode anyway.

Source text is CC BY-SA 4.0; the generated header carries the attribution.
"""
import re
import sys

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


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit(stations, out):
    out.write(f"""/* GENERATED by tools/gen_navtex_stations.py — do not edit by hand.
 *
 * NAVTEX transmitter table, from Wikipedia "List of Navtex stations"
 * ({WIKI_URL}), licensed CC BY-SA 4.0.
 * {len(stations)} stations.
 *
 * minutes: scheduled transmission starts, minutes after 00:00 UTC.
 * active:  1 yes, 0 no, -1 not stated on the source page.
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


def main():
    text = open(sys.argv[1], encoding="utf-8").read()
    stations = apply_corrections(parse(text))
    if len(stations) < 100:
        sys.exit(f"only {len(stations)} stations parsed — page layout changed?")
    emit(stations, sys.stdout)


if __name__ == "__main__":
    main()
