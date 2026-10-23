/* mqtt_selftest.cpp — unit tests for the MQTT payload builders.
 *
 * The decoder's HTTP and Home Assistant surfaces are exercised by running the
 * real binary against a stub ingest server. What that cannot easily reach is
 * the completed-message payload, which needs a decoded NAVTEX transmission —
 * so the pure payload builders are tested directly here.
 *
 * The whole translation unit is included rather than linked, because those
 * builders are file-static in the decoder. `main` is renamed out of the way so
 * this file can supply its own.
 *
 * Build:  cmake --build build --target mqtt_selftest && ./build/src/mqtt_selftest
 */

#define main navtex_rx_from_ubersdr_main
#include "navtex_rx_from_ubersdr.cpp"
#undef main

#include <cstdio>
#include <string>

static int g_failures = 0;

static void check(bool ok, const std::string &what)
{
    if (ok) {
        printf("  ok   %s\n", what.c_str());
    } else {
        printf("  FAIL %s\n", what.c_str());
        g_failures++;
    }
}

static bool contains(const std::string &hay, const std::string &needle)
{
    return hay.find(needle) != std::string::npos;
}

/* Minimal check that a byte string is well-formed UTF-8. JSON strings must be,
 * and a garbled SITOR-B decode is exactly what would break it. */
static bool is_valid_utf8(const std::string &s)
{
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        size_t extra;
        if      (c < 0x80)            extra = 0;
        else if ((c & 0xE0) == 0xC0)  extra = 1;
        else if ((c & 0xF0) == 0xE0)  extra = 2;
        else if ((c & 0xF8) == 0xF0)  extra = 3;
        else return false;

        if (i + extra >= s.size()) return false;
        for (size_t k = 1; k <= extra; k++)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += extra + 1;
    }
    return true;
}

/* ------------------------------------------------------------------ */

static void test_derive_base()
{
    printf("derive_base\n");
    unsetenv("UBERSDR_INGEST_URL");

    check(MqttPublisher::derive_base("http://ubersdr:8080") == "http://ubersdr:6926",
          "plain host:port");
    check(MqttPublisher::derive_base("http://ubersdr:8080/") == "http://ubersdr:6926",
          "trailing slash");
    check(MqttPublisher::derive_base("https://sdr.example.com") == "http://sdr.example.com:6926",
          "https base becomes http on the ingest port");
    check(MqttPublisher::derive_base("http://192.168.1.10:8073/some/path")
              == "http://192.168.1.10:6926",
          "path is discarded");
    check(MqttPublisher::derive_base("http://user:pw@ubersdr:8080") == "http://ubersdr:6926",
          "credentials are discarded");
    check(MqttPublisher::derive_base("http://[fd00::1]:8080") == "http://[fd00::1]:6926",
          "IPv6 literal stays bracketed");
    check(MqttPublisher::derive_base("") == "http://ubersdr:6926",
          "empty falls back to the compose default");

    setenv("UBERSDR_INGEST_URL", "http://elsewhere:7000/", 1);
    check(MqttPublisher::derive_base("http://ubersdr:8080") == "http://elsewhere:7000",
          "env override wins and is de-slashed");
    unsetenv("UBERSDR_INGEST_URL");
}

static void test_sanitiser()
{
    printf("mqtt_sanitise_text\n");

    check(mqtt_sanitise_text("HELLO 123") == "HELLO 123", "plain ASCII passes through");
    check(mqtt_sanitise_text("line1\nline2\r\n\ttab") == "line1\nline2\r\n\ttab",
          "ordinary whitespace is preserved");

    /* CCIR476::code_to_char returns a NEGATED code for unassigned bit patterns
     * and filter_print forwards it, so these bytes really do reach the body. */
    std::string garbled = "OK";
    garbled += static_cast<char>(0xFB);
    garbled += static_cast<char>(0x80);
    garbled += "END";
    std::string clean = mqtt_sanitise_text(garbled);
    check(clean == "OK??END", "high bytes from a garbled decode become '?'");
    check(is_valid_utf8(clean), "sanitised output is valid UTF-8");

    std::string ctrl = "A";
    ctrl += static_cast<char>(0x01);
    ctrl += "B";
    check(mqtt_sanitise_text(ctrl) == "A?B", "control characters become '?'");
    check(mqtt_sanitise_text("") == "", "empty stays empty");
}

static MqttMessageEvent sample_event()
{
    MqttMessageEvent ev;
    ev.freq_hz      = 518000;
    ev.freq_label   = "518 kHz";
    ev.channel_name = "International";
    ev.station      = 'E';
    ev.subject      = 'A';
    ev.serial       = 2;
    ev.body         = "GALE WARNING\nDOVER 0600 UTC\n";
    ev.text         = "ZCZC EA02\n" + ev.body + "NNNN\n";
    ev.start_utc    = "2026-08-18T14:00:00Z";
    ev.end_utc      = "2026-08-18T14:01:30Z";
    ev.duration_s   = 90;
    ev.has_snr      = true;
    ev.snr_db       = 18.25;
    ev.has_char_stats   = true;
    ev.chars_clean_pct  = 97.5;
    ev.chars_fec_pct    = 2.0;
    ev.chars_failed_pct = 0.5;
    return ev;
}

static void test_message_json()
{
    printf("mqtt_message_json\n");
    std::string j = mqtt_message_json(sample_event());

    check(contains(j, "\"id\":\"EA02\""),              "message id is station+subject+serial");
    check(contains(j, "\"freq_hz\":518000"),           "freq_hz");
    check(contains(j, "\"freq_khz\":518.0"),           "freq_khz");
    check(contains(j, "\"subject_name\":\"Navigational warning\""),
          "subject letter is decoded to words");
    check(contains(j, "\"serial\":2"),                 "serial");
    check(contains(j, "\"duration_s\":90"),            "duration");
    check(contains(j, "\"snr_db\":18.25"),             "snr");
    check(contains(j, "\"chars_clean_pct\":97.5"),     "char quality");
    check(contains(j, "\"channel\":\"International\""), "channel name");

    /* The whole point of this addon's feed: the complete decoded message. */
    check(contains(j, "GALE WARNING"),                 "message text is included");
    check(contains(j, "\\nDOVER 0600 UTC"),            "newlines are escaped, not dropped");
    check(contains(j, "ZCZC EA02"),                    "ZCZC header is included");
    check(contains(j, "NNNN"),                         "NNNN terminator is included");
    check(is_valid_utf8(j),                            "payload is valid UTF-8");

    /* A message with no serial, no SNR and no char stats must still emit
     * well-formed JSON with explicit nulls rather than dangling commas. */
    MqttMessageEvent bare;
    bare.freq_hz    = 490000;
    bare.freq_label = "490 kHz";
    bare.body       = "TEST";
    bare.text       = "ZCZC\nTEST\nNNNN\n";
    std::string bj = mqtt_message_json(bare);
    check(contains(bj, "\"serial\":null"),  "missing serial emits null");
    check(contains(bj, "\"snr_db\":null"),  "missing snr emits null");
    check(!contains(bj, ",,") && !contains(bj, "{,") && !contains(bj, ",}"),
          "no malformed comma sequences");
    check(is_valid_utf8(bj), "bare payload is valid UTF-8");
}

static void test_message_json_escapes_hostile_text()
{
    printf("mqtt_message_json — hostile input\n");

    MqttMessageEvent ev = sample_event();
    /* Quote, backslash and a raw high byte all in the message text. */
    std::string nasty = "SAY \"HELLO\"\\ ";
    nasty += static_cast<char>(0xC3);   /* lone lead byte: invalid UTF-8 */
    nasty += "END";
    ev.body = mqtt_sanitise_text(nasty);
    ev.text = "ZCZC EA02\n" + ev.body + "NNNN\n";

    std::string j = mqtt_message_json(ev);
    check(contains(j, "\\\"HELLO\\\""), "double quotes are escaped");
    check(contains(j, "\\\\"),          "backslash is escaped");
    check(is_valid_utf8(j),             "hostile text still yields valid UTF-8");

    /* Balanced braces is a cheap structural sanity check without a parser. */
    int depth = 0;
    bool in_str = false, esc = false, ok = true;
    for (char c : j) {
        if (esc) { esc = false; continue; }
        if (c == '\\' && in_str) { esc = true; continue; }
        if (c == '"') { in_str = !in_str; continue; }
        if (in_str) continue;
        if (c == '{') depth++;
        if (c == '}') depth--;
        if (depth < 0) ok = false;
    }
    check(ok && depth == 0 && !in_str, "braces and quotes are balanced");
}

static void test_subject_names()
{
    printf("navtex_subject_name\n");
    check(std::string(navtex_subject_name('A')) == "Navigational warning", "A");
    check(std::string(navtex_subject_name('B')) == "Meteorological warning", "B");
    check(std::string(navtex_subject_name('D')) == "Search and rescue", "D");
    check(std::string(navtex_subject_name('L')) == "Additional navigational warning", "L");
    check(std::string(navtex_subject_name('Z')) == "No messages on hand", "Z");
    check(std::string(navtex_subject_name('Q')).empty(), "unassigned letter yields empty");
    check(std::string(navtex_subject_name(0)).empty(), "no subject yields empty");
}

static void test_json_find_bool()
{
    printf("json_find_bool\n");
    check(json_find_bool("{\"ha_discovery\":true}", "ha_discovery", false) == true,
          "compact form");
    check(json_find_bool("{\"ha_discovery\": true}", "ha_discovery", false) == true,
          "space after colon — the form that silently broke discovery before");
    check(json_find_bool("{\"ha_discovery\" : true}", "ha_discovery", false) == true,
          "spaces either side of the colon");
    check(json_find_bool("{\"ha_discovery\":false}", "ha_discovery", true) == false,
          "false is read as false");
    check(json_find_bool("{\"other\":true}", "ha_discovery", false) == false,
          "absent key falls back to the default");
    check(json_find_bool("{\"ha_discovery\":\"yes\"}", "ha_discovery", false) == false,
          "non-boolean falls back to the default");
}


/* ------------------------------------------------------------------ */
/* Audio protocol version 4 SNR scale                                   */
/* ------------------------------------------------------------------ */

/*
 * Version 4 changed the noise figure from a density in dBFS/Hz to a power in
 * dBFS, which drops every SNR by 10*log10(passband) -- 34.2 dB on the 2650 Hz
 * filter this program asks for.  The thresholds moved with it.  Nothing about
 * that failure is loud: a mis-scaled threshold just paints every message red.
 *
 * The numbers checked here are the ones measured live on 490 and 518 kHz; see
 * navtex_snr.h for the captures they came from.
 */
static void test_snr_scale()
{
    printf("SNR scale (audio protocol version 4)\n");

    /* A real 518 kHz transmission measured a median of 15.85 dB with a 5th
     * percentile of 12.03.  A strong signal must read good. */
    check(navtex_snr_class(15.85) == "good", "a measured 518 kHz signal is good");
    check(navtex_snr_class(18.62) == "good", "its 95th percentile is good");
    check(navtex_snr_class(25.12) == "good", "its peak is good");

    /* The 490 kHz idle capture: median -6.59, max +2.32.  None of it may
     * read as a signal. */
    check(navtex_snr_class(-6.59) == "bad", "the measured idle floor is bad");
    check(navtex_snr_class(-9.76) == "bad", "its 5th percentile is bad");
    check(navtex_snr_class(2.32)  == "bad", "even its peak excursion is bad");

    /* The empty band between the two distributions is where "warn" belongs. */
    check(navtex_snr_class(6.0) == "warn", "the gap between them is warn");

    check(navtex_snr_class(std::nan("")) == "dim", "no reading is dim");

    /* The bar must not be pinned at either end for real traffic. */
    check(navtex_snr_bar_pct(15.85) > 60.0 && navtex_snr_bar_pct(15.85) < 90.0,
          "the bar is most of the way up for a real signal");
    check(navtex_snr_bar_pct(-6.59) > 0.0 && navtex_snr_bar_pct(-6.59) < 25.0,
          "the bar is low but not empty when idle");
    check(navtex_snr_bar_pct(-40.0) == 0.0,   "the bar clamps at the bottom");
    check(navtex_snr_bar_pct(999.0) == 100.0, "the bar clamps at the top");

    /* The shift itself, against the paired live measurement of 34.22 dB. */
    const double shift = navtex_snr_scale_shift_db();
    check(shift > 34.1 && shift < 34.4, "the scale shift matches the measured 34.22 dB");
}

/* ------------------------------------------------------------------ */
/* Stored records written on either scale                               */
/* ------------------------------------------------------------------ */

/*
 * The log directory outlives the migration, so one field holds records from
 * both scales.  A pre-migration record read back unconverted would be about
 * 34 dB high -- it would show every old message as the best signal ever
 * received, right next to new ones showing as the worst.
 */
static void test_sidecar_scale()
{
    printf("stored metrics scale\n");

    /* What save_message() writes today. */
    const std::string modern =
        "{\n  \"audio_protocol_version\": 4,\n  \"noise_scale\": \"passband_power\",\n"
        "  \"avg_snr_db\": 15.85,\n  \"avg_noise_density_dbfs\": -112.70,\n"
        "  \"duration_s\": 132\n}\n";

    /* What a pre-migration build wrote: the same physical signal, 34.2 dB
     * higher, with no stamp to say so. */
    const std::string legacy =
        "{\n  \"freq_hz\": 518000,\n"
        "  \"avg_snr_db\": 50.07,\n  \"avg_noise_density_dbfs\": -146.92,\n"
        "  \"duration_s\": 132\n}\n";

    check(!sidecar_is_legacy_scale(modern), "a stamped record is not legacy");
    check(sidecar_is_legacy_scale(legacy),  "an unstamped record is legacy");

    /* A modern record passes through untouched -- no double conversion. */
    check(sidecar_normalise_scale(modern) == modern,
          "a version 4 record is returned unchanged");

    const std::string fixed = sidecar_normalise_scale(legacy);
    const double snr = atof(sidecar_field(fixed, "avg_snr_db").c_str());
    const double noise = atof(sidecar_field(fixed, "avg_noise_density_dbfs").c_str());

    /* 50.07 - 34.23 = 15.84: the same transmission the modern record holds. */
    check(snr > 15.7 && snr < 16.0,
          "a pre-migration SNR converts onto the version 4 scale");
    check(navtex_snr_class(snr) == "good",
          "and then classifies the same as the equivalent new record");
    check(navtex_snr_class(atof(sidecar_field(legacy, "avg_snr_db").c_str())) == "good"
              && atof(sidecar_field(legacy, "avg_snr_db").c_str()) > 45.0,
          "unconverted, it would have read 50 dB -- off the new scale entirely");

    /* -146.92 + 34.23 = -112.69: a power, matching the modern record. */
    check(noise > -112.9 && noise < -112.5,
          "the noise density converts to a passband power");

    check(contains(fixed, "\"snr_rescaled_from_density\": true"),
          "the conversion is declared in what comes back");
    check(contains(fixed, "\"audio_protocol_version\": 2"),
          "and the record still reports the version it was recorded on");

    /* A record whose metrics were never populated must survive the trip. */
    const std::string nulls =
        "{\n  \"avg_snr_db\": null,\n  \"avg_noise_density_dbfs\": null\n}\n";
    const std::string nfixed = sidecar_normalise_scale(nulls);
    check(contains(nfixed, "\"avg_snr_db\": null"),
          "a null SNR is left null rather than becoming a number");

    /* Older sidecars can hold "-nan"; converting one would make it look real. */
    const std::string nan_rec =
        "{\n  \"avg_snr_db\": -nan,\n  \"avg_noise_density_dbfs\": -nan\n}\n";
    check(contains(sidecar_normalise_scale(nan_rec), "-nan"),
          "a non-numeric value is left alone");
}

/*
 * The page carries the thresholds as generated JavaScript.  A literal left
 * behind in navtex_html.h would compile, render and simply be wrong, so check
 * the constants reach the page and that none of the old ones survive.
 */
static void test_page_snr_constants()
{
    printf("generated page constants\n");

    std::vector<ChannelContext> chans(1);
    chans[0].channel_id = 0;
    chans[0].label      = "518 kHz";
    const std::string page = make_html_page("http://sdr.example", chans, "", true);

    check(contains(page, "const SNR_GOOD_DB     = 12.00;"),
          "the good boundary reaches the page");
    check(contains(page, "const SNR_WARN_DB     = 3.00;"),
          "the warn boundary reaches the page");
    check(contains(page, "const SNR_BAR_MIN_DB  = -10.00;"),
          "the bar floor reaches the page");
    check(contains(page, "const SNR_BAR_SPAN_DB = 35.00;"),
          "the bar span reaches the page");

    check(!contains(page, "> 45 ?"), "no version 2 'good' threshold is left in the page");
    check(!contains(page, "> 35 ?"), "no version 2 'warn' threshold is left in the page");
    check(!contains(page, "- 25) / 35"), "no version 2 bar range is left in the page");
    check(!contains(page, "dBFS/Hz"),
          "the noise label no longer claims a density");

    /* Every call site must resolve to the shared helpers. */
    check(contains(page, "function snrCls("),      "snrCls is defined once, at page scope");
    check(contains(page, "function snrBarPct("),   "snrBarPct is defined");
    check(contains(page, "snrCls(m.snr)"),         "the history table uses it");
    check(contains(page, "snrClass(m.avg_snr_db)"),"the metrics modal uses it");
}

/* ------------------------------------------------------------------ */
/* Station identification                                               */
/* ------------------------------------------------------------------ */

static int station_index(long freq_hz, char id, const char *name)
{
    for (int i = 0; i < NAVTEX_STATION_COUNT; i++)
        if (NAVTEX_STATIONS[i].freq_hz == freq_hz && NAVTEX_STATIONS[i].id == id &&
            strcmp(NAVTEX_STATIONS[i].name, name) == 0)
            return i;
    return -1;
}

/* A hand-made identification of Niton, so these tests pin the plumbing and
 * not whatever the algorithm (or its stub) happens to answer. */
static IdentRecord niton_record()
{
    NavtexIdentQuery q {};
    q.freq_hz = 518000; q.b1 = 'E'; q.b2 = 'A'; q.serial = 2;
    q.start_utc = iso8601_to_epoch("2026-08-18T00:41:42Z");
    q.duration_s = 90;
    q.rx_valid = true; q.rx_lat = 56.0403; q.rx_lon = -3.3554;
    NavtexIdentResult r {};
    r.station = station_index(518000, 'E', "Niton");
    r.confidence = 0.973; r.level = "high"; r.agree = true;
    r.member[0] = { "bayes", r.station, 0.98, "prior" };
    r.member[1] = { "rules", r.station, 0.95, "nearest" };
    r.distance_km = r.bearing_deg = -1;
    r.in_slot = true; r.slot_offset_s = 102; r.burst_offset_s = 102; r.n_candidates = 7;
    r.rx_pos_checked = true;   /* as v2 reports once ~100 messages are heard here */
    r.reason = "nearest in-slot candidate";
    r.alt[0] = station_index(518000, 'W', "Valentia"); r.alt_p[0] = 0.02;
    r.alt[1] = -1;
    return ident_record(q, r);
}

static void test_ident_json()
{
    printf("ident JSON\n");
    IdentRecord rec = niton_record();
    check(rec.r.station >= 0, "Niton E is in the table");
    std::string j = ident_json(rec, false);
    check(contains(j, "\"v\":" + std::to_string(NAVTEX_IDENT_VERSION)), "algorithm version");
    check(contains(j, "\"table\":\"" + ident_table_fp() + "\""), "table fingerprint");
    check(ident_table_fp().size() == 16, "fingerprint is 16 hex digits");
    check(contains(j, "\"rx\":[56.0403,-3.3554]"), "receiver position used");
    check(contains(j, "\"key\":\"518:E:Niton\""), "stable station key");
    check(contains(j, "\"country\":\"GBR\",\"country_name\":\"United Kingdom\",\"iso2\":\"gb\""),
          "country code, name and flag code");
    check(contains(j, "\"level\":\"high\""), "level");
    check(contains(j, "\"conf\":0.973"), "confidence");
    check(contains(j, "\"dist_km\":6"), "distance computed when the algorithm left it unknown");
    check(contains(j, "\"alts\":[{\"key\":\"518:W:Valentia\""), "alternatives by key");
    check(contains(j, "Started 1 min 42 s into E's 00:40 slot"), "facts are embedded");
    check(contains(j, "\"provisional\":false"), "final");
    check(contains(ident_json(rec, true), "\"provisional\":true"), "provisional");
    check(is_valid_utf8(j), "valid UTF-8");
    check(sidecar_ident_sig(j) == ident_sig(NAVTEX_IDENT_VERSION, ident_table_fp(),
                                            "[56.0403,-3.3554]"),
          "the up-to-date signature reads back from what was written");

    /* Strings are owned by the record, not borrowed from the identifier. */
    check(rec.r.reason == nullptr && rec.reason == "nearest in-slot candidate",
          "result strings are copied and the pointers nulled");

    NavtexIdentQuery q = rec.q;
    q.b1 = 'Z';
    NavtexIdentResult none {};
    none.station = -1; none.level = "none"; none.alt[0] = none.alt[1] = -1;
    none.distance_km = none.bearing_deg = -1;
    std::string u = ident_json(ident_record(q, none), false);
    check(contains(u, "\"key\":null") && contains(u, "\"level\":\"none\"") &&
          contains(u, "\"name\":null") && contains(u, "\"iso2\":null"),
          "unattributed: key and names null, level none");
    check(contains(u, "not in the station list"), "unattributed still explains itself");
    check(ident_st_json(ident_record(q, none)) == "{\"level\":\"none\",\"key\":null}",
          "history summary of an unattributed message");
    check(contains(ident_st_json(rec), "\"key\":\"518:E:Niton\"") &&
          contains(ident_st_json(rec), "\"navarea\":1"),
          "history summary of an identified one");
}

static void test_ident_mqtt()
{
    printf("MQTT with identification\n");
    IdentRecord rec = niton_record();
    MqttMessageEvent ev = sample_event();

    std::string bare = mqtt_message_json(ev);
    check(contains(bare, "\"station\":\"E\""), "station stays the letter");
    check(contains(bare, "\"ident\":null") && contains(bare, "\"station_name\":null"),
          "no identification reads as nulls");

    ident_fill_mqtt(ev, rec, ident_json(rec, false));
    std::string j = mqtt_message_json(ev);
    check(contains(j, "\"station\":\"E\""), "station is still the letter");
    check(contains(j, "\"station_name\":\"Niton\""), "station_name");
    check(contains(j, "\"station_country\":\"United Kingdom\""), "station_country");
    check(contains(j, "\"station_iso2\":\"gb\""), "station_iso2");
    check(contains(j, "\"station_confidence\":0.973"), "station_confidence (0..1)");
    check(contains(j, "\"station_level\":\"high\""), "station_level");
    check(contains(j, "\"station_distance_km\":6"), "station_distance_km");
    check(contains(j, "\"ident\":{\"v\":"), "full ident object");
    check(contains(j, "\"facts\":["), "facts ride along");
    check(is_valid_utf8(j), "valid UTF-8");
}

static void test_rx_description()
{
    printf("receiver location from /api/description\n");
    rx_apply_description("{\"receiver\":{\"callsign\":\"M9PSY\",\"location\":\"Dalgety Bay, "
                         "Scotland, UK\",\"gps\":{\"lat\":56.0403,\"lon\":-3.3554,"
                         "\"maidenhead\":\"IO86ha\"}}}");
    RxLocation rx = rx_snapshot();
    check(rx.valid && rx.lat == 56.0403 && rx.lon == -3.3554, "lat/lon");
    check(rx.source == "ubersdr", "source");
    check(rx.locator == "IO86ha" && rx.callsign == "M9PSY" &&
          rx.location == "Dalgety Bay, Scotland, UK", "locator, callsign, location");

    rx_apply_description("{ \"version\": \"1\", \"receiver\" : { \"gps\" : { \"lat\" : 0, "
                         "\"lon\" : 0 } , \"callsign\" : \"X\" } }");
    rx = rx_snapshot();
    check(!rx.valid && rx.source == "none", "0,0 means not set (whitespace tolerated)");

    check(!rx_apply_description("<html>"), "a non-description is rejected");
    check(!rx_apply_description("{\"receiver\":\"x\"}"), "receiver must be an object");

    RxLocation a, b;
    a.valid = b.valid = true; a.lat = b.lat = 56; a.lon = -3.3; b.lon = -3.35;
    check(!rx_changed(a, b), "a 3 km move does not re-identify");
    b.lon = -3.5;
    check(rx_changed(a, b), "a 12 km move does");
    b.valid = false;
    check(rx_changed(a, b), "losing the position does");

    check(iso8601_to_epoch("2026-10-18T00:52:03Z") == 1792284723, "ISO-8601 to epoch");
    check(iso8601_to_epoch("garbage") == 0, "garbage is 0");
}

static std::string slurp(const std::string &p)
{
    std::string s;
    read_whole_file(p, s);
    return s;
}

static void spit(const std::string &p, const std::string &s)
{
    FILE *f = fopen(p.c_str(), "w");
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

static const char *SAMPLE_SIDECAR =
    "{\n  \"audio_protocol_version\": 4,\n  \"noise_scale\": \"passband_power\",\n"
    "  \"freq_hz\": 518000,\n  \"freq_label\": \"518 kHz\",\n  \"station\": \"E\",\n"
    "  \"subject\": \"A\",\n  \"serial\": 2,\n  \"start_utc\": \"2026-08-18T00:41:42Z\",\n"
    "  \"end_utc\": \"2026-08-18T00:43:12Z\",\n  \"duration_s\": 90,\n"
    "  \"avg_snr_db\": 15.85,\n  \"avg_chars_failed_pct\": 0.5,\n  \"char_count\": 12\n}\n";

static void test_sidecar_ident()
{
    printf("sidecar ident member\n");
    char tmpl[] = "/tmp/navtex_selftest_XXXXXX";
    std::string dir = mkdtemp(tmpl);
    std::string p = dir + "/m.json";
    spit(p, SAMPLE_SIDECAR);

    IdentRecord rec = niton_record();
    check(sidecar_write_ident(p, ident_json(rec, false)), "first write");
    std::string a = slurp(p);
    check(a.compare(0, strlen(SAMPLE_SIDECAR) - 3, SAMPLE_SIDECAR, strlen(SAMPLE_SIDECAR) - 3) == 0,
          "everything before it is byte-for-byte unchanged");
    check(sidecar_field(a, "station") == "\"E\"", "top-level station still reads as the letter");
    check(sidecar_field(a, "serial") == "2", "top-level serial unaffected");

    check(sidecar_write_ident(p, ident_json(rec, true)), "rewrite");
    std::string b = slurp(p);
    size_t n = 0;
    for (size_t at = b.find("\"ident\""); at != std::string::npos; at = b.find("\"ident\"", at + 1)) n++;
    check(n == 1, "a rewrite replaces the member rather than adding a second");
    check(contains(b, "\"provisional\":true"), "with the new content");
    check(access((p + ".tmp").c_str(), F_OK) != 0, "no temporary file is left behind");

    HistoryIndexEntry e;
    check(sidecar_load(p, "518kHz/2026/08/18/004312Z_EA02.txt", e), "sidecar_load");
    check(e.q.freq_hz == 518000 && e.q.b1 == 'E' && e.q.b2 == 'A' && e.q.serial == 2,
          "query: freq, B1, B2, serial");
    check(e.q.start_utc == iso8601_to_epoch("2026-08-18T00:41:42Z") && e.q.duration_s == 90,
          "query: start, duration");
    check(e.q.has_snr && e.q.snr_db == 15.85 && e.q.has_chars && e.q.chars_failed_pct == 0.5,
          "query: SNR and failed-character rate");
    check(e.snr == "15.85" && e.duration == "90", "history summary fields");
    check(e.disk_sig == ident_sig(NAVTEX_IDENT_VERSION, ident_table_fp(), "[56.0403,-3.3554]"),
          "stored signature is recovered for the skip check");

    remove(p.c_str());
    rmdir(dir.c_str());
}

/* Contract section 11 and the sidecar edge cases found in review: nothing
 * non-finite or implausible reaches the identifier, and the sidecar writer
 * never turns a file into invalid JSON. */
static void test_input_cleaning()
{
    printf("input cleaning (section 11)\n");
    for (const char *ok : { "0", "15.85", "-3.2", "1e3", "-0.5E-2", "90" })
        check(sidecar_is_number(ok), std::string("number: ") + ok);
    for (const char *bad : { "", "-", "e", ".", "1e999", "-1e999", "-nan", "nan", "inf",
                             "-inf", "1.", ".5", "01", "1e", "+1", "1-2" })
        check(!sidecar_is_number(bad), std::string("not a number: '") + bad + "'");

    NavtexIdentQuery q {};
    q.start_utc = iso8601_to_epoch("2026-08-18T00:41:42Z");
    q.has_snr = true;   q.snr_db = std::nan("");
    q.has_chars = true; q.chars_failed_pct = INFINITY;
    q.rx_valid = true;  q.rx_lat = std::nan(""); q.rx_lon = 1;
    check(ident_query_clean(q), "a plausible start time is kept");
    check(!q.has_snr && !q.has_chars && !q.rx_valid, "NaN SNR, inf chars, NaN position dropped");
    q.rx_valid = true; q.rx_lat = 91; q.rx_lon = 0;
    ident_query_clean(q);
    check(!q.rx_valid, "latitude out of range dropped");

    q.start_utc = 0;
    check(!ident_query_clean(q), "1970 (clock not set) is rejected");
    q.start_utc = iso8601_to_epoch("2100-01-01T00:00:00Z");
    check(!ident_query_clean(q), "2100 is rejected");
    q.start_utc = (int64_t)time(nullptr) + 3600;
    check(ident_query_clean(q), "an hour ahead (clock skew) is tolerated");

    char tmpl[] = "/tmp/navtex_selftest_XXXXXX";
    std::string dir = mkdtemp(tmpl);
    std::string p = dir + "/m.json";
    auto with = [](const std::string &key, const std::string &val) {
        std::string sc = SAMPLE_SIDECAR;
        size_t a = sc.find("\"" + key + "\": ");
        size_t v = a + key.size() + 4, e = sc.find_first_of(",\n", v);
        return sc.substr(0, v) + val + sc.substr(e);
    };
    HistoryIndexEntry e;

    spit(p, with("start_utc", "\"1970-01-01T00:00:12Z\""));
    check(sidecar_load(p, "518kHz/2026/08/18/004312Z_EA02.txt", e) && !e.skipped &&
          e.q.start_utc == iso8601_to_epoch("2026-08-18T00:43:12Z"),
          "a 1970 start_utc falls back on the file name's time");
    spit(p, with("start_utc", "\"2100-01-01T00:00:00Z\""));
    check(sidecar_load(p, "518kHz/1970/01/01/000012Z_EA02.txt", e) && e.skipped &&
          contains(e.st_json, "\"skipped\""),
          "2100 with a 1970 file name: indexed but never identified");
    spit(p, with("avg_snr_db", "1e999"));
    check(sidecar_load(p, "518kHz/2026/08/18/004312Z_EA02.txt", e) && !e.q.has_snr && e.snr.empty(),
          "avg_snr_db 1e999 is not an SNR");
    spit(p, with("avg_snr_db", "-nan"));
    check(sidecar_load(p, "518kHz/2026/08/18/004312Z_EA02.txt", e) && !e.q.has_snr,
          "avg_snr_db -nan is not an SNR");

    /* Writer edge cases. */
    IdentRecord rec = niton_record();
    const std::string id = ident_json(rec, false);
    spit(p, "{}");
    check(sidecar_write_ident(p, id), "'{}' gains an ident member");
    std::string t = slurp(p);
    check(t.compare(0, 2, "{\n") == 0 && !contains(t, "{,"), "...without a leading comma");
    check(json_balanced_at(t, 0).size() + 1 == t.size(), "...and stays one balanced object");
    spit(p, "");
    check(!sidecar_write_ident(p, id) && slurp(p).empty(), "an empty sidecar is left alone");
    check(!sidecar_load(p, "518kHz/2026/08/18/004312Z_EA02.txt", e), "...and not indexed");
    std::string trunc = std::string(SAMPLE_SIDECAR).substr(0, 120);
    spit(p, trunc);
    check(!sidecar_write_ident(p, id) && slurp(p) == trunc, "a truncated sidecar is left alone");
    check(!sidecar_load(p, "518kHz/2026/08/18/004312Z_EA02.txt", e), "...and not indexed");
    spit(p, std::string(SAMPLE_SIDECAR) + "}\n");
    check(!sidecar_write_ident(p, id), "a sidecar with a stray '}' is left alone");
    spit(p, "{\n  \"freq_hz\": 518000\n}\n");
    check(!sidecar_load(p, "518kHz/2026/08/18/004312Z_EA02.txt", e), "no station/subject: not indexed");

    /* The rewrite keeps the file's date, so retention still sees its age. */
    spit(p, SAMPLE_SIDECAR);
    struct timespec old[2] = { { 1600000000, 0 }, { 1600000000, 0 } };
    utimensat(AT_FDCWD, p.c_str(), old, 0);
    check(sidecar_write_ident(p, id), "rewrite an old sidecar");
    struct stat st {};
    stat(p.c_str(), &st);
    check(st.st_mtime == 1600000000, "its modification time is unchanged");

    /* A subject the decoder garbled into '"' is escaped, and read back. */
    spit(p, with("subject", "\"\\\"\""));
    check(sidecar_load(p, "518kHz/2026/08/18/004312Z_E_.txt", e) && e.q.b2 == '"',
          "an escaped '\"' subject reads back as '\"'");

    remove(p.c_str());
    rmdir(dir.c_str());
}

/* Retention: a message's .json goes with its .txt, judged by the .txt's date;
 * stale .json.tmp files are swept; an orphan .json goes by its own date. */
static void test_purge()
{
    printf("log retention\n");
    char tmpl[] = "/tmp/navtex_selftest_XXXXXX";
    std::string dir = mkdtemp(tmpl);
    std::string d = dir + "/518kHz/2026/01/01";
    mkdir((dir + "/518kHz").c_str(), 0755); mkdir((dir + "/518kHz/2026").c_str(), 0755);
    mkdir((dir + "/518kHz/2026/01").c_str(), 0755); mkdir(d.c_str(), 0755);
    auto age = [](const std::string &f, long days) {
        time_t t = time(nullptr) - days * 86400;
        struct timespec ts[2] = { { t, 0 }, { t, 0 } };
        utimensat(AT_FDCWD, f.c_str(), ts, 0);
    };
    spit(d + "/old.txt", "x");  age(d + "/old.txt", 40);
    spit(d + "/old.json", "{}");                                  /* rewritten today */
    spit(d + "/new.txt", "x");  spit(d + "/new.json", "{}"); age(d + "/new.json", 40);
    spit(d + "/orph.json", "{}"); age(d + "/orph.json", 40);
    spit(d + "/x.json.tmp", "{"); age(d + "/x.json.tmp", 1);
    purge_old_logs(dir, 30);
    auto there = [](const std::string &f) { return access(f.c_str(), F_OK) == 0; };
    check(!there(d + "/old.txt") && !there(d + "/old.json"),
          "an old message's sidecar goes with it even if rewritten today");
    check(there(d + "/new.txt") && there(d + "/new.json"),
          "a new message keeps its sidecar whatever the sidecar's date");
    check(!there(d + "/orph.json"), "an old orphan sidecar is removed");
    check(!there(d + "/x.json.tmp"), "a stale .json.tmp is removed");
    remove((d + "/new.txt").c_str()); remove((d + "/new.json").c_str());
    purge_old_logs(dir, 30);   /* removes the now-empty directories */
    rmdir(dir.c_str());
}

/* Odds and ends from the security review. */
static void test_hardening()
{
    printf("hardening\n");
    rx_apply_description("{\"receiver\":{\"callsign\":\"LA1X\",\"location\":"
                         "\"Bod\\u00f8 \\u0026 Troms\\u00f8 \\u003cN\\u003e \\ud83d\\udce1\","
                         "\"gps\":{\"lat\":67.28,\"lon\":14.4}}}");
    RxLocation rx = rx_snapshot();
    check(rx.location == "Bod\xc3\xb8 & Troms\xc3\xb8 <N> \xf0\x9f\x93\xa1",
          "\\u escapes (incl. a surrogate pair) decode to UTF-8: " + rx.location);
    rx_apply_description("{\"receiver\":{\"location\":\"S\xc3\xa3o Miguel, Bjur\xc3\xb6klubb\","
                         "\"callsign\":\"bad\xff\\u0001\"}}");
    rx = rx_snapshot();
    check(rx.location == "S\xc3\xa3o Miguel, Bjur\xc3\xb6klubb", "raw UTF-8 is kept");
    check(rx.callsign == "bad??" || rx.callsign == "bad? ", "invalid UTF-8 and controls replaced: " + rx.callsign);
    check(is_valid_utf8(rx.callsign), "...leaving valid UTF-8");

    check(base_path_ok("") && base_path_ok("/navtex") && base_path_ok("/a/b-c_d.e~f"),
          "ordinary prefixes accepted");
    for (const char *bad : { "navtex", "/p\"><img src=x>", "/a//b", "/a b", "/a?b", "/'", "/a/" })
        check(!base_path_ok(bad), std::string("prefix rejected: ") + bad);

    std::string v;
    check(query_param("xrel=a.txt", "rel", v) == false, "xrel= is not rel=");
    check(query_param("rel=a.txt&foo=1", "rel", v) && v == "a.txt", "rel= stops at '&'");
    check(url_decode("a%zzb") == "a%zzb" && url_decode("a%41") == "aA", "url_decode: only hex escapes");

    std::string gz = gzip_compress(std::string(5000, 'x'));
    check(gz.size() > 18 && gz.size() < 200 && (unsigned char)gz[0] == 0x1f &&
          (unsigned char)gz[1] == 0x8b, "gzip_compress makes a gzip stream");
}

/* The worker end to end, on a scratch log directory: replay, a live
 * completion, a provisional answer, and the history list. */
static void test_analytics_format()
{
    printf("analytics formatting\n");
    auto num = [](double v, int dp) { std::string s; analytics_num(s, v, dp); return s; };
    check(num(0.973, 3) == "0.973", "conf keeps 3 dp");
    check(num(0.97, 3) == "0.97" && num(1.0, 3) == "1", "trailing zeros are dropped");
    check(num(518.0, 1) == "518" && num(4209.5, 1) == "4209.5", "frequencies");
    check(num(612.44, 1) == "612.4" && num(612.46, 1) == "612.5", "rounds to 1 dp");
    check(num(-3.25, 1) == "-3.3", "negative values round half away from zero");
    check(num(-0.04, 1) == "0", "never \"-0\"");
    check(num(-12.0, 1) == "-12" && num(0.05, 1) == "0.1", "signs and half-up");
    check(num(NAN, 1) == "null" && num(INFINITY, 1) == "null", "non-finite is null");
    check(num(0.0001, 3) == "0" && num(0.0105, 2) == "0.01", "small values");

    std::string v;
    check(query_param("from=1&to=2", "to", v) && v == "2", "query_param finds a later key");
    check(query_param("xto=5&to=7", "to", v) && v == "7", "query_param matches whole keys only");
    check(!query_param("xto=5", "to", v), "query_param: absent");
    bool set = true; int64_t t = 0;
    check(analytics_time_param("from=1790000000", "from", set, t) && set && t == 1790000000,
          "time param parsed");
    check(analytics_time_param("", "from", set, t) && !set, "time param absent");
    check(!analytics_time_param("from=abc", "from", set, t), "time param rejects text");
    check(!analytics_time_param("to=12x", "to", set, t), "time param rejects trailing junk");
}

static void test_ident_worker()
{
    printf("ident worker\n");
    char tmpl[] = "/tmp/navtex_selftest_XXXXXX";
    std::string dir = mkdtemp(tmpl);
    std::string day = dir + "/518kHz/2026/08/18";
    mkdir((dir + "/518kHz").c_str(), 0755);
    mkdir((dir + "/518kHz/2026").c_str(), 0755);
    mkdir((dir + "/518kHz/2026/08").c_str(), 0755);
    mkdir(day.c_str(), 0755);
    spit(day + "/004312Z_EA02.txt", "ZCZC EA02\nTEST\nNNNN\n");
    spit(day + "/004312Z_EA02.json", SAMPLE_SIDECAR);

    rx_apply_description("{\"receiver\":{\"gps\":{\"lat\":56.0403,\"lon\":-3.3554}}}");
    g_ident.start(dir, nullptr);
    for (int i = 0; i < 500 && !g_ident.replay_done(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(g_ident.replay_done(), "replay completes");
    check(contains(slurp(day + "/004312Z_EA02.json"), "\"ident\": {\"v\":"),
          "replay adds the ident member to the sidecar");
    std::string h = history_list_json(dir);
    check(contains(h, "\"st\":{\"key\":\"518:E:"), "history carries the summary");

    /* A message saved but not yet seen by the worker is pending. */
    spit(day + "/005000Z_EB03.txt", "ZCZC EB03\nTEST\nNNNN\n");
    std::string side = SAMPLE_SIDECAR;
    side.replace(side.find("\"subject\": \"A\""), 14, "\"subject\": \"B\"");
    spit(day + "/005000Z_EB03.json", side);
    h = history_list_json(dir);
    check(contains(h, "005000Z_EB03.txt") && contains(h, "\"st\":null,\"st_pending\":true"),
          "an unindexed message is pending");

    /* ...until its completion is processed. */
    {
        std::lock_guard<std::mutex> lk(g_latest_messages_mu);
        LatestMessage lm; lm.station = 'E'; lm.subject = 'B'; lm.seq = 4242;
        g_latest_messages["518 kHz:E:B"] = lm;
    }
    IdentJob job;
    job.kind = IdentJobKind::Final;
    job.channel = 0;
    job.rel = "518kHz/2026/08/18/005000Z_EB03.txt";
    job.msg_id = "EB03";
    job.q = niton_record().q;
    job.q.b2 = 'B';
    job.latest_key = "518 kHz:E:B";
    job.latest_seq = 4242;
    job.mqtt = sample_event();
    ident_submit(std::move(job));
    bool done = false;
    for (int i = 0; i < 300 && !done; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::lock_guard<std::mutex> lk(g_latest_messages_mu);
        done = !g_latest_messages["518 kHz:E:B"].ident_json.empty();
    }
    check(done, "a live completion reaches the latest store");
    check(contains(latest_messages_json(), "\"ident\":{\"v\":"), "/api/latest carries it");
    check(contains(slurp(day + "/005000Z_EB03.json"), "\"ident\": {"), "and the sidecar");
    std::string ij;
    check(ident_lookup_json(dir, "518kHz/2026/08/18/005000Z_EB03.txt", ij) == 200 &&
          contains(ij, "\"key\":\"518:E:"), "/api/ident serves it from the index");
    check(!contains(history_list_json(dir), "st_pending"), "nothing is pending any more");
    check(contains(g_ident.frame_for_new_client(0, false, ""), "\"msg_id\":\"EB03\""),
          "the final frame is kept for a browser that connects later");

    IdentJob prov;
    prov.kind = IdentJobKind::Provisional;
    prov.channel = 1;
    prov.msg_id = "EA07";
    prov.q = niton_record().q;
    prov.q.duration_s = 0;
    ident_submit(std::move(prov));
    std::string pf;
    for (int i = 0; i < 300 && pf.empty(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        pf = g_ident.frame_for_new_client(1, true, "EA07");
    }
    check(contains(pf, "\"provisional\":true"), "a provisional answer is produced");
    check(g_ident.frame_for_new_client(1, true, "EA08").empty(),
          "and is not replayed for a different message");
    check(contains(stations_json(), "\"replay\":{\"done\":true"), "/api/stations reports the replay");

    /* /api/analytics: both messages, as columns of equal length, oldest
     * first, the station by dictionary index. */
    {
        std::string a = analytics_json(false, 0, false, 0);
        check(contains(a, "\"n\":2,\"pending\":0"), "analytics lists both identified messages");
        check(contains(a, "\"stations\":[{\"key\":\"518:E:"), "analytics station dictionary");
        check(contains(a, "\"s\":[0,0]") && contains(a, "\"f\":[518,518]") &&
              contains(a, "\"b1\":[\"E\",\"E\"]"), "analytics columns reference the dictionary");
        check(contains(a, "\"receiver\":{\"valid\":true,\"lat\":56.0403"),
              "analytics carries the receiver, as /api/stations does");
        check(a.find(' ', a.find("\"cols\"")) == std::string::npos,
              "analytics columns have no whitespace");

        /* from/to are inclusive bounds on start_utc. */
        const int64_t t0 = niton_record().q.start_utc;
        check(contains(analytics_json(true, t0 + 1000000, false, 0), "\"n\":0,"),
              "analytics from= past every message is empty");
        check(contains(analytics_json(true, t0 + 1000000, false, 0), "\"cols\":{\"t\":[],"),
              "and still has every column");

        /* An entry the worker has not reached is counted, not listed. */
        {
            std::unique_lock<std::shared_mutex> lk(g_index_mu);
            HistoryIndexEntry e;
            e.q.start_utc = t0;
            g_index["518kHz/2026/08/18/009999Z_EA09.txt"] = e;
        }
        check(contains(analytics_json(false, 0, false, 0), "\"n\":2,\"pending\":1"),
              "analytics counts pending messages separately");
        index_forget("518kHz/2026/08/18/009999Z_EA09.txt");
    }

    index_forget("518kHz/2026/08/18/005000Z_EB03.txt");
    for (const char *f : { "/004312Z_EA02.txt", "/004312Z_EA02.json",
                           "/005000Z_EB03.txt", "/005000Z_EB03.json" })
        remove((day + f).c_str());
    rmdir(day.c_str());
    rmdir((dir + "/518kHz/2026/08").c_str());
    rmdir((dir + "/518kHz/2026").c_str());
    rmdir((dir + "/518kHz").c_str());
    rmdir(dir.c_str());
}

int main()
{
    printf("navtex MQTT self-test\n\n");
    test_derive_base();
    test_sanitiser();
    test_message_json();
    test_message_json_escapes_hostile_text();
    test_subject_names();
    test_json_find_bool();
    test_snr_scale();
    test_sidecar_scale();
    test_page_snr_constants();
    test_ident_json();
    test_ident_mqtt();
    test_rx_description();
    test_sidecar_ident();
    test_analytics_format();
    test_input_cleaning();
    test_purge();
    test_hardening();
    test_ident_worker();

    printf("\n%s\n", g_failures == 0 ? "all checks passed"
                                     : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
