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

int main()
{
    printf("navtex MQTT self-test\n\n");
    test_derive_base();
    test_sanitiser();
    test_message_json();
    test_message_json_escapes_hostile_text();
    test_subject_names();
    test_json_find_bool();

    printf("\n%s\n", g_failures == 0 ? "all checks passed"
                                     : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
