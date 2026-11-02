/* navtex_mapdata.h — what a NAVTEX message says about places, for the map.
 *
 * Two readers of message text, both pure (no I/O, no globals) so they can be
 * tested against real broadcasts without a server:
 *
 *   navtex_parse_positions()  every "50-46.3N 001-17.8W"-style position in a
 *                             warning, in the formats seen on air (degrees and
 *                             decimal minutes with '-' or ' ', with seconds,
 *                             and the compact DDMMN DDDMMW form);
 *   navtex_warning_kind()     what the warning is about (buoy, light, wind
 *                             farm, cable, ...), from keywords;
 *   navtex_parse_forecasts()  the UK Met Office shipping and inshore waters
 *                             forecasts, split per sea area and reduced to
 *                             "NE 4-7 · ROUGH · POOR VIS".
 *
 * Received text is noisy: a garbled character turns "ROUGH" into "RO?GH" and
 * a digit into another digit.  Everything here prefers to say nothing over
 * saying something wrong — an area name must match exactly, a forecast needs
 * a wind force to count, and positions are range-checked by the caller
 * against the transmitting station.
 *
 * Forecasts are read from Met Office bulletins and from the Norwegian
 * Meteorological Institute's, which name the North Sea areas the same way.
 * Sea-area positions are label anchors only (a point inside each area, at sea),
 * not area boundaries.  Other formats (Norway's coded areas such as "C3", the
 * Faroes', DWD's, Météo-France's) are not parsed yet.
 */
#pragma once

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

struct NavtexPos { double lat, lon; };

/* ------------------------------------------------------------------ */
/* Positions                                                           */
/* ------------------------------------------------------------------ */

namespace navtex_mapdata_detail {

static double num(const std::string &s)
{
    std::string t = s;
    for (char &c : t) if (c == ',') c = '.';
    return t.empty() ? 0.0 : atof(t.c_str());
}

/* Degrees, minutes (decimal), optional seconds -> decimal degrees, or NAN
 * when a part is out of range (a garbled digit, usually). */
static double dms(const std::string &d, const std::string &m, const std::string &s, double maxdeg)
{
    const double D = num(d), M = num(m), S = num(s);
    if (D > maxdeg || M >= 60.0 || S >= 60.0) return NAN;
    const double v = D + M / 60.0 + S / 3600.0;
    return v > maxdeg ? NAN : v;
}

}  // namespace navtex_mapdata_detail

static std::vector<NavtexPos> navtex_parse_positions(const std::string &text)
{
    using namespace navtex_mapdata_detail;
    /* lat:  DD-MM.mm N | DD MM'SS''N | DDMM.mmN      (sep: '-' or spaces)
     * lon:  DDD-MM.mm W | DD MM'SS''E | DDDMM.mmW
     * between: up to four of space , / ; */
    static const std::regex re(
        "\\b(?:(\\d{1,2})[- ]+(\\d{1,2}(?:[.,]\\d+)?)(?:'(\\d{1,2}(?:[.,]\\d+)?)(?:''|\")?)?|(\\d{2})(\\d{2}(?:\\.\\d+)?))"
        " *([NS])[ ,/;]{0,4}"
        "(?:(\\d{1,3})[- ]+(\\d{1,2}(?:[.,]\\d+)?)(?:'(\\d{1,2}(?:[.,]\\d+)?)(?:''|\")?)?|(\\d{3})(\\d{2}(?:\\.\\d+)?))"
        " *([EW])\\b");
    std::string t;
    t.reserve(text.size());
    for (char c : text) t += (char)toupper((unsigned char)c);
    /* Curly quotes are not in the NAVTEX alphabet, but logs may be edited. */
    std::vector<NavtexPos> out;
    for (auto it = std::sregex_iterator(t.begin(), t.end(), re); it != std::sregex_iterator(); ++it) {
        const std::smatch &m = *it;
        const double lat = m[1].matched ? dms(m[1], m[2], m[3], 90) : dms(m[4], m[5], "", 90);
        const double lon = m[7].matched ? dms(m[7], m[8], m[9], 180) : dms(m[10], m[11], "", 180);
        if (std::isnan(lat) || std::isnan(lon)) continue;
        NavtexPos p { m[6] == "S" ? -lat : lat, m[12] == "W" ? -lon : lon };
        bool dup = false;
        for (const auto &q : out) if (fabs(q.lat - p.lat) < 1e-6 && fabs(q.lon - p.lon) < 1e-6) { dup = true; break; }
        if (!dup) out.push_back(p);
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* Warning kinds                                                       */
/* ------------------------------------------------------------------ */

/* Ordered: the first keyword group that appears wins, so "LIGHT BUOY" is a
 * buoy and "WIND FARM ... LIGHTS UNLIT" is a wind farm. */
static const char *navtex_warning_kind(const std::string &text, char subject)
{
    if (subject == 'D') return "sar";
    std::string t;
    t.reserve(text.size() + 2);
    t += ' ';
    for (char c : text) t += isalnum((unsigned char)c) ? (char)toupper((unsigned char)c) : ' ';
    t += ' ';
    auto has = [&](const char *w) { return t.find(w) != std::string::npos; };
    if (has(" WIND FARM") || has(" WINDFARM") || has(" TURBINE")) return "windfarm";
    if (has(" PERSON IN THE WATER") || has(" OVERDUE") || has(" MISSING ") || has(" MAN OVERBOARD")
        || has(" SEARCH AND RESCUE") || has(" MAYDAY")) return "sar";
    if (has(" CABLE") || has(" PIPELINE") || has(" PIPE LAYING")) return "cable";
    /* Also the Dutch / German / French words some coastal stations use. */
    if (has("BUOY") || has(" BEACON") || has(" MARK ") || has(" WAVERIDER") || has("BOEI")
        || has(" TONNE") || has(" BOUEE") || has(" BALISE")) return "buoy";
    if (has(" LIGHT ") || has(" LIGHTS ") || has(" LIGHTHOUSE") || has(" UNLIT") || has(" LT ")
        || has(" LICHT ") || has(" FEU ") || has(" LEUCHTFEUER") || has(" VUURTOREN")) return "light";
    if (has(" WRECK") || has(" DERELICT") || has(" ADRIFT") || has(" DRIFTING") || has(" CONTAINER")
        || has(" OBSTRUCTION") || has(" SUNKEN") || has(" SUBMERGED") || has(" DEBRIS")) return "hazard";
    if (has(" FIRING") || has(" GUNNERY") || has(" MISSILE") || has(" EXERCISE") || has(" MILITARY")
        || has(" ROCKET") || has(" LIVE AMMUNITION") || has(" EXPLOSIVE")) return "military";
    if (has(" SURVEY") || has(" SEISMIC") || has(" DREDG") || has(" HYDROGRAPHIC")) return "survey";
    if (has(" RIG ") || has(" RIGS ") || has(" PLATFORM") || has(" DRILLING") || has(" JACK UP")
        || has(" JACKUP") || has(" FPSO")) return "rig";
    if (has(" TOW ") || has(" TOWING") || has(" TOWED") || has(" TOW OF ")) return "tow";
    return "warning";
}

/* ------------------------------------------------------------------ */
/* Forecasts                                                           */
/* ------------------------------------------------------------------ */

struct NavtexSeaArea {
    const char *key;     /* as written in the forecast (inshore: the "A TO B" part) */
    const char *label;   /* as shown on the map */
    bool        inshore;
    double      lat, lon;   /* label anchor, at sea */
};

/* UK Met Office shipping forecast areas, plus the alternative spellings the
 * bulletins use.  Anchors are approximate area centres. */
static const NavtexSeaArea NAVTEX_SEA_AREAS[] = {
    { "VIKING",           "Viking",          false, 60.0,   2.0 },
    { "NORTH UTSIRE",     "North Utsire",    false, 60.2,   3.8 },
    { "N UTSIRE",         "North Utsire",    false, 60.2,   3.8 },
    { "SOUTH UTSIRE",     "South Utsire",    false, 58.8,   4.0 },
    { "S UTSIRE",         "South Utsire",    false, 58.8,   4.0 },
    { "FORTIES",          "Forties",         false, 57.6,   1.0 },
    { "CROMARTY",         "Cromarty",        false, 57.9,  -2.3 },
    { "FORTH",            "Forth",           false, 56.4,  -1.0 },
    { "TYNE",             "Tyne",            false, 55.1,  -0.6 },
    { "DOGGER",           "Dogger",          false, 54.8,   2.5 },
    { "FISHER",           "Fisher",          false, 56.6,   5.8 },
    { "GERMAN BIGHT",     "German Bight",    false, 54.4,   6.5 },
    { "HUMBER",           "Humber",          false, 53.7,   1.6 },
    { "THAMES",           "Thames",          false, 52.0,   2.2 },
    { "DOVER",            "Dover",           false, 50.9,   1.0 },
    { "WIGHT",            "Wight",           false, 50.3,  -1.3 },
    { "PORTLAND",         "Portland",        false, 50.0,  -2.8 },
    { "PLYMOUTH",         "Plymouth",        false, 49.6,  -4.6 },
    { "BISCAY",           "Biscay",          false, 46.0,  -4.5 },
    { "TRAFALGAR",        "Trafalgar",       false, 37.0, -10.5 },
    { "FITZROY",          "FitzRoy",         false, 45.5, -11.5 },
    { "SOLE",             "Sole",            false, 49.0, -10.0 },
    { "LUNDY",            "Lundy",           false, 51.3,  -5.6 },
    { "FASTNET",          "Fastnet",         false, 50.9,  -8.0 },
    { "IRISH SEA",        "Irish Sea",       false, 53.6,  -5.4 },
    { "SHANNON",          "Shannon",         false, 53.0, -12.5 },
    { "ROCKALL",          "Rockall",         false, 57.5, -14.0 },
    { "MALIN",            "Malin",           false, 55.6,  -8.6 },
    { "HEBRIDES",         "Hebrides",        false, 58.0,  -9.8 },
    { "BAILEY",           "Bailey",          false, 60.5, -14.5 },
    { "FAIR ISLE",        "Fair Isle",       false, 59.8,  -2.4 },
    { "FAEROES",          "Faeroes",         false, 62.0,  -6.5 },
    { "SOUTHEAST ICELAND","SE Iceland",      false, 63.0, -11.5 },
    { "SE ICELAND",       "SE Iceland",      false, 63.0, -11.5 },
    /* Named areas only the Norwegian Meteorological Institute uses. */
    { "INNER SKAGERRAK",  "Inner Skagerrak", false, 58.0,   9.9 },
    { "OUTER SKAGERRAK",  "Outer Skagerrak", false, 57.7,   7.6 },
    { "EAST TAMPEN",      "East Tampen",     false, 61.4,   3.2 },
    { "WEST TAMPEN",      "West Tampen",     false, 61.4,   1.0 },
    /* Inshore waters (to 12 nm offshore).  Keys are the bulletin's area
     * names up to any "INCLUDING ..."; apostrophes are stripped first. */
    { "CAPE WRATH TO RATTRAY HEAD",          "Cape Wrath to Rattray Head",          true, 58.85, -2.4 },
    { "RATTRAY HEAD TO BERWICK UPON TWEED",  "Rattray Head to Berwick",             true, 56.7,  -1.9 },
    { "BERWICK UPON TWEED TO WHITBY",        "Berwick to Whitby",                   true, 55.1,  -1.1 },
    { "WHITBY TO GIBRALTAR POINT",           "Whitby to Gibraltar Point",           true, 54.0,   0.2 },
    { "GIBRALTAR POINT TO N FORELAND",       "Gibraltar Point to North Foreland",   true, 52.6,   1.95 },
    { "GIBRALTAR POINT TO NORTH FORELAND",   "Gibraltar Point to North Foreland",   true, 52.6,   1.95 },
    { "N FORELAND TO SELSEY BILL",           "North Foreland to Selsey Bill",       true, 50.65,  0.4 },
    { "NORTH FORELAND TO SELSEY BILL",       "North Foreland to Selsey Bill",       true, 50.65,  0.4 },
    { "SELSEY BILL TO LYME REGIS",           "Selsey Bill to Lyme Regis",           true, 50.45, -1.7 },
    { "LYME REGIS TO LANDS END",             "Lyme Regis to Land's End",            true, 49.95, -4.1 },
    { "LANDS END TO ST DAVIDS HEAD",         "Land's End to St David's Head",       true, 51.3,  -5.1 },
    { "ST DAVIDS HEAD TO GREAT ORME HEAD",   "St David's Head to Great Orme Head",  true, 52.55, -4.6 },
    { "GREAT ORME HEAD TO THE MULL OF GALLOWAY", "Great Orme Head to Mull of Galloway", true, 54.05, -3.65 },
    { "GREAT ORME HEAD TO MULL OF GALLOWAY", "Great Orme Head to Mull of Galloway", true, 54.05, -3.65 },
    { "ISLE OF MAN",                         "Isle of Man",                         true, 54.45, -4.95 },
    { "LOUGH FOYLE TO CARLINGFORD LOUGH",    "Lough Foyle to Carlingford Lough",    true, 54.75, -5.45 },
    { "MULL OF GALLOWAY TO MULL OF KINTYRE", "Mull of Galloway to Mull of Kintyre", true, 55.35, -5.2 },
    { "MULL OF KINTYRE TO ARDNAMURCHAN POINT", "Mull of Kintyre to Ardnamurchan Point", true, 56.2, -6.45 },
    { "THE MINCH",                           "The Minch",                           true, 57.85, -5.95 },
    { "ARDNAMURCHAN POINT TO CAPE WRATH",    "Ardnamurchan Point to Cape Wrath",    true, 57.1,  -6.9 },
    { "SHETLAND ISLES",                      "Shetland Isles",                      true, 60.3,  -0.6 },
    { "CHANNEL ISLANDS",                     "Channel Islands",                     true, 49.4,  -2.55 },
};
static const int NAVTEX_SEA_AREA_COUNT = (int)(sizeof(NAVTEX_SEA_AREAS) / sizeof(NAVTEX_SEA_AREAS[0]));

/* One area's forecast, reduced. */
struct NavtexAreaForecast {
    int         area = -1;          /* index into NAVTEX_SEA_AREAS */
    std::string wind_dir;           /* "NE", "S/SW", "CYC", "VRB", or "" */
    int         force_min = -1, force_max = -1;   /* at first */
    int         force_peak = -1;                  /* anywhere in the period */
    int         sea_min = 0, sea_max = 0;   /* 1 smooth .. 8 phenomenal, 0 unknown */
    int         vis_worst = 0;      /* at first: 1 good, 2 moderate, 3 poor, 4 very poor, 0 unknown */
    bool        fog = false;
    std::string body;               /* the area's 24 h forecast as broadcast */
};

static const char *navtex_sea_state_name(int s)
{
    static const char *n[] = { "", "SMOOTH", "SLIGHT", "MODERATE", "ROUGH", "VERY ROUGH",
                               "HIGH", "VERY HIGH", "PHENOMENAL" };
    return (s >= 0 && s <= 8) ? n[s] : "";
}

static const char *navtex_force_name(int f)
{
    switch (f) {
    case 8:  return "GALE 8";
    case 9:  return "SEV GALE 9";
    case 10: return "STORM 10";
    case 11: return "VIOLENT STORM 11";
    case 12: return "HURRICANE 12";
    default: return "";
    }
}

/* "NE 4-7 · ROUGH · POOR VIS" (UTF-8 en dash and middle dot): the conditions
 * the forecast opens with, as the shipping forecast is read, plus the worst
 * wind anywhere in it when that is a gale or more ("UP TO SEV GALE 9"). */
static std::string navtex_forecast_summary(const NavtexAreaForecast &f)
{
    std::string s;
    if (!f.wind_dir.empty()) s += f.wind_dir;
    if (f.force_max >= 0) {
        if (!s.empty()) s += ' ';
        s += std::to_string(f.force_min);
        if (f.force_max != f.force_min) s += "–" + std::to_string(f.force_max);
    }
    if (f.sea_max) {
        if (!s.empty()) s += " · ";
        s += navtex_sea_state_name(f.sea_min);
        if (f.sea_max != f.sea_min) { s += "–"; s += navtex_sea_state_name(f.sea_max); }
    }
    if (f.force_peak >= 8 && f.force_peak > f.force_max) { s += " · UP TO "; s += navtex_force_name(f.force_peak); }
    if (f.fog)                s += " · FOG";
    else if (f.vis_worst == 4) s += " · VERY POOR VIS";
    else if (f.vis_worst == 3) s += " · POOR VIS";
    return s;
}

namespace navtex_mapdata_detail {

static std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

/* Upper case, apostrophes dropped, runs of space collapsed. */
static std::string norm_line(const std::string &s)
{
    std::string o;
    for (char c : s) {
        if (c == '\'' || c == '\r') continue;
        const char u = (char)toupper((unsigned char)c);
        if (isspace((unsigned char)u)) { if (!o.empty() && o.back() != ' ') o += ' '; }
        else o += u;
    }
    return trim(o);
}

static bool starts_with(const std::string &s, const std::string &p)
{
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

static std::vector<std::string> words(const std::string &s)
{
    std::vector<std::string> w;
    std::string cur;
    for (char c : s) {
        if (isalnum((unsigned char)c)) cur += c;
        else { if (!cur.empty()) w.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) w.push_back(cur);
    return w;
}

/* An inshore header: "<AREA>[ INCLUDING ...][ - (SWW)][ 24 HR FCST:][ body]".
 * Returns the area, and in rest whatever follows "FCST:" on the line. */
static int inshore_header(const std::string &line, std::string &rest)
{
    std::string n = norm_line(line);
    for (char &c : n) if (c == ',') c = ' ';
    n = norm_line(n);
    for (int i = 0; i < NAVTEX_SEA_AREA_COUNT; i++) {
        const NavtexSeaArea &a = NAVTEX_SEA_AREAS[i];
        if (!a.inshore || !starts_with(n, a.key)) continue;
        std::string tail = trim(n.substr(strlen(a.key)));
        if (starts_with(tail, "INCLUDING ")) {
            const size_t e = tail.find(" - ");
            const size_t f = tail.find("24 HR");
            tail = trim((e != std::string::npos) ? tail.substr(e) : (f != std::string::npos) ? tail.substr(f) : "");
        }
        if (starts_with(tail, "- (SWW)")) tail = trim(tail.substr(7));
        else if (starts_with(tail, "(SWW)")) tail = trim(tail.substr(5));
        if (starts_with(tail, "24 HR FCST:")) tail = trim(tail.substr(11));
        else if (starts_with(tail, "24 HR FCST")) tail = trim(tail.substr(10));
        else if (!tail.empty()) continue;   /* "ISLE OF MAN" inside a sentence */
        rest = tail;
        return i;
    }
    return -1;
}

static bool is_qualifier(const std::string &w)
{
    static const char *q[] = { "N", "S", "E", "W", "NE", "NW", "SE", "SW", "NORTH", "SOUTH", "EAST",
                               "WEST", "NORTHEAST", "NORTHWEST", "SOUTHEAST", "SOUTHWEST", "NORTHERN",
                               "SOUTHERN", "EASTERN", "WESTERN", "FAR", "AND", "CENTRAL" };
    for (const char *x : q) if (w == x) return true;
    return false;
}

/* A shipping header: nothing but area names (and direction qualifiers such
 * as "SW FORTIES").  Returns the areas, or nothing. */
static std::vector<int> shipping_header(const std::string &line)
{
    std::vector<int> areas;
    const std::vector<std::string> w = words(norm_line(line));
    if (w.empty() || w.size() > 24) return areas;
    size_t i = 0;
    while (i < w.size()) {
        int best = -1; size_t best_len = 0;
        for (int a = 0; a < NAVTEX_SEA_AREA_COUNT; a++) {
            if (NAVTEX_SEA_AREAS[a].inshore) continue;
            const std::vector<std::string> k = words(NAVTEX_SEA_AREAS[a].key);
            if (i + k.size() > w.size() || k.size() <= best_len) continue;
            bool ok = true;
            for (size_t j = 0; j < k.size(); j++) if (w[i + j] != k[j]) { ok = false; break; }
            if (ok) { best = a; best_len = k.size(); }
        }
        if (best >= 0) {
            bool dup = false;
            for (int x : areas) if (strcmp(NAVTEX_SEA_AREAS[x].label, NAVTEX_SEA_AREAS[best].label) == 0) dup = true;
            if (!dup) areas.push_back(best);
            i += best_len;
        } else if (is_qualifier(w[i])) {
            i++;
        } else {
            return {};
        }
    }
    return areas;
}

/* Split letters from digits ("S7TO9" -> "S 7 TO 9"), punctuation to spaces. */
static std::vector<std::string> tokens(const std::string &s)
{
    std::string o;
    char prev = 0;
    for (char c : s) {
        const char u = (char)toupper((unsigned char)c);
        const bool al = isalpha((unsigned char)u), di = isdigit((unsigned char)u);
        if (!al && !di) { o += ' '; prev = 0; continue; }
        if (prev && ((al && isdigit((unsigned char)prev)) || (di && isalpha((unsigned char)prev)))) o += ' ';
        o += u;
        prev = u;
    }
    return words(o);
}

static const char *direction(const std::string &w)
{
    static const char *map[][2] = {
        { "N", "N" }, { "NE", "NE" }, { "E", "E" }, { "SE", "SE" }, { "S", "S" }, { "SW", "SW" },
        { "W", "W" }, { "NW", "NW" },
        { "NORTH", "N" }, { "NORTHEAST", "NE" }, { "EAST", "E" }, { "SOUTHEAST", "SE" },
        { "SOUTH", "S" }, { "SOUTHWEST", "SW" }, { "WEST", "W" }, { "NORTHWEST", "NW" },
        { "NORTHERLY", "N" }, { "NORTHEASTERLY", "NE" }, { "EASTERLY", "E" }, { "SOUTHEASTERLY", "SE" },
        { "SOUTHERLY", "S" }, { "SOUTHWESTERLY", "SW" }, { "WESTERLY", "W" }, { "NORTHWESTERLY", "NW" },
        { "CYCLONIC", "CYC" }, { "VRB", "VRB" }, { "VARIABLE", "VRB" },
    };
    for (auto &m : map) if (w == m[0]) return m[1];
    return nullptr;
}

/* Words that end the opening statement of a clause once it has said
 * something: "SLT OR MOD BECMG MOD OR ROUGH" opens SLT OR MOD. */
static bool is_stop(const std::string &w)
{
    static const char *st[] = { "BECMG", "BECOMING", "THEN", "OCNL", "OCCASIONALLY", "LTR", "LATER",
                                "INCR", "INCREASING", "DECR", "DECREASING", "VEER", "VEERING", "BACK",
                                "BACKING", "PERHAPS", "AT", "FOR", "IN", "EXCEPT", "BUT", "OTHERWISE",
                                "SOON", "IMMINENT", "TEMPO", "TEMPORARILY" };
    for (const char *x : st) if (w == x) return true;
    return false;
}

static std::vector<std::string> split_commas(const std::string &s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == ';') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

static int force_of(const std::string &w)
{
    if (w.empty() || !isdigit((unsigned char)w[0]) || w.size() > 2) return -1;
    const int v = atoi(w.c_str());
    return v <= 12 ? v : -1;
}

/* Wind: the opening direction and forces ("SW BACK E 3 TO 5, OCNL 6" opens
 * SW 3-5), and the strongest force named anywhere. */
static void parse_wind(const std::string &sentence, NavtexAreaForecast &f)
{
    for (const std::string &w : tokens(sentence)) {
        const int v = force_of(w);
        if (v > f.force_peak) f.force_peak = v;
    }
    std::vector<std::string> prim;
    bool num = false;
    for (const std::string &clause : split_commas(sentence)) {
        for (const std::string &w : tokens(clause)) {
            if (num && is_stop(w)) break;
            prim.push_back(w);
            if (force_of(w) >= 0) num = true;
        }
        if (num) break;
    }
    const std::vector<std::string> all = tokens(sentence);
    for (int pass = 0; pass < 2 && f.wind_dir.empty(); pass++) {
        const std::vector<std::string> &t = pass ? all : prim;
        for (size_t i = 0; i < t.size(); i++) {
            const char *d = direction(t[i]);
            if (!d) continue;
            f.wind_dir = d;
            if (i + 2 < t.size() && t[i + 1] == "OR") {
                const char *d2 = direction(t[i + 2]);
                if (d2 && strcmp(d2, "CYC") && strcmp(d2, "VRB") && strcmp(d, "CYC") && strcmp(d, "VRB"))
                    f.wind_dir += std::string("/") + d2;
            }
            break;
        }
    }
    for (const std::string &w : prim) {
        const int v = force_of(w);
        if (v < 0) continue;
        if (f.force_min < 0 || v < f.force_min) f.force_min = v;
        if (v > f.force_max) f.force_max = v;
    }
}

static int sea_word(const std::vector<std::string> &t, size_t i)
{
    const std::string &w = t[i];
    const bool very = i > 0 && t[i - 1] == "VERY";
    if (w == "SMOOTH") return 1;
    if (w == "SLT" || w == "SLIGHT") return 2;
    if (w == "MOD" || w == "MODERATE") return 3;
    if (w == "ROUGH") return very ? 5 : 4;
    if (w == "HIGH") return very ? 7 : 6;
    if (w == "PHENOMENAL") return 8;
    return 0;
}

/* Sea state the forecast opens with, as a range: the first clause that names
 * one, up to the first change ("MOD OR ROUGH, OCNL VERY ROUGH" -> 3..4). */
static void sea_states(const std::string &sentence, int &lo, int &hi)
{
    lo = hi = 0;
    for (const std::string &clause : split_commas(sentence)) {
        const std::vector<std::string> t = tokens(clause);
        for (size_t i = 0; i < t.size(); i++) {
            if (hi && is_stop(t[i])) break;
            const int v = sea_word(t, i);
            if (!v) continue;
            if (!lo || v < lo) lo = v;
            if (v > hi) hi = v;
        }
        if (hi) return;
    }
}

static bool has_sea_word(const std::string &sentence)
{
    for (const std::string &w : tokens(sentence))
        if (w == "SMOOTH" || w == "SLT" || w == "SLIGHT" || w == "ROUGH" || w == "PHENOMENAL" || w == "HIGH")
            return true;
    return false;
}

static int vis_word(const std::vector<std::string> &t, size_t i, bool &fog)
{
    const std::string &w = t[i];
    if (w == "GOOD") return 1;
    if (w == "MOD" || w == "MODERATE") return 2;
    if (w == "POOR") return (i > 0 && t[i - 1] == "VERY") ? 4 : 3;
    if (w == "FOG") { fog = true; return 4; }
    return 0;
}

/* Visibility sentence ("GOOD, OCNL POOR"): 0 when it is not one, else the
 * worst in its opening statement — GOOD here, since POOR is only at times. */
static int visibility(const std::string &sentence, bool &fog)
{
    int seen = 0;
    {
        bool f2 = false;
        const std::vector<std::string> t = tokens(sentence);
        for (size_t i = 0; i < t.size(); i++) if (vis_word(t, i, f2)) seen++;
    }
    if (!seen) return 0;
    int worst = 0;
    for (const std::string &clause : split_commas(sentence)) {
        const std::vector<std::string> t = tokens(clause);
        for (size_t i = 0; i < t.size(); i++) {
            if (worst && is_stop(t[i])) break;
            const int v = vis_word(t, i, fog);
            if (v > worst) worst = v;
        }
        if (worst) break;
    }
    return worst;
}

/* Sentences: split on '.', but not inside a number. */
static std::vector<std::string> sentences(const std::string &body)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < body.size(); i++) {
        const char c = body[i];
        if (c == '.' && !(i > 0 && isdigit((unsigned char)body[i - 1]) && i + 1 < body.size()
                          && isdigit((unsigned char)body[i + 1]))) {
            if (!trim(cur).empty()) out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += (c == '\n' || c == '\r') ? ' ' : c;
        }
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

static bool reduce(const std::string &body, NavtexAreaForecast &f)
{
    const std::vector<std::string> s = sentences(body);
    if (s.empty()) return false;
    parse_wind(s[0], f);
    if (f.force_max < 0) return false;
    size_t last = s.size();
    bool fog = false;
    if (s.size() >= 2) {
        const int v = visibility(s.back(), fog);
        if (v && !has_sea_word(s.back())) { f.vis_worst = v; last = s.size() - 1; }
        else fog = false;
    }
    for (size_t i = 1; i < last; i++) {
        if (has_sea_word(s[i]) || i == 1) {   /* the second sentence is the sea state, even "MOD." */
            sea_states(s[i], f.sea_min, f.sea_max);
            if (f.sea_max) break;
        }
    }
    /* Fog in the visibility's opening statement, or opening the weather
     * ("FOG PATCHES. MOD OR POOR"). */
    for (size_t i = 1; i < last; i++) {
        const std::vector<std::string> t = tokens(split_commas(s[i])[0]);
        if (!t.empty() && (t[0] == "FOG" || (t.size() > 1 && t[1] == "FOG"))) fog = true;
    }
    f.fog = fog;
    return f.sea_max > 0 || f.vis_worst > 0;
}

static bool ends_body(const std::string &n)
{
    return starts_with(n, "OUTLOOK") || starts_with(n, "NNNN") || starts_with(n, "THE GENERAL")
        || starts_with(n, "GENERAL SITUATION") || starts_with(n, "GENERAL SYNOPSIS")
        || starts_with(n, "STORM WARNING") || starts_with(n, "GALE WARNING");
}

}  // namespace navtex_mapdata_detail

/* Every area forecast in a Met Office bulletin; empty for anything else. */
static std::vector<NavtexAreaForecast> navtex_parse_forecasts(const std::string &text)
{
    using namespace navtex_mapdata_detail;
    std::vector<NavtexAreaForecast> out;
    if (text.find("MET OFFICE") == std::string::npos
        && text.find("NORWEGIAN METEOROLOGICAL INSTITUTE") == std::string::npos) return out;
    /* High-seas bulletins (METAREA I sections) are not sea areas. */
    if (text.find("HIGH SEAS BULLETIN") != std::string::npos) return out;

    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : text) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else cur += c;
        }
        lines.push_back(cur);
    }

    std::vector<int> areas;
    std::string body;
    bool collecting = false;
    auto flush = [&]() {
        if (collecting && !areas.empty()) {
            for (int a : areas) {
                NavtexAreaForecast f;
                f.area = a;
                f.body = trim(body);
                if (reduce(body, f)) {
                    /* A later copy in the same bulletin (it happens when an
                     * area is repeated) replaces the earlier. */
                    bool replaced = false;
                    for (auto &o : out)
                        if (strcmp(NAVTEX_SEA_AREAS[o.area].label, NAVTEX_SEA_AREAS[a].label) == 0) {
                            o = f; replaced = true;
                        }
                    if (!replaced) out.push_back(f);
                }
            }
        }
        areas.clear();
        body.clear();
        collecting = false;
    };

    for (const std::string &raw : lines) {
        const std::string n = norm_line(raw);
        std::string rest;
        const int in = inshore_header(raw, rest);
        if (in >= 0) {
            flush();
            areas = { in };
            body = rest;
            collecting = true;
            continue;
        }
        const std::vector<int> sh = shipping_header(raw);
        if (!sh.empty()) {
            flush();
            areas = sh;
            collecting = true;
            continue;
        }
        if (!collecting) continue;
        if (ends_body(n)) { flush(); continue; }
        /* The Norwegian bulletins indent every area name, including the
         * coded ones ("  C3") this does not place: one ends the body. */
        if (raw.size() > 2 && raw[0] == ' ' && raw[1] == ' ' && !trim(body).empty()) { flush(); continue; }
        if (n.empty()) {
            if (!trim(body).empty()) flush();
            continue;
        }
        if (n == "24 HR FCST:" || n == "24 HR FCST") continue;
        body += ' ';
        body += raw;
    }
    flush();
    return out;
}
