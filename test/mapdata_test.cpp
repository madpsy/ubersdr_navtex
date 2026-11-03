/* mapdata_test.cpp — unit tests for src/navtex_mapdata.h: positions in
 * warnings, warning kinds, the reduction of sea-area forecasts to "NE 4–7 ·
 * ROUGH · POOR VIS", and gale warnings per sea area.
 *
 * The first texts are written in the formats seen on air (the layouts, the
 * abbreviations, the glued "S7TO9", a trailing operator sign-off).  The
 * later ones ("received:") are short excerpts of real broadcasts, garbles
 * included, each for a format or a bug it guards.
 *
 *   cmake --build build --target mapdata_test
 *   ctest --test-dir build -R mapdata --output-on-failure
 */

#include "navtex_mapdata.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

static void check(bool ok, const std::string &what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

static bool near(double a, double b) { return fabs(a - b) < 1e-3; }

static const NavtexAreaForecast *find(const std::vector<NavtexAreaForecast> &v, const char *label)
{
    for (const auto &f : v) if (std::string(NAVTEX_SEA_AREAS[f.area].label) == label) return &f;
    return nullptr;
}

static void positions()
{
    printf("positions\n");
    auto p = navtex_parse_positions("BRENNES LIGHT 70-02.16N 019-32.41E IS UNRELIABLE.");
    check(p.size() == 1 && near(p[0].lat, 70.036) && near(p[0].lon, 19.540167), "DD-MM.mmN DDD-MM.mmE");

    p = navtex_parse_positions("WRECK IN 50-46.3 N 001-17.8 W MARKED.");
    check(p.size() == 1 && near(p[0].lat, 50.771667) && near(p[0].lon, -1.296667), "space before hemisphere, west");

    p = navtex_parse_positions("AREA 54 12.500N  005 30.250E");
    check(p.size() == 1 && near(p[0].lat, 54.208333) && near(p[0].lon, 5.504167), "space-separated degrees and minutes");

    p = navtex_parse_positions("POSITION 5846N 00512E AND 5850.5N 00515.5E");
    check(p.size() == 2 && near(p[0].lat, 58.766667) && near(p[1].lon, 5.258333), "compact DDMMN DDDMME, two positions");

    p = navtex_parse_positions("BUOY 36 51'30''N  10 20'15''E");
    check(p.size() == 1 && near(p[0].lat, 36.858333) && near(p[0].lon, 10.3375), "degrees, minutes, seconds");

    p = navtex_parse_positions("51-24.85N, 003-14.98E LICHT GEDOOFD");
    check(p.size() == 1 && near(p[0].lon, 3.249667), "comma between latitude and longitude");

    p = navtex_parse_positions("33-10S 071-40W");
    check(p.size() == 1 && p[0].lat < 0 && p[0].lon < 0, "south and west are negative");

    p = navtex_parse_positions("50-75.0N 001-17.8W");
    check(p.empty(), "minutes of 75 (a garbled digit) are rejected");

    p = navtex_parse_positions("NAV WARNING 345 50 46N");
    check(p.empty(), "a latitude without a longitude is not a position");

    p = navtex_parse_positions("A 50-10N 001-00W B 50-10N 001-00W");
    check(p.size() == 1, "repeated positions are listed once");
}

static void positions_received()
{
    printf("positions (received)\n");
    auto p = navtex_parse_positions("LIGHT BUOY NO 13 45-16-45.0N 036-27-44.0E UNLIT");
    check(p.size() == 1 && near(p[0].lat, 45.279167) && near(p[0].lon, 36.462222), "degrees-minutes-seconds with dashes");

    p = navtex_parse_positions("INSTRUMENT ESTABLISHED IN POS 51-21-663N 003-06,817E  UNMARKED");
    check(p.size() == 1 && near(p[0].lat, 51.36105) && near(p[0].lon, 3.113617),
          "three digits after the second dash are thousandths of a minute");

    p = navtex_parse_positions("M/V HAPPY PELICAN ANCHOR LOST\nIN POS 51 24.57N - 002 40.20E\nNNNN");
    check(p.size() == 1 && near(p[0].lat, 51.4095) && near(p[0].lon, 2.67), "' - ' between latitude and longitude");

    p = navtex_parse_positions("LOST AN EXPLOSIVE IN POS 51-23.632N \n003-11.262E SHIPPING IS REQUESTED");
    check(p.size() == 1 && near(p[0].lon, 3.187700), "a line break between latitude and longitude");

    p = navtex_parse_positions("POSITION (WGS 84) : 51!15.1'N - 001!54.5'E.");
    check(p.size() == 1 && near(p[0].lat, 51.251667) && near(p[0].lon, 1.908333), "'!' for the degree sign");

    p = navtex_parse_positions("(E.F.4039.2) 44 31 22N  012 30 37E");
    check(p.size() == 1 && near(p[0].lat, 44.522778) && near(p[0].lon, 12.510278), "degrees minutes seconds with spaces");

    p = navtex_parse_positions("11 44 31.5N 012 30.5E");
    check(p.empty(), "\"11 44 31.5N\" with a degrees-minutes longitude is not 11°44'31.5\" (nor guessed at)");

    p = navtex_parse_positions("APO TA STIGMATA:\nA. 36-48.00B 021-40.00A\nB. 36-48.00B 021-50.00A");
    check(p.size() == 2 && near(p[0].lat, 36.8) && near(p[0].lon, 21.666667), "Greek B/A for north/east");
    check(navtex_parse_positions("36-48.00B 021-40.00E").empty(), "a Greek B needs a Greek A");

    p = navtex_parse_positions("40-04.16N\n008-39.29W40-00.45N 008-39.05W -");
    check(p.size() == 2 && near(p[1].lat, 40.0075), "a position glued to the one before");

    p = navtex_parse_positions("45-19-26.1N 037---52.7E");
    check(p.empty(), "'---' (garbled digits) is not a separator");

    p = navtex_parse_positions("ZCZC GA08\nNOUP HEAD 59-19.9N 003-04.2W\n7XSGIRSX ZCZC GA94\nLIGHT 54-28N 012-30E UNLIT.\nNNNN");
    check(p.size() == 1 && near(p[0].lat, 59.331667), "a second broadcast glued on (lost NNNN) is not read");

    p = navtex_parse_positions("49-02.62N 002-47.46W\n49-07.00N 002-45.51W\n9-08.27N 002-44.25W\n49-08.83N 002-41.94W");
    check(p.size() == 3, "a corner hundreds of km from the rest (a lost digit) is dropped");
}

static void kinds()
{
    printf("warning kinds\n");
    check(std::string(navtex_warning_kind("LIGHT BUOY NO 3 UNLIT", 'A')) == "buoy", "light buoy is a buoy");
    check(std::string(navtex_warning_kind("KLKSTAD LIGHT 67-22.86N IS UNLIT.", 'A')) == "light", "unlit light");
    check(std::string(navtex_warning_kind("LICHTBOEI WK2 LICHT GEDOOFD", 'A')) == "buoy", "Dutch light buoy");
    check(std::string(navtex_warning_kind("WIND FARM TURBINE T12 LIGHTS UNLIT", 'A')) == "windfarm",
          "wind farm before light");
    check(std::string(navtex_warning_kind("CABLE OPERATIONS BY CS SOVEREIGN", 'A')) == "cable", "cable");
    check(std::string(navtex_warning_kind("GUNNERY FIRING EXERCISES", 'A')) == "military", "firing exercise");
    check(std::string(navtex_warning_kind("DERELICT VESSEL ADRIFT", 'A')) == "hazard", "derelict");
    check(std::string(navtex_warning_kind("ANYTHING AT ALL", 'D')) == "sar", "subject D is search and rescue");
    check(std::string(navtex_warning_kind("RACON INOPERATIVE", 'A')) == "warning", "otherwise a plain warning");

    printf("warning kinds (received)\n");
    auto kind = [](const char *t) { return std::string(navtex_warning_kind(t, 'A')); };
    check(kind("GNSS, AIS, RADAR AND DGPS INTERFERENCE OBSERVED IN AREA.\nMARINERS ADVISED TO EXERCISE CAUTION") == "warning",
          "\"EXERCISE CAUTION\" is not an exercise");
    check(kind("WESTHINDER ANCHORAGE ANCHOR AND CHAIN LOST IN POS 51-24,32N 002-40,82E\n"
               "PROHIBITED TO ANCHOR AND KEEP A SAFE DISTANCE OF 2 CABLES.") == "hazard",
          "\"2 CABLES\" is a distance; a lost anchor is a hazard");
    check(kind("GERMAN BIGHT. NORTH OF OFFSHORE WINDPARK 'DEUTSCHE BUCHT'.\nAMMUNITION FOUND IN POSITION:") == "hazard",
          "ammunition found, by a wind farm, is a hazard");
    check(kind("OSTFRIESISCHE INSELN. NOERDLICH NORDERNEY.\nMUNITIONSFUNDE IN DER NAEHE DER FOLGENDEN POSITIONEN") == "hazard",
          "German ammunition finds");
    check(kind("DENMARK. KATTEGAT.\nMINE LAYING AND MINE\nSWEEPING EXERCISE IN\nAREA") == "military", "a mine exercise");
    check(kind("PLATFORM SOUTH 42/30-B 54-03.6N 000-54.0E UNLIT AND ALL NAVAIDS INOPERATIVE.") == "rig",
          "an unlit platform is a platform");
    check(kind("1. SANDETTIE WEST BUOY 51-15.1N 001-54.5E MISSING.") == "buoy", "a missing buoy is not search and rescue");
    check(kind("LEUCHTTONNE 'SPERR-G.1'   54-31,5N 011-18,5E  VERSCHWUNDEN.") == "buoy", "German light buoy");
    check(kind("LEUCHTTURM 'DARSSER ORT'  54-28N 012-30E  VERLOESCHT.") == "light", "German lighthouse");
    check(kind("KABELLEGEARBEITEN DURCH MS 'ATALANTI'") == "cable", "German cable laying");
    check(kind("GERMAN BIGHT. NORTHERLY OFFSHORE WINDPARK 'NORDSEE 1'\nUNDERWATER OPERATIONS BY MV 'ENEA'") == "windfarm",
          "work at a wind farm, with nothing else to say, is the wind farm");
    check(kind("M/N WHITEHEAD SECONDA WILL CARRY OUT EXPERIMENTAL LAUNCH\nTORPEDOES AT SEA") == "military", "torpedo launches");
}

static void inshore()
{
    printf("inshore waters forecast\n");
    const char *text =
        "ZCZC AE99\n"
        "MRSC MALIN HEAD\n"
        "ISSUED BY THE MET OFFICE AT 1100 UTC ON FRI 23 OCT 2026\n"
        "GENERAL SITUATION\n"
        "A DEEP LOW IS MOV EASTWARDS ACROSS THE ISLE OF MAN AND THE NORTH.\n"
        "ISLE OF MAN - (SWW)\n"
        "24 HR FCST:\n"
        "NW 7OR8, BACK W OR NW 4OR 5 LTR,THEN S7TO9.\n"
        "MAINLY ROUGH, BECMG SLT OR MOD, THEN ROUGH OR VERY ROUGH LTR. RAIN AT TIMES.\n"
        "MOD OR POOR, BECMG GOOD FOR A TIME\n"
        "OUTLOOK FOR THE FLW 24 HR:\n"
        "S 7 TO 9, VEER W OR SW 8 OR 9.\n"
        "ST DAVIDS HEAD TO GREAT ORME HEAD, INCLUDING ST GEORGES CHANNEL - (SWW) 24 HR FCST:\n"
        "NE 4 TO 6. SLT OR MOD. FAIR. GOOD, OCNL POOR.\n"
        "CAPE WRATH TO RATTRAY HEAD INCLUDING ORKNEY - (SWW)\n"
        "24 HR FCST:\n"
        "VRB 2 TO 4. SMOOTH OR SLT. FOG PATCHES. MOD, OCNL VERY POOR.\n"
        "NNNN\n";
    auto v = navtex_parse_forecasts(text);
    check(v.size() == 3, "three areas (the general situation's \"ISLE OF MAN\" is not a header)");
    const NavtexAreaForecast *m = find(v, "Isle of Man");
    check(m && navtex_forecast_summary(*m) == "NW 7–8 · ROUGH · UP TO SEV GALE 9 · POOR VIS",
          "glued numbers, opening conditions, peak and poor visibility: "
          + (m ? navtex_forecast_summary(*m) : std::string("(missing)")));
    const NavtexAreaForecast *d = find(v, "St David's Head to Great Orme Head");
    check(d && navtex_forecast_summary(*d) == "NE 4–6 · SLIGHT–MODERATE",
          "header with \"INCLUDING\" and the forecast on the next line; \"GOOD, OCNL POOR\" is not poor: "
          + (d ? navtex_forecast_summary(*d) : std::string("(missing)")));
    const NavtexAreaForecast *c = find(v, "Cape Wrath to Rattray Head");
    check(c && c->fog && navtex_forecast_summary(*c) == "VRB 2–4 · SMOOTH–SLIGHT · FOG",
          "fog: " + (c ? navtex_forecast_summary(*c) : std::string("(missing)")));
}

static void shipping()
{
    printf("shipping forecast\n");
    const char *text =
        "ZCZC GE36\n"
        "\n"
        "ISSUED BY THE MET OFFICE AT 0900 ON FRI 23 OCT\n"
        "\n"
        "GALE WARNINGS: FORTIES FORTH TYNE DOGGER HUMBER THAMES\n"
        "\n"
        "THE GENERAL SITUATION AT MIDNIGHT\n"
        "LOW PLYMOUTH 974 EXP NETHERLANDS 990 BY MIDNIGHT TONIGHT\n"
        "\n"
        "24-HR FORECASTS\n"
        "\n"
        "VIKING N UTSIRE\n"
        "SW BACK E 3 TO 5, OCNL 6 IN S UTSIRE. MOD OR ROUGH, BECMG SLT OR MOD IN VIKING. SHWRS. MOD OR GOOD\n"
        "\n"
        "SW FORTIES CROMARTY FORTH\n"
        "SE 4 TO 6 BECMG CYCLONIC 5 TO 7, THEN NW 7 TO SEV GALE 9 LTR. SLT OR MOD AT FIRST. RAIN. GOOD, OCNL POOR\n"
        "EJ M\n"
        "\n"
        "THAMES\n"
        "SE BECMG CYCLONIC THEN NW, 6 TO GALE 8, INCR SEV GALE 9 FOR A TIME. MOD OR ROUGH. SQUALLY SHWRS. GOOD\n"
        "\n"
        "OUTLOOK FLW 24 HOURS:\n"
        "STORM FORCE WINDS EXP IN VIKING.\n"
        "NNNN\n";
    auto v = navtex_parse_forecasts(text);
    check(v.size() == 6, "six areas from three headers (the gale-warning list is not one)");
    const NavtexAreaForecast *vk = find(v, "Viking"), *nu = find(v, "North Utsire");
    check(vk && nu && navtex_forecast_summary(*vk) == "SW 3–5 · MODERATE–ROUGH"
          && navtex_forecast_summary(*nu) == navtex_forecast_summary(*vk),
          "a group header shares one forecast: " + (vk ? navtex_forecast_summary(*vk) : std::string("(missing)")));
    const NavtexAreaForecast *fo = find(v, "Forties"), *cr = find(v, "Cromarty");
    check(fo && cr && navtex_forecast_summary(*fo) == "SE 4–6 · SLIGHT–MODERATE · UP TO SEV GALE 9",
          "\"SW FORTIES\" (a qualifier) is Forties; the operator sign-off is ignored: "
          + (fo ? navtex_forecast_summary(*fo) : std::string("(missing)")));
    const NavtexAreaForecast *th = find(v, "Thames");
    check(th && th->force_peak == 9 && navtex_forecast_summary(*th) == "SE 6–8 · MODERATE–ROUGH · UP TO SEV GALE 9",
          "the opening direction, though the forces come after the changes: " + (th ? navtex_forecast_summary(*th) : std::string("(missing)")));

    check(navtex_parse_forecasts("ZCZC XE01\nHIGH SEAS BULLETIN FOR METAREA 1\nISSUED BY THE MET OFFICE\nVIKING\nSW 5 TO 7. ROUGH. GOOD\nNNNN").empty(),
          "high-seas bulletins are not area forecasts");
    check(navtex_parse_forecasts("ZCZC XE02\nVIKING\nSW 5 TO 7. ROUGH. GOOD\nNNNN").empty(),
          "only Met Office and Norwegian bulletins are read");
}

static void norwegian()
{
    printf("Norwegian bulletin\n");
    const char *text =
        "ZCZC LE50\n"
        "WEATHER BULLETIN ISSUED BY NORWEGIAN METEOROLOGICAL INSTITUTE\n"
        "  FORECAST VALID NEXT 36 HOURS:\n"
        "  INNER SKAGERRAK\n"
        "E 6, OCNL SE NEAR GALE 7. SLT OR MOD, OCNL ROUGH. RAIN, MOD.\n"
        "  OUTER SKAGERRAK\n"
        "E 6. MOD. RAIN, MOD.\n"
        "  C3\n"
        "W GALE 8, DECR TO VRB 6. HIGH TO ROUGH.\n"
        "  EAST-TAMPEN\n"
        "VRB 4. SLT OR MOD. DRY, GOOD.\n"
        "NNNN\n";
    auto v = navtex_parse_forecasts(text);
    check(v.size() == 3, "named areas only (coded \"C3\" is skipped and does not join the previous area)");
    const NavtexAreaForecast *o = find(v, "Outer Skagerrak");
    check(o && navtex_forecast_summary(*o) == "E 6 · MODERATE",
          "a bare \"MOD.\" second sentence is the sea state: " + (o ? navtex_forecast_summary(*o) : std::string("(missing)")));
    const NavtexAreaForecast *i = find(v, "Inner Skagerrak");
    check(i && navtex_forecast_summary(*i) == "E 6 · SLIGHT–MODERATE",
          "near gale 7 \"occasionally\" is not a gale: " + (i ? navtex_forecast_summary(*i) : std::string("(missing)")));
    check(find(v, "East Tampen") != nullptr, "hyphenated EAST-TAMPEN");
}

/* Group forecasts that limit a clause to some of the group's areas. */
static void shipping_received()
{
    printf("shipping forecast, per area of a group (received)\n");
    const char *text =
        "ZCZC GE26\n"
        "ISSUED BY THE MET OFFICE AT 2100 ON THU 17 SEP\n"
        "FORTIES CROMARTY FORTH TYNE DOGGER\n"
        "W OR SW 5 TO 7, BACK S OR SW 4 TO 6 LTR IN CROMARTY, FORTH AND TYNE. SLT OR MOD IN W CROMARTY, FORTH, "
        "TYNE AND W HUMBER, BUT ELSEWHERE MOD OR ROUGH. SHWRS, THEN OCCASIONAL RAIN LTR. GOOD, OCNL MOD\n"
        "\n"
        "THAMES DOVER WIGHT\n"
        "W OR SW VEER NW LTR, 3 TO 5, OCNL 6 IN PORTLAND. OCNL SMOOTH AT FIRST IN DOVER AND WIGHT, OTHERWISE "
        "MAINLY SLT OR MOD. SHWRS. GOOD\n"
        "\n"
        "ROCKALL MALIN HEBRIDES BAILEY\n"
        "S OR SE 6 TO GALE 8, OCNL SEV GALE 9 IN HEBRIDES, INCR GALE 8 TO STORM 10 FOR A TIME IN BAILEY. IN BAILEY, "
        "VERY ROUGH OR HIGH BECMG HIGH OR VERY HIGH, BUT ELSEWHERE ROUGH OR VERY ROUGH, OCNL MOD IN E AND HIGH IN W. "
        "RAIN OR SQUALLY SHWRS. GOOD, OCNL POOR\n"
        "\n"
        "FASTNET IRISH SEA\n"
        "W 4 TO 6, BACK SW 5 TO 7, OCNL GALE 8 LTR IN SOLE AND IRISH SEA. ROUGH OCNL VERY ROUGH LTR IN NW FITZROY, "
        "SOLE AND FASTNET, OTHERWISE MOD OR ROUGH. SHWRS. GOOD\n"
        "\n"
        "PORTLAND PLYMOUTH\n"
        "SW 3 OR 4,5INCR 5 OR 6, LCNL 7 754.8SLT OR M D, 75,) 4&)#954. 55#7 ERY DEWRC#GOOO95OJNL POOR\n"
        "\n"
        "LUNDY\n"
        "CYCLONIC BECOMING SOUTH OR SOUTHWEST, 4106, OCCASIONALLY 7 LATER. MODERATE OR ROUGH. RAIN. GOOD\n"
        "\n"
        "FAIR ISLE\n"
        "S OR SE, VEER SW FOR A TIME, 4 TO 6, OCNL 7 LTR IN W. MAINLY ROUGH OR VERY ROUGH, BUT MOD IN FAR S AND LTR "
        "IN E. SHWRS. MOD OR GOOD, OCNL VERY POOR AT FIRST IN E\n"
        "NNNN\n";
    auto v = navtex_parse_forecasts(text);
    auto sum = [&](const char *l) {
        const NavtexAreaForecast *f = find(v, l);
        return f ? navtex_forecast_summary(*f) : std::string("(missing)");
    };
    check(sum("Forties") == "W/SW 5–7 · MODERATE–ROUGH", "\"ELSEWHERE\" is Forties' sea state: " + sum("Forties"));
    check(sum("Cromarty") == "W/SW 5–7 · SLIGHT–MODERATE", "\"IN W CROMARTY\" is Cromarty's: " + sum("Cromarty"));
    check(sum("Tyne") == "W/SW 5–7 · SLIGHT–MODERATE", "an area later in the list (\"FORTH, TYNE AND W HUMBER\"): " + sum("Tyne"));
    check(sum("Thames") == "W/SW 3–5 · SLIGHT–MODERATE",
          "\"OCNL SMOOTH ... IN DOVER\" does not open the forecast: " + sum("Thames"));
    check(sum("Dover") == "W/SW 3–5 · SLIGHT–MODERATE", "nor for Dover, which it names: " + sum("Dover"));
    check(sum("Hebrides") == "S/SE 6–8 · ROUGH–VERY ROUGH · UP TO SEV GALE 9",
          "\"IN BAILEY, ...\" is Bailey's, and so is its storm: " + sum("Hebrides"));
    check(sum("Bailey") == "S/SE 6–8 · VERY ROUGH–HIGH · UP TO STORM 10", "Bailey: " + sum("Bailey"));
    check(sum("Fastnet") == "W 4–6 · ROUGH", "a gale only in Sole and the Irish Sea is not Fastnet's: " + sum("Fastnet"));
    check(sum("Irish Sea") == "W 4–6 · MODERATE–ROUGH · UP TO GALE 8", "Irish Sea: " + sum("Irish Sea"));
    check(!find(v, "Portland") && !find(v, "Plymouth"), "a garbled forecast is not read");
    check(sum("Lundy") == "CYC 4–6 · MODERATE–ROUGH", "\"4106\" is 4 to 6: " + sum("Lundy"));
    check(sum("Fair Isle") == "S/SE 4–6 · ROUGH–VERY ROUGH", "\"MAINLY ...\" opens the sea state: " + sum("Fair Isle"));
}

static void inshore_received()
{
    printf("inshore waters forecast, relayed (received)\n");
    const char *text =
        "ZCZC AE58\n"
        "MRSC MALIN, EJM.\n"
        "INSHORE WATERS FCST TO 12 MILES OFFSHORE FOR THE PERIOD 0600 UTC TUE 13 OCT TO 0600 UTC WED 14 OCT 2026\n"
        "ISLE OF MAN - (SWW)\n"
        "24 HR FCST:\n"
        "WORSW5TO7, DECR 4 OR 5, BACKS OR SW 5 OR 6 LTR.\n"
        "MOD OR ROUGH BECMG MOD.\n"
        "SHWRS.\n"
        "GOOD.\n"
        "LOUGH FOYLE TO CARLINGFORD LOUGH - (SWW)\n"
        "24 HR FCST:\n"
        "E4TO6BACKNW5TO7,OCNL8INS,BACKW4OR5LTR.\n"
        "SLT OR MOD, OCNL ROUGH IN FAR N.\n"
        "RAIN OR SHWRS.\n"
        "MOD OR GOOD, OCNL POOR.\n"
        "NNNN\n";
    auto v = navtex_parse_forecasts(text);
    check(v.size() == 2, "read without a \"MET OFFICE\" line");
    const NavtexAreaForecast *m = find(v, "Isle of Man");
    check(m && navtex_forecast_summary(*m) == "W/SW 5–7 · MODERATE–ROUGH",
          "\"WORSW5TO7\" is W OR SW 5 TO 7: " + (m ? navtex_forecast_summary(*m) : std::string("(missing)")));
    const NavtexAreaForecast *l = find(v, "Lough Foyle to Carlingford Lough");
    check(l && navtex_forecast_summary(*l) == "E 4–6 · SLIGHT–MODERATE · UP TO GALE 8",
          "\"E4TO6BACKNW5TO7,OCNL8INS\" opens E 4-6, not 4-7, and 8 in the south: " + (l ? navtex_forecast_summary(*l) : std::string("(missing)")));
}

static void met_eireann()
{
    printf("Met Éireann coastal waters (received)\n");
    const char *text =
        "ZCZC QE79\n"
        "MRSC MALIN HEAD\n"
        "SEA AREA FORECAST UNTIL 29/08/2026 06:00 ISSUED: 28/08/2026 05:30\n"
        "WARNINGS\n"
        "GALE WARNING: NIL.\n"
        "SMALL CRAFT WARNING: 1. NORTHWEST WINDS WILL REACH FORCE 6 THIS AFTERNOON (FRIDAY).\n"
        "FORECAST FOR BLOODY FORELAND TO FAIR HEAD TO STRANGFORD LOUGH AND FOR THE NORTH IRISH SEA\n"
        "WIND: CYCLONIC VARIABLE FORCE 3 TO 5. WEST TO NORTHWEST FORCE 4 OR 5 IMMINENT.\n"
        "WEATHER: CLOUDY WITH SHOWERS OR RAIN.\n"
        "VISIBILITY: MODERATE OR POOR IN PRECIPITATION, MIST AND FOG.\n"
        "FORECAST FOR STRANGFORD LOUGH TO HOWTH HEAD TO CARNSORE POINT AND FOR THE SOUTH IRISH SEA\n"
        "WIND: CYCLONIC VARIABLE FORCE 2 TO 4, OCCASIONALLY INCREASING FORCE 5.\n"
        "WEATHER: CLOUDY.\n"
        "VISIBILITY: GOOD. FORECAST FOR CARNSORE POINT TO DUNGARVAN TO VALENTIA\n"
        "WIND: WEST TO NORTHWEST FORCE 3 OR 4. LATER INCREASING CYCLONIC VARIABLE FORCE 4 TO 6.\n"
        "WEATHER: CLOUDY TO FAIR.\n"
        "FORECAST FOR VALENTIA TO ERRIS HEAD TO BLOODY FORELAND\n"
        "WIND: SOUTHWESTERLY FORCE 5 OR 6. POSSIBLY REACHING GALE FORCE 8 AT TIMES IN THE WEST LATER. WEATHER: FAIR. "
        "VISIBILITY: MOSTLY GOOD.\n"
        "OUTLOOK FOR A FURTHER 24 HOURS UNTIL SUNDAY 30/08/2026 06:00\n"
        "FORECAST FOR THE NEXT DAYS: STRONG WINDS.\n"
        "NNNN\n";
    auto v = navtex_parse_forecasts(text);
    auto sum = [&](const char *l) {
        const NavtexAreaForecast *f = find(v, l);
        return f ? navtex_forecast_summary(*f) : std::string("(missing)");
    };
    check(v.size() == 11, "every section of the coast, from four stretches: " + std::to_string(v.size()));
    check(sum("Bloody Foreland to Malin Head") == "CYC 3–5", "round the north coast: " + sum("Bloody Foreland to Malin Head"));
    check(sum("Fair Head to Carlingford Lough") == "CYC 3–5",
          "a section goes to the stretch covering most of it (to Strangford Lough): " + sum("Fair Head to Carlingford Lough"));
    check(sum("Wicklow Head to Carnsore Point") == "CYC 2–4", "east coast: " + sum("Wicklow Head to Carnsore Point"));
    check(sum("Roches Point to Mizen Head") == "W/NW 3–4", "\"WEST TO NORTHWEST\": " + sum("Roches Point to Mizen Head"));
    check(sum("Mizen Head to Loop Head") == "SW 5–6 · UP TO GALE 8",
          "Valentia to Loop Head is the larger part of Mizen Head to Loop Head: " + sum("Mizen Head to Loop Head"));
    const NavtexAreaForecast *f = find(v, "Malin Head to Fair Head");
    check(f && f->vis_worst == 0, "\"POOR IN PRECIPITATION\" is not the visibility");
}

static void knmi()
{
    printf("KNMI Dutch EEZ (received)\n");
    const char *text =
        "ZCZC PE63\n"
        "FORECAST DUTCH EEZ ISSUED AT 23:12 UTC 160926.\n"
        "\n"
        "GALE WARNINGS.\n"
        "THAMES. HUMBER. GERMAN BIGHT. DOGGER.\n"
        "NO WARNING.\n"
        "\n"
        "FORECAST VALID THURSDAY 03:00 TILL THURSDAY 15:00 UTC.\n"
        "\n"
        "THAMES.\n"
        "SOUTHWEST 4 - 5, INCREASING 5 - 6 SOON, IN THE SOUTHERN PART POSSIBLY 7 LATER.\n"
        "AN ISOLATED SHOWER FIRST, PERIODS OF RAIN LATER.\n"
        "GOOD, IN PRECIPITATION MODERATE.\n"
        "WAVEHEIGHT AROUND 1.0 METER, INCREASING 1.5 - 2.0 METER SOON, TEMPORARILY INCREASING 2.0 - 2.5 METER.\n"
        "\n"
        "HUMBER. GERMAN BIGHT. DOGGER.\n"
        "SOUTHWEST 4 - 5, INCREASING 5 - 6 SOON.\n"
        "GOOD, IN PRECIPITATION MODERATE.\n"
        "WAVEHEIGHT 2.0 - 2.5 METER.\n"
        "\n"
        "FORECAST VALID THURSDAY 15:00 TILL FRIDAY 03:00 UTC.\n"
        "\n"
        "THAMES.\n"
        "WEST 6 - 7.\n"
        "GOOD.\n"
        "WAVEHEIGHT 3.0 METER.\n"
        "NNNN\n";
    auto v = navtex_parse_forecasts(text);
    check(v.size() == 4, "four areas, from the first period only");
    const NavtexAreaForecast *t = find(v, "Thames");
    check(t && navtex_forecast_summary(*t) == "SW 4–5 · WAVES 1 M",
          "opening wind and wave height: " + (t ? navtex_forecast_summary(*t) : std::string("(missing)")));
    const NavtexAreaForecast *d = find(v, "Dogger");
    check(d && navtex_forecast_summary(*d) == "SW 4–5 · WAVES 2–2.5 M",
          "a group header: " + (d ? navtex_forecast_summary(*d) : std::string("(missing)")));
}

static void dwd()
{
    printf("DWD German Bight, English (received)\n");
    const char *en =
        "ZCZC SE84\n"
        "221500 NAVTEX-HAMBURG (NCC)\n"
        "\n"
        "WEATHERFORECAST FOR GERMAN BIGHT\n"
        "\n"
        "THURSDAY:\n"
        "WIND: SOUTHWESTERN PART FIRST SOUTHWEST 6 TO 7, OTHERWISE SOUTHWESTERLY WINDS ABOUT 5, DECREASING 3 TO 4.\n"
        "VISIBILITY/WEATHER: SHOWER WITH POOR VISIBILITY, LOCALLY THUNDERSTORMS.\n"
        "SEA: FIRST 3 METER.\n"
        "\n"
        "FRIDAY:\n"
        "WIND: WEST 7.\n"
        "NNNN\n";
    auto v = navtex_parse_forecasts(en);
    const NavtexAreaForecast *g = find(v, "German Bight");
    check(v.size() == 1 && g && navtex_forecast_summary(*g) == "SW 5 · WAVES 3 M",
          "\"OTHERWISE\" is the area, the part is not; the first day only: " + (g ? navtex_forecast_summary(*g) : std::string("(missing)")));

    printf("DWD sea and coastal forecasts, German (received)\n");
    const char *de =
        "ZCZC LE76\n"
        "230150 NAVTEX-HAMBURG (NCC)\n"
        "\n"
        "SEEWETTERVORHERSAGE\n"
        "VORHERSAGE FUER MITTWOCH:\n"
        "DEUTSCHE BUCHT\n"
        "ANFANGS SCHWACHWINDIG, VORUEBERGH. SW UM 4, SPAETER\n"
        "ZUNEHMEND UM 7 UND NW-DREHEND, BOEEN 10, GEWITTER MIT SCHLECHTER\n"
        "SICHT.\n"
        "SEE: SPAETER 3M.\n"
        "WESTLICHE OSTSEE\n"
        "E-SE 5, ABNEHMEND 3-4, SW-DREHEND, STRW. GEWITTERBOEEN 10, ZTW. GEWITTER MIT SCHLECHTER SICHT.\n"
        "SEE: 1,5M.\n"
        "\n"
        "VORHERSAGE FUER DONNERSTAG:\n"
        "WESTLICHE OSTSEE\n"
        "N-NW UM 7, BOEEN 9.\n"
        "SEE: 4M.\n"
        "\n"
        "KUESTENVORHERSAGE DEUTSCHE OSTSEEKUESTE\n"
        "VORHERSAGE FUER MITTWOCH:\n"
        "FLENSBURG BIS FEHMARN\n"
        "N-NW 4-5, SPAETER W-DREHEND.\n"
        "SEE: BIS 0,5M.\n"
        "NNNN\n";
    v = navtex_parse_forecasts(de);
    check(!find(v, "German Bight"), "light winds at first (\"SCHWACHWINDIG\") give no force: not shown");
    const NavtexAreaForecast *w = find(v, "Western Baltic");
    check(w && navtex_forecast_summary(*w) == "E/SE 5 · WAVES 1.5 M",
          "first day; gusts (\"GEWITTERBOEEN 10\") are not the wind: " + (w ? navtex_forecast_summary(*w) : std::string("(missing)")));
    const NavtexAreaForecast *f = find(v, "Flensburg to Fehmarn");
    check(f && navtex_forecast_summary(*f) == "N/NW 4–5 · WAVES UP TO 0.5 M",
          "coastal area, \"BIS 0,5M\": " + (f ? navtex_forecast_summary(*f) : std::string("(missing)")));
}

static void oostende()
{
    printf("Oostende Radio (received)\n");
    const char *text =
        "ZCZC TE14\n"
        "071910 UTC OCT\n"
        "OOSTENDERADIO WX 14/26\n"
        "THAMES: \n"
        "NNW TO N 5-7, LATER ON BACK NW.\n"
        "DOVER:\n"
        "IN EAST, WSW 7, LOC 8. IN WEST, WNW 5-7.\n"
        "BELGIAN COAST: \n"
        "SW 1-2, TONIGHT INCR TO 3 AND TEMPO BACK SSW. GOOD VIS, MOD DURING PRECIPITATIONS.\n"
        "NNNN\n";
    auto v = navtex_parse_forecasts(text);
    const NavtexAreaForecast *t = find(v, "Thames"), *b = find(v, "Belgian coast");
    check(t && navtex_forecast_summary(*t) == "NNW/N 5–7", "16-point directions: " + (t ? navtex_forecast_summary(*t) : std::string("(missing)")));
    check(b && navtex_forecast_summary(*b) == "SW 1–2", "Belgian coast: " + (b ? navtex_forecast_summary(*b) : std::string("(missing)")));
    check(!find(v, "Dover"), "a forecast split by part of the area (\"IN EAST, ...\") is not read");
}

static void gales()
{
    printf("gale warnings (received)\n");
    const char *mo =
        "ZCZC OB44\n"
        "GALE WARNING WEDNESDAY 14 OCTOBER 1548 UTC\n"
        "IRISH SEA\n"
        "SOUTHWESTERLY GALE FORCE 8 CONTINUING\n"
        "ROCKALL\n"
        "STORM FORCE 10 NOW VEERED SOUTHWESTERLY AND DECREASED GALE FORE 8\n"
        "MALIN\n"
        "GALE NOW CEASED BUT WESTERLY GALE FORCE 8 EXPECTED LATER\n"
        "BAILEY\n"
        "GALE NOW CEASED\n"
        "FORTIES\n"
        "GALE FORCE 8 VEERING WESTERLY SOON\n"
        "HEBRIDES\n"
        "SOUTHEASTERLY GALE FORCE 8 INCREASING SEVERE GALE FORCE 9 IMMINENT\n"
        "NNNN\n";
    auto v = navtex_parse_gale_warnings(mo);
    auto get = [&](const char *l) -> const NavtexGaleWarning * {
        for (const auto &g : v) if (std::string(NAVTEX_SEA_AREAS[g.area].label) == l) return &g;
        return nullptr;
    };
    const NavtexGaleWarning *i = get("Irish Sea"), *r = get("Rockall"), *m = get("Malin"), *b = get("Bailey"),
                            *f = get("Forties"), *h = get("Hebrides");
    check(v.size() == 6, "six areas");
    check(i && i->status == 'N' && i->force_now == 8 && i->wind_dir == "SW", "continuing: in force");
    check(r && r->status == 'N' && r->force_now == 8 && r->force == 8 && r->wind_dir == "SW",
          "\"NOW VEERED ... AND DECREASED GALE 8\": in force at 8, from the south-west");
    check(m && m->status == 'E' && m->force == 8, "ceased, but expected again");
    check(b && b->status == 'C', "ceased");
    check(f && f->status == 'N' && f->force_now == 8, "\"VEERING WESTERLY SOON\": the gale is here now");
    check(h && h->status == 'N' && h->force_now == 8 && h->force == 9, "in force at 8, 9 imminent");

    v = navtex_parse_gale_warnings("ZCZC PB36\n130308 UTC OCT 26\nGALEWARNING, 13 OCT 03:08 UTC.\nHUMBER.\n"
                                   "SOUTH TO SOUTHWEST 7.\nDOVER. THAMES.\nNO WARNING.\nEND OF GALEWARNING\nNNNN\n");
    check(v.size() == 1 && std::string(NAVTEX_SEA_AREAS[v[0].area].label) == "Humber" && v[0].force == 7,
          "KNMI: only the areas with a warning");
    v = navtex_parse_gale_warnings("ZCZC SB75\nNAVTEX-HAMBURG (NCC)\nWARNING NO. 142  200810UTC SEP\nFOR GERMAN BIGHT:\n"
                                   "NW 7, CEASING LATER.\nNNNN\n");
    check(v.size() == 1 && v[0].status == 'N' && v[0].force_now == 7, "DWD: in force, ceasing later");
    check(navtex_parse_gale_warnings("ZCZC NB71\nGALE WARNING NO. GX628,\nISSUED BY NORWEGIAN METEOROLOGICAL INSTITUTE\n"
                                     "FROEYA - ROERVIK:\nSOUTHEAST GALE FORCE 8.\nNNNN\n").empty(),
          "Norwegian coastal stretches are not sea areas this places");
}

int main()
{
    positions();
    positions_received();
    kinds();
    inshore();
    shipping();
    norwegian();
    shipping_received();
    inshore_received();
    met_eireann();
    knmi();
    dwd();
    oostende();
    gales();
    if (g_failures) { printf("%d FAILED\n", g_failures); return 1; }
    printf("all passed\n");
    return 0;
}
