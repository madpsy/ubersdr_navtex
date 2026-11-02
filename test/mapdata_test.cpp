/* mapdata_test.cpp — unit tests for src/navtex_mapdata.h: positions in
 * warnings, warning kinds, and the reduction of Met Office / Norwegian sea-area
 * forecasts to "NE 4–7 · ROUGH · POOR VIS".
 *
 * The texts are written in the formats seen on air (the layouts, the
 * abbreviations, the glued "S7TO9", a trailing operator sign-off), not copied
 * from a receiver's logs.
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

int main()
{
    positions();
    kinds();
    inshore();
    shipping();
    norwegian();
    if (g_failures) { printf("%d FAILED\n", g_failures); return 1; }
    printf("all passed\n");
    return 0;
}
