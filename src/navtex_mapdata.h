/* navtex_mapdata.h — what a NAVTEX message says about places, for the map.
 *
 * Readers of message text, all pure (no I/O, no globals) so they can be
 * tested against real broadcasts without a server:
 *
 *   navtex_parse_positions()  every "50-46.3N 001-17.8W"-style position in a
 *                             warning, in the formats seen on air (degrees and
 *                             decimal minutes with '-', ' ' or '!', with
 *                             seconds after an apostrophe, a dash or a space,
 *                             the compact DDMMN DDDMMW form, and the Greek
 *                             stations' B/A for north/east);
 *   navtex_warning_kind()     what the warning is about (buoy, light, wind
 *                             farm, cable, ...), from keywords;
 *   navtex_parse_forecasts()  sea-area forecasts, split per area and reduced
 *                             to "NE 4-7 · ROUGH · POOR VIS";
 *   navtex_parse_gale_warnings()  gale warnings (subject B), per sea area.
 *
 * Received text is noisy: a garbled character turns "ROUGH" into "RO?GH" and
 * a digit into another digit.  Everything here prefers to say nothing over
 * saying something wrong — an area name must match exactly, a forecast needs
 * a wind force to count, a garbled sentence is skipped, and positions are
 * range-checked by the caller against the transmitting station.
 *
 * Forecasts are read from these bulletins (sea-area positions are label
 * anchors only, a point inside each area at sea, not area boundaries):
 *
 *   Met Office shipping and inshore waters forecasts, also when relayed by an
 *   Irish station without the "MET OFFICE" line;
 *   the Norwegian Meteorological Institute's, which name the North Sea areas
 *   the same way (its coded areas such as "C3" are not placed);
 *   Met Éireann's coastal waters forecast ("FORECAST FOR MIZEN HEAD TO SLYNE
 *   HEAD TO MALIN HEAD"), mapped onto fixed stretches of the Irish coast;
 *   KNMI's "FORECAST DUTCH EEZ" (wave heights in metres, no sea state);
 *   DWD's German Bight forecast in English and its German sea and coastal
 *   forecasts (Seewetter-/Küstenvorhersage);
 *   Oostende Radio's English forecast for Thames, Dover and the Belgian coast.
 *
 * Not read (see the comments at navtex_parse_forecasts()): high-seas and
 * extended-outlook bulletins, the Faroese and Icelandic forecasts (wind in
 * m/s), Météo-France's and AEMET's, and the other languages.
 */
#pragma once

#include <algorithm>
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

/* Degrees, minutes (decimal), and a third field -> decimal degrees, or NAN
 * when a part is out of range (a garbled digit, usually).  The third field
 * is seconds, or with frac set thousandths of a minute ("51-21-663N" is
 * 51°21.663'). */
static double dms(const std::string &d, const std::string &m, const std::string &s, bool frac, double maxdeg)
{
    const double D = num(d);
    double M = num(m), S = 0;
    if (!s.empty()) {
        if (frac) M += num("0." + s);
        else      S = num(s);
    }
    if (D > maxdeg || M >= 60.0 || S >= 60.0) return NAN;
    const double v = D + M / 60.0 + S / 3600.0;
    return v > maxdeg ? NAN : v;
}

static double km_between(double la1, double lo1, double la2, double lo2)
{
    const double r = M_PI / 180.0;
    const double a = sin((la2 - la1) * r / 2), b = sin((lo2 - lo1) * r / 2);
    return 2 * 6371.0 * asin(sqrt(a * a + cos(la1 * r) * cos(la2 * r) * b * b));
}

static double median(std::vector<double> v)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

/* A garbled degree digit moves one corner of an area hundreds of km ("45-1-21N
 * 13-35-3E" among corners at 36°E).  With three or more positions, one far
 * from the others' median (300 km, and five times the typical spread) is
 * dropped. */
static void drop_outliers(std::vector<NavtexPos> &v)
{
    if (v.size() < 3) return;
    std::vector<double> la, lo, d;
    for (const auto &p : v) { la.push_back(p.lat); lo.push_back(p.lon); }
    const double mla = median(la), mlo = median(lo);
    for (const auto &p : v) d.push_back(km_between(mla, mlo, p.lat, p.lon));
    const double lim = std::max(300.0, 5 * median(d));
    std::vector<NavtexPos> keep;
    for (size_t i = 0; i < v.size(); i++) if (d[i] <= lim) keep.push_back(v[i]);
    v.swap(keep);
}

/* The text a reader should see: upper case, and cut at a second "ZCZC" — a
 * lost "NNNN" glues the next broadcast on, whose positions and keywords
 * belong to another warning. */
static std::string one_message(const std::string &text)
{
    std::string t;
    t.reserve(text.size());
    for (char c : text) t += (char)toupper((unsigned char)c);
    const size_t z = t.find("ZCZC");
    const size_t z2 = t.find("ZCZC", z == std::string::npos ? 0 : z + 4);
    if (z2 != std::string::npos) t.resize(z2);
    return t;
}

/* One coordinate of a position match: which of the five forms matched, and
 * the value.  Groups start at g: dash DMS (3), apostrophe DMS (3), space DMS
 * (3), degrees-minutes (2), compact (2). */
enum { POS_DASH_DMS, POS_APOS_DMS, POS_SPACE_DMS, POS_DM, POS_COMPACT, POS_NONE };

static int coord_form(const std::smatch &m, int g)
{
    if (m[g].matched)      return POS_DASH_DMS;
    if (m[g + 3].matched)  return POS_APOS_DMS;
    if (m[g + 6].matched)  return POS_SPACE_DMS;
    if (m[g + 9].matched)  return POS_DM;
    if (m[g + 11].matched) return POS_COMPACT;
    return POS_NONE;
}

static double coord_value(const std::smatch &m, int g, int form, double maxdeg)
{
    switch (form) {
    case POS_DASH_DMS: {
        /* Three digits after the second dash are a fraction of a minute;
         * two are seconds. */
        const std::string s = m[g + 2];
        const bool frac = s.size() == 3 && s.find_first_of(".,") == std::string::npos;
        return dms(m[g], m[g + 1], s, frac, maxdeg);
    }
    case POS_APOS_DMS:  return dms(m[g + 3], m[g + 4], m[g + 5], false, maxdeg);
    case POS_SPACE_DMS: return dms(m[g + 6], m[g + 7], m[g + 8], false, maxdeg);
    case POS_DM:        return dms(m[g + 9], m[g + 10], "", false, maxdeg);
    case POS_COMPACT:   return dms(m[g + 11], m[g + 12], "", false, maxdeg);
    default:            return NAN;
    }
}

}  // namespace navtex_mapdata_detail

static std::vector<NavtexPos> navtex_parse_positions(const std::string &text)
{
    using namespace navtex_mapdata_detail;
    /* lat:  DD-MM-SS(.s) | DD-MM-mmm | DD MM'SS''  | DD MM SS(.s) |
     *       DD-MM.mm (sep '-', spaces or '!', an optional ' after) | DDMM.mm
     *       then N, S, or B (Greek "Voreio", north)
     * lon:  the same with up to three degree digits, then E, W, or A (Greek
     *       "Anatoliko", east)
     * between: up to six of space , / ; - and line breaks. */
#define NAVTEX_NUMF "(?:[.,]\\d+)?"
#define NAVTEX_COORD(D, D2, D3)                                                        \
    "(?:(\\d{" D "})-(\\d{2})-(\\d{2,3}" NAVTEX_NUMF ")"                                 \
    "|(\\d{" D "})[- ]+(\\d{1,2}" NAVTEX_NUMF ")'(\\d{1,2}" NAVTEX_NUMF ")(?:''|\")?"      \
    "|(\\d{" D2 "}) +(\\d{2}) +(\\d{2}" NAVTEX_NUMF ")"                                  \
    "|(\\d{" D "})(?: *- *| +|! ?)(\\d{1,2}" NAVTEX_NUMF ")'?"                         \
    "|(\\d{" D3 "})(\\d{2}" NAVTEX_NUMF "))"
    static const std::regex re(
        "\\b" NAVTEX_COORD("1,2", "2", "2") " *([NSB])"
        "[ \\t\\r\\n,/;-]{0,6}"
        NAVTEX_COORD("1,3", "2,3", "3") " *([EWA])\\b");
#undef NAVTEX_COORD
#undef NAVTEX_NUMF
    const int LAT = 1, LAT_H = 14, LON = 15, LON_H = 28;
    /* "008-39.29W40-00.45N": a lost space between one position and the next. */
    static const std::regex glued("(\\d['\"]?)([NSEWBA])(?=\\d)");
    const std::string t = std::regex_replace(one_message(text), glued, "$1$2 ");
    std::vector<NavtexPos> out;
    std::smatch m;
    auto it = t.cbegin();
    while (it != t.cend() && std::regex_search(it, t.cend(), m, re,
                                               it == t.cbegin() ? std::regex_constants::match_default
                                                                : std::regex_constants::match_prev_avail)) {
        const auto start = it + m.position(0);
        const int fl = coord_form(m, LAT), fo = coord_form(m, LON);
        const bool greek_lat = m[LAT_H] == "B", greek_lon = m[LON_H] == "A";
        /* "11 44 31.5N 012 30.5E" is item 11 and a degrees-minutes position,
         * not 11°44'31.5": a space-separated DMS needs a DMS partner.  And
         * the Greek letters come as a pair. */
        if ((fl == POS_SPACE_DMS) != (fo == POS_SPACE_DMS) || greek_lat != greek_lon) {
            it = t.cbegin() + (m.position(LAT_H) + (it - t.cbegin())) + 1;
            continue;
        }
        it = start + m.length(0);
        const double lat = coord_value(m, LAT, fl, 90), lon = coord_value(m, LON, fo, 180);
        if (std::isnan(lat) || std::isnan(lon)) continue;
        NavtexPos p { m[LAT_H] == "S" ? -lat : lat, m[LON_H] == "W" ? -lon : lon };
        bool dup = false;
        for (const auto &q : out) if (fabs(q.lat - p.lat) < 1e-6 && fabs(q.lon - p.lon) < 1e-6) { dup = true; break; }
        if (!dup) out.push_back(p);
    }
    drop_outliers(out);
    return out;
}

/* ------------------------------------------------------------------ */
/* Warning kinds                                                       */
/* ------------------------------------------------------------------ */

/* Ordered: the first keyword group that appears wins, so "LIGHT BUOY" is a
 * buoy and "WIND FARM ... LIGHTS UNLIT" is a wind farm.  Stock phrases that
 * contain a keyword ("EXERCISE CAUTION", "A SAFE DISTANCE OF 2 CABLES") are
 * removed first, and a wind farm named only as the place ("NORTH OF
 * OFFSHORE WINDPARK 'X'. AMMUNITION FOUND") counts only when nothing else
 * does. */
static const char *navtex_warning_kind(const std::string &text, char subject)
{
    using namespace navtex_mapdata_detail;
    if (subject == 'D') return "sar";
    const std::string u = one_message(text);
    std::string t;
    t.reserve(u.size() + 2);
    t += ' ';
    for (char c : u) {
        const char x = isalnum((unsigned char)c) ? c : ' ';
        if (x == ' ' && t.back() == ' ') continue;
        t += x;
    }
    t += ' ';
    auto erase_all = [&](const char *p) {
        size_t i;
        while ((i = t.find(p)) != std::string::npos) t.replace(i, strlen(p), " ");
    };
    for (const char *p : { " EXERCISE EXTREME CAUTION ", " EXERCISE GREAT CAUTION ", " EXERCISE DUE CAUTION ",
                           " EXERCISE CAUTION ", " EXERCISE CARE ", " EXEZCISE EXTREME CAUTION " })
        erase_all(p);
    /* "2 CABLES", "0 5 CABLE" (0.5), "TWO CABLES": a distance. */
    {
        static const std::regex dist(" (?:\\d+|ONE|TWO|THREE|FOUR|FIVE|SIX|SEVEN|EIGHT|NINE|TEN|HALF A) CABLES? ");
        t = std::regex_replace(t, dist, " ");
        erase_all(" CABLE LENGTHS ");
        erase_all(" CABLES LENGTH ");
    }
    auto has = [&](const char *w) { return t.find(w) != std::string::npos; };

    /* Explosives found (unexploded ordnance) are a hazard where they lie;
     * blowing them up is an operation like firing. */
    if (has(" AMMUNITION FOUND") || has(" AMMUNITION FINDING") || has(" MUNITION") || has(" UXO ")
        || has(" UNEXPLODED") || has(" ORDNANCE") || has(" EXPLOSIEVEN") || has(" KAMPFMITTEL")
        || has(" EKSPLOZIVN") || has(" MINE ") || has(" MINES ") || has(" DRIFTING MINE")
        || has(" LOST AN EXPLOSIVE") || has(" EXPLOSIVE FOUND") || has(" EXPLOSIVES FOUND")) {
        if (has(" DETONATION") || has(" DISPOSAL") || has(" CONTROLLED EXPLOSION") || has(" SPRENG")
            || has(" FIRING") || has(" GUNNERY") || has(" EXERCISE") || has(" MINE LAYING")
            || has(" MINEHUNT") || has(" MINE COUNTERMEASURE") || has(" SWEEPING"))
            return "military";
        return "hazard";
    }
    /* A wind farm as the subject, not as where something else is. */
    static const std::regex place_farm(
        " (?:N|S|E|W|NE|NW|SE|SW|NORTH|SOUTH|EAST|WEST|NORTHEAST|NORTHWEST|SOUTHEAST|SOUTHWEST|"
        "NORTHERLY|SOUTHERLY|EASTERLY|WESTERLY|NORTHERN|SOUTHERN|EASTERN|WESTERN|"
        "NOERDLICH|SUEDLICH|OESTLICH|WESTLICH|NORDOESTLICH|NORDWESTLICH|SUEDOESTLICH|SUEDWESTLICH)"
        "(?: OF)?(?: THE)?(?: OFFSHORE)? (?:WIND ?FARM|WINDPARK|WINDENERGIEPARK|OWP) ");
    const bool farm_place = std::regex_search(t, place_farm);
    const bool farm = has(" WIND FARM") || has(" WINDFARM") || has(" TURBINE") || has(" WINDPARK")
                   || has(" WINDENERGIEPARK") || has(" OWP ") || has(" WINDMOLENPARK");
    if (farm && !farm_place) return "windfarm";
    if (has(" PERSON IN THE WATER") || has(" OVERDUE") || has(" MISSING PERSON") || has(" MAN OVERBOARD")
        || has(" SEARCH AND RESCUE") || has(" MAYDAY") || has(" PERSONS MISSING") || has(" MISSING VESSEL")
        || has(" MISSING DIVER") || has(" IS MISSING") || has(" ARE MISSING")) {
        /* "BUOY ... IS MISSING" is a buoy. */
        if (!(has("BUOY") || has(" LIGHT ") || has(" BEACON") || has(" MARK "))) return "sar";
    }
    /* Also the Dutch / German words some coastal stations use. */
    if (has(" CABLE") || has(" PIPELINE") || has(" PIPE LAYING") || has("KABEL")) return "cable";
    if (has("BUOY") || has(" BEACON") || has(" MARK ") || has(" WAVERIDER") || has("BOEI")
        || has(" TONNE ") || has("LEUCHTTONNE") || has(" TONNEN ") || has(" BOUEE") || has(" BALISE")
        || has(" BOYA") || has(" BOA ")) return "buoy";
    /* A platform with its lights out is a platform. */
    if (has(" RIG ") || has(" RIGS ") || has(" PLATFORM") || has(" DRILLING") || has(" JACK UP")
        || has(" JACKUP") || has(" FPSO")) return "rig";
    if (has(" LIGHT ") || has(" LIGHTS ") || has(" LIGHTHOUSE") || has(" UNLIT") || has(" LT ")
        || has(" LICHT ") || has(" FEU ") || has(" LEUCHTFEUER") || has(" LEUCHTTURM") || has(" VUURTOREN")
        || has(" LIGHT VESSEL") || has(" LIGHTVESSEL") || has(" FARO ")) return "light";
    if (has(" WRECK") || has(" DERELICT") || has(" ADRIFT") || has(" DRIFTING") || has(" CONTAINER")
        || has(" OBSTRUCTION") || has(" SUNKEN") || has(" SUBMERGED") || has(" DEBRIS")
        || has(" ANCHOR LOST") || has(" ANCHOR AND CHAIN LOST") || has(" ANKER VERLOREN")
        || has(" OBSTACLE") || has("HINDERNIS") || has(" WRAK") || has(" ICEBERG")) return "hazard";
    if (has(" FIRING") || has(" GUNNERY") || has(" MISSILE") || has(" EXERCISE") || has(" MILITARY")
        || has(" ROCKET") || has(" LIVE AMMUNITION") || has(" EXPLOSIVE") || has(" TORPEDO")
        || has(" SUBMARINE EXERCISE") || has(" DIVED SUBMARINE") || has(" SCHIESS") || has(" EXERCICIO")
        || has(" EJERCICIO") || has(" ESERCITAZION") || has(" PYRON ")) return "military";
    if (has(" SURVEY") || has(" SEISMIC") || has(" DREDG") || has(" HYDROGRAPHIC") || has(" OCEANOGRA")
        || has(" VERMESSUNG") || has(" BAGGER")) return "survey";
    if (has(" TOW ") || has(" TOWING") || has(" TOWED") || has(" TOW OF ")) return "tow";
    if (farm) return "windfarm";
    return "warning";
}

/* ------------------------------------------------------------------ */
/* Forecasts                                                           */
/* ------------------------------------------------------------------ */

/* Which bulletins name an area: the Met Office / Norwegian names are read
 * from those bulletins' headers; the others only by their own readers. */
enum NavtexAreaSource {
    NAVTEX_SRC_MET_OFFICE = 0,   /* also KNMI, which uses the same area names */
    NAVTEX_SRC_IRISH,            /* fixed stretches of the Irish coast */
    NAVTEX_SRC_DWD,              /* DWD's German area names */
    NAVTEX_SRC_BELGIAN,          /* Oostende Radio */
};

struct NavtexSeaArea {
    const char *key;     /* as written in the forecast (inshore: the "A TO B" part) */
    const char *label;   /* as shown on the map */
    bool        inshore;
    double      lat, lon;   /* label anchor, at sea */
    int         src;        /* NavtexAreaSource */
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
    /* Met Éireann's coastal waters (to 30 nm): its forecasts name a
     * stretch by headlands ("MIZEN HEAD TO SLYNE HEAD TO MALIN HEAD") and the
     * stretches change from one bulletin to the next, so each is mapped onto
     * these fixed sections between the main headlands (see IRISH_HEADLANDS).
     * Anchors: a point offshore of each section's middle, placed by hand
     * from the coastline's shape, good to about 0.1° (not surveyed). */
    { "MALIN HEAD-FAIR HEAD",           "Malin Head to Fair Head",           true, 55.42,  -6.75, NAVTEX_SRC_IRISH },
    { "FAIR HEAD-CARLINGFORD LOUGH",    "Fair Head to Carlingford Lough",    true, 54.65,  -5.35, NAVTEX_SRC_IRISH },
    { "CARLINGFORD LOUGH-WICKLOW HEAD", "Carlingford Lough to Wicklow Head", true, 53.55,  -5.85, NAVTEX_SRC_IRISH },
    { "WICKLOW HEAD-CARNSORE POINT",    "Wicklow Head to Carnsore Point",    true, 52.55,  -6.0,  NAVTEX_SRC_IRISH },
    { "CARNSORE POINT-ROCHES POINT",    "Carnsore Point to Roches Point",    true, 51.85,  -7.3,  NAVTEX_SRC_IRISH },
    { "ROCHES POINT-MIZEN HEAD",        "Roches Point to Mizen Head",        true, 51.35,  -8.85, NAVTEX_SRC_IRISH },
    { "MIZEN HEAD-LOOP HEAD",           "Mizen Head to Loop Head",           true, 51.95, -10.7,  NAVTEX_SRC_IRISH },
    { "LOOP HEAD-SLYNE HEAD",           "Loop Head to Slyne Head",           true, 53.0,  -10.15, NAVTEX_SRC_IRISH },
    { "SLYNE HEAD-ERRIS HEAD",          "Slyne Head to Erris Head",          true, 53.9,  -10.5,  NAVTEX_SRC_IRISH },
    { "ERRIS HEAD-BLOODY FORELAND",     "Erris Head to Bloody Foreland",     true, 54.75,  -9.25, NAVTEX_SRC_IRISH },
    { "BLOODY FORELAND-MALIN HEAD",     "Bloody Foreland to Malin Head",     true, 55.4,   -7.85, NAVTEX_SRC_IRISH },
    /* DWD's areas, as named in its German bulletins ("DEUTSCHE BUCHT" is the
     * German Bight, above).  Anchors: approximate points at sea inside each
     * area as DWD's sea and coastal forecasts divide the coast, placed by
     * hand (about 0.1-0.2°; the Elbe one is on the river). */
    { "DEUTSCHE BUCHT",                          "German Bight",               false, 54.4,  6.5,  NAVTEX_SRC_DWD },
    { "WESTLICHE OSTSEE",                        "Western Baltic",             false, 54.6, 11.2,  NAVTEX_SRC_DWD },
    { "SUEDLICHE OSTSEE",                        "Southern Baltic",            false, 55.0, 14.8,  NAVTEX_SRC_DWD },
    { "OSTFRIESISCHE KUESTE",                    "East Frisian coast",         true,  53.8,  7.2,  NAVTEX_SRC_DWD },
    { "ELBMUENDUNG",                             "Elbe estuary",               true,  53.95, 8.45, NAVTEX_SRC_DWD },
    { "HELGOLAND",                               "Helgoland",                  true,  54.2,  7.75, NAVTEX_SRC_DWD },
    { "NORDFRIESISCHE KUESTE",                   "North Frisian coast",        true,  54.6,  8.15, NAVTEX_SRC_DWD },
    { "ELBE VON HAMBURG BIS CUXHAVEN",           "Elbe, Hamburg to Cuxhaven",  true,  53.75, 9.3,  NAVTEX_SRC_DWD },
    { "FLENSBURG BIS FEHMARN",                   "Flensburg to Fehmarn",       true,  54.6, 10.4,  NAVTEX_SRC_DWD },
    { "OESTLICH FEHMARN BIS RUEGEN",             "East of Fehmarn to Rügen",   true,  54.35, 11.9, NAVTEX_SRC_DWD },
    { "BODDENGEWAESSER OST UND OESTLICH RUEGEN", "East of Rügen",              true,  54.45, 13.9, NAVTEX_SRC_DWD },
    /* Oostende Radio's forecast for the Belgian coast. */
    { "BELGIAN COAST",                           "Belgian coast",              true,  51.35, 2.75, NAVTEX_SRC_BELGIAN },
};
static const int NAVTEX_SEA_AREA_COUNT = (int)(sizeof(NAVTEX_SEA_AREAS) / sizeof(NAVTEX_SEA_AREAS[0]));

/* One area's forecast, reduced. */
struct NavtexAreaForecast {
    int         area = -1;          /* index into NAVTEX_SEA_AREAS */
    std::string wind_dir;           /* "NE", "S/SW", "CYC", "VRB", or "" */
    int         force_min = -1, force_max = -1;   /* at first */
    int         force_peak = -1;                  /* anywhere in the period */
    int         sea_min = 0, sea_max = 0;   /* 1 smooth .. 8 phenomenal, 0 unknown */
    double      wave_min = -1, wave_max = -1;  /* at first, metres (KNMI, DWD), -1 unknown */
    bool        wave_upto = false;  /* "up to wave_max" */
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

namespace navtex_mapdata_detail {

static std::string metres(double m)
{
    char b[16];
    snprintf(b, sizeof(b), "%.1f", m);
    std::string s = b;
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
    return s;
}

}  // namespace navtex_mapdata_detail

/* "NE 4-7 · ROUGH · POOR VIS" (UTF-8 en dash and middle dot): the conditions
 * the forecast opens with, as the shipping forecast is read, plus the worst
 * wind anywhere in it when that is a gale or more ("UP TO SEV GALE 9").
 * Where the forecast gives wave heights instead of a sea state: "WAVES 1–1.5 M". */
static std::string navtex_forecast_summary(const NavtexAreaForecast &f)
{
    using navtex_mapdata_detail::metres;
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
    } else if (f.wave_max >= 0) {
        if (!s.empty()) s += " · ";
        s += f.wave_upto ? "WAVES UP TO " : "WAVES ";
        if (!f.wave_upto && f.wave_min >= 0 && f.wave_min != f.wave_max) s += metres(f.wave_min) + "–";
        s += metres(f.wave_max) + " M";
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

static std::vector<std::string> split_lines(const std::string &text)
{
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    lines.push_back(cur);
    return lines;
}

/* Characters a clean broadcast does not use in a forecast: two or more in a
 * sentence and it is garbled ("754.8SLT OR M D, 75,) 4&)#954"), and better
 * skipped than read. */
static bool garbled(const std::string &s)
{
    int n = 0;
    for (char c : s) {
        const unsigned char u = (unsigned char)c;
        if (isalnum(u) || isspace(u) || strchr(".,;:/()-'+", c)) continue;
        n++;
    }
    return n >= 2;
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
        if (!a.inshore || a.src != NAVTEX_SRC_MET_OFFICE || !starts_with(n, a.key)) continue;
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

/* The shipping area (Met Office / Norwegian names) whose name starts at
 * w[i], the longest that matches; its length in words in len. */
static int area_at(const std::vector<std::string> &w, size_t i, size_t &len)
{
    int best = -1;
    len = 0;
    for (int a = 0; a < NAVTEX_SEA_AREA_COUNT; a++) {
        if (NAVTEX_SEA_AREAS[a].inshore || NAVTEX_SEA_AREAS[a].src != NAVTEX_SRC_MET_OFFICE) continue;
        const std::vector<std::string> k = words(NAVTEX_SEA_AREAS[a].key);
        if (i + k.size() > w.size() || k.size() <= len) continue;
        bool ok = true;
        for (size_t j = 0; j < k.size(); j++) if (w[i + j] != k[j]) { ok = false; break; }
        if (ok) { best = a; len = k.size(); }
    }
    return best;
}

static bool same_area(int a, int b)
{
    return a >= 0 && b >= 0 && strcmp(NAVTEX_SEA_AREAS[a].label, NAVTEX_SEA_AREAS[b].label) == 0;
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
        size_t len = 0;
        const int best = area_at(w, i, len);
        if (best >= 0) {
            bool dup = false;
            for (int x : areas) if (same_area(x, best)) dup = true;
            if (!dup) areas.push_back(best);
            i += len;
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
 * something: "SLT OR MOD BECMG MOD OR ROUGH" opens SLT OR MOD.  With the
 * German ones DWD uses. */
static bool is_stop(const std::string &w)
{
    static const char *st[] = { "BECMG", "BECOMING", "THEN", "OCNL", "OCCASIONALLY", "LTR", "LATER",
                                "INCR", "INCREASING", "DECR", "DECREASING", "VEER", "VEERING", "BACK",
                                "BACKING", "PERHAPS", "AT", "FOR", "IN", "EXCEPT", "BUT", "OTHERWISE",
                                "SOON", "IMMINENT", "TEMPO", "TEMPORARILY", "VEERS", "BACKS", "EASING",
                                "SHIFTING", "ELSEWHERE", "GUSTS", "GUST",
                                "SPAETER", "ZUNEHMEND", "ABNEHMEND", "DREHEND", "ABFLAUEND", "BOEEN",
                                "VORUEBERGEHEND", "ZEITWEISE", "SONST", "UND", "RASCH", "ALLMAEHLICH" };
    for (const char *x : st) if (w == x) return true;
    return false;
}

/* Words a modifying clause starts with ("OCNL 6 IN S UTSIRE"): not the
 * opening statement. */
static bool is_modifier(const std::string &w)
{
    static const char *st[] = { "BECMG", "BECOMING", "THEN", "OCNL", "OCCASIONALLY", "LTR", "LATER",
                                "INCR", "INCREASING", "DECR", "DECREASING", "VEER", "VEERING", "BACK",
                                "BACKING", "PERHAPS", "SOON", "IMMINENT", "TEMPO", "TEMPORARILY", "VEERS",
                                "BACKS", "EASING", "SHIFTING" };
    for (const char *x : st) if (w == x) return true;
    return false;
}

/* "WORSW" -> W OR SW, "BACKNW" -> BACK NW, "INN" -> IN N: words the
 * broadcast ran together, when they split exactly into these; and "3105",
 * 3 TO 5. */
static std::vector<std::string> unglue(const std::vector<std::string> &in)
{
    static const char *v[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW", "OR", "TO", "BACK", "BACKS",
                               "VEER", "VEERS", "BECMG", "OCNL", "INCR", "DECR", "IN", "AT", "LTR", "THEN",
                               "AND" };
    std::vector<std::string> out;
    for (const std::string &w : in) {
        /* "4106" is "4 TO 6" as some relays print it (never a force). */
        if (w.size() == 4 && isdigit((unsigned char)w[0]) && w[1] == '1' && w[2] == '0' && isdigit((unsigned char)w[3])
            && w[0] > '0' && w[0] < w[3]) {
            out.push_back(w.substr(0, 1));
            out.push_back("TO");
            out.push_back(w.substr(3, 1));
            continue;
        }
        bool known = w.size() < 3 || !isalpha((unsigned char)w[0]) || direction(w) || is_stop(w);
        if (!known) for (const char *x : v) if (w == x) known = true;
        if (known) { out.push_back(w); continue; }
        /* Fewest pieces, by dynamic programming over prefixes. */
        const size_t n = w.size();
        std::vector<int> best(n + 1, 1 << 20), from(n + 1, -1);
        best[0] = 0;
        for (size_t i = 0; i < n; i++) {
            if (best[i] >= (1 << 20)) continue;
            for (const char *x : v) {
                const size_t l = strlen(x);
                if (i + l <= n && w.compare(i, l, x) == 0 && best[i] + 1 < best[i + l]) {
                    best[i + l] = best[i] + 1;
                    from[i + l] = (int)i;
                }
            }
        }
        if (best[n] >= (1 << 20) || best[n] < 2) { out.push_back(w); continue; }
        std::vector<std::string> parts;
        for (size_t j = n; j > 0; j = (size_t)from[j]) parts.insert(parts.begin(), w.substr((size_t)from[j], j - (size_t)from[j]));
        for (auto &p : parts) out.push_back(p);
    }
    return out;
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

static std::string join_commas(const std::vector<std::string> &v)
{
    std::string s;
    for (const auto &x : v) { if (!s.empty()) s += ", "; s += x; }
    return s;
}

/* Clauses of a shipping-forecast sentence and the areas they are limited to:
 * "SLT OR MOD IN W CROMARTY, FORTH, TYNE AND W HUMBER, BUT ELSEWHERE MOD OR
 * ROUGH" is two clauses, the first for Cromarty, Forth, Tyne and Humber, the
 * second for the rest of the group. */
struct Clause {
    std::string      text;
    std::vector<int> areas;        /* named after "IN" */
    bool             except = false;   /* "EXCEPT IN ..." */
    bool             elsewhere = false;
    bool             modifier = false;
};

static std::vector<Clause> clauses(const std::string &sentence)
{
    std::vector<Clause> out;
    std::vector<int> pending;   /* "IN BAILEY, ROUGH ..." names the next clause's area */
    for (const std::string &raw : split_commas(sentence)) {
        const std::vector<std::string> w = tokens(raw);
        if (w.empty()) continue;
        /* Areas named, and whether that is all the clause says. */
        std::vector<int> named;
        /* in_list: after "IN", or at the start of a clause that may continue
         * the previous one's list; areas, qualifiers and AND keep it going. */
        bool only_areas = true, in_list = !out.empty(), except = false, saw_in = false;
        for (size_t i = 0; i < w.size();) {
            size_t len = 0;
            const int a = area_at(w, i, len);
            if (a >= 0 && in_list) {
                named.push_back(a);
                i += len;
                continue;
            }
            if (w[i] == "IN") { in_list = true; saw_in = true; if (i > 0 && w[i - 1] == "EXCEPT") except = true; i++; continue; }
            if (w[i] == "EXCEPT" && i + 1 < w.size() && w[i + 1] == "IN") { i++; continue; }
            if (in_list && is_qualifier(w[i])) { i++; continue; }   /* includes AND */
            in_list = false;
            only_areas = false;
            i++;
        }
        if (only_areas && !named.empty() && !saw_in && !out.empty() && !out.back().areas.empty()) {
            /* "FORTH, TYNE AND W HUMBER" continuing the previous list. */
            for (int a : named) out.back().areas.push_back(a);
            out.back().text += ", " + raw;
            continue;
        }
        if (only_areas && !named.empty() && saw_in) {
            pending = named;
            continue;
        }
        Clause c;
        c.text = raw;
        c.areas = named;
        c.except = except;
        if (!pending.empty() && c.areas.empty()) c.areas = pending;
        pending.clear();
        size_t k = 0;
        while (k < w.size() && (w[k] == "BUT" || w[k] == "AND")) k++;
        if (k < w.size()) {
            c.elsewhere = w[k] == "ELSEWHERE" || w[k] == "OTHERWISE";
            c.modifier = is_modifier(w[k]);
        }
        out.push_back(c);
    }
    return out;
}

/* The part of a sentence that applies to one area of a group forecast.
 * opening: only the clauses that can open it (not "OCNL ...", unless that is
 * all there is).  area < 0: the whole sentence. */
static std::string area_view(const std::string &sentence, int area, bool opening)
{
    if (area < 0 || NAVTEX_SEA_AREAS[area].inshore) {
        if (!opening) return sentence;
        std::vector<std::string> keep;
        for (const std::string &c : split_commas(sentence)) {
            const std::vector<std::string> w = tokens(c);
            if (!keep.empty() && !w.empty() && is_modifier(w[0])) continue;
            keep.push_back(c);
        }
        return join_commas(keep);
    }
    const std::vector<Clause> cl = clauses(sentence);
    auto names = [&](const Clause &c) {
        bool in = false;
        for (int a : c.areas) if (same_area(a, area)) in = true;
        return in;
    };
    bool named = false;
    for (const Clause &c : cl)
        if (!c.modifier && !c.areas.empty() && names(c) != c.except) named = true;
    std::vector<std::string> all, open;
    for (const Clause &c : cl) {
        bool applies;
        if (!c.areas.empty()) applies = names(c) != c.except;
        else if (c.elsewhere) applies = !named;
        else applies = true;
        if (!applies) continue;
        all.push_back(c.text);
        if (!c.modifier) open.push_back(c.text);
    }
    if (!opening) return join_commas(all);
    return join_commas(open.empty() ? all : open);
}

static int force_of(const std::string &w)
{
    if (w.empty() || !isdigit((unsigned char)w[0]) || w.size() > 2) return -1;
    const int v = atoi(w.c_str());
    return v <= 12 ? v : -1;
}

/* Wind: the opening direction and forces ("SW BACK E 3 TO 5, OCNL 6" opens
 * SW 3-5), and the strongest force named anywhere for this area.  Gusts are
 * not the wind force.  "WEST TO NORTHWEST" (Met Éireann) and "W OR NW" are
 * both "W/NW". */
static void parse_wind(const std::string &sentence, NavtexAreaForecast &f, int area = -1)
{
    {
        const std::vector<std::string> t = unglue(tokens(area_view(sentence, area, false)));
        for (size_t i = 0; i < t.size(); i++) {
            /* "GUSTS 9", "BOEEN 10", "GEWITTERBOEEN 10" (thunderstorm gusts). */
            if (i > 0 && (t[i - 1].find("GUST") != std::string::npos || t[i - 1].find("BOEEN") != std::string::npos
                          || t[i - 1].find("BOEN") != std::string::npos)) continue;
            const int v = force_of(t[i]);
            if (v > f.force_peak) f.force_peak = v;
        }
    }
    const std::string open = area_view(sentence, area, true);
    std::vector<std::string> prim;
    bool num = false;
    for (const std::string &clause : split_commas(open)) {
        for (const std::string &w : unglue(tokens(clause))) {
            if (num && is_stop(w)) break;
            prim.push_back(w);
            if (force_of(w) >= 0) num = true;
        }
        if (num) break;
    }
    const std::vector<std::string> all = unglue(tokens(open));
    for (int pass = 0; pass < 2 && f.wind_dir.empty(); pass++) {
        const std::vector<std::string> &t = pass ? all : prim;
        for (size_t i = 0; i < t.size(); i++) {
            const char *d = direction(t[i]);
            if (!d) continue;
            f.wind_dir = d;
            if (i + 2 < t.size() && (t[i + 1] == "OR" || t[i + 1] == "TO")) {
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

/* A Met Office / Norwegian area body: wind. sea state. weather. visibility. */
static bool reduce(const std::string &body, NavtexAreaForecast &f)
{
    const std::vector<std::string> s = sentences(body);
    if (s.empty() || garbled(s[0])) return false;
    parse_wind(s[0], f, f.area);
    if (f.force_max < 0) return false;
    size_t last = s.size();
    bool fog = false;
    if (s.size() >= 2 && !garbled(s.back())) {
        const int v = visibility(area_view(s.back(), f.area, true), fog);
        if (v && !has_sea_word(s.back())) { f.vis_worst = v; last = s.size() - 1; }
        else fog = false;
    } else if (s.size() >= 2) {
        last = s.size() - 1;
    }
    for (size_t i = 1; i < last; i++) {
        if (garbled(s[i])) break;
        if (has_sea_word(s[i]) || i == 1) {   /* the second sentence is the sea state, even "MOD." */
            sea_states(area_view(s[i], f.area, true), f.sea_min, f.sea_max);
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

/* Add f to out; a later forecast for the same area (an area repeated in a
 * bulletin) replaces the earlier, or with first_wins is dropped. */
static void add_forecast(std::vector<NavtexAreaForecast> &out, const NavtexAreaForecast &f, bool first_wins = false)
{
    for (auto &o : out)
        if (same_area(o.area, f.area)) {
            if (!first_wins) o = f;
            return;
        }
    out.push_back(f);
}

/* Met Office shipping and inshore bulletins, and the Norwegian Institute's. */
static std::vector<NavtexAreaForecast> parse_met_office(const std::string &text)
{
    std::vector<NavtexAreaForecast> out;
    std::vector<int> areas;
    std::string body;
    bool collecting = false;
    auto flush = [&]() {
        if (collecting && !areas.empty()) {
            for (int a : areas) {
                NavtexAreaForecast f;
                f.area = a;
                f.body = trim(body);
                if (reduce(body, f)) add_forecast(out, f);
            }
        }
        areas.clear();
        body.clear();
        collecting = false;
    };

    for (const std::string &raw : split_lines(text)) {
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

/* ---- Met Éireann coastal waters ---- */

/* The headlands Met Éireann names, clockwise round Ireland from Malin Head,
 * with approximate positions (to a few km: they only weigh how much of a
 * section a stretch covers); `zone` marks the ones that start a section of
 * NAVTEX_SEA_AREAS (its key "<A>-<B>"). */
struct IrishHeadland { const char *name; double lat, lon; bool zone; };
static const IrishHeadland IRISH_HEADLANDS[] = {
    { "MALIN HEAD",        55.38,  -7.37, true  },
    { "FAIR HEAD",         55.23,  -6.15, true  },
    { "BELFAST LOUGH",     54.68,  -5.65, false },
    { "STRANGFORD LOUGH",  54.37,  -5.53, false },
    { "CARLINGFORD LOUGH", 54.02,  -6.07, true  },
    { "HOWTH HEAD",        53.37,  -6.05, false },
    { "WICKLOW HEAD",      52.97,  -5.99, true  },
    { "CARNSORE POINT",    52.17,  -6.36, true  },
    { "HOOK HEAD",         52.12,  -6.93, false },
    { "DUNGARVAN",         52.08,  -7.6,  false },
    { "ROCHES POINT",      51.79,  -8.25, true  },
    { "MIZEN HEAD",        51.45,  -9.82, true  },
    { "VALENTIA",          51.93, -10.35, false },
    { "LOOP HEAD",         52.56,  -9.93, true  },
    { "SLYNE HEAD",        53.40, -10.24, true  },
    { "ERRIS HEAD",        54.30, -10.0,  true  },
    { "ROSSAN POINT",      54.70,  -8.80, false },
    { "BLOODY FORELAND",   55.15,  -8.28, true  },
};
static const int IRISH_HEADLAND_COUNT = (int)(sizeof(IRISH_HEADLANDS) / sizeof(IRISH_HEADLANDS[0]));

static int irish_headland(const std::string &name)
{
    for (int i = 0; i < IRISH_HEADLAND_COUNT; i++) if (name == IRISH_HEADLANDS[i].name) return i;
    return -1;
}

/* The coast from headland i to the next, clockwise. */
static double irish_leg_km(int i)
{
    const IrishHeadland &a = IRISH_HEADLANDS[i], &b = IRISH_HEADLANDS[(i + 1) % IRISH_HEADLAND_COUNT];
    return km_between(a.lat, a.lon, b.lat, b.lon);
}

/* The section (NAVTEX_SEA_AREAS index) the leg from headland i belongs to. */
static int irish_zone_of_leg(int i)
{
    int z = i;
    while (!IRISH_HEADLANDS[z].zone) z = (z + IRISH_HEADLAND_COUNT - 1) % IRISH_HEADLAND_COUNT;
    int nx = (z + 1) % IRISH_HEADLAND_COUNT;
    while (!IRISH_HEADLANDS[nx].zone) nx = (nx + 1) % IRISH_HEADLAND_COUNT;
    const std::string key = std::string(IRISH_HEADLANDS[z].name) + "-" + IRISH_HEADLANDS[nx].name;
    for (int a = 0; a < NAVTEX_SEA_AREA_COUNT; a++)
        if (NAVTEX_SEA_AREAS[a].src == NAVTEX_SRC_IRISH && key == NAVTEX_SEA_AREAS[a].key) return a;
    return -1;
}

/* "MIZEN HEAD TO SLYNE HEAD TO MALIN HEAD" -> the legs it covers (indices of
 * the leg's first headland), or nothing when a name is unknown or the
 * headlands are not in order round the coast. */
static std::vector<int> irish_legs(const std::string &chain)
{
    std::vector<int> pts;
    std::string s = " " + chain + " ";
    for (const char *ab : { " HD ", " PT " }) {
        size_t i;
        while ((i = s.find(ab)) != std::string::npos) s.replace(i, 4, ab[1] == 'H' ? " HEAD " : " POINT ");
    }
    size_t a = 0;
    s = trim(s);
    while (true) {
        const size_t e = s.find(" TO ", a);
        const int h = irish_headland(trim(s.substr(a, e == std::string::npos ? std::string::npos : e - a)));
        if (h < 0) return {};
        pts.push_back(h);
        if (e == std::string::npos) break;
        a = e + 4;
    }
    if (pts.size() < 2) return {};
    const int n = IRISH_HEADLAND_COUNT;
    for (int dir : { 1, -1 }) {
        int total = 0;
        bool ok = true;
        for (size_t k = 0; k + 1 < pts.size(); k++) {
            const int d = ((pts[k + 1] - pts[k]) * dir + n) % n;
            if (d == 0) { ok = false; break; }
            total += d;
        }
        if (!ok || total >= n) continue;
        /* Two headlands could go either way: take the shorter. */
        if (pts.size() == 2 && dir == 1 && total > n / 2) continue;
        std::vector<int> legs;
        int at = pts[0];
        for (int k = 0; k < total; k++) {
            legs.push_back(dir == 1 ? at : (at + n - 1) % n);
            at = (at + dir + n) % n;
        }
        return legs;
    }
    return {};
}

/* Text from the first of `from` after pos to the first of `to` after it. */
static std::string section(const std::string &s, size_t pos, const char *from, const std::vector<const char *> &to)
{
    const size_t a = s.find(from, pos);
    if (a == std::string::npos) return "";
    const size_t b0 = a + strlen(from);
    size_t b = s.size();
    for (const char *t : to) {
        const size_t x = s.find(t, b0);
        if (x != std::string::npos && x < b) b = x;
    }
    return trim(s.substr(b0, b - b0));
}

/* Met Éireann's "SEA AREA FORECAST": per stretch of coast "FORECAST FOR A TO
 * B TO C[ AND FOR THE IRISH SEA]", then WIND:, WEATHER:, VISIBILITY:.  There
 * is no sea state; visibility counts only when it is not "in precipitation".
 * Each fixed section goes to the stretch covering most of its coast. */
static std::vector<NavtexAreaForecast> parse_met_eireann(const std::string &text)
{
    std::vector<NavtexAreaForecast> out;
    std::string s;
    for (const std::string &l : split_lines(text)) { s += ' '; s += norm_line(l); }
    s += ' ';
    const size_t stop = s.find(" OUTLOOK FOR");
    struct Cover { NavtexAreaForecast f; double km; };
    std::vector<Cover> best(NAVTEX_SEA_AREA_COUNT, Cover { NavtexAreaForecast(), 0.0 });
    std::vector<double> zone_km(NAVTEX_SEA_AREA_COUNT, 0.0);
    for (int i = 0; i < IRISH_HEADLAND_COUNT; i++) {
        const int z = irish_zone_of_leg(i);
        if (z >= 0) zone_km[z] += irish_leg_km(i);
    }
    size_t pos = 0;
    while ((pos = s.find(" FORECAST FOR ", pos)) != std::string::npos && pos < stop) {
        const size_t c0 = pos + 14;
        const size_t w = s.find(" WIND:", c0);
        if (w == std::string::npos) break;
        /* A heading without its WIND: is not this one's. */
        const size_t nf = s.find(" FORECAST FOR ", c0);
        if (nf < w) { pos = c0; continue; }
        std::string chain = trim(s.substr(c0, w - c0));
        pos = w;
        const size_t andfor = chain.find(" AND FOR ");
        if (andfor != std::string::npos) chain = chain.substr(0, andfor);
        const std::vector<int> legs = irish_legs(chain);
        if (legs.empty()) continue;
        const std::vector<const char *> ends = { " WEATHER:", " VISIBILITY:", " FORECAST FOR ", " OUTLOOK", " COASTAL REPORTS", " NNNN" };
        const std::string wind = section(s, w, " WIND:", ends);
        const size_t next = s.find(" FORECAST FOR ", w);
        const std::string vis_all = section(s, w, " VISIBILITY:", { " FORECAST FOR ", " OUTLOOK", " COASTAL REPORTS", " NNNN", " WEATHER:", " WIND:" });
        const bool vis_here = !vis_all.empty() && (next == std::string::npos || s.find(" VISIBILITY:", w) < next);
        const std::vector<std::string> ws = sentences(wind);
        if (ws.empty() || garbled(ws[0])) continue;
        NavtexAreaForecast f;
        parse_wind(ws[0], f);
        if (f.force_max < 0) continue;
        for (size_t i = 1; i < ws.size(); i++) {
            if (garbled(ws[i])) continue;
            NavtexAreaForecast g;
            parse_wind(ws[i], g);
            if (g.force_peak > f.force_peak) f.force_peak = g.force_peak;
        }
        if (vis_here) {
            const std::vector<std::string> vs = sentences(vis_all);
            const std::string v0 = vs.empty() ? "" : split_commas(vs[0])[0];
            const std::vector<std::string> t = tokens(v0);
            bool conditional = false;
            for (const auto &x : t) if (x == "IN" || x == "REDUCING" || x == "DECREASING" || x == "AT" || x == "LOCALLY") conditional = true;
            bool fog = false;
            if (!conditional && !garbled(v0)) f.vis_worst = visibility(v0, fog);
            f.fog = fog && !conditional;
        }
        f.body = trim(s.substr(c0 - 13, (next == std::string::npos || next > stop ? std::min(stop, s.size()) : next) - (c0 - 13)));
        /* Coast covered, per section. */
        std::vector<double> km(NAVTEX_SEA_AREA_COUNT, 0.0);
        for (int l : legs) {
            const int z = irish_zone_of_leg(l);
            if (z >= 0) km[z] += irish_leg_km(l);
        }
        for (int z = 0; z < NAVTEX_SEA_AREA_COUNT; z++) {
            if (km[z] <= 0 || km[z] < 0.5 * zone_km[z] || km[z] <= best[z].km) continue;
            best[z].f = f;
            best[z].f.area = z;
            best[z].km = km[z];
        }
    }
    for (int z = 0; z < NAVTEX_SEA_AREA_COUNT; z++) if (best[z].km > 0) out.push_back(best[z].f);
    return out;
}

/* ---- Wave heights (KNMI, DWD) ---- */

/* "1,5M" -> "1.5M", so that splitting on commas leaves the number whole. */
static std::string decimal_points(const std::string &s)
{
    std::string c = s;
    for (size_t i = 1; i + 1 < c.size(); i++)
        if (c[i] == ',' && isdigit((unsigned char)c[i - 1]) && isdigit((unsigned char)c[i + 1])) c[i] = '.';
    return c;
}

/* "AROUND 1.0 METER", "1.5 - 2.0 METER", "4M", "BIS 0,5M": the opening wave
 * height of a clause, or false when the clause qualifies it ("IN THE NORTH",
 * "LATER", "SPAETER 3M"). */
static bool wave_height(const std::string &clause, NavtexAreaForecast &f)
{
    const std::string c = decimal_points(norm_line(clause));
    std::vector<std::string> w;
    {
        std::string cur;
        for (size_t i = 0; i <= c.size(); i++) {
            const char ch = i < c.size() ? c[i] : ' ';
            const bool numch = isdigit((unsigned char)ch) || (ch == '.' && !cur.empty() && isdigit((unsigned char)cur.back())
                                                              && i + 1 < c.size() && isdigit((unsigned char)c[i + 1]));
            if (isalpha((unsigned char)ch) || numch) {
                if (!cur.empty() && (isdigit((unsigned char)cur.back()) || cur.back() == '.') != (bool)numch) { w.push_back(cur); cur.clear(); }
                cur += ch;
            } else {
                if (!cur.empty()) w.push_back(cur);
                cur.clear();
            }
        }
    }
    static const char *ok_words[] = { "AROUND", "ABOUT", "OR", "LESS", "THAN", "TO", "UP", "BIS", "UNTER",
                                      "ANFANGS", "METER", "METERS", "METRE", "METRES", "M", "WAVEHEIGHT",
                                      "SEA", "SEE", "AT", "FIRST", "UM" };
    std::vector<double> v;
    bool upto = false;
    for (const std::string &x : w) {
        if (isdigit((unsigned char)x[0])) { v.push_back(atof(x.c_str())); continue; }
        if (!v.empty() && (x == "M" || starts_with(x, "MET"))) break;   /* the first height is it */
        bool ok = false;
        for (const char *o : ok_words) if (x == o) ok = true;
        if (!ok) return false;
        if (x == "BIS" || x == "UNTER" || x == "LESS" || x == "UP") upto = true;
    }
    if (v.empty() || v.size() > 2) return false;
    for (double x : v) if (x <= 0 || x > 20) return false;
    f.wave_min = v[0];
    f.wave_max = v.back();
    if (f.wave_min > f.wave_max) std::swap(f.wave_min, f.wave_max);
    f.wave_upto = upto;
    if (upto) f.wave_min = -1;
    return true;
}

/* ---- KNMI ---- */

/* "FORECAST DUTCH EEZ": headers "HUMBER. GERMAN BIGHT. DOGGER." then wind.
 * weather. visibility. waveheight.  Only the first period ("FORECAST VALID
 * ... 03:00 TILL ... 15:00") is the opening. */
static std::vector<NavtexAreaForecast> parse_knmi(const std::string &text)
{
    std::vector<NavtexAreaForecast> out;
    std::vector<int> areas;
    std::string body;
    int periods = 0;
    auto flush = [&]() {
        if (!areas.empty()) {
            const std::vector<std::string> s = sentences(body);
            if (!s.empty() && !garbled(s[0])) {
                NavtexAreaForecast f;
                parse_wind(s[0], f);
                if (f.force_max >= 0) {
                    for (size_t i = 1; i < s.size(); i++) {
                        if (garbled(s[i])) continue;
                        const std::vector<std::string> t = tokens(s[i]);
                        if (t.empty()) continue;
                        if (t[0] == "WAVEHEIGHT") wave_height(split_commas(decimal_points(s[i]))[0], f);
                        else if (t[0] == "GOOD" || t[0] == "MODERATE" || t[0] == "POOR"
                                 || (t[0] == "VERY" && t.size() > 1 && t[1] == "POOR")) {
                            bool fog = false;
                            f.vis_worst = visibility(split_commas(s[i])[0], fog);
                        }
                    }
                    f.body = trim(body);
                    for (int a : areas) { f.area = a; add_forecast(out, f, true); }
                }
            }
        }
        areas.clear();
        body.clear();
    };
    for (const std::string &raw : split_lines(text)) {
        const std::string n = norm_line(raw);
        if (starts_with(n, "FORECAST VALID")) { flush(); if (++periods > 1) break; continue; }
        if (!periods) continue;
        const std::vector<int> sh = shipping_header(n);
        if (!sh.empty()) { flush(); areas = sh; continue; }
        if (n.empty() || starts_with(n, "NNNN")) { flush(); continue; }
        body += ' ';
        body += n;
    }
    flush();
    return out;
}

/* ---- DWD ---- */

/* The English German Bight forecast: "THURSDAY:" then wind, weather (or
 * "VISIBILITY/WEATHER:"), sea ("SEA: 2 METER").  The first day is the
 * opening. */
static std::vector<NavtexAreaForecast> parse_dwd_english(const std::string &text)
{
    std::vector<NavtexAreaForecast> out;
    static const char *days[] = { "MONDAY:", "TUESDAY:", "WEDNESDAY:", "THURSDAY:", "FRIDAY:", "SATURDAY:",
                                  "SUNDAY:", "TODAY:", "TONIGHT:" };
    int area = -1;
    for (int a = 0; a < NAVTEX_SEA_AREA_COUNT; a++)
        if (std::string(NAVTEX_SEA_AREAS[a].key) == "GERMAN BIGHT") area = a;
    std::vector<std::string> body;
    int day = 0;
    for (const std::string &raw : split_lines(text)) {
        const std::string n = norm_line(raw);
        bool is_day = false;
        for (const char *d : days) if (n == d) is_day = true;
        if (is_day) { if (++day > 1) break; continue; }
        if (!day) continue;
        if (starts_with(n, "NNNN")) break;
        if (!n.empty()) body.push_back(n);
    }
    if (body.empty()) return out;
    /* Lines that start a part; the rest continue the one before. */
    std::string wind, vis, sea, all;
    std::string *cur = &wind;
    for (const std::string &l : body) {
        all += (all.empty() ? "" : " ") + l;
        std::string x = l;
        if (starts_with(x, "WIND:")) { cur = &wind; x = trim(x.substr(5)); }
        else if (starts_with(x, "VISIBILITY/WEATHER:") || starts_with(x, "WEATHER/VISIBILITY:")) { cur = &vis; x = trim(x.substr(19)); }
        else if (starts_with(x, "SEA:") || starts_with(x, "SEA ")) { cur = &sea; x = trim(x.substr(4)); }
        else if (cur == &wind && !wind.empty() && wind.back() == '.') cur = &vis;   /* "SOUTHWEST 5 TO 6." then weather */
        *cur += (cur->empty() ? "" : " ") + x;
    }
    const std::vector<std::string> ws = sentences(wind);
    if (ws.empty() || garbled(ws[0])) return out;
    /* "EASTERN PART FIRST WEST, OTHERWISE SOUTHWEST TO SOUTH 3 TO 4": the
     * part of the area is not the area. */
    std::vector<std::string> cl = split_commas(ws[0]);
    if (cl.size() > 1 && cl[0].find(" PART") != std::string::npos) {
        const std::vector<std::string> t = tokens(cl[1]);
        if (t.empty() || t[0] != "OTHERWISE") return out;
        cl.erase(cl.begin());
        cl[0] = trim(cl[0]).substr(9);
    } else if (cl[0].find(" PART") != std::string::npos) {
        return out;
    }
    NavtexAreaForecast f;
    f.area = area;
    parse_wind(join_commas(cl), f);
    if (f.force_max < 0) return out;
    for (size_t i = 1; i < ws.size(); i++) {
        NavtexAreaForecast g;
        if (!garbled(ws[i])) { parse_wind(ws[i], g); if (g.force_peak > f.force_peak) f.force_peak = g.force_peak; }
    }
    if (!sea.empty() && !garbled(sea)) {
        const std::vector<std::string> ss = sentences(decimal_points(sea));
        wave_height(split_commas(ss.empty() ? sea : ss[0])[0], f);
    }
    {
        const std::vector<std::string> t = tokens(vis);
        if (t.size() >= 2 && t[1] == "VISIBILITY") {
            bool fog = false;
            f.vis_worst = visibility(t[0], fog);
        }
    }
    f.body = all;
    out.push_back(f);
    return out;
}

/* German: "SW 4-5", "N-NW UM 7", "SW-LICHE WINDE UM 5", "NO" north-east,
 * "O" east.  Abbreviations with a full stop would end the sentence. */
static std::string dwd_german_wind(const std::string &s)
{
    std::string t = " " + s + " ";
    const std::pair<const char *, const char *> rep[] = {
        { "VORUEBERGH.", "VORUEBERGEHEND" }, { "VORUEBERG.", "VORUEBERGEHEND" }, { "ZTW.", "ZEITWEISE" },
        { "-LICHE", " " }, { "-DREHEND", " DREHEND" }, { ".", "," },
    };
    for (const auto &r : rep) {
        size_t i;
        while ((i = t.find(r.first)) != std::string::npos) t.replace(i, strlen(r.first), r.second);
    }
    /* "N-NW" -> "N OR NW"; German letters to English directions. */
    std::vector<std::string> w;
    std::string cur;
    for (char c : t) {
        if (isalnum((unsigned char)c)) cur += c;
        else {
            if (!cur.empty()) w.push_back(cur);
            cur.clear();
            if (c == '-' || c == ',') w.push_back(std::string(1, c));
        }
    }
    auto en = [](const std::string &x) -> std::string {
        if (x == "NO") return "NE";
        if (x == "O")  return "E";
        if (x == "SO") return "SE";
        if (x == "N" || x == "S" || x == "W" || x == "SW" || x == "NW") return x;
        return "";
    };
    std::string o;
    for (size_t i = 0; i < w.size(); i++) {
        std::string x = w[i];
        const std::string d = en(x);
        if (!d.empty()) x = d;
        if (x == "-") {
            /* Between two directions: "or"; between numbers: a range. */
            const bool dirs = i > 0 && i + 1 < w.size() && !en(w[i - 1]).empty() && !en(w[i + 1]).empty();
            o += dirs ? " OR " : " TO ";
            continue;
        }
        if (x == ",") { o += ", "; continue; }
        if (x == "BIS") x = "TO";
        o += ' ';
        o += x;
    }
    return trim(o);
}

/* The German sea and coastal forecasts: "VORHERSAGE FUER MITTWOCH:" then
 * per area its name on a line, wind and weather, "SEE: 2M.".  The first
 * forecast for each area in a bulletin is the opening. */
static std::vector<NavtexAreaForecast> parse_dwd_german(const std::string &text)
{
    std::vector<NavtexAreaForecast> out;
    int area = -1;
    std::string wind, sea, all;
    bool in_sea = false;
    auto flush = [&]() {
        if (area >= 0) {
            NavtexAreaForecast f;
            f.area = area;
            const std::string w = dwd_german_wind(wind);
            const std::vector<std::string> cl = split_commas(w);
            /* Light winds have no force to show, and "WESTTEIL SW 5" is a
             * part of the area: say nothing. */
            const std::vector<std::string> t0 = tokens(cl.empty() ? "" : cl[0]);
            bool skip = garbled(wind) || t0.empty();
            for (const auto &x : t0) if (x == "SCHWACHWINDIG" || x.find("TEIL") != std::string::npos) skip = true;
            if (!skip) {
                parse_wind(w, f);
                if (f.force_max >= 0) {
                    if (!sea.empty() && !garbled(sea)) wave_height(split_commas(decimal_points(sea))[0], f);
                    f.body = trim(all);
                    add_forecast(out, f, true);
                }
            }
        }
        area = -1;
        wind.clear();
        sea.clear();
        all.clear();
        in_sea = false;
    };
    for (const std::string &raw : split_lines(text)) {
        std::string n = norm_line(raw);
        int a = -1;
        for (int i = 0; i < NAVTEX_SEA_AREA_COUNT; i++)
            if (NAVTEX_SEA_AREAS[i].src == NAVTEX_SRC_DWD && n == NAVTEX_SEA_AREAS[i].key) a = i;
        if (a >= 0) { flush(); area = a; continue; }
        if (n.empty() || starts_with(n, "VORHERSAGE") || starts_with(n, "NNNN") || n.find("VORHERSAGE") != std::string::npos) {
            flush();
            continue;
        }
        if (area < 0) continue;
        all += ' ';
        all += n;
        if (starts_with(n, "SEE:") || starts_with(n, "SEE ")) { in_sea = true; sea += n.substr(4); continue; }
        (in_sea ? sea : wind) += ' ' + n;
    }
    flush();
    return out;
}

/* ---- Oostende Radio ---- */

static const char *direction16(const std::string &w)
{
    static const char *d[] = { "NNE", "ENE", "ESE", "SSE", "SSW", "WSW", "WNW", "NNW" };
    for (const char *x : d) if (w == x) return x;
    return direction(w);
}

/* "THAMES:", "DOVER:", "BELGIAN COAST:" or "THAMES, DOVER AND (THE) BELGIAN
 * COAST:", then one paragraph.  The wind opens it: "SSW TO SW 4 TO 5, ...";
 * anything else ("IN EAST, WSW 7 ...", "THIS MORNING 2 TO 3 ...") is not
 * read. */
static std::vector<NavtexAreaForecast> parse_oostende(const std::string &text)
{
    std::vector<NavtexAreaForecast> out;
    auto find_key = [](const char *k) {
        for (int i = 0; i < NAVTEX_SEA_AREA_COUNT; i++) if (std::string(NAVTEX_SEA_AREAS[i].key) == k) return i;
        return -1;
    };
    std::vector<int> areas;
    std::string body;
    auto flush = [&]() {
        if (!areas.empty()) {
            const std::vector<std::string> s = sentences(body);
            const std::vector<std::string> t = s.empty() ? std::vector<std::string>() : tokens(split_commas(s[0])[0]);
            /* DIR [(TO|TOT) DIR] F[ TO F] and nothing else. */
            size_t i = 0;
            std::string dir;
            if (i < t.size() && direction16(t[i]) && strcmp(direction16(t[i]), "CYC")) { dir = direction16(t[i]); i++; }
            if (!dir.empty() && i + 1 < t.size() && (t[i] == "TO" || t[i] == "TOT") && direction16(t[i + 1])) {
                dir += std::string("/") + direction16(t[i + 1]);
                i += 2;
            }
            int f1 = -1, f2 = -1;
            if (i < t.size()) f1 = force_of(t[i++]);
            if (f1 >= 0 && i + 1 < t.size() && t[i] == "TO") { f2 = force_of(t[i + 1]); i += 2; }
            else if (f1 >= 0 && i < t.size() && force_of(t[i]) >= 0) f2 = force_of(t[i++]);   /* "3-4" */
            if (!dir.empty() && f1 >= 0 && i == t.size() && !garbled(s[0])) {
                NavtexAreaForecast f;
                f.wind_dir = dir;
                f.force_min = f1;
                f.force_max = f2 >= 0 ? std::max(f1, f2) : f1;
                if (f2 >= 0 && f2 < f1) f.force_min = f2;
                NavtexAreaForecast g;
                parse_wind(s[0], g);
                f.force_peak = std::max(g.force_peak, f.force_max);
                for (const std::string &x : s) {
                    const std::vector<std::string> v = tokens(split_commas(x)[0]);
                    if (v.size() == 2 && (v[1] == "VIS" || v[1] == "VISIBILITY")) {
                        bool fog = false;
                        f.vis_worst = visibility(v[0], fog);
                    }
                }
                f.body = trim(body);
                for (int a : areas) { f.area = a; add_forecast(out, f, true); }
            }
        }
        areas.clear();
        body.clear();
    };
    for (const std::string &raw : split_lines(text)) {
        const std::string n = norm_line(raw);
        if (!n.empty() && n.back() == ':') {
            std::string h = n.substr(0, n.size() - 1);
            std::vector<int> a;
            if (h == "THAMES") a = { find_key("THAMES") };
            else if (h == "DOVER") a = { find_key("DOVER") };
            else if (h == "BELGIAN COAST") a = { find_key("BELGIAN COAST") };
            else if (h == "THAMES, DOVER AND BELGIAN COAST" || h == "THAMES, DOVER AND THE BELGIAN COAST")
                a = { find_key("THAMES"), find_key("DOVER"), find_key("BELGIAN COAST") };
            else if (h == "DOVER AND THE BELGIAN COAST" || h == "DOVER AND BELGIAN COAST")
                a = { find_key("DOVER"), find_key("BELGIAN COAST") };
            if (!a.empty()) { flush(); areas = a; continue; }
        }
        if (n.empty() || starts_with(n, "NNNN")) { flush(); continue; }
        if (!areas.empty()) { body += ' '; body += n; }
    }
    flush();
    return out;
}

}  // namespace navtex_mapdata_detail

/* Every area forecast in a bulletin this can read; empty for anything else.
 *
 * Not read, by choice: high-seas bulletins (METAREA sections are not sea
 * areas) and the Met Office's 3-day extended outlook (no opening
 * conditions); the Faroese and Icelandic forecasts (wind in m/s, areas this
 * does not place); Météo-France (French and English, about 65 bulletins in
 * the corpus it was tested on) and AEMET (Spanish), whose areas are not in
 * the table yet. */
static std::vector<NavtexAreaForecast> navtex_parse_forecasts(const std::string &text)
{
    using namespace navtex_mapdata_detail;
    std::string up;
    up.reserve(text.size());
    for (char c : text) up += (char)toupper((unsigned char)c);
    auto has = [&](const char *s) { return up.find(s) != std::string::npos; };
    if (has("HIGH SEAS BULLETIN") || has("EXTENDED OUTLOOK")) return {};
    if (has("SEA AREA FORECAST") && has("FORECAST FOR ")) return parse_met_eireann(up);
    if (has("FORECAST DUTCH EEZ")) return parse_knmi(up);
    if (has("WEATHERFORECAST FOR GERMAN BIGHT")) return parse_dwd_english(up);
    if (has("SEEWETTERVORHERSAGE") || has("STENVORHERSAGE DEUTSCHE")) return parse_dwd_german(up);
    if (has("OOSTENDERADIO WX") || has("OOSTENDE RADIO WX")) return parse_oostende(up);
    if (has("MET OFFICE") || has("NORWEGIAN METEOROLOGICAL INSTITUTE") || has("INSHORE WATERS FCST")
        || has("INSHORE WATERS FORECAST"))
        return parse_met_office(text);
    return {};
}

/* ------------------------------------------------------------------ */
/* Gale warnings                                                       */
/* ------------------------------------------------------------------ */

/* One sea area's gale warning. */
struct NavtexGaleWarning {
    int         area = -1;      /* index into NAVTEX_SEA_AREAS */
    std::string wind_dir;       /* "SW", or "" */
    int         force = -1;     /* the strongest force it names, now or later */
    int         force_now = -1; /* in force: the force now */
    char        status = 'N';   /* 'N' in force, 'E' expected (imminent, soon, later), 'C' ceased */
    std::string text;           /* as broadcast */
};

namespace navtex_mapdata_detail {

/* "SOUTHERLY GALE FORCE 8 EXPECTED LATER" (expected), "GALE FORCE 8 VEERING
 * WESTERLY SOON" (in force, and changing), "STORM FORCE 10 NOW VEERED
 * SOUTHWESTERLY AND DECREASED GALE FORCE 8" (in force at 8), "GALE NOW
 * CEASED [BUT ... EXPECTED LATER]".  The opening statement, up to the first
 * change, says whether the gale is here or expected. */
static bool gale_text(const std::string &text, NavtexGaleWarning &g)
{
    std::vector<std::string> t = tokens(text);
    if (t.empty() || garbled(text)) return false;
    /* "GALE NOW CEASED BUT ...": what follows BUT is the warning. */
    bool ceased = false;
    for (size_t i = 0; i < t.size(); i++)
        if (t[i] == "CEASED") {
            ceased = true;
            for (size_t j = i + 1; j < t.size(); j++)
                if (t[j] == "BUT") { t.erase(t.begin(), t.begin() + (long)j + 1); ceased = false; break; }
            break;
        }
    if (ceased) { g.status = 'C'; return true; }
    auto change = [](const std::string &w) {
        return w == "VEERING" || w == "BACKING" || w == "INCREASING" || w == "DECREASING" || w == "BECOMING"
            || w == "CEASING" || w == "SHIFTING";
    };
    bool expected = false;
    size_t first_end = t.size();
    for (size_t i = 0; i < t.size(); i++) {
        if (change(t[i])) { first_end = i; break; }
        if (t[i] == "EXPECTED" || t[i] == "IMMINENT" || t[i] == "SOON" || t[i] == "LATER") expected = true;
    }
    /* Past changes ("NOW DECREASED SEVERE GALE FORCE 9") replace what came
     * before them. */
    size_t from = 0;
    for (size_t i = 0; i < t.size(); i++)
        if (t[i] == "DECREASED" || t[i] == "INCREASED") from = i + 1;
    int now = -1, peak = -1;
    for (size_t i = from; i < t.size(); i++) {
        const int v = force_of(t[i]);
        if (v < 0) continue;
        if (now < 0 && (from > 0 || i < first_end)) now = v;
        if (v > peak) peak = v;
    }
    if (peak < 7) return false;
    std::string dir;
    for (size_t i = 0; i + 1 < t.size(); i++)
        if ((t[i] == "VEERED" || t[i] == "BACKED") && direction(t[i + 1])) dir = direction(t[i + 1]);
    if (dir.empty())
        for (size_t i = 0; i < first_end; i++) if (direction(t[i])) { dir = direction(t[i]); break; }
    g.status = expected ? 'E' : 'N';
    g.force_now = g.status == 'N' ? now : -1;
    g.force = peak;
    g.wind_dir = dir;
    return true;
}

}  // namespace navtex_mapdata_detail

/* The areas a gale warning (subject B) names: the Met Office's ("GALE
 * WARNING ... <AREA> / <text>"), KNMI's ("GALEWARNING", and the "GALE
 * WARNINGS." part of its forecast) and DWD's English one ("FOR GERMAN
 * BIGHT: NW 7.", a near-gale 7 warning).  Empty for anything else. */
static std::vector<NavtexGaleWarning> navtex_parse_gale_warnings(const std::string &text)
{
    using namespace navtex_mapdata_detail;
    std::vector<NavtexGaleWarning> out;
    std::string up;
    for (char c : text) up += (char)toupper((unsigned char)c);
    const bool knmi = up.find("FORECAST DUTCH EEZ") != std::string::npos;
    const bool dwd = up.find("NAVTEX-HAMBURG") != std::string::npos;
    if (!knmi && !dwd && up.find("GALE WARNING") == std::string::npos && up.find("GALEWARNING") == std::string::npos
        && up.find("STORM WARNING") == std::string::npos)
        return out;
    /* Norwegian, Faroese and Icelandic warnings name their own areas. */
    if (up.find("NORWEGIAN") != std::string::npos || up.find("FAROE") != std::string::npos
        || up.find("REYKJAVIK") != std::string::npos) return out;
    std::vector<int> areas;
    std::string body;
    bool started = !knmi;
    auto flush = [&]() {
        if (!areas.empty() && !trim(body).empty()) {
            NavtexGaleWarning g;
            if (gale_text(body, g)) {
                g.text = trim(body);
                for (int a : areas) {
                    g.area = a;
                    bool dup = false;
                    for (auto &o : out) if (same_area(o.area, a)) { o = g; dup = true; }
                    if (!dup) out.push_back(g);
                }
            }
        }
        areas.clear();
        body.clear();
    };
    for (const std::string &raw : split_lines(up)) {
        std::string n = norm_line(raw);
        if (knmi) {
            if (n == "GALE WARNINGS.") { started = true; continue; }
            if (!started) continue;
            if (starts_with(n, "SYNOPSIS") || starts_with(n, "SYNOPSUS") || starts_with(n, "FORECAST VALID")) break;
            if (n == "NO WARNING.") { areas.clear(); body.clear(); continue; }
        }
        if (dwd && starts_with(n, "FOR ") && n.back() == ':') n = n.substr(4, n.size() - 5);
        const std::vector<int> sh = shipping_header(n);
        if (!sh.empty()) { flush(); areas = sh; continue; }
        if (n.empty() || starts_with(n, "NNNN")) { flush(); continue; }
        if (!areas.empty()) { body += ' '; body += n; }
    }
    flush();
    return out;
}
