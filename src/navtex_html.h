/* -*- c++ -*- */
/*
 * navtex_html.h — embedded HTML/CSS/JS for the dual-frequency NAVTEX web UI.
 * Generated as a C++ inline function; included by navtex_rx_from_ubersdr.cpp.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "navtex_snr.h"

/* Forward declaration — ChannelContext is defined in the including TU */
struct ChannelContext;

/* Provided by the including TU (navtex_rx_from_ubersdr.cpp). */
static std::string json_escape(const std::string &s);
static const char *navtex_subject_name(char subject);

/* A double-quoted JavaScript string literal for text that is spliced
 * into an inline <script>.  json_escape() handles quotes, backslashes
 * and control characters; '<' is escaped as well so that no value —
 * an operator's tile URL, a proxy's X-Forwarded-Prefix header — can
 * close the script element early with "</script>". */
static std::string navtex_js_string(const std::string &s)
{
    std::string e = json_escape(s), out;
    out.reserve(e.size() + 8);
    for (char c : e) {
        if (c == '<') out += "\\u003c";
        else          out += c;
    }
    return "\"" + out + "\"";
}

static std::string make_html_page(const std::string &sdr_url,
                                  const std::vector<ChannelContext> &channels,
                                  const std::string &base_path = "",
                                  bool logging_enabled = false)

{
    /* ---- Channel freq-label JS array (for per-channel history button) ----
     * The history API derives the "freq" field from the on-disk directory name,
     * which is the label with spaces stripped (e.g. "518 kHz" -> "518kHz").
     * We must use the same sanitised form so the dropdown value matches. */
    std::string ch_labels_js = "  const CH_FREQ_LABELS = [";
    for (size_t i = 0; i < channels.size(); i++) {
        if (i) ch_labels_js += ", ";
        std::string freq_dir = channels[i].label;
        freq_dir.erase(std::remove(freq_dir.begin(), freq_dir.end(), ' '), freq_dir.end());
        ch_labels_js += "\"" + freq_dir + "\"";
    }
    ch_labels_js += "];\n";

    /* ---- SNR scale JS, generated from navtex_snr.h ----
     * Emitted rather than written into the page as literals so the page and
     * the C++ that classifies stored records cannot disagree.  See
     * navtex_snr.h for how these numbers were measured. */
    std::string snr_js;
    {
        char b[512];
        snprintf(b, sizeof(b),
                 "  const SNR_GOOD_DB     = %.2f;\n"
                 "  const SNR_WARN_DB     = %.2f;\n"
                 "  const SNR_BAR_MIN_DB  = %.2f;\n"
                 "  const SNR_BAR_SPAN_DB = %.2f;\n",
                 navtex_snr_good_db(), navtex_snr_warn_db(),
                 navtex_snr_bar_min_db(), NAVTEX_SNR_BAR_SPAN_DB);
        snr_js = b;
    }

    /* ---- Subject names, generated from navtex_subject_name() ----
     * The page used to carry its own SUBJECT table, which drifted from the
     * server's (it had no V/W/Y, so 490 kHz special-service messages
     * showed a bare letter while MQTT said "Special service").  Emitting
     * the JS map and the History subject filter from the one C++ function
     * means the two can no longer disagree. */
    std::string subject_js = "  const SUBJECT = {";
    std::string subject_opts;
    {
        bool first = true;
        for (char c = 'A'; c <= 'Z'; c++) {
            const char *nm = navtex_subject_name(c);
            if (!nm || !*nm) continue;
            subject_js += std::string(first ? "\n    " : ",\n    ") + c + ": " + navtex_js_string(nm);
            first = false;
            subject_opts += std::string("        <option value=\"") + c + "\">" + c + " &mdash; " + nm + "</option>\n";
        }
        subject_js += "\n  };\n";
    }

    /* ---- Tab buttons (individual channels) ---- */
    std::string tab_buttons;
    for (size_t i = 0; i < channels.size(); i++) {
        char btn[512];
        snprintf(btn, sizeof(btn),
            "    <button class=\"tab-btn%s\" id=\"tab-btn-%zu\" onclick=\"switchTab(%zu)\">"
            "<span class=\"tab-dot\" id=\"tab-dot-%zu\"></span>"
            "%s <span class=\"tab-name\">%s</span></button>\n",
            (false) ? " active" : "",  /* default tab is 'both'; set via switchTab() on load */
            i, i, i,
            channels[i].label.c_str(),
            channels[i].name.c_str());
        tab_buttons += btn;
    }
    /* "Both" tab */
    tab_buttons +=
        "    <button class=\"tab-btn tab-btn-both\" id=\"tab-btn-both\" onclick=\"switchTab('both')\">"
        "&#x25A6; <span class=\"tab-name\">Both</span></button>\n";
    /* "Latest" tab — pushed to far right via margin-left:auto */
    tab_buttons +=
        "    <button class=\"tab-btn tab-btn-latest\" id=\"tab-btn-latest\" onclick=\"switchTab('latest')\">"
        "&#x1F4CB; <span class=\"tab-name\">Latest</span></button>\n";
    /* "Stations" tab — the identified transmitters on a map and in a table.
     * Sits beside Latest on the right; both are views over received
     * traffic rather than live channels. */
    tab_buttons +=
        "    <button class=\"tab-btn tab-btn-stations\" id=\"tab-btn-stations\" onclick=\"switchTab('stations')\">"
        "&#x1F310; <span class=\"tab-name\">Stations</span></button>\n";

    /* ---- Per-channel panels ---- */
    std::string panels;
    for (size_t i = 0; i < channels.size(); i++) {
        std::string s    = std::to_string(i);
        std::string disp = "none";

        panels +=
            "  <div class=\"tab-panel\" id=\"panel-" + s + "\" style=\"display:" + disp + "\">\n"
            "    <div class=\"stats-bar\">\n"
            "      <div class=\"stat\"><span class=\"stat-label\">Signal (dBFS)</span>"
                   "<span class=\"stat-value\" id=\"bb-val-" + s + "\">&mdash;</span></div>\n"
            "      <div class=\"stat\"><span class=\"stat-label\">Noise (dBFS)</span>"
                   "<span class=\"stat-value\" id=\"nd-val-" + s + "\">&mdash;</span></div>\n"
            "      <div class=\"stat\"><span class=\"stat-label\">Sample Rate</span>"
                   "<span class=\"stat-value\" id=\"rate-val-" + s + "\">&mdash;</span></div>\n"
            "      <div class=\"stat\"><span class=\"stat-label\">Decoder</span>"
                   "<span class=\"stat-value\" id=\"dec-state-" + s + "\">&mdash;</span></div>\n"
            "      <div class=\"stat\"><span class=\"stat-label\">FEC Rate</span>"
                   "<span class=\"stat-value\" id=\"fec-val-" + s + "\">&mdash;</span></div>\n"
            "      <div class=\"stat\"><span class=\"stat-label\">Errors</span>"
                   "<span class=\"stat-value\" id=\"err-val-" + s + "\">&mdash;</span></div>\n"
            "      <div class=\"stat\"><span class=\"stat-label\">Decoding</span>"
                   "<span class=\"stat-value\">"
                   "<span class=\"act-dot idle\" id=\"act-dot-" + s + "\"></span>"
                   "<span id=\"act-text-" + s + "\">Idle</span></span></div>\n"
            "      <div class=\"stat\"><span class=\"stat-label\">SNR (dB)</span>"
                   "<span class=\"stat-value\" id=\"snr-val-" + s + "\">&mdash;</span></div>\n"
            "      <div class=\"bar-wrap\"><span class=\"stat-label\">Signal Level</span>"
                   "<div class=\"bar-track bar-track-sig\"><div class=\"bar-fill\" id=\"sig-fill-" + s + "\"></div></div></div>\n"
            "      <div class=\"bar-wrap\"><span class=\"stat-label\">SNR Level</span>"
                   "<div class=\"bar-track\"><div class=\"snr-fill\" id=\"snr-fill-" + s + "\"></div></div></div>\n"
            "      <div class=\"bar-wrap\"><span class=\"stat-label\">Quality (clean/fec/err)</span>"
                   "<div class=\"bar-track fec-track\">"
                   "<div class=\"fec-clean\" id=\"fec-clean-" + s + "\" style=\"width:0%\"></div>"
                   "<div class=\"fec-fec\"   id=\"fec-fec-"   + s + "\" style=\"width:0%\"></div>"
                   "<div class=\"fec-fail\"  id=\"fec-fail-"  + s + "\" style=\"width:0%\"></div>"
                   "</div></div>\n"
            + "      <div style=\"display:flex;gap:4px;align-items:center;margin-left:auto\">\n"
            + (logging_enabled
                ? "        <button class=\"icon-btn\" id=\"hist-ch-btn-" + s + "\" title=\"History for this frequency\" onclick=\"openHistoryForChannel(" + s + ")\" aria-label=\"History\">&#x1F4DC;</button>\n"
                : "")
            + "        <button class=\"audio-btn\" id=\"audio-btn-" + s + "\" data-ch=\"" + s + "\">"
                   "&#x1F50A;</button>\n"
            + "      </div>\n"
            "    </div>\n"
            "    <div class=\"msg-bar idle\" id=\"msg-bar-" + s + "\">\n"
            "      <div class=\"stat\"><span class=\"stat-label\">Message</span>"
                   "<span class=\"stat-value\">"
                   "<span class=\"msg-dot\" id=\"msg-dot-" + s + "\"></span>"
                   "<span id=\"msg-status-" + s + "\">Idle</span></span></div>\n"
            "      <div class=\"stat\"><span class=\"stat-label\">Station</span>"
                   "<span class=\"stat-value\" id=\"msg-station-" + s + "\">&mdash;</span></div>\n"
            "      <div class=\"stat\"><span class=\"stat-label\">Subject</span>"
                   "<span class=\"stat-value\" id=\"msg-subject-" + s + "\">&mdash;</span></div>\n"
            "      <div class=\"stat\"><span class=\"stat-label\">Serial</span>"
                   "<span class=\"stat-value\" id=\"msg-serial-" + s + "\">&mdash;</span></div>\n"
            "      <div class=\"output-actions\" style=\"margin-left:auto;display:flex;gap:5px;align-items:center\">\n"
            "        <button class=\"icon-btn\" id=\"clear-btn-" + s + "\" title=\"Clear output\" onclick=\"clearOutput(" + s + ")\" aria-label=\"Clear\">&#x1F5D1;</button>\n"
            "        <button class=\"icon-btn\" id=\"copy-btn-" + s + "\" title=\"Copy to clipboard\" onclick=\"copyOutput(" + s + ")\" aria-label=\"Copy\">&#x1F4CB;</button>\n"
            "        <button class=\"icon-btn\" id=\"dl-btn-" + s + "\" title=\"Download as text file\" onclick=\"downloadOutput(" + s + ")\" aria-label=\"Download\">&#x2B07;</button>\n"
            "      </div>\n"
            "    </div>\n"
            "    <div class=\"output\" id=\"output-" + s + "\"><span class=\"dim\">Waiting for signal&hellip;</span></div>\n"
            "  </div>\n";
    }

    /* ---- Assemble full page ---- */
    std::string html = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>NAVTEX Decoder</title>
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  font-family: 'Courier New', Courier, monospace;
  background: #1a1a2e;
  color: #e0e0e0;
  display: flex;
  flex-direction: column;
  height: 100vh;
  padding: 12px;
  gap: 8px;
  overflow: hidden;
}
header {
  background: #16213e;
  border: 1px solid #0f3460;
  border-radius: 6px;
  padding: 8px 14px;
  display: flex;
  align-items: center;
  gap: 18px;
  flex-wrap: wrap;
  flex-shrink: 0;
}
header h1 { font-size: 1.05rem; color: #e94560; letter-spacing: 2px; text-transform: uppercase; }
.info-item { display: flex; flex-direction: column; gap: 2px; }
.info-label { font-size: 0.6rem; color: #888; text-transform: uppercase; letter-spacing: 1px; }
.info-value { font-size: 0.85rem; color: #53d8fb; }
#status-dot {
  width: 9px; height: 9px; border-radius: 50%; background: #555;
  display: inline-block; margin-right: 5px; transition: background 0.3s;
}
#status-dot.connected    { background: #4caf50; }
#status-dot.disconnected { background: #e94560; }

/* ---- Tab bar ---- */
.tab-bar {
  display: flex;
  gap: 4px;
  flex-shrink: 0;
  align-items: flex-end;
}
.tab-btn-both {
  border-color: #2a4a6a;
  color: #7ab8d8;
}
.tab-btn-both:hover { color: #53d8fb; }
.tab-btn-both.active { color: #53d8fb; }
.tab-btn-latest {
  margin-left: auto;  /* push to far right */
  border-color: #2a4a6a;
  color: #a0c8a0;
}
.tab-btn-latest:hover { color: #4caf50; }
.tab-btn-latest.active { color: #4caf50; }
.tab-btn {
  background: #16213e;
  border: 1px solid #0f3460;
  border-bottom: none;
  border-radius: 6px 6px 0 0;
  color: #888;
  cursor: pointer;
  font-family: inherit;
  font-size: 0.85rem;
  padding: 7px 18px;
  display: flex;
  align-items: center;
  gap: 7px;
  transition: background 0.2s, color 0.2s;
}
.tab-btn:hover { background: #1e2d50; color: #ccc; }
.tab-btn.active {
  background: #0d0d1a;
  border-color: #0f3460;
  color: #53d8fb;
}
.tab-name { font-size: 0.72rem; color: inherit; opacity: 0.75; }
.tab-dot {
  width: 8px; height: 8px; border-radius: 50%; background: #444;
  display: inline-block; transition: background 0.4s;
}
.tab-dot.locked   { background: #4caf50; box-shadow: 0 0 5px #4caf50; }
.tab-dot.syncing  { background: #ffeb3b; box-shadow: 0 0 5px #ffeb3b; }
.tab-dot.searching{ background: #555; }

/* ---- Tab content area ---- */
.tab-content {
  flex: 1;
  background: #0d0d1a;
  border: 1px solid #0f3460;
  border-radius: 0 6px 6px 6px;
  display: flex;
  flex-direction: column;
  gap: 0;
  overflow: hidden;
  min-height: 0;
}
.tab-panel {
  flex-direction: column;
  flex: 1;
  gap: 0;
  min-height: 0;
  overflow: hidden;
}

/* ---- Stats bar ---- */
.stats-bar {
  background: #16213e;
  border-bottom: 1px solid #0f3460;
  padding: 7px 14px;
  display: flex;
  align-items: center;
  gap: 16px;
  flex-wrap: wrap;
  font-size: 0.8rem;
  flex-shrink: 0;
}
.stat { display: flex; flex-direction: column; gap: 2px; min-width: 70px; }
.stat-label { font-size: 0.58rem; color: #888; text-transform: uppercase; letter-spacing: 1px; }
.stat-value { color: #53d8fb; }
.stat-value.good { color: #4caf50; }
.stat-value.warn { color: #ffeb3b; }
.stat-value.bad  { color: #e94560; }
.stat-value.dim  { color: #555; }
.act-dot {
  width: 8px; height: 8px; border-radius: 50%; background: #333;
  display: inline-block; margin-right: 4px; transition: background 0.5s;
}
.act-dot.active { background: #4caf50; box-shadow: 0 0 5px #4caf50; }
.act-dot.idle   { background: #ffeb3b; box-shadow: 0 0 4px #ffeb3b; }
.bar-wrap { flex: 1; min-width: 100px; display: flex; flex-direction: column; gap: 3px; }
.bar-track {
  height: 7px; background: #0d0d1a; border-radius: 4px; overflow: hidden;
  border: 1px solid #0f3460;
}
.fec-track { display: flex; }
.bar-track-sig {
  background: linear-gradient(90deg, #e94560, #ffeb3b, #4caf50, #1a6b1a);
}
.bar-fill {
  height: 100%; width: 100%;
  background: #0d0d1a;
  border-radius: 4px;
  transition: margin-left 0.3s;
}
.snr-fill {
  height: 100%; width: 0%; border-radius: 4px;
  transition: width 0.3s, background 0.3s; background: #4caf50;
}
.fec-clean { height: 100%; background: #4caf50; transition: width 0.3s; }
.fec-fec   { height: 100%; background: #ffeb3b; transition: width 0.3s; }
.fec-fail  { height: 100%; background: #e94560; transition: width 0.3s; }
.audio-btn {
  background: #0f3460; border: 1px solid #1a6b8a; color: #53d8fb;
  border-radius: 4px; padding: 4px 9px; cursor: pointer;
  font-family: inherit; font-size: 0.75rem; letter-spacing: 0.5px;
  transition: background 0.2s, color 0.2s; white-space: nowrap;
}
.audio-btn:hover { background: #1a6b8a; }
.audio-btn.active { background: #1a6b1a; border-color: #4caf50; color: #4caf50; }

/* ---- Message bar ---- */
.msg-bar {
  background: #16213e;
  border-bottom: 1px solid #0f3460;
  padding: 6px 14px;
  display: flex;
  align-items: flex-start;
  gap: 16px;
  flex-wrap: wrap;
  font-size: 0.8rem;
  transition: opacity 0.4s;
  flex-shrink: 0;
}
.msg-bar.idle { opacity: 0.4; }
.msg-bar .stat { min-width: 55px; }
/* One row, always.  The Station stat (flag + name + letter + pill) and the
 * Subject are the only stats whose width varies, so they are the ones that
 * give way — ellipsis on the name/subject — rather than pushing the
 * clear/copy/download buttons onto a second row, which made the bar jump
 * from ~40 to ~80 px mid-message and put the two "Both" panels out of line. */
.msg-bar { flex-wrap: nowrap; gap: 14px; }
.msg-bar .stat { flex-shrink: 0; white-space: nowrap; }
.msg-bar .stat:nth-child(2), .msg-bar .stat:nth-child(3) { flex: 0 1 auto; min-width: 0; overflow: hidden; }
.msg-bar .stat:nth-child(2) .stat-value, .msg-bar .stat:nth-child(3) .stat-value {
  display: block; overflow: hidden; white-space: nowrap; text-overflow: ellipsis;
}
.msg-bar .stat:nth-child(2) .stchip { display: flex; max-width: 100%; }
.msg-bar .stchip .st-name { min-width: 0; }
.msg-bar .output-actions { flex-shrink: 0; }
.msg-dot {
  width: 8px; height: 8px; border-radius: 50%; background: #333;
  display: inline-block; margin-right: 4px; transition: background 0.3s;
}
.msg-dot.receiving { background: #ffeb3b; box-shadow: 0 0 5px #ffeb3b; }
.msg-dot.complete  { background: #4caf50; box-shadow: 0 0 5px #4caf50; }

/* ---- Message start/end dividers in output ---- */
.msg-divider {
  display: flex;
  align-items: center;
  gap: 8px;
  margin: 6px 0 2px;
  font-size: 0.7rem;
  font-family: monospace;
  user-select: none;
  pointer-events: none;
}
.msg-divider::before,
.msg-divider::after {
  content: '';
  flex: 1;
  height: 1px;
}
.msg-divider.msg-start {
  color: #53d8fb;
}
.msg-divider.msg-start::before,
.msg-divider.msg-start::after {
  background: #1a4a6a;
}
.msg-divider.msg-end {
  color: #4caf50;
  margin-top: 2px;
  margin-bottom: 6px;
}
.msg-divider.msg-end::before,
.msg-divider.msg-end::after {
  background: #1a4a2a;
}

/* ---- Output area ---- */
.output {
  flex: 1;
  padding: 12px 14px;
  overflow-y: auto;
  white-space: pre-wrap;
  word-break: break-all;
  font-size: 0.92rem;
  line-height: 1.5;
  color: #b0ffb0;
  min-height: 0;
}
.output .dim { color: #555; }

/* ---- Split (Both) panel ---- */
#panel-both {
  flex-direction: row !important;
  gap: 6px;
}
.split-col {
  flex: 1;
  display: flex;
  flex-direction: column;
  min-width: 0;
  min-height: 0;
  overflow: hidden;
}
.split-col-header {
  background: #16213e;
  border: 1px solid #0f3460;
  border-bottom: none;
  border-radius: 6px 6px 0 0;
  padding: 5px 12px;
  font-size: 0.78rem;
  color: #53d8fb;
  display: flex;
  align-items: center;
  gap: 8px;
  flex-shrink: 0;
}
.split-col-body {
  flex: 1;
  display: flex;
  flex-direction: column;
  border: 1px solid #0f3460;
  border-radius: 0 0 6px 6px;
  overflow: hidden;
  min-height: 0;
}
/* Inside split columns, stats/msg/output inherit normal styles */
.split-col-body .stats-bar  { border-radius: 0; border-left: none; border-right: none; border-top: none; }
.split-col-body .msg-bar    { border-radius: 0; border-left: none; border-right: none; border-top: none; }
.split-col-body .output     { border-radius: 0; }

/* ---- Timestamp label in output ---- */
.ts-stamp {
  color: #4a6a8a;
  font-size: 0.78rem;
  margin-right: 6px;
  user-select: none;
  flex-shrink: 0;
}
.output-line {
  display: flex;
  align-items: baseline;
  white-space: pre-wrap;
  word-break: break-all;
}

/* ---- Output action icon buttons ---- */
.icon-btn {
  background: #16213e;
  border: 1px solid #2a4a7f;
  border-radius: 4px;
  color: #a0b8d8;
  cursor: pointer;
  font-size: 0.95rem;
  line-height: 1;
  padding: 3px 6px;
  transition: color 0.15s, border-color 0.15s, background 0.15s;
}
.icon-btn:hover {
  background: #1e3a6e;
  border-color: #53d8fb;
  color: #53d8fb;
}
.icon-btn:active {
  background: #0f3460;
  color: #ffffff;
}

/* ---- History button ---- */
.history-btn {
  background: #16213e;
  border: 1px solid #2a4a7f;
  border-radius: 4px;
  color: #a0b8d8;
  cursor: pointer;
  font-size: 0.78rem;
  padding: 4px 10px;
  transition: color 0.15s, border-color 0.15s, background 0.15s;
  white-space: nowrap;
}
.history-btn:hover { background: #1e3a6e; border-color: #53d8fb; color: #53d8fb; }

/* ---- History modal overlay ---- */
.hist-overlay {
  position: fixed; inset: 0;
  background: rgba(0,0,0,0.72);
  z-index: 1000;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 16px;
  box-sizing: border-box;
}
.hist-dialog {
  background: #0d1b2a;
  border: 1px solid #1a3a5c;
  border-radius: 8px;
  display: flex;
  flex-direction: column;
  width: 100%;
  max-width: 1060px;
  max-height: 85vh;
  box-shadow: 0 8px 40px rgba(0,0,0,0.7);
  overflow: hidden;
}
.hist-dialog-msg { max-width: 900px; }
.hist-header {
  display: flex;
  align-items: center;
  padding: 14px 18px 12px;
  border-bottom: 1px solid #1a3a5c;
  flex-shrink: 0;
  gap: 8px;
}
.hist-title { font-size: 1rem; font-weight: 600; color: #c8dff0; letter-spacing: 0.02em; }
.hist-title-group { display: flex; align-items: baseline; gap: 8px; }
.hist-close {
  background: transparent; border: none; color: #7d8aa3; cursor: pointer;
  font-size: 1.4rem; line-height: 1; padding: 0 4px;
  transition: color 0.15s;
}
.hist-close:hover { color: #e94560; }
.hist-toolbar {
  display: flex; gap: 10px; padding: 10px 18px;
  border-bottom: 1px solid #1a3a5c; flex-shrink: 0;
}
.hist-search-input {
  flex: 1; background: #16213e; border: 1px solid #2a4a7f;
  border-radius: 4px; color: #c8dff0; font-size: 0.82rem;
  padding: 5px 10px; outline: none;
}
.hist-search-input:focus { border-color: #53d8fb; }
.hist-freq-select {
  background: #16213e; border: 1px solid #2a4a7f;
  border-radius: 4px; color: #c8dff0; font-size: 0.82rem;
  padding: 5px 8px; cursor: pointer; outline: none;
}
.hist-list-wrap { overflow-y: auto; flex: 1; }
.hist-table {
  width: 100%; border-collapse: collapse; font-size: 0.82rem;
}
.hist-table thead th {
  background: #0a1628; color: #7d8aa3; font-weight: 600;
  padding: 8px 14px; text-align: left; position: sticky; top: 0;
  border-bottom: 1px solid #1a3a5c; white-space: nowrap;
}
.hist-table tbody tr {
  border-bottom: 1px solid #111e30;
  transition: background 0.1s;
  cursor: pointer;
}
.hist-table tbody tr:hover { background: #16213e; }
.hist-table tbody td { padding: 8px 14px; color: #a0b8d8; vertical-align: middle; }
.hist-table tbody td.hist-freq { color: #53d8fb; font-family: monospace; }
.hist-table tbody td.hist-id   { color: #c8dff0; font-family: monospace; font-weight: 600; }
.hist-table tbody td.hist-date { color: #778; }
.hist-table tbody td.hist-time { color: #778; font-family: monospace; }
.hist-table tbody td.hist-snr  { font-family: monospace; font-size: 0.8rem; }
.hist-table tbody td.hist-snr.good { color: #4caf50; }
.hist-table tbody td.hist-snr.warn { color: #ff9800; }
.hist-table tbody td.hist-snr.bad  { color: #f44336; }
.hist-table tbody td.hist-snr.dim  { color: #446; }
.hist-table tbody td.hist-dur  { font-family: monospace; font-size: 0.8rem; color: #6a8aaa; }
.hist-pagination {
  display: flex; align-items: center; justify-content: center;
  gap: 8px; padding: 10px 0 4px; font-size: 0.82rem; color: #7d8aa3;
}
.hist-pagination button {
  background: #16213e; border: 1px solid #2a4a7f; border-radius: 3px;
  color: #a0b8d8; padding: 3px 10px; cursor: pointer; font-size: 0.8rem;
}
.hist-pagination button:hover:not(:disabled) { border-color: #53d8fb; color: #53d8fb; }
.hist-pagination button:disabled { opacity: 0.35; cursor: default; }
.hist-pagination .hist-page-info { color: #7d8aa3; min-width: 80px; text-align: center; }
.hist-view-btn {
  background: #16213e; border: 1px solid #2a4a7f; border-radius: 3px;
  color: #a0b8d8; cursor: pointer; font-size: 0.75rem; padding: 3px 8px;
  transition: color 0.15s, border-color 0.15s;
}
.hist-view-btn:hover { border-color: #53d8fb; color: #53d8fb; }
.hist-empty { padding: 32px; text-align: center; color: #446; font-size: 0.85rem; }
.msg-modal-body {
  flex: 1; overflow-y: auto; margin: 0;
  padding: 16px 20px;
  font-family: 'Courier New', monospace; font-size: 0.82rem;
  color: #c8dff0; line-height: 1.6;
  white-space: pre-wrap; word-break: break-word;
  background: #0a1628;
}

/* ---- Message metrics summary panel ---- */
.msg-metrics-panel {
  background: #0d1f35;
  border-bottom: 1px solid #1a3a5c;
  padding: 12px 20px;
  display: flex;
  flex-wrap: wrap;
  gap: 14px 24px;
  flex-shrink: 0;
}
.msg-metrics-panel.hidden { display: none; }
.mm-item {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 90px;
}
.mm-label {
  font-size: 0.58rem;
  color: #7d8aa3;
  text-transform: uppercase;
  letter-spacing: 1px;
  font-family: 'Courier New', monospace;
}
.mm-value {
  font-size: 0.82rem;
  color: #53d8fb;
  font-family: 'Courier New', monospace;
}
.mm-value.good  { color: #4caf50; }
.mm-value.warn  { color: #ffeb3b; }
.mm-value.bad   { color: #e94560; }
.mm-value.dim   { color: #446; }
.mm-fec-bar {
  display: flex;
  height: 5px;
  border-radius: 3px;
  overflow: hidden;
  width: 100%;
  min-width: 120px;
  background: #0d0d1a;
  border: 1px solid #1a3a5c;
  margin-top: 2px;
}
.mm-fec-clean { background: #4caf50; height: 100%; }
.mm-fec-fec   { background: #ffeb3b; height: 100%; }
.mm-fec-fail  { background: #e94560; height: 100%; }

/* ---- Metrics modal ---- */
.metrics-dialog {
  background: #0d1b2a;
  border: 1px solid #1a3a5c;
  border-radius: 8px;
  display: flex;
  flex-direction: column;
  width: 100%;
  max-width: 1200px;
  max-height: 85vh;
  box-shadow: 0 8px 40px rgba(0,0,0,0.7);
  overflow: hidden;
}
.metrics-body {
  flex: 1;
  overflow-y: auto;
  padding: 18px 20px;
  display: flex;
  flex-direction: column;
  gap: 28px;
}
.metrics-section-title {
  font-size: 0.72rem;
  color: #7d8aa3;
  text-transform: uppercase;
  letter-spacing: 1px;
  margin-bottom: 10px;
}
/* Chart wrapper: Y-axis on left, bars+labels on right */
.metrics-chart-wrap {
  display: flex;
  flex-direction: row;
  gap: 6px;
}
.metrics-yaxis {
  display: flex;
  flex-direction: column;
  justify-content: space-between;
  align-items: flex-end;
  height: 110px;
  flex-shrink: 0;
  padding-bottom: 0;
}
.metrics-ytick {
  font-size: 0.55rem;
  color: #c8dff0;
  line-height: 1;
  white-space: nowrap;
}
.metrics-chart-inner {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
}
.metrics-bars-row {
  display: flex;
  align-items: flex-end;
  gap: 2px;
  height: 110px;
}
.metrics-labels-row {
  display: flex;
  gap: 2px;
  margin-top: 4px;
}
.metrics-col-bars {
  display: flex;
  align-items: flex-end;
  gap: 1px;
  flex: 1;
  min-width: 0;
}
.metrics-col-label {
  flex: 1;
  min-width: 0;
  overflow: hidden;
  text-align: center;
}
.metrics-bar-group {
  display: flex;
  align-items: flex-end;
  gap: 1px;
  width: 100%;
}
.metrics-bar {
  flex: 1;
  min-width: 3px;
  min-height: 1px;
  border-radius: 2px 2px 0 0;
}
.metrics-bar.freq-0 { background: #53d8fb; }
.metrics-bar.freq-1 { background: #ffeb3b; }
.metrics-bar.freq-2 { background: #d55181; }
.metrics-bar.freq-3 { background: #9085e9; }
.metrics-label {
  font-size: 0.55rem;
  color: #c8dff0;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  display: block;
  text-align: center;
}
.metrics-legend {
  display: flex;
  gap: 14px;
  flex-wrap: wrap;
  margin-bottom: 8px;
}
.metrics-legend-item {
  display: flex;
  align-items: center;
  gap: 5px;
  font-size: 0.72rem;
  color: #a0b8d8;
}
.metrics-legend-dot {
  width: 10px; height: 10px; border-radius: 2px;
}
.metrics-legend-dot.freq-0 { background: #53d8fb; }
.metrics-legend-dot.freq-1 { background: #ffeb3b; }
.metrics-legend-dot.freq-2 { background: #d55181; }
.metrics-legend-dot.freq-3 { background: #9085e9; }
.metrics-empty { padding: 32px; text-align: center; color: #446; font-size: 0.85rem; }

/* ---- Latest panel ---- */
#panel-latest {
  flex-direction: column;
  overflow-y: auto;
  padding: 14px 16px;
  gap: 20px;
}
.latest-toolbar {
  display: flex;
  align-items: center;
  gap: 10px;
  flex-shrink: 0;
  margin-bottom: 4px;
}
.latest-toolbar-title {
  font-size: 0.85rem;
  color: #53d8fb;
  letter-spacing: 1px;
  text-transform: uppercase;
}
.latest-refresh-btn {
  background: #16213e;
  border: 1px solid #2a4a7f;
  border-radius: 4px;
  color: #a0b8d8;
  cursor: pointer;
  font-size: 0.82rem;
  padding: 4px 10px;
  transition: color 0.15s, border-color 0.15s, background 0.15s;
  white-space: nowrap;
}
.latest-refresh-btn:hover { background: #1e3a6e; border-color: #53d8fb; color: #53d8fb; }
.latest-last-updated {
  font-size: 0.72rem;
  color: #446;
  margin-left: auto;
}
.latest-freq-section {
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.latest-freq-section + .latest-freq-section { margin-top: 18px; }
.latest-freq-title {
  font-size: 0.78rem;
  color: #53d8fb;
  text-transform: uppercase;
  letter-spacing: 1px;
  border-bottom: 1px solid #1a3a5c;
  padding-bottom: 4px;
}
.latest-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 0.82rem;
}
.latest-table thead th {
  background: #0a1628;
  color: #7d8aa3;
  font-weight: 600;
  padding: 7px 12px;
  text-align: left;
  border-bottom: 1px solid #1a3a5c;
  white-space: nowrap;
}
.latest-table tbody tr {
  border-bottom: 1px solid #111e30;
  transition: background 0.1s;
}
.latest-table tbody tr:hover { background: #16213e; }
.latest-table tbody td { padding: 7px 12px; color: #a0b8d8; vertical-align: middle; }
.latest-table tbody td.lt-station { color: #c8dff0; font-family: monospace; font-weight: 600; font-size: 1rem; }
.latest-table tbody td.lt-ident   { max-width: 280px; }
.latest-table tbody td.lt-ident .st-letter { font-weight: 600; color: #778; }
.latest-table tbody td.lt-subject { color: #a0b8d8; }
.latest-table tbody td.lt-serial  { font-family: monospace; color: #778; }
.latest-table tbody td.lt-ts      { font-family: monospace; color: #7d8aa3; font-size: 0.78rem; }
.latest-table tbody td.lt-snr.good { color: #4caf50; font-family: monospace; }
.latest-table tbody td.lt-snr.warn { color: #ff9800; font-family: monospace; }
.latest-table tbody td.lt-snr.bad  { color: #f44336; font-family: monospace; }
.latest-table tbody td.lt-snr.dim  { color: #446; font-family: monospace; }
.latest-empty { padding: 20px; text-align: center; color: #446; font-size: 0.82rem; }
.latest-view-btn {
  background: #16213e; border: 1px solid #2a4a7f; border-radius: 3px;
  color: #a0b8d8; cursor: pointer; font-size: 0.75rem; padding: 3px 8px;
  transition: color 0.15s, border-color 0.15s;
}
.latest-view-btn:hover { border-color: #53d8fb; color: #53d8fb; }

/* ==================================================================
 * Station identification.
 *
 * Everything below renders what the server's identifier concluded —
 * the page never works out a station or a fact of its own.  One chip
 * (flag + name + muted letter + confidence pill) is used everywhere a
 * station is named, so the live bar, Latest, History, the message
 * modal and the Stations view all read the same way.
 * ================================================================== */

/* Flags are 4:3 SVGs served by /flags/<iso2>.svg — never emoji, which
 * Windows does not render as flags.  The box-shadow is the "subtle
 * border": it sits outside the image so a white flag (Japan, Finland)
 * still has an edge against the dark background. */
.flag {
  width: 16px; height: 12px; border-radius: 2px; flex-shrink: 0;
  object-fit: cover; vertical-align: -1px; background: #0a1628;
  box-shadow: 0 0 0 1px rgba(160,184,216,0.22);
}
.flag-lg { width: 44px; height: 33px; border-radius: 3px; }
.flag-none { display: inline-block; background: #111e30; }
.stchip {
  display: inline-flex; align-items: center; gap: 6px;
  white-space: nowrap; max-width: 100%; vertical-align: middle;
}
.stchip .st-name   { color: #c8dff0; overflow: hidden; text-overflow: ellipsis; }
.stchip .st-letter { color: #7d8aa3; font-size: 0.85em; font-family: 'Courier New', monospace; }
.stchip.st-none .st-letter { color: #778; }
.stchip.prov .st-name { color: #8fb0cc; }
.stchip[data-facts] { cursor: help; }
.st-big-letter { color: #c8dff0; font-weight: 600; }
.st-pending {
  color: #6a8aaa; font-style: italic; font-size: 0.92em;
  display: inline-flex; align-items: center; gap: 5px;
}
.st-pending::before {
  content: ''; width: 6px; height: 6px; border-radius: 50%;
  background: #53d8fb; opacity: 0.6; animation: st-pulse 1.2s ease-in-out infinite;
}
@keyframes st-pulse { 0%,100% { opacity: 0.15; } 50% { opacity: 0.85; } }

/* Confidence pill.  High/Medium/Low/Unidentified map onto the page's
 * good / warn / bad / dim colours; Medium uses amber rather than the
 * stat yellow so it does not shout in a long table. */
.cpill {
  display: inline-block; font-size: 0.6rem; font-weight: 600;
  letter-spacing: 0.6px; text-transform: uppercase; line-height: 1.4;
  padding: 0 6px; border-radius: 8px; border: 1px solid; white-space: nowrap;
  font-family: 'Courier New', monospace;
}
.msg-bar .cpill { font-size: 0.64rem; }
.cpill[data-facts], .cpill[data-ident-rel] { cursor: help; }
.cp-high   { color: #4caf50; border-color: #2e6b31; background: rgba(76,175,80,0.10); }
.cp-medium { color: #ffc107; border-color: #6b5410; background: rgba(255,193,7,0.08); }
.cp-low    { color: #c98a92; border-color: #5a3a44; background: rgba(233,69,96,0.07); }
.cp-none   { color: #8592a8; border-color: #2a3346; background: transparent; }
.cpill.prov { border-style: dashed; opacity: 0.85; }

/* Facts popover — one shared element, positioned next to whatever
 * triggered it (hover on desktop, tap on touch). */
#ident-pop {
  position: fixed; z-index: 3000; display: none;
  max-width: min(340px, calc(100vw - 16px));
  background: #0d1b2a; border: 1px solid #2a4a7f; border-radius: 6px;
  box-shadow: 0 6px 24px rgba(0,0,0,0.6);
  padding: 9px 12px 10px; font-size: 0.74rem; color: #a0b8d8; line-height: 1.45;
}
#ident-pop .ip-title {
  display: flex; align-items: center; gap: 7px; margin-bottom: 6px;
  color: #c8dff0; font-weight: 600;
}
#ident-pop .ip-sub { color: #7d8aa3; font-size: 0.66rem; margin: -3px 0 6px; }
#ident-pop ul { list-style: none; display: flex; flex-direction: column; gap: 3px; }
#ident-pop li { padding-left: 11px; position: relative; }
#ident-pop li::before { content: '\203A'; position: absolute; left: 0; color: #53d8fb; }
#ident-pop .ip-empty { color: #7d8aa3; font-style: italic; }

/* ---- Header: replay progress and receiver-location badges ---- */
.ident-hdr { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
.ident-hdr:empty { display: none; }
.hdr-badge {
  display: inline-flex; align-items: center; gap: 7px;
  font-size: 0.7rem; padding: 3px 9px; border-radius: 10px; border: 1px solid;
  white-space: nowrap; user-select: none;
}
.hb-replay { color: #7ab8d8; border-color: #1a4a6a; background: rgba(83,216,251,0.06); }
.hb-replay .hb-bar {
  width: 70px; height: 4px; border-radius: 2px; background: #0d0d1a; overflow: hidden;
  border: 1px solid #1a3a5c;
}
.hb-replay .hb-bar i { display: block; height: 100%; background: #53d8fb; transition: width 0.6s; }
.hb-warn { color: #ffc107; border-color: #6b5410; background: rgba(255,193,7,0.07); cursor: help; }

/* ---- History: filters, custom dropdowns, sortable headers ---- */
.hist-dialog-wide { max-width: 1280px; }
.hist-search-input, .hist-freq-select { font-family: inherit; }
.hist-toolbar { flex-wrap: wrap; align-items: center; }
.hist-toolbar .hist-search-input { min-width: 180px; }
.hist-filter-row {
  display: flex; gap: 8px; padding: 0 18px 10px; flex-wrap: wrap; align-items: center;
  border-bottom: 1px solid #1a3a5c; flex-shrink: 0;
}
.hist-toolbar { border-bottom: none; padding-bottom: 8px; }
.dd { position: relative; }
.dd-btn {
  background: #16213e; border: 1px solid #2a4a7f; border-radius: 4px;
  color: #c8dff0; font-family: inherit; font-size: 0.82rem; padding: 5px 8px;
  cursor: pointer; display: inline-flex; align-items: center; gap: 7px;
  min-width: 150px; max-width: 260px; text-align: left;
}
.dd-btn:hover, .dd.open .dd-btn { border-color: #53d8fb; }
.dd-btn.dd-set { border-color: #1a6b8a; background: #13294a; }
.dd-btn .dd-label { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.dd-btn::after { content: '\25BE'; color: #7d8aa3; font-size: 0.7rem; }
.dd-panel {
  position: fixed; z-index: 2000; display: none;
  background: #0d1b2a; border: 1px solid #2a4a7f; border-radius: 6px;
  box-shadow: 0 8px 28px rgba(0,0,0,0.65);
  width: 300px; max-width: calc(100vw - 16px); max-height: 340px;
  flex-direction: column; overflow: hidden;
}
.dd.open .dd-panel { display: flex; }
.dd-search {
  margin: 8px; background: #16213e; border: 1px solid #2a4a7f; border-radius: 4px;
  color: #c8dff0; font-family: inherit; font-size: 0.8rem; padding: 5px 8px; outline: none;
}
.dd-search:focus { border-color: #53d8fb; }
.dd-list { overflow-y: auto; flex: 1; padding-bottom: 4px; }
.dd-item {
  display: flex; align-items: center; gap: 8px; padding: 6px 12px;
  font-size: 0.8rem; color: #a0b8d8; cursor: pointer; white-space: nowrap;
}
.dd-item:hover, .dd-item.kb { background: #16213e; color: #c8dff0; }
.dd-item.sel { color: #53d8fb; }
.dd-item .dd-name { overflow: hidden; text-overflow: ellipsis; }
.dd-item .dd-meta { color: #7d8aa3; font-size: 0.72rem; }
.dd-item .dd-count { margin-left: auto; color: #7d8aa3; font-size: 0.72rem; padding-left: 8px; }
.dd-item.dd-sub { padding-left: 34px; }
.dd-group {
  display: flex; align-items: center; gap: 7px; padding: 8px 12px 3px;
  font-size: 0.6rem; color: #7d8aa3; text-transform: uppercase; letter-spacing: 1px;
}
.dd-empty { padding: 14px; color: #446; font-size: 0.78rem; text-align: center; }
.hist-clear {
  background: transparent; border: 1px solid transparent; border-radius: 4px;
  color: #e94560; font-family: inherit; font-size: 0.78rem; padding: 5px 8px; cursor: pointer;
}
.hist-clear:hover { border-color: #5a2a3a; background: rgba(233,69,96,0.08); }
.hist-clear[hidden] { display: none; }
.hist-table thead th.sortable { cursor: pointer; user-select: none; }
.hist-table thead th.sortable:hover { color: #a0b8d8; }
.hist-table thead th.sorted { color: #53d8fb; }
.sort-ind { display: inline-block; width: 0.8em; margin-left: 3px; font-size: 0.7em; }
.hist-table tbody td.hist-station { max-width: 260px; }
/* Subject names now come from navtex_subject_name() and can be long
 * ("Additional navigational warning"): let that column wrap, and keep
 * the short fixed-format columns (date, time, SNR, duration) on one line. */
.hist-table tbody td:nth-child(1), .hist-table tbody td:nth-child(2),
.hist-table tbody td:nth-child(9), .hist-table tbody td:nth-child(10) { white-space: nowrap; }
.hist-table tbody td.hist-dist { font-family: monospace; font-size: 0.8rem; color: #a0b8d8; white-space: nowrap; }
.dist-brg { color: #7d8aa3; margin-left: 4px; }
.dist-night { color: #7a8ad8; margin-left: 4px; }
.hist-replay-note {
  padding: 6px 18px; font-size: 0.72rem; color: #7ab8d8; background: rgba(83,216,251,0.05);
  border-bottom: 1px solid #1a3a5c; flex-shrink: 0;
}
.hist-replay-note[hidden] { display: none; }

/* ---- Message modal: Identification panel ---- */
.msg-ident-panel {
  background: #0b1a2e; border-bottom: 1px solid #1a3a5c; padding: 14px 20px;
  display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 12px 28px;
  flex-shrink: 0; font-size: 0.8rem; color: #a0b8d8;
  max-height: 46vh; overflow-y: auto;
}
.msg-ident-panel.hidden { display: none; }
.mi-head { display: flex; gap: 12px; align-items: center; }
.mi-name { font-size: 1.05rem; color: #c8dff0; font-weight: 600; display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
.mi-sub  { color: #6a8aaa; font-size: 0.74rem; margin-top: 2px; }
.mi-flag-none {
  width: 44px; height: 33px; border-radius: 3px; border: 1px dashed #2a3346;
  display: flex; align-items: center; justify-content: center; color: #446; flex-shrink: 0;
}
.mi-conf { margin-top: 12px; }
.mi-conf-track {
  height: 6px; border-radius: 3px; background: #0d0d1a; border: 1px solid #1a3a5c; overflow: hidden;
}
.mi-conf-fill { height: 100%; border-radius: 3px; }
.mi-conf-lbl { display: flex; justify-content: space-between; font-size: 0.66rem; color: #7d8aa3; margin-top: 3px; text-transform: uppercase; letter-spacing: 1px; }
.mi-rows { display: flex; flex-direction: column; gap: 5px; margin-top: 12px; }
.mi-row { display: flex; gap: 8px; align-items: baseline; }
.mi-k { color: #7d8aa3; font-size: 0.6rem; text-transform: uppercase; letter-spacing: 1px; min-width: 74px; flex-shrink: 0; }
.mi-v { color: #c8dff0; }
.mi-agree    { color: #4caf50; }
.mi-disagree { color: #ffc107; }
.mi-sec { color: #7d8aa3; font-size: 0.6rem; text-transform: uppercase; letter-spacing: 1px; margin-bottom: 5px; }
.mi-facts { list-style: none; display: flex; flex-direction: column; gap: 4px; }
.mi-facts li { padding-left: 12px; position: relative; line-height: 1.4; }
.mi-facts li::before { content: '\203A'; position: absolute; left: 0; color: #53d8fb; }
.mi-alts { display: flex; flex-direction: column; gap: 4px; margin-top: 12px; }
.mi-alt { display: flex; align-items: center; gap: 7px; }
.mi-alt-bar { flex: 1; max-width: 120px; height: 4px; background: #0d0d1a; border-radius: 2px; overflow: hidden; border: 1px solid #1a3a5c; }
.mi-alt-bar i { display: block; height: 100%; background: #557; }
.mi-alt-p { color: #778; font-family: monospace; font-size: 0.74rem; min-width: 34px; text-align: right; }
.mi-warn {
  grid-column: 1 / -1; color: #ffc107; background: rgba(255,193,7,0.06);
  border: 1px solid #6b5410; border-radius: 4px; padding: 6px 10px; font-size: 0.76rem;
}
.mi-prov { color: #7ab8d8; font-size: 0.66rem; border: 1px dashed #1a4a6a; border-radius: 8px; padding: 0 6px; }
/* Per-message map: receiver, sender and the great-circle path between
 * them.  isolation keeps Leaflet's z-index 400+ panes inside the map, so
 * they never paint over the modal's sticky header. */
.mi-map-box { margin-top: 12px; }
.mi-map-box[hidden] { display: none; }
.mi-map {
  position: relative; isolation: isolate; z-index: 0;
  height: 170px; border: 1px solid #1a3a5c; border-radius: 6px; overflow: hidden; background: #0d0d1a;
}
.mi-map[hidden] { display: none; }
.mi-map-note { font-size: 0.7rem; color: #6a8aaa; margin-top: 5px; }
.mi-map-note:empty { display: none; }
.mi-sender-icon {
  width: 12px; height: 12px; border-radius: 50%; border: 2px solid #0d1b2a;
  box-shadow: 0 0 0 1.5px currentColor; background: currentColor;
}
.leaflet-tooltip.mi-path-lbl {
  background: rgba(13,27,42,0.9); color: #c8dff0; border-color: #2a4a7f; padding: 2px 6px;
  font-size: 0.68rem; white-space: nowrap;
}
.leaflet-tooltip.mi-path-lbl::before { display: none; }

/* ---- Stations tab ---- */
.tab-btn-stations { border-color: #2a4a6a; color: #c8b8e8; }
.tab-btn-stations:hover, .tab-btn-stations.active { color: #b39ddb; }
#panel-stations {
  flex-direction: column; overflow-y: auto; padding: 14px 16px; gap: 12px;
}
.stn-summary { font-size: 0.74rem; color: #6a8aaa; }
.stn-toggle {
  display: flex; align-items: center; gap: 6px; cursor: pointer;
  font-size: 0.76rem; color: #888; user-select: none;
}
.stn-toggle input { accent-color: #53d8fb; width: 14px; height: 14px; cursor: pointer; }
.stn-note {
  font-size: 0.76rem; color: #8a9ab0; padding: 7px 12px; border-radius: 4px;
  border: 1px solid #1a3a5c; background: #0b1a2e;
}
.stn-note[hidden] { display: none; }
/* isolation: Leaflet's controls sit at z-index 1000 — the same as the
 * History / Metrics overlays — so without a stacking context of its own the
 * +/- buttons and attribution painted over those modals (as .mi-map and
 * .mx-map already guard against). */
.stn-map-wrap {
  position: relative; isolation: isolate; z-index: 0; flex-shrink: 0;
  height: clamp(260px, 50vh, 560px);
  border: 1px solid #1a3a5c; border-radius: 6px; overflow: hidden; background: #0d0d1a;
}
.stn-map-wrap[hidden] { display: none; }
#stn-map { position: absolute; inset: 0; background: #0d0d1a; }
.stn-legend {
  position: absolute; left: 10px; bottom: 22px; z-index: 500;
  background: rgba(13,27,42,0.88); border: 1px solid #1a3a5c; border-radius: 5px;
  padding: 6px 9px; font-size: 0.66rem; color: #a0b8d8;
  display: flex; flex-direction: column; gap: 3px; pointer-events: none;
}
.stn-legend span { display: flex; align-items: center; gap: 6px; }
.stn-legend i { width: 9px; height: 9px; border-radius: 50%; display: inline-block; border: 1px solid rgba(255,255,255,0.35); }
.stn-legend .lg-rx { background: #e94560; border-color: #fff; }
.stn-table-wrap { overflow-x: auto; flex-shrink: 0; }
.stn-table thead th.sortable { cursor: pointer; user-select: none; }
.stn-table thead th.sortable:hover { color: #a0b8d8; }
.stn-table thead th.sorted { color: #53d8fb; }
.stn-table tbody tr { cursor: pointer; }
.stn-table tbody td { white-space: nowrap; }
.stn-table td.num { font-family: monospace; text-align: right; }
.stn-table th.num { text-align: right; }
.stn-table td.mono { font-family: monospace; color: #53d8fb; }
.stn-table tr.unheard td { color: #7d8aa3; }
.stn-table tr.lowonly td { opacity: 0.55; }
.stn-table tr.unheard .st-name { color: #6a7a8a; }
.stn-table tr.stn-sep td {
  cursor: default; color: #7d8aa3; font-size: 0.62rem; text-transform: uppercase;
  letter-spacing: 1px; padding-top: 14px; background: transparent;
}
.stn-table tr.stn-sep:hover { background: transparent; }
.rec-dot { display: inline-block; width: 7px; height: 7px; border-radius: 50%; margin-right: 6px; vertical-align: 0; }

/* Leaflet, recoloured to the page's dark theme. */
.leaflet-container { background: #0d0d1a; font-family: 'Courier New', Courier, monospace; }
.leaflet-popup-content-wrapper, .leaflet-popup-tip {
  background: #0d1b2a; color: #c8dff0; border: 1px solid #2a4a7f;
  box-shadow: 0 6px 24px rgba(0,0,0,0.6);
}
.leaflet-popup-content { margin: 10px 12px; font-size: 0.76rem; line-height: 1.45; }
.leaflet-container a.leaflet-popup-close-button { color: #7d8aa3; }
.leaflet-container a.leaflet-popup-close-button:hover { color: #e94560; }
.leaflet-control-attribution {
  background: rgba(13,13,26,0.78) !important; color: #6a7a8a !important; font-size: 0.6rem;
}
.leaflet-control-attribution a { color: #7ab8d8 !important; }
.leaflet-bar a, .leaflet-bar a:hover {
  background: #16213e; color: #a0b8d8; border-bottom-color: #2a4a7f;
}
.leaflet-bar a:hover { color: #53d8fb; }
.leaflet-bar { border: 1px solid #2a4a7f !important; }
.leaflet-tooltip {
  background: #0d1b2a; color: #c8dff0; border: 1px solid #2a4a7f; font-size: 0.7rem;
  box-shadow: none; font-family: 'Courier New', monospace;
}
.leaflet-tooltip-top::before { border-top-color: #2a4a7f; }
.pp-title { display: flex; align-items: center; gap: 7px; font-weight: 600; font-size: 0.82rem; margin-bottom: 2px; }
.pp-sub   { color: #6a8aaa; font-size: 0.7rem; margin-bottom: 6px; }
.pp-st    { border-top: 1px solid #1a3a5c; padding-top: 5px; margin-top: 5px; }
.pp-grid  { display: grid; grid-template-columns: auto 1fr; gap: 1px 10px; }
.pp-grid b { color: #7d8aa3; font-weight: normal; font-size: 0.64rem; text-transform: uppercase; letter-spacing: 0.5px; }
.pp-link  { color: #53d8fb; cursor: pointer; text-decoration: underline; font-size: 0.7rem; margin-top: 4px; display: inline-block; }
.rx-icon {
  width: 14px; height: 14px; border-radius: 50%; background: #e94560;
  border: 2px solid #fff; box-shadow: 0 0 0 4px rgba(233,69,96,0.3), 0 0 10px #e94560;
}

/* ---- Metrics view: tabs, global filters, chart cards ----
 * The Metrics modal grew from two fixed bar charts into a small analytics
 * view.  Everything below is prefixed "mx-" so it cannot collide with the
 * history and stations styles it sits beside.  Charts are hand-drawn SVG
 * (and one canvas for the scatter), so the only external dependency is
 * Leaflet for the coverage map, and the rest of the view works offline.
 *
 * Colour jobs (checked with the dataviz palette validator on #0d1b2a):
 *   frequency  #53d8fb #ffeb3b #d55181 #9085e9  (first two = the existing
 *              Activity chart's 518/490 colours; adjacent CVD dE >= 16)
 *   confidence #4caf50 / #ffc107 / #c98a92 / #56627a, the pill colours
 *   day/night  #f2b33d / #7b8cff  (protan dE 31)
 *   magnitude  one hue, #13294a -> #53d8fb (heatmap)
 * Text never wears a series colour; a swatch or line key beside it does. */
.metrics-dialog.mx-dialog { max-width: 1400px; height: 92vh; max-height: 92vh; }
.mx-count { font-size: 0.75rem; color: #7d8aa3; }
.mx-tabs {
  display: flex; gap: 2px; padding: 0 14px; border-bottom: 1px solid #1a3a5c;
  flex-shrink: 0; overflow-x: auto; scrollbar-width: none;
}
.mx-tabs::-webkit-scrollbar { display: none; }
.mx-tab {
  background: transparent; border: none; border-bottom: 2px solid transparent;
  color: #6a8aaa; font-family: inherit; font-size: 0.8rem; padding: 9px 12px 8px;
  cursor: pointer; white-space: nowrap; flex-shrink: 0;
}
.mx-tab:hover { color: #c8dff0; }
.mx-tab.active { color: #53d8fb; border-bottom-color: #53d8fb; }
.mx-controls {
  display: flex; gap: 8px; padding: 10px 18px; flex-wrap: wrap; align-items: center;
  border-bottom: 1px solid #1a3a5c; flex-shrink: 0;
}
.mx-controls[hidden] { display: none; }
.mx-seg { display: inline-flex; border: 1px solid #2a4a7f; border-radius: 4px; overflow: hidden; }
.mx-seg button {
  background: #16213e; border: none; border-left: 1px solid #2a4a7f; color: #a0b8d8;
  font-family: inherit; font-size: 0.78rem; padding: 5px 9px; cursor: pointer; white-space: nowrap;
}
.mx-seg button:first-child { border-left: none; }
.mx-seg button:hover { color: #c8dff0; background: #1a2a4a; }
.mx-seg button.on { background: #13294a; color: #53d8fb; }
.mx-custom { display: inline-flex; align-items: center; gap: 5px; color: #7d8aa3; font-size: 0.78rem; }
.mx-custom[hidden] { display: none; }
.mx-custom input {
  background: #16213e; border: 1px solid #2a4a7f; border-radius: 4px; color: #c8dff0;
  font-family: inherit; font-size: 0.78rem; padding: 4px 6px; color-scheme: dark;
}
.mx-controls .dd-btn { min-width: 140px; }
.dd-item .dd-check {
  width: 12px; height: 12px; border: 1px solid #2a4a7f; border-radius: 2px; flex-shrink: 0;
  display: inline-flex; align-items: center; justify-content: center; font-size: 0.6rem; color: #0d1b2a;
}
.dd-item.sel .dd-check { background: #53d8fb; border-color: #53d8fb; }
.dd-item.sel .dd-check::after { content: '\2713'; font-weight: 700; }
.mx-note {
  padding: 7px 18px; font-size: 0.74rem; color: #6a8aaa; border-bottom: 1px solid #1a3a5c; flex-shrink: 0;
}
.mx-note[hidden] { display: none; }
.mx-note.warn { color: #ffc107; }
.metrics-body.mx-body { gap: 16px; padding: 14px 18px 22px; transition: opacity 0.15s; }
.metrics-body.mx-body.mx-busy { opacity: 0.55; }
.mx-pane { display: flex; flex-direction: column; gap: 16px; }
.mx-pane[hidden] { display: none; }
.mx-card {
  background: #0a1628; border: 1px solid #16304e; border-radius: 6px;
  padding: 12px 14px 14px; min-width: 0; position: relative;
}
.mx-card-head { display: flex; align-items: flex-start; gap: 10px; flex-wrap: wrap; margin-bottom: 10px; }
.mx-card-title { font-size: 0.8rem; color: #c8dff0; font-weight: 600; }
.mx-card-sub { font-size: 0.68rem; color: #7d8aa3; margin-top: 2px; }
.mx-card-ctl { margin-left: auto; display: flex; gap: 6px; align-items: center; flex-wrap: wrap; }
.mx-mini {
  background: transparent; border: 1px solid #2a4a7f; border-radius: 4px; color: #6a8aaa;
  font-family: inherit; font-size: 0.68rem; padding: 3px 8px; cursor: pointer;
}
.mx-mini:hover { color: #c8dff0; border-color: #53d8fb; }
.mx-mini.on { color: #53d8fb; border-color: #1a6b8a; background: #13294a; }
.mx-sel {
  background: #16213e; border: 1px solid #2a4a7f; border-radius: 4px; color: #c8dff0;
  font-family: inherit; font-size: 0.72rem; padding: 3px 6px; cursor: pointer; outline: none;
}
.mx-sel:focus { border-color: #53d8fb; }
.mx-lab { font-size: 0.66rem; color: #7d8aa3; text-transform: uppercase; letter-spacing: 0.5px; }
.mx-empty { padding: 34px 12px; text-align: center; color: #7d8aa3; font-size: 0.8rem; line-height: 1.6; }
.mx-empty .pp-link { font-size: 0.78rem; }
.mx-grid2 { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 16px; }
.mx-cov-grid { display: grid; grid-template-columns: minmax(0, 1.7fr) minmax(0, 1fr); gap: 16px; }
.mx-svgwrap { position: relative; }
.mx-svgwrap svg { display: block; width: 100%; height: auto; overflow: visible; }
.mx-svgwrap text { font-family: 'Courier New', Courier, monospace; fill: #6a7f98; font-size: 10px; }
.mx-legend { display: flex; flex-wrap: wrap; gap: 4px 14px; margin: 0 0 8px; font-size: 0.7rem; color: #a0b8d8; }
.mx-lg { display: inline-flex; align-items: center; gap: 6px; white-space: nowrap; }
.mx-lg i { display: inline-block; width: 10px; height: 10px; border-radius: 2px; }
.mx-lg i.ln { width: 14px; height: 0; border-top: 2px solid; border-radius: 0; }
.mx-lg i.ln.dash { border-top-style: dashed; }
.mx-lg i.dot { width: 8px; height: 8px; border-radius: 50%; }
.mx-lg i.shade { background: rgba(123,140,255,0.22); border: 1px solid rgba(123,140,255,0.35); }

/* Stat tiles (Coverage key numbers) */
.mx-tiles { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 10px; }
.mx-tile { background: #0a1628; border: 1px solid #16304e; border-radius: 6px; padding: 10px 12px; min-width: 0; }
.mx-tile-l { font-size: 0.64rem; color: #7d8aa3; text-transform: uppercase; letter-spacing: 0.6px; }
.mx-tile-v {
  font-size: 1.3rem; color: #c8dff0; margin-top: 4px; display: flex; align-items: center; gap: 7px;
  white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
}
.mx-tile-v .flag { width: 20px; height: 15px; }
.mx-tile-s { font-size: 0.68rem; color: #6a8aaa; margin-top: 3px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.mx-tile-v small { font-size: 0.72rem; color: #6a8aaa; }

/* Tooltip shared by every chart */
.mx-tip {
  position: fixed; z-index: 3000; pointer-events: none; display: none;
  background: #0d1b2a; border: 1px solid #2a4a7f; border-radius: 5px;
  box-shadow: 0 6px 22px rgba(0,0,0,0.6); padding: 7px 10px; font-size: 0.72rem;
  color: #a0b8d8; max-width: 300px; line-height: 1.45;
}
.mx-tip .mx-th { color: #c8dff0; font-weight: 600; margin-bottom: 3px; display: flex; align-items: center; gap: 6px; }
.mx-tip .mx-tr { display: flex; align-items: center; gap: 7px; white-space: nowrap; }
.mx-tip .mx-tr i { width: 10px; height: 0; border-top: 2px solid; flex-shrink: 0; }
.mx-tip .mx-tr b { color: #e8f4ff; font-weight: 600; min-width: 4ch; }
.mx-tip .mx-tm { color: #7d8aa3; font-size: 0.66rem; margin-top: 3px; }

/* Horizontal bars (Breakdown by station / country / ...) */
.mx-hb { display: flex; flex-direction: column; gap: 3px; }
.mx-hb-row {
  display: grid; grid-template-columns: minmax(90px, 210px) minmax(0, 1fr) 70px; gap: 10px;
  align-items: center; padding: 2px 4px; border-radius: 3px; cursor: default;
}
.mx-hb-row:hover { background: #12233a; }
.mx-hb-lab {
  font-size: 0.74rem; color: #a0b8d8; display: flex; align-items: center; gap: 6px;
  overflow: hidden; white-space: nowrap; text-overflow: ellipsis; min-width: 0;
}
.mx-hb-lab span { overflow: hidden; text-overflow: ellipsis; }
.mx-hb-lab em { font-style: normal; color: #7d8aa3; font-size: 0.68rem; }
.mx-hb-track { display: flex; flex-direction: column; gap: 2px; min-width: 0; }
.mx-hb-bar { display: flex; gap: 2px; height: 14px; min-width: 2px; }
.mx-hb-bar.thin { height: 7px; }
.mx-hb-bar span { min-width: 1px; height: 100%; }
.mx-hb-bar span:last-child { border-radius: 0 4px 4px 0; }
.mx-hb-bar.neg span:last-child { border-radius: 4px 0 0 4px; }
.mx-hb-val { font-size: 0.72rem; color: #c8dff0; text-align: right; font-variant-numeric: tabular-nums; white-space: nowrap; }
.mx-more { margin-top: 8px; }

/* Heatmap (station x UTC hour) */
.mx-heat-wrap { overflow-x: auto; }
.mx-heat { border-collapse: separate; border-spacing: 2px; font-size: 0.7rem; }
.mx-heat th { color: #7d8aa3; font-weight: normal; font-size: 0.62rem; padding: 0 0 3px; text-align: center; }
.mx-heat td.mx-heat-lab {
  position: sticky; left: 0; background: #0a1628; z-index: 1; padding-right: 8px; white-space: nowrap;
  color: #a0b8d8; max-width: 220px; overflow: hidden; text-overflow: ellipsis;
}
.mx-heat td.mx-heat-lab .flag { margin-right: 5px; }
.mx-heat td.mx-heat-lab em { font-style: normal; color: #7d8aa3; margin-left: 5px; font-size: 0.64rem; }
.mx-heat td.c { width: 22px; min-width: 18px; height: 16px; border-radius: 2px; background: #0e1d31; }
.mx-heat td.c:hover { outline: 1px solid #c8dff0; }
.mx-heat td.mx-heat-n { color: #6a8aaa; padding-left: 8px; text-align: right; font-variant-numeric: tabular-nums; }
.mx-scale { display: flex; align-items: center; gap: 8px; font-size: 0.66rem; color: #6a8aaa; margin-top: 8px; }
.mx-scale i { display: inline-block; width: 120px; height: 8px; border-radius: 2px; }

/* Plain tables: ranked stations and every chart's Table view */
.mx-tbl-wrap { overflow-x: auto; max-height: 520px; overflow-y: auto; }
.mx-tbl { width: 100%; border-collapse: collapse; font-size: 0.76rem; }
.mx-tbl th {
  background: #0a1628; color: #7d8aa3; font-weight: 600; text-align: left; padding: 6px 10px;
  border-bottom: 1px solid #1a3a5c; white-space: nowrap; position: sticky; top: 0; z-index: 1;
}
.mx-tbl th.num, .mx-tbl td.num { text-align: right; font-variant-numeric: tabular-nums; }
.mx-tbl th[data-mx-rsort] { cursor: pointer; user-select: none; }
.mx-tbl th[data-mx-rsort]:hover { color: #a0b8d8; }
.mx-tbl th.sorted { color: #53d8fb; }
.mx-tbl td { padding: 5px 10px; color: #a0b8d8; border-bottom: 1px solid #111e30; white-space: nowrap; }
.mx-tbl tr:hover td { background: #12233a; }
.mx-tbl td .flag { margin-right: 6px; }
.mx-tbl td em { font-style: normal; color: #7d8aa3; margin-left: 6px; }

/* Coverage map */
.mx-map { height: 440px; border-radius: 4px; overflow: hidden; background: #0d0d1a; }
/* Leaflet's panes use z-index 400+; keep them inside the map's own
 * stacking context so they never paint over the sticky dialog header. */
.mx-map { position: relative; isolation: isolate; z-index: 0; }
.mx-map[hidden] { display: none; }
.mx-map-note { font-size: 0.72rem; color: #6a8aaa; padding: 6px 0 0; }
.mx-map-note[hidden] { display: none; }
/* Every map's base tiles (Stations tab, Coverage, the message modal) are
 * light OpenStreetMap tiles darkened here, on the tile images only, so the
 * markers and paths drawn over them keep their real colours.
 *
 * Per image rather than on the whole layer: at a fractional zoom
 * (zoomSnap 0.5) Leaflet scales the 256 px tiles by a non-integer factor
 * and leaves hairline gaps between some of them.  Filtering the layer as
 * one surface inverts those gaps too, into a bright grid across the map;
 * filtering each tile leaves the gaps showing the map's own dark
 * background, where they all but disappear. */
.map-tiles-dark .leaflet-tile { filter: invert(1) hue-rotate(180deg) brightness(.85) contrast(.9); }
.mx-polar { max-width: 400px; margin: 0 auto; }
.mx-scatter { position: relative; }
.mx-scatter canvas { display: block; width: 100%; }
.mx-scatter .mx-hl {
  position: absolute; width: 12px; height: 12px; margin: -6px 0 0 -6px; border-radius: 50%;
  border: 2px solid #e8f4ff; pointer-events: none; display: none;
}
.mx-opts { display: flex; gap: 8px 14px; flex-wrap: wrap; align-items: center; }
.mx-opts label { display: inline-flex; align-items: center; gap: 6px; }

@media (max-width: 900px) {
  .mx-cov-grid, .mx-grid2 { grid-template-columns: minmax(0, 1fr); }
}
@media (max-width: 700px) {
  .metrics-dialog.mx-dialog { height: 96vh; max-height: 96vh; }
  .mx-controls { padding: 8px 10px; gap: 6px; }
  .mx-controls .dd { flex: 1 1 140px; min-width: 0; }
  .mx-controls select { flex: 1 1 120px; min-width: 0; }
  .mx-seg { flex-wrap: nowrap; max-width: 100%; overflow-x: auto; }
  .mx-seg button { padding: 5px 7px; }
  .metrics-body.mx-body { padding: 10px 10px 18px; }
  .mx-card { padding: 10px 10px 12px; }
  .mx-map { height: 300px; }
  .mx-hb-row { grid-template-columns: minmax(0, 46%) minmax(0, 1fr) 50px; gap: 6px; }
  /* One scroll for the whole dialog: the filter bar scrolls away with
   * the charts instead of pinning a third of a phone screen. */
  .metrics-dialog.mx-dialog { overflow-y: auto; }
  .mx-dialog .hist-header { position: sticky; top: 0; z-index: 3; background: #0d1b2a; }
  .mx-dialog .mx-tabs { position: sticky; top: 49px; z-index: 3; background: #0d1b2a; }
  .metrics-body.mx-body { flex: none; overflow: visible; }
  .mx-tile-v { font-size: 1.1rem; }
  .mx-note { padding: 6px 10px; }
}

/* ---- Phone width ----
 * The page was laid out for a desktop: a fixed-height body with several
 * scrolling panes.  Below ~700 px the header and tab bar wrap or scroll
 * instead of overflowing, wide tables scroll sideways inside their own
 * box, and the split view stacks its two columns. */
@media (max-width: 700px) {
  body { padding: 6px; gap: 6px; }
  header { gap: 8px 12px; padding: 7px 10px; }
  header > div[style*="margin-left:auto"] { margin-left: 0 !important; flex-wrap: wrap; gap: 8px !important; }
  /* Scrolls sideways; the fade on the right edge says there is more
   * (the Stations tab used to be cut to "St…" with no hint). */
  .tab-bar { overflow-x: auto; scrollbar-width: none;
             -webkit-mask-image: linear-gradient(to right, #000 calc(100% - 28px), transparent);
             mask-image: linear-gradient(to right, #000 calc(100% - 28px), transparent);
             padding-right: 24px; }
  .tab-bar::-webkit-scrollbar { display: none; }
  /* A phone has no room for one row: let the bar wrap (the station on a
   * line of its own reads better than a flag squeezed to nothing). */
  .msg-bar { flex-wrap: wrap; row-gap: 6px; }
  .msg-bar .stat:nth-child(3) { flex: 1 1 80px; }              /* subject: takes what is left, ellipsis */
  .msg-bar .stat:nth-child(2) { flex: 1 1 calc(100% - 150px); order: 5; }   /* row 2: station ... */
  .msg-bar .output-actions    { order: 6; }                    /* ... then the buttons */
  .tab-btn { padding: 6px 10px; white-space: nowrap; flex-shrink: 0; }
  .tab-btn .tab-name { display: none; }
  .tab-btn-both .tab-name, .tab-btn-latest .tab-name, .tab-btn-stations .tab-name { display: inline; }
  #panel-both { flex-direction: column !important; }
  .hist-overlay { padding: 6px; }
  .hist-dialog { max-height: 94vh; }
  .hist-toolbar, .hist-filter-row { padding-left: 10px; padding-right: 10px; }
  .hist-toolbar .hist-search-input { flex: 1 1 100%; }
  .hist-toolbar select, .hist-filter-row select { flex: 1 1 130px; min-width: 0; }
  .hist-filter-row .dd { flex: 1 1 140px; min-width: 0; }
  .dd-btn { min-width: 0; width: 100%; max-width: none; }
  .hist-table thead th, .hist-table tbody td { padding: 7px 9px; }
  .hist-list-wrap { overflow-x: auto; }
  .msg-ident-panel { grid-template-columns: minmax(0, 1fr); padding: 12px 14px; }
  .mi-map { height: 180px; }
  #panel-latest, #panel-stations { padding: 10px; }
  .latest-table thead th, .latest-table tbody td { padding: 6px 8px; }
  #latest-content { overflow-x: auto; }
  .latest-toolbar { flex-wrap: wrap; }
  .hist-table tbody td, .latest-table tbody td { white-space: nowrap; }
  .tab-btn { padding: 6px 9px; gap: 5px; font-size: 0.8rem; }
  /* Message modal: one scroll for identification, metrics and text,
   * rather than three panes fighting over a short screen. */
  .hist-dialog-msg { overflow-y: auto; }
  .hist-dialog-msg .msg-ident-panel { max-height: none; overflow: visible; }
  .hist-dialog-msg .msg-modal-body { flex: none; overflow: visible; }
  .hist-dialog-msg .hist-header { position: sticky; top: 0; z-index: 2; background: #0d1b2a; }
  .stn-map-wrap { height: 300px; }
  .stn-legend { font-size: 0.56rem; padding: 4px 7px; gap: 1px; bottom: 30px; left: 6px; }
  .stn-legend i { width: 7px; height: 7px; }
}
</style>
</head>
<body>
<header>
  <h1>&#x1F4E1; NAVTEX</h1>
  <div class="info-item">
    <span class="info-label">SDR Server</span>
    <span class="info-value">)HTML" + sdr_url + R"HTML(</span>
  </div>
  <div class="info-item">
    <span class="info-label">Status</span>
    <span class="info-value"><span id="status-dot"></span><span id="status-text">Connecting&hellip;</span></span>
  </div>
  <!-- Filled from /api/stations: history-replay progress, receiver-location warnings -->
  <div class="ident-hdr" id="ident-hdr"></div>
  <div style="margin-left:auto;display:flex;align-items:center;gap:12px">
    <label class="ts-label" style="display:flex;align-items:center;gap:6px;cursor:pointer;font-size:0.78rem;color:#888;user-select:none">
      <input type="checkbox" id="ts-toggle" checked style="accent-color:#53d8fb;width:14px;height:14px;cursor:pointer">
      Timestamps
    </label>
    <label class="ts-label" style="display:flex;align-items:center;gap:6px;cursor:pointer;font-size:0.78rem;color:#888;user-select:none">
      <input type="checkbox" id="alert-sound-toggle" style="accent-color:#53d8fb;width:14px;height:14px;cursor:pointer">
      Alert Sound
    </label>
)HTML" + (logging_enabled ? R"HTML(    <button class="history-btn" id="metrics-btn" onclick="openMetrics()">&#x1F4CA; Metrics</button>
    <button class="history-btn" id="history-btn" onclick="openHistory()">&#x1F4DC; History</button>
)HTML" : "") + R"HTML(  </div>
</header>

<!-- History modal -->
<div id="history-modal" class="hist-overlay" role="dialog" aria-modal="true" style="display:none" onclick="if(event.target===this)closeHistory()">
  <div class="hist-dialog hist-dialog-wide">
    <div class="hist-header">
      <div class="hist-title-group">
        <span class="hist-title">Message History</span>
        <span id="hist-count" style="font-size:0.75rem;color:#7d8aa3;font-family:'Courier New',monospace;"></span>
      </div>
      <button class="hist-close" style="margin-left:auto" onclick="closeHistory()" title="Close">&times;</button>
    </div>
    <div class="hist-toolbar">
      <input type="text" id="hist-search" placeholder="Search id, station, country, NAVAREA&hellip;" oninput="filterHistoryDebounced()" class="hist-search-input">
      <select id="hist-type-filter" onchange="filterHistory()" class="hist-freq-select">
        <option value="">Messages &amp; Raw Logs</option>
        <option value="msg" selected>Messages only</option>
        <option value="raw">Raw Logs only</option>
      </select>
      <select id="hist-subject-filter" onchange="filterHistory()" class="hist-freq-select">
        <option value="">All subjects</option>
)HTML" + subject_opts + R"HTML(      </select>
      <select id="hist-freq-filter" onchange="filterHistory()" class="hist-freq-select">
        <option value="">All frequencies</option>
      </select>
    </div>
    <div class="hist-filter-row">
      <div class="dd" id="hist-country-dd"></div>
      <div class="dd" id="hist-station-dd"></div>
      <select id="hist-level-filter" onchange="filterHistory()" class="hist-freq-select" title="Identification confidence">
        <option value="">Any confidence</option>
        <option value="high">High</option>
        <option value="medium">Medium</option>
        <option value="low">Low</option>
        <option value="none">Unidentified</option>
      </select>
      <select id="hist-dist-filter" onchange="filterHistory()" class="hist-freq-select" title="Distance from the receiver">
        <option value="">Any distance</option>
        <option value="lt300">&lt; 300 km</option>
        <option value="300-1000">300 &ndash; 1,000 km</option>
        <option value="1000-3000">1,000 &ndash; 3,000 km</option>
        <option value="gt3000">&gt; 3,000 km</option>
      </select>
      <select id="hist-path-filter" onchange="filterHistory()" class="hist-freq-select" title="Day or night propagation path">
        <option value="">Day &amp; night</option>
        <option value="day">Day path</option>
        <option value="night">Night path</option>
      </select>
      <button class="hist-clear" id="hist-clear-btn" onclick="clearHistFilters()" hidden>&times; Clear filters</button>
    </div>
    <div class="hist-replay-note" id="hist-replay-note" hidden></div>
    <div class="hist-list-wrap">
      <table class="hist-table" id="hist-table">
        <thead>
          <tr>
            <th class="sortable" tabindex="0" aria-sort="none" data-sort="date">Date<span class="sort-ind"></span></th>
            <th class="sortable" tabindex="0" aria-sort="none" data-sort="time" title="When the message started (UTC)">Start (UTC)<span class="sort-ind"></span></th>
            <th class="sortable" tabindex="0" aria-sort="none" data-sort="freq">Frequency<span class="sort-ind"></span></th>
            <th class="sortable" tabindex="0" aria-sort="none" data-sort="id">ID<span class="sort-ind"></span></th>
            <th class="sortable" tabindex="0" aria-sort="none" data-sort="serial">Serial<span class="sort-ind"></span></th>
            <th class="sortable" tabindex="0" aria-sort="none" data-sort="station">Station<span class="sort-ind"></span></th>
            <th class="sortable" tabindex="0" aria-sort="none" data-sort="subject">Subject<span class="sort-ind"></span></th>
            <th class="sortable" tabindex="0" aria-sort="none" data-sort="dist">Distance<span class="sort-ind"></span></th>
            <th class="sortable" tabindex="0" aria-sort="none" data-sort="snr">SNR<span class="sort-ind"></span></th>
            <th class="sortable" tabindex="0" aria-sort="none" data-sort="dur">Duration<span class="sort-ind"></span></th>
            <th></th>
          </tr>
        </thead>
        <tbody id="hist-tbody"></tbody>
      </table>
      <div id="hist-empty" class="hist-empty" style="display:none">No messages found.</div>
      <div id="hist-loading" class="hist-empty">Loading&hellip;</div>
      <div class="hist-pagination" id="hist-pagination" style="display:none">
        <button id="hist-prev-btn" onclick="histPagePrev()" disabled>&#8592; Prev</button>
        <span class="hist-page-info" id="hist-page-info"></span>
        <button id="hist-next-btn" onclick="histPageNext()">Next &#8594;</button>
      </div>
    </div>
  </div>
</div>

<!-- Metrics view: Activity (/api/metrics) plus the coverage / reception
     analytics built in the browser from /api/analytics.  The filter bar
     scopes every analytics tab; Activity keeps its fixed 24 h / 30 d view. -->
<div id="metrics-modal" class="hist-overlay" style="display:none" onclick="if(event.target===this)closeMetrics()">
  <div class="metrics-dialog mx-dialog" role="dialog" aria-modal="true" aria-label="Metrics">
    <div class="hist-header">
      <div class="hist-title-group">
        <span class="hist-title">&#x1F4CA; Metrics</span>
        <span class="mx-count" id="mx-count"></span>
      </div>
      <button class="hist-close" style="margin-left:auto" onclick="closeMetrics()" title="Close">&times;</button>
    </div>
    <div class="mx-tabs" id="mx-tabs" role="tablist">
      <button class="mx-tab" data-mx-tab="activity" role="tab">Activity</button>
      <button class="mx-tab" data-mx-tab="coverage" role="tab">Coverage</button>
      <button class="mx-tab" data-mx-tab="distance" role="tab">Distance</button>
      <button class="mx-tab" data-mx-tab="stations" role="tab">Stations</button>
      <button class="mx-tab" data-mx-tab="breakdown" role="tab">Breakdown</button>
      <button class="mx-tab" data-mx-tab="trends" role="tab">Trends</button>
    </div>
    <div class="mx-controls" id="mx-controls">
      <div class="mx-seg" id="mx-range" title="Date range (UTC)">
        <button data-mx-range="24h">24 h</button><button data-mx-range="7d">7 d</button><button data-mx-range="30d">30 d</button><button data-mx-range="all">All</button><button data-mx-range="custom">Custom</button>
      </div>
      <span class="mx-custom" id="mx-custom" hidden>
        <input type="date" id="mx-from" aria-label="From (UTC)"> &ndash; <input type="date" id="mx-to" aria-label="To (UTC)">
      </span>
      <select id="mx-freq" class="hist-freq-select" title="Frequency"><option value="">All frequencies</option></select>
      <select id="mx-lvl" class="hist-freq-select" title="Minimum identification confidence">
        <option value="auto" title="Counts use every message; distances use Medium or better, the furthest station High only">Auto (Med+ for distance)</option>
        <option value="0">Any confidence</option>
        <option value="1">Low or better</option>
        <option value="2">Medium or better</option>
        <option value="3">High only</option>
      </select>
      <div class="dd" id="mx-country-dd"></div>
      <div class="dd" id="mx-station-dd"></div>
      <div class="mx-seg" id="mx-path" title="Day or night propagation path">
        <button data-mx-path="">Day &amp; night</button><button data-mx-path="day">Day</button><button data-mx-path="night">Night</button>
      </div>
      <button class="hist-clear" id="mx-reset" hidden>&times; Reset filters</button>
    </div>
    <div class="mx-note" id="mx-note" hidden></div>
    <div class="metrics-body mx-body" id="metrics-body">
      <div class="mx-pane" id="mx-pane-activity" hidden></div>
      <div class="mx-pane" id="mx-pane-coverage" hidden></div>
      <div class="mx-pane" id="mx-pane-distance" hidden></div>
      <div class="mx-pane" id="mx-pane-stations" hidden></div>
      <div class="mx-pane" id="mx-pane-breakdown" hidden></div>
      <div class="mx-pane" id="mx-pane-trends" hidden></div>
    </div>
  </div>
</div>
<div class="mx-tip" id="mx-tip" role="tooltip"></div>

<!-- Message detail modal -->
<div id="msg-modal" class="hist-overlay" role="dialog" aria-modal="true" style="display:none" onclick="if(event.target===this)closeMsgModal()">
  <div class="hist-dialog hist-dialog-msg">
    <div class="hist-header">
      <span class="hist-title" id="msg-modal-title">Message</span>
      <div style="display:flex;align-items:center;gap:6px;margin-left:auto">
        <button class="icon-btn" id="msg-modal-copy-btn" title="Copy to clipboard" aria-label="Copy">&#x1F4CB;</button>
        <button class="icon-btn" id="msg-modal-dl-btn" title="Download as text file" aria-label="Download">&#x2B07;</button>
        <button class="hist-close" onclick="closeMsgModal()" title="Close">&times;</button>
      </div>
    </div>
    <div id="msg-ident-panel" class="msg-ident-panel hidden"></div>
    <div id="msg-metrics-panel" class="msg-metrics-panel hidden"></div>
    <pre id="msg-modal-body" class="msg-modal-body"></pre>
  </div>
</div>
<div id="ident-pop" role="tooltip"></div>
<div class="tab-bar">
)HTML" + tab_buttons + R"HTML(</div>
<div class="tab-content">
)HTML" + panels + R"HTML(
  <div class="tab-panel" id="panel-both" style="display:none">
  </div>
  <div class="tab-panel" id="panel-latest" style="display:none">
    <div class="latest-toolbar">
      <span class="latest-toolbar-title">&#x1F4CB; Latest Messages</span>
      <button class="latest-refresh-btn" onclick="fetchLatest()">&#x1F504; Refresh</button>
      <span class="latest-last-updated" id="latest-last-updated"></span>
    </div>
    <div id="latest-content">
      <div class="latest-empty">Select this tab to load latest messages.</div>
    </div>
  </div>
  <div class="tab-panel" id="panel-stations" style="display:none">
    <div class="latest-toolbar">
      <span class="latest-toolbar-title" style="color:#b39ddb">&#x1F310; Stations</span>
      <span class="stn-summary" id="stn-summary"></span>
      <label class="stn-toggle" title="Also show table stations that have not been heard">
        <input type="checkbox" id="stn-unheard-toggle" onchange="renderStations()"> Show unheard
      </label>
      <button class="latest-refresh-btn" onclick="fetchStations(true)">&#x1F504; Refresh</button>
      <span class="latest-last-updated" id="stn-last-updated"></span>
    </div>
    <div class="stn-note" id="stn-note" hidden></div>
    <div class="stn-map-wrap" id="stn-map-wrap">
      <div id="stn-map"></div>
      <div class="stn-legend" id="stn-legend">
        <span><i style="background:#4caf50"></i>Heard in the last hour</span>
        <span><i style="background:#53d8fb"></i>Last 24 hours</span>
        <span><i style="background:#ffc107"></i>Last 7 days</span>
        <span><i style="background:#6a7a8a"></i>Older</span>
        <span><i class="lg-rx"></i>Receiver</span>
      </div>
    </div>
    <div class="stn-table-wrap" id="stn-table-wrap">
      <div class="latest-empty">Loading&hellip;</div>
    </div>
  </div>
</div>
<script>
)HTML" + subject_js + R"HTML(  function decodeNavtexId(id) {
    /* id is like "EA42", "BZ", "unknown" */
    if (!id || id === 'unknown') return { station: '—', subject: '—', serial: '—' };
    const s = id.toUpperCase();
    const stCode  = s.length >= 1 ? s[0] : '';
    const subCode = s.length >= 2 ? s[1] : '';
    const serial  = s.length >= 3 ? s.slice(2) : '—';
    return {
      station: stCode ? stCode : '—',
      subject: SUBJECT[subCode]        || (subCode ? subCode : '—'),
      serial:  serial
    };
  }
  const STATE_NAMES = ['Searching', 'Syncing', 'Locked'];
  const STATE_CLASS = ['dim', 'warn', 'good'];
  const TAB_DOT_CLASS = ['searching', 'syncing', 'locked'];

  const NUM_CHANNELS = )HTML" + std::to_string(channels.size()) + R"HTML(;
  const LOGGING_ENABLED = )HTML" + std::string(logging_enabled ? "true" : "false") + R"HTML(;
)HTML" + ch_labels_js + R"HTML(

  /* ---------------------------------------------------------------
   * SNR scale.
   *
   * GENERATED from src/navtex_snr.h — do not write a threshold here as
   * a literal.  Four hand-written copies of the good/warn boundaries and
   * a fifth for the bar range used to live in this file, and audio
   * protocol version 4 moved the scale under all of them at once.  They
   * come from one calibration in that header now, so the next change to
   * it moves every one of them together.
   * --------------------------------------------------------------- */
)HTML" + snr_js + R"HTML(

  /* 'good' / 'warn' / 'bad', or 'dim' when there is no reading. */
  function snrCls(snr) {
    if (snr === null || snr === undefined || !isFinite(snr)) return 'dim';
    return snr > SNR_GOOD_DB ? 'good' : snr > SNR_WARN_DB ? 'warn' : 'bad';
  }
  /* The same classes, but blank rather than 'dim' for a missing reading —
   * the metrics modal styles an unclassed value itself. */
  function snrClass(snr) {
    if (snr === null || snr === undefined || !isFinite(snr)) return '';
    return snrCls(snr);
  }
  /* Fill percentage for the SNR bar, clamped to the track. */
  function snrBarPct(snr) {
    if (snr === null || snr === undefined || !isFinite(snr)) return 0;
    return Math.max(0, Math.min(100, (snr - SNR_BAR_MIN_DB) / SNR_BAR_SPAN_DB * 100));
  }
  /* Bar colour, from the same two boundaries as the text. */
  function snrBarColour(snr) {
    return snr > SNR_GOOD_DB ? '#4caf50' : snr > SNR_WARN_DB ? '#ffeb3b' : '#e94560';
  }

  /* ===============================================================
   * Station identification — shared rendering.
   *
   * The server's identifier decides which transmitter sent a message
   * and writes the reasons as plain-English "facts" (navtex_ident_facts.h).
   * This page only ever DISPLAYS that answer; it never infers a station
   * or writes a fact of its own, so what the page says and what MQTT /
   * the API say cannot drift apart.
   *
   * Two shapes arrive here:
   *   - the full ident object (sidecar "ident", /api/ident, /api/latest
   *     "ident", WS frame 0x05) — has facts, members, alts;
   *   - the history summary "st" (/api/history) — key, name, iso2,
   *     country_name, navarea, conf, level, dist_km, bearing, night.
   * Both carry name/iso2/level/conf under the same names, so one chip
   * renderer serves both; facts for a history row are fetched lazily
   * from /api/ident when its pill is hovered.
   * =============================================================== */

  function apiBase() { return (window._BASE_PATH || '').replace(/\/$/, ''); }

  /* A confidence as a whole percent, exactly as navtex_fmt_pct() rounds it
   * for the facts (to the 3 decimals the JSON carries, then halves up), and
   * never 100 short of certainty — so a panel cannot say "Rules 45%" beside
   * a fact saying 44%, or "HIGH 100%" for 0.995. */
  function confPct(c) {
    const pm = Math.round(c * 1000);
    let p = Math.floor((pm + 5) / 10);
    if (p >= 100 && pm < 1000) p = 99;
    return p;
  }

  function escHtml(s) {
    return String(s === null || s === undefined ? '' : s)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
  }

  /* 1234 -> "1,234".  Thousands separators are fixed to a comma rather
   * than the browser locale so the page reads the same as the facts the
   * server writes ("1,140 km"). */
  function fmtInt(n) {
    return String(Math.round(Number(n))).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
  }

  function fmtKm(km) {
    return (typeof km === 'number' && isFinite(km) && km >= 0) ? fmtInt(km) + ' km' : '—';
  }

  const COMPASS16 = ['N','NNE','NE','ENE','E','ESE','SE','SSE','S','SSW','SW','WSW','W','WNW','NW','NNW'];
  function compassPoint(deg) {
    if (typeof deg !== 'number' || !isFinite(deg) || deg < 0) return '';
    return COMPASS16[Math.round(deg / 22.5) % 16];
  }
  function fmtBearing(deg) {
    if (typeof deg !== 'number' || !isFinite(deg) || deg < 0) return '—';
    return Math.round(deg) + '° ' + compassPoint(deg);
  }

  /* NAVAREAs are numbered in Roman numerals on every official chart and
   * notice (NAVAREA I is the UK/North Sea area), so show them that way. */
  function roman(n) {
    n = Number(n);
    if (!n || n < 1 || n > 39) return n ? String(n) : '';
    const t = ['', 'X', 'XX', 'XXX'], u = ['', 'I','II','III','IV','V','VI','VII','VIII','IX'];
    return t[Math.floor(n / 10)] + u[n % 10];
  }

  /* "3 h ago" style age, for last-heard columns. */
  function fmtAgo(iso) {
    if (!iso) return '—';
    const t = Date.parse(iso);
    if (!isFinite(t)) return '—';
    const s = Math.max(0, (Date.now() - t) / 1000);
    if (s < 90)     return 'just now';
    if (s < 3600)   return Math.round(s / 60) + ' min ago';
    if (s < 86400)  return Math.round(s / 3600) + ' h ago';
    if (s < 86400 * 60) return Math.round(s / 86400) + ' d ago';
    return iso.slice(0, 10);
  }

  /* Flag image.  iso2 is validated here as well as on the server: it goes
   * into a URL, and a value that is not two lower-case letters gets an
   * empty placeholder box instead of a request.  onerror hides the image
   * (rather than showing a broken-image glyph) if a flag is missing. */
  function flagHtml(iso2, cls) {
    if (!/^[a-z]{2}$/.test(iso2 || ''))
      return '<span class="flag flag-none' + (cls ? ' ' + cls : '') + '"></span>';
    /* escHtml: the base path comes from X-Forwarded-Prefix.  The server
     * already refuses anything but path characters; this is the belt to
     * that pair of braces, since this string is an HTML attribute. */
    return '<img class="flag' + (cls ? ' ' + cls : '') + '" src="' + escHtml(apiBase()) + '/flags/' + iso2
         + '.svg" alt="" width="16" height="12" loading="lazy" decoding="async"'
         + ' onerror="this.style.visibility=\'hidden\'">';
  }

  const LEVEL_TEXT = { high: 'High', medium: 'Medium', low: 'Low', none: 'Unidentified' };
  function identLevel(st) {
    if (!st || !st.key) return 'none';
    return LEVEL_TEXT[st.level] ? st.level : 'none';
  }
  function levelColour(level) {
    return level === 'high' ? '#4caf50' : level === 'medium' ? '#ffc107'
         : level === 'low' ? '#c98a92' : '#446';
  }

  /* The pop-over attributes for an element: inline facts when we have
   * them (full ident object), else the message's rel so the pop-over
   * can fetch /api/ident on demand (history rows). */
  function popAttrs(st, opts) {
    opts = opts || {};
    let a = '';
    if (st && Array.isArray(st.facts)) {
      a += ' data-facts="' + escHtml(JSON.stringify(st.facts)) + '"';
      a += ' data-pop-title="' + escHtml(identTitle(st)) + '"';
      if (st.provisional) a += ' data-pop-sub="Provisional — header decoded, message still arriving"';
    } else if (opts.rel) {
      a += ' data-ident-rel="' + escHtml(opts.rel) + '"';
    }
    /* Focusable, so keyboard and screen-reader users can reach the facts:
     * focus shows the popover, Enter/Space pins it (see the key handlers
     * after popHide). */
    if (a) a += ' tabindex="0" role="button" aria-label="Identification details"';
    return a;
  }

  function identTitle(st) {
    const lv = identLevel(st);
    if (lv === 'none') return 'Unidentified';
    return (st.name || '?') + ' · ' + LEVEL_TEXT[lv]
         + (typeof st.conf === 'number' ? ' ' + confPct(st.conf) + '%' : '');
  }

  /* Confidence pill: "High 97%" / "Medium 71%" / "Low 32%" / "Unidentified". */
  function pillHtml(st, opts) {
    opts = opts || {};
    const lv = identLevel(st);
    let txt = LEVEL_TEXT[lv];
    if (lv !== 'none' && typeof st.conf === 'number') txt += ' ' + confPct(st.conf) + '%';
    return '<span class="cpill cp-' + lv + (st && st.provisional ? ' prov' : '') + '"'
         + (opts.noPop ? '' : popAttrs(st, opts)) + '>' + txt + '</span>';
  }

  /* The one station chip used everywhere: flag + name + letter (muted) + pill.
   *   opts.letter  B1 letter to show after the name (omit to hide)
   *   opts.rel     history rel, for lazily fetched facts
   *   opts.chipPop put the pop-over on the whole chip rather than the pill
   *                (the live bar, where the chip is the only thing there) */
  function stationChipHtml(st, opts) {
    opts = opts || {};
    const lv = identLevel(st);
    const prov = st && st.provisional;
    const letter = opts.letter ? '<span class="st-letter">' + escHtml(opts.letter) + '</span>' : '';
    const host = opts.chipPop ? popAttrs(st, opts) : '';
    const pill = pillHtml(st, { rel: opts.rel, noPop: !!opts.chipPop });
    if (lv === 'none')
      return '<span class="stchip st-none"' + host + '>' + letter + pill + '</span>';
    return '<span class="stchip' + (prov ? ' prov' : '') + '"' + host + '>'
         + flagHtml(st.iso2)
         + '<span class="st-name">' + escHtml(st.name) + '</span>'
         + letter + pill + '</span>';
  }

  function pendingHtml(letter) {
    return '<span class="stchip">'
         + (letter ? '<span class="st-big-letter">' + escHtml(letter) + '</span>' : '')
         + '<span class="st-pending">identifying…</span></span>';
  }

  /* ---------------------------------------------------------------
   * Facts pop-over.
   *
   * One floating element shared by every pill/chip.  Desktop: hover
   * shows it after a short delay and it stays while the pointer is on
   * it.  Touch: tap toggles it (there is no hover).  Click anywhere
   * else, Escape, or scrolling closes it.  Event delegation on the
   * document means rows rendered later need no wiring of their own.
   * --------------------------------------------------------------- */
  const _identCache = new Map();   /* rel -> full ident (or null) */
  let _popTarget = null, _popShowT = null, _popHideT = null, _popPinned = false;

  function popTrigger(node) {
    return node && node.closest ? node.closest('[data-facts],[data-ident-rel],[data-pop-text]') : null;
  }

  function popRender(target) {
    const pop = document.getElementById('ident-pop');
    if (!pop || !target) return;
    let html = '';
    if (target.hasAttribute('data-pop-text')) {
      html = '<div class="ip-title">' + escHtml(target.getAttribute('data-pop-title') || '') + '</div>'
           + '<div>' + escHtml(target.getAttribute('data-pop-text')) + '</div>';
    } else if (target.hasAttribute('data-facts')) {
      let facts = [];
      try { facts = JSON.parse(target.getAttribute('data-facts')) || []; } catch (e) {}
      html = popFactsHtml(target.getAttribute('data-pop-title') || '', facts,
                          target.getAttribute('data-pop-sub') || '');
    } else {
      const rel = target.getAttribute('data-ident-rel');
      if (_identCache.has(rel)) {
        const id = _identCache.get(rel);
        html = id ? popFactsHtml(identTitle(id), id.facts || [], '')
                  : '<div class="ip-empty">No identification details stored for this message.</div>';
      } else {
        html = '<div class="ip-empty">Loading identification…</div>';
        fetch(apiBase() + '/api/ident?rel=' + encodeURIComponent(rel))
          .then(function(r) { return r.ok ? r.json() : null; })
          .catch(function() { return null; })
          .then(function(id) {
            _identCache.set(rel, id);
            if (_popTarget === target) { popRender(target); popPlace(target); }
          });
      }
    }
    pop.innerHTML = html;
  }

  function popFactsHtml(title, facts, sub) {
    return '<div class="ip-title">' + escHtml(title) + '</div>'
         + (sub ? '<div class="ip-sub">' + escHtml(sub) + '</div>' : '')
         + (facts.length
            ? '<ul>' + facts.map(function(f) { return '<li>' + escHtml(f) + '</li>'; }).join('') + '</ul>'
            : '<div class="ip-empty">No details.</div>');
  }

  /* Below the trigger if it fits, otherwise above; clamped to the viewport. */
  function popPlace(target) {
    const pop = document.getElementById('ident-pop');
    if (!pop || !target) return;
    pop.style.display = 'block';
    const r = target.getBoundingClientRect();
    const pw = pop.offsetWidth, ph = pop.offsetHeight;
    let left = Math.min(Math.max(8, r.left), window.innerWidth - pw - 8);
    let top  = r.bottom + 6;
    if (top + ph > window.innerHeight - 8) top = Math.max(8, r.top - ph - 6);
    pop.style.left = left + 'px';
    pop.style.top  = top + 'px';
  }

  function popShow(target, pinned) {
    clearTimeout(_popHideT); clearTimeout(_popShowT);
    _popTarget = target; _popPinned = !!pinned;
    popRender(target);
    popPlace(target);
  }
  function popHide() {
    clearTimeout(_popShowT); clearTimeout(_popHideT);
    const pop = document.getElementById('ident-pop');
    if (pop) pop.style.display = 'none';
    _popTarget = null; _popPinned = false;
  }

  (function() {
    document.addEventListener('mouseover', function(ev) {
      const pop = document.getElementById('ident-pop');
      if (pop && pop.contains(ev.target)) { clearTimeout(_popHideT); return; }
      const t = popTrigger(ev.target);
      if (!t) return;
      if (t === _popTarget) { clearTimeout(_popHideT); return; }
      if (_popPinned) return;
      clearTimeout(_popShowT);
      _popShowT = setTimeout(function() { popShow(t, false); }, 140);
    });
    document.addEventListener('mouseout', function(ev) {
      const t = popTrigger(ev.target);
      const pop = document.getElementById('ident-pop');
      const into = ev.relatedTarget;
      if (!t && !(pop && pop.contains(ev.target))) return;
      if (into && (popTrigger(into) === _popTarget || (pop && pop.contains(into)))) return;
      clearTimeout(_popShowT);
      if (_popPinned) return;
      clearTimeout(_popHideT);
      _popHideT = setTimeout(popHide, 160);
    });
    /* Capture phase, so a tap on a pill inside a clickable table row
     * opens the facts instead of the row's action. */
    document.addEventListener('click', function(ev) {
      const pop = document.getElementById('ident-pop');
      if (pop && pop.contains(ev.target)) return;
      const t = popTrigger(ev.target);
      if (t) {
        ev.stopPropagation(); ev.preventDefault();
        if (_popTarget === t && _popPinned) popHide(); else popShow(t, true);
        return;
      }
      if (_popTarget) popHide();
    }, true);
    document.addEventListener('keydown', function(ev) { if (ev.key === 'Escape') popHide(); });
    /* Keyboard: focus a pill or chip to see its facts, Enter/Space to pin. */
    document.addEventListener('focusin', function(ev) {
      const t = popTrigger(ev.target);
      if (t && t === ev.target && t !== _popTarget) popShow(t, false);
    });
    document.addEventListener('focusout', function(ev) {
      const t = popTrigger(ev.target);
      if (t && t === _popTarget && !_popPinned) popHide();
    });
    document.addEventListener('keydown', function(ev) {
      if (ev.key !== 'Enter' && ev.key !== ' ') return;
      const t = popTrigger(ev.target);
      if (!t || t !== ev.target) return;
      ev.preventDefault(); ev.stopPropagation();
      if (_popTarget === t && _popPinned) popHide(); else popShow(t, true);
    }, true);
    /* Only a scroll that moves the trigger closes it — the live output
     * panes scroll themselves on every received character. */
    window.addEventListener('scroll', function(ev) {
      const t = ev.target;
      if (_popTarget && t && t.contains && t.contains(_popTarget)) popHide();
    }, true);
    window.addEventListener('resize', function() { if (_popTarget) popHide(); });
  })();

  /* ---- Identification panel (message modal) ----
   * Everything the server said about who sent this message: the answer,
   * how sure it is and why.  Shown above the signal metrics. */
  function renderIdentPanel(id) {
    const panel = document.getElementById('msg-ident-panel');
    if (!panel) return;
    /* The map lives inside the panel's markup, which is about to be
     * replaced: release the old Leaflet instance first. */
    dropIdentMap();
    if (!id) { panel.className = 'msg-ident-panel hidden'; panel.innerHTML = ''; return; }
    const lv = identLevel(id);
    const known = lv !== 'none';
    const pct = typeof id.conf === 'number' ? Math.max(0, Math.min(100, id.conf * 100)) : 0;

    /* Left column: who, how sure, where. */
    let left = '<div class="mi-head">'
      + (known && /^[a-z]{2}$/.test(id.iso2 || '') ? flagHtml(id.iso2, 'flag-lg')
                                                   : '<span class="mi-flag-none">?</span>')
      + '<div style="min-width:0"><div class="mi-name">'
      + (known ? escHtml(id.name) : 'Unidentified')
      + pillHtml(id, { noPop: true })
      + (id.provisional ? '<span class="mi-prov">provisional</span>' : '')
      + '</div><div class="mi-sub">';
    if (known) {
      const kp = String(id.key || '').split(':');
      const bits = [];
      if (id.country_name) bits.push(escHtml(id.country_name));
      if (id.navarea) bits.push('NAVAREA ' + roman(id.navarea));
      if (kp.length >= 2) bits.push(escHtml(kp[0]) + ' kHz · ' + escHtml(kp[1]));
      left += bits.join(' · ');
    } else {
      left += 'The transmitter could not be attributed';
    }
    left += '</div></div></div>';

    if (known) {
      left += '<div class="mi-conf"><div class="mi-conf-track"><div class="mi-conf-fill" style="width:'
            + pct.toFixed(1) + '%;background:' + levelColour(lv) + '"></div></div>'
            + '<div class="mi-conf-lbl"><span>Confidence</span><span style="color:' + levelColour(lv) + '">'
            + LEVEL_TEXT[lv] + ' · ' + Math.round(pct) + '%</span></div></div>';
    }

    left += '<div class="mi-rows">';
    if (known && typeof id.dist_km === 'number' && id.dist_km >= 0) {
      left += '<div class="mi-row"><span class="mi-k">Distance</span><span class="mi-v">'
            + fmtKm(id.dist_km) + ' · bearing ' + fmtBearing(id.bearing) + '</span></div>';
    }
    if (known && typeof id.night === 'boolean') {
      left += '<div class="mi-row"><span class="mi-k">Path</span><span class="mi-v">'
            + (id.night ? 'Night' : 'Day') + '</span></div>';   /* words: the bundled font has no sun glyph */
    }
    const mem = Array.isArray(id.members) ? id.members : [];
    if (mem.length) {
      const verdicts = mem.map(function(mb) {
        const nm = String(mb.name || '');
        return '<span style="color:#778">' + escHtml(nm.charAt(0).toUpperCase() + nm.slice(1)) + '</span> '
             + (mb.station ? escHtml(mb.station) : '<span style="color:#7d8aa3">none</span>')
             + (typeof mb.conf === 'number' && mb.station ? ' <span style="color:#778">' + confPct(mb.conf) + '%</span>' : '');
      }).join(' · ');
      /* "agree" is false when neither method named anyone, but that is
       * not a disagreement: both found nothing, and saying otherwise
       * would suggest a conflict the facts do not mention. */
      const named = mem.some(function(mb) { return !!mb.station; });
      left += '<div class="mi-row"><span class="mi-k">Methods</span><span class="mi-v">'
            + (id.agree ? '<span class="mi-agree">✓ Both methods agreed</span>'
               : named ? '<span class="mi-disagree">≠ Methods disagreed</span>'
                       : '<span style="color:#778">Neither method named a transmitter</span>')
            + '<br>' + verdicts + '</span></div>';
    }
    left += '</div>';

    /* Right column: the facts, then the alternatives. */
    const facts = Array.isArray(id.facts) ? id.facts : [];
    let right = '<div class="mi-sec">Why</div>'
      + (facts.length ? '<ul class="mi-facts">' + facts.map(function(f) { return '<li>' + escHtml(f) + '</li>'; }).join('') + '</ul>'
                      : '<div style="color:#7d8aa3">No details recorded.</div>');
    const alts = Array.isArray(id.alts) ? id.alts.filter(function(a) { return a && a.name; }) : [];
    if (alts.length) {
      right += '<div class="mi-alts"><div class="mi-sec" style="margin-bottom:1px">Alternatives</div>';
      alts.forEach(function(a) {
        const p = typeof a.p === 'number' ? Math.max(0, Math.min(1, a.p)) : 0;
        const kp = String(a.key || '').split(':');
        right += '<div class="mi-alt">' + flagHtml(a.iso2) + '<span>' + escHtml(a.name)
               + (kp.length >= 2 ? ' <span style="color:#7d8aa3">' + escHtml(kp[0]) + ' ' + escHtml(kp[1]) + '</span>' : '')
               + '</span><span class="mi-alt-bar"><i style="width:' + (p * 100).toFixed(1) + '%"></i></span>'
               + '<span class="mi-alt-p">' + (p * 100 < 1 && p > 0 ? '<1' : Math.round(p * 100)) + '%</span></div>';
      });
      right += '</div>';
    }

    let warn = '';
    if (id.rx_pos_suspect) {
      warn = '<div class="mi-warn">⚠︎ The receiver’s configured position looks inconsistent with what it hears, '
           + 'so distances and this identification may be wrong. Check the GPS position in UberSDR, '
           + 'or NAVTEX_RX_LAT / NAVTEX_RX_LON.</div>';
    } else if (id.rx === null) {
      warn = '<div class="mi-warn">⚠︎ Receiver location unknown — identification could not use distance.</div>';
    }

    /* The map sits under the left column's details, where there is
     * usually room beside the facts, rather than in a row of its own
     * that would push the panel past its height and into a scroll. */
    left += '<div class="mi-map-box" id="mi-map-box"><div class="mi-map" id="mi-map" hidden></div>'
          + '<div class="mi-map-note" id="mi-map-note"></div></div>';
    panel.innerHTML = '<div>' + left + '</div><div>' + right + '</div>' + warn;
    panel.className = 'msg-ident-panel';
    renderIdentMap(id);
  }

  /* ---- Per-message map (Identification panel) ----
   *
   * Where this one message came from: the receiver, the identified
   * sender, and the great-circle path between them, labelled with the
   * distance and bearing the server computed (the label never re-derives
   * them, so it cannot disagree with the panel above).  The path is
   * dashed for a night path, the same convention as the moon mark
   * elsewhere.  Below High confidence the alternative candidates are
   * shown as faint markers, so a doubtful answer looks doubtful.
   *
   * Sender coordinates come from the ident JSON itself; the alternatives'
   * coordinates and the receiver come from /api/stations, which the page
   * already keeps (_stationsData) for the header badges.  When the ident
   * carries the receiver position it was computed with (id.rx), that is
   * used if /api/stations has none, so an old message still draws.
   *
   * Whatever is unknown is simply left off, with a one-line note: an
   * unidentified message shows the receiver (and any candidates), a
   * receiver with no location shows the sender alone.  If Leaflet cannot
   * be loaded at all the map hides and the rest of the panel stands.
   *
   * Each render creates a fresh Leaflet map (the panel's markup is
   * rebuilt on every open) and _miMapSeq discards an async completion
   * that belongs to a message the user has already moved on from. */
  let _miMap = null, _miMapSeq = 0;

  function dropIdentMap() {
    _miMapSeq++;
    if (_miMap) { try { _miMap.remove(); } catch (e) { /* already detached */ } _miMap = null; }
  }

  /* Points along the great circle from a to b ([lat, lon] degrees), by
   * spherical linear interpolation of the two unit vectors.  Longitudes
   * are unwrapped as we go, so a path across 180° stays one continuous
   * line instead of streaking back across the whole map. */
  function greatCirclePoints(a, b) {
    const r = Math.PI / 180;
    const v = function(p) {
      return [Math.cos(p[0] * r) * Math.cos(p[1] * r), Math.cos(p[0] * r) * Math.sin(p[1] * r), Math.sin(p[0] * r)];
    };
    const A = v(a), B = v(b);
    const dot = Math.max(-1, Math.min(1, A[0] * B[0] + A[1] * B[1] + A[2] * B[2]));
    const om = Math.acos(dot);
    if (om < 1e-9) return [a.slice(), b.slice()];
    /* About one vertex per 50 km, bounded: smooth at any zoom we use. */
    const n = Math.max(16, Math.min(128, Math.ceil(om * 6371 / 50)));
    const out = [];
    let prevLon = a[1];
    for (let i = 0; i <= n; i++) {
      const f = i / n, sa = Math.sin((1 - f) * om) / Math.sin(om), sb = Math.sin(f * om) / Math.sin(om);
      const x = sa * A[0] + sb * B[0], y = sa * A[1] + sb * B[1], z = sa * A[2] + sb * B[2];
      const lat = Math.atan2(z, Math.sqrt(x * x + y * y)) / r;
      let lon = Math.atan2(y, x) / r;
      while (lon - prevLon > 180) lon -= 360;
      while (lon - prevLon < -180) lon += 360;
      prevLon = lon;
      out.push([lat, lon]);
    }
    return out;
  }

  function renderIdentMap(id) {
    const seq = ++_miMapSeq;
    const note = function(t) { const el = document.getElementById('mi-map-note'); if (el) el.textContent = t || ''; };
    const haveStations = _stationsData ? Promise.resolve() : fetchStations(false).catch(function() {});
    Promise.all([loadLeaflet(), haveStations]).then(function() {
      if (seq !== _miMapSeq) return;
      const L = window.L, el = document.getElementById('mi-map');
      if (!L || !el) return;

      const known = identLevel(id) !== 'none';
      const sender = known && typeof id.lat === 'number' && typeof id.lon === 'number' ? [id.lat, id.lon] : null;
      const srx = _stationsData && _stationsData.receiver;
      let rx = null;
      if (srx && srx.valid && typeof srx.lat === 'number' && typeof srx.lon === 'number') rx = [srx.lat, srx.lon];
      else if (Array.isArray(id.rx) && typeof id.rx[0] === 'number' && typeof id.rx[1] === 'number') rx = [id.rx[0], id.rx[1]];

      /* Candidates, only when the answer is not already High. */
      const alts = [];
      if (identLevel(id) !== 'high' && Array.isArray(id.alts) && _stationsData && Array.isArray(_stationsData.stations)) {
        const byKey = {};
        _stationsData.stations.forEach(function(s) { if (s.key) byKey[s.key] = s; });
        id.alts.forEach(function(a) {
          const s = a && a.key && byKey[a.key];
          if (s && typeof s.lat === 'number' && typeof s.lon === 'number') alts.push({ a: a, s: s });
        });
      }

      if (!sender && !rx && !alts.length) {
        note('Nothing to show on a map: the transmitter was not identified and the receiver location is unknown.');
        return;
      }
      el.hidden = false;
      _miMap = L.map(el, { worldCopyJump: true, minZoom: 1, zoomSnap: 0.5, attributionControl: true,
                           scrollWheelZoom: false });
      mapTileLayer(L).addTo(_miMap);
      const pts = [];

      alts.forEach(function(x) {
        const p = typeof x.a.p === 'number' ? Math.round(x.a.p * 100) : null;
        L.circleMarker([x.s.lat, x.s.lon], { radius: 4, color: '#8a9ab0', weight: 1, opacity: 0.55,
                                              fillColor: '#8a9ab0', fillOpacity: 0.15 })
          .bindTooltip('Alternative: ' + escHtml(x.s.name) + ' · ' + x.s.freq_khz + ' ' + escHtml(x.s.letter)
                       + (p !== null ? ' · ' + (p < 1 && x.a.p > 0 ? '<1' : p) + '%' : ''), { direction: 'top' })
          .addTo(_miMap);
        /* Below High the alternatives are part of the answer: keep them
         * in view (Grindavik 46% with Cross Corsen off-screen read as a
         * certainty). */
        pts.push([x.s.lat, x.s.lon]);
      });

      let senderAt = sender;
      if (sender && rx) {
        const path = greatCirclePoints(rx, sender);
        senderAt = path[path.length - 1];
        /* The path is drawn as confidently as the answer is held: solid and
         * bright for High, fainter and thinner for Medium, faint for Low —
         * and a Low guess gets no distance label, which would lend it a
         * precision it does not have. */
        const lv = identLevel(id);
        const sty = lv === 'high' ? { weight: 2, opacity: 0.85 }
                  : lv === 'medium' ? { weight: 1.6, opacity: 0.6 } : { weight: 1.2, opacity: 0.35 };
        L.polyline(path, { color: '#53d8fb', weight: sty.weight, opacity: sty.opacity,
                           dashArray: id.night ? '6 6' : null, interactive: false }).addTo(_miMap);
        if (lv !== 'low') {
          const mid = path[Math.floor(path.length / 2)];
          L.tooltip({ permanent: true, direction: 'center', className: 'mi-path-lbl', interactive: false })
            .setLatLng(mid)
            .setContent(fmtKm(id.dist_km) + ' · ' + fmtBearing(id.bearing) + (id.night ? ' · night' : ''))
            .addTo(_miMap);
        }
      }
      if (senderAt) {
        L.marker(senderAt, {
          icon: L.divIcon({ className: '', iconSize: [16, 16], iconAnchor: [8, 8],
                            html: '<div class="mi-sender-icon" style="color:' + levelColour(identLevel(id)) + '"></div>' }),
          zIndexOffset: 900, keyboard: false
        }).bindTooltip(escHtml(id.name) + (id.key ? ' · ' + escHtml(String(id.key).split(':').slice(0, 2).join(' ')) : ''),
                       { direction: 'top', offset: [0, -8] })
          .addTo(_miMap);
        pts.push(senderAt);
      }
      if (rx) {
        L.marker(rx, {
          icon: L.divIcon({ className: '', html: '<div class="rx-icon"></div>', iconSize: [14, 14], iconAnchor: [7, 7] }),
          zIndexOffset: 1000, keyboard: false
        }).bindTooltip('Receiver', { direction: 'top', offset: [0, -7] }).addTo(_miMap);
        pts.push(rx);
      }

      if (pts.length >= 2) _miMap.fitBounds(pts, { padding: [28, 28], maxZoom: 7 });
      else _miMap.setView(pts[0], 5);

      if (!sender && rx) note(alts.length ? 'Transmitter not identified — the receiver and the possible senders are shown.'
                                          : 'Transmitter not identified — only the receiver is shown.');
      else if (sender && !rx) note('Receiver location unknown — only the sender is shown. Set it in UberSDR, or with NAVTEX_RX_LAT / NAVTEX_RX_LON.');
      else note('');
      /* The modal may still have been laying out when the map was made. */
      setTimeout(function() { if (seq === _miMapSeq && _miMap) _miMap.invalidateSize(); }, 50);
    }, function() {
      if (seq !== _miMapSeq) return;
      const box = document.getElementById('mi-map-box');
      if (box) box.hidden = true;
    });
  }

  /* Per-channel first-char flag */
  const firstChar = Array(NUM_CHANNELS).fill(true);

  /* Per-channel previous 'act' state for alert-sound edge detection */
  const prevActState = Array(NUM_CHANNELS).fill(0);

  /* Play a short ding using the Web Audio API */
  function playAlertDing() {
    try {
      const ctx = new (window.AudioContext || window.webkitAudioContext)();
      const osc = ctx.createOscillator();
      const gain = ctx.createGain();
      osc.connect(gain);
      gain.connect(ctx.destination);
      osc.type = 'sine';
      osc.frequency.setValueAtTime(880, ctx.currentTime);
      osc.frequency.exponentialRampToValueAtTime(660, ctx.currentTime + 0.15);
      gain.gain.setValueAtTime(0.5, ctx.currentTime);
      gain.gain.exponentialRampToValueAtTime(0.001, ctx.currentTime + 0.5);
      osc.start(ctx.currentTime);
      osc.stop(ctx.currentTime + 0.5);
      osc.onended = function() { ctx.close(); };
    } catch(e) {}
  }

  /* Per-channel debounce timers for decoder state display */
  const decStateTimer     = Array(NUM_CHANNELS).fill(null);
  const decStatePending   = Array(NUM_CHANNELS).fill(null);
  const decStateCommitted = Array(NUM_CHANNELS).fill(null); /* last value shown in UI */

  /* ---- Latest tab ---- */
  var _latestPollTimer = null;

  function fetchLatest() {
    var base = (window._BASE_PATH || '').replace(/\/$/, '');
    var content = document.getElementById('latest-content');
    var lastUpd = document.getElementById('latest-last-updated');
    if (content) content.innerHTML = '<div class="latest-empty">Loading\u2026</div>';
    fetch(base + '/api/latest')
      .then(function(r) { return r.json(); })
      .then(function(data) {
        renderLatestPanel(data);
        if (lastUpd) {
          var d = new Date();
          lastUpd.textContent = 'Updated ' + d.toISOString().slice(11,19) + 'Z';
        }
      })
      .catch(function(err) {
        if (content)
          content.innerHTML = '<div class="latest-empty">Failed to load: ' + err + '</div>';
      });
  }

  function renderLatestPanel(data) {
    var content = document.getElementById('latest-content');
    if (!content) return;

    if (!data || data.length === 0) {
      content.innerHTML = '<div class="latest-empty">No complete messages received yet.</div>';
      return;
    }

    /* Group by freq, preserving order of first appearance */
    var freqOrder = [];
    var byFreq = {};
    data.forEach(function(m) {
      if (!byFreq[m.freq]) {
        byFreq[m.freq] = [];
        freqOrder.push(m.freq);
      }
      byFreq[m.freq].push(m);
    });

    var html = '';
    _latestRows = [];
    freqOrder.forEach(function(freq) {
      var rows = byFreq[freq];
      html += '<div class="latest-freq-section">';
      html += '<div class="latest-freq-title">' + freq + '</div>';
      html += '<table class="latest-table"><thead><tr>'
            + '<th>Station</th><th>Subject</th><th>Serial</th>'
            + '<th>Received (UTC)</th><th>Distance</th><th>SNR</th><th></th>'
            + '</tr></thead><tbody>';

      rows.forEach(function(m) {
        var subCode = m.subject || '';
        var subName = SUBJECT[subCode] || '';
        var subDisp = subCode + (subName ? ' \u2013 ' + subName : '');
        var serialDisp = (m.serial !== null && m.serial !== undefined) ? String(m.serial).padStart(2,'0') : '\u2014';
        var tsDisp = (m.timestamp || '').replace('T',' ').replace('Z',' UTC');
        var snrDisp = (m.snr_db !== null && m.snr_db !== undefined)
                    ? Number(m.snr_db).toFixed(1) + ' dB' : '\u2014';
        /* Not named snrClass: that is a page-scope function now, and a var of
         * the same name would shadow it for this whole callback. */
        var snrCellCls = 'lt-snr ' + snrCls(m.snr_db);

        /* Station: flag + name + letter + pill when the server sent an
         * identification; just the letter from an older server. */
        const id = m.ident || null;
        const stCell = id
          ? '<td class="lt-ident">' + stationChipHtml(id, { letter: m.station || '' }) + '</td>'
          : '<td class="lt-station">' + escHtml(m.station || '\u2014') + '</td>';
        const distCell = (id && typeof id.dist_km === 'number' && id.dist_km >= 0)
          ? fmtKm(id.dist_km) + '<span class="dist-brg">' + compassPoint(id.bearing) + '</span>'
            + (id.night ? '<span class="dist-night" title="Night path">\u263E</span>' : '')
          : '<span style="color:#446">\u2014</span>';
        const title = freq + ' \u00b7 ' + (m.station||'') + (m.subject||'') + (serialDisp !== '\u2014' ? serialDisp : '')
                    + (id && id.key && id.name ? ' \u00b7 ' + id.name : '');
        _latestRows.push({ text: m.text || '', title: title, ident: id });

        html += '<tr>'
              + stCell
              + '<td class="lt-subject">' + escHtml(subDisp) + '</td>'
              + '<td class="lt-serial">'  + serialDisp + '</td>'
              + '<td class="lt-ts">'      + escHtml(tsDisp) + '</td>'
              + '<td class="lt-dist" style="font-family:monospace;white-space:nowrap">' + distCell + '</td>'
              + '<td class="' + snrCellCls + '">' + snrDisp + '</td>'
              + '<td><button class="latest-view-btn" data-row="' + (_latestRows.length - 1) + '">View</button></td>'
              + '</tr>';
      });

      html += '</tbody></table></div>';
    });

    content.innerHTML = html;

    /* Wire View buttons */
    content.querySelectorAll('.latest-view-btn').forEach(function(btn) {
      btn.addEventListener('click', function() {
        var row = _latestRows[Number(btn.getAttribute('data-row'))];
        if (row) viewLatestMessage(row.text, row.title || 'Message', row.ident);
      });
    });
  }
  /* Rows behind the View buttons (text, title, ident) — kept here rather
   * than URI-encoded into attributes, now that each carries an object. */
  var _latestRows = [];

  function viewLatestMessage(text, title, ident) {
    var modal   = document.getElementById('msg-modal');
    var body    = document.getElementById('msg-modal-body');
    var ttl     = document.getElementById('msg-modal-title');
    var copyBtn = document.getElementById('msg-modal-copy-btn');
    var dlBtn   = document.getElementById('msg-modal-dl-btn');
    var panel   = document.getElementById('msg-metrics-panel');
    if (!modal || !body) return;
    ttl.textContent  = title || 'Message';
    body.textContent = text;
    if (panel) panel.className = 'msg-metrics-panel hidden';
    renderIdentPanel(ident || null);
    if (copyBtn) {
      copyBtn.style.opacity = '1';
      copyBtn.onclick = function() {
        if (navigator.clipboard && navigator.clipboard.writeText)
          navigator.clipboard.writeText(text).catch(function() { fallbackCopy(text); });
        else
          fallbackCopy(text);
      };
    }
    if (dlBtn) {
      dlBtn.style.opacity = '1';
      dlBtn.onclick = function() {
        var blob = new Blob([text], { type: 'text/plain' });
        var url  = URL.createObjectURL(blob);
        var a    = document.createElement('a');
        a.href     = url;
        a.download = 'navtex-latest-' + new Date().toISOString().slice(0,19).replace(/:/g,'-') + '.txt';
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
      };
    }
    modal.style.display = 'flex';
  }

  /* ---- Tab switching ---- */
  let activeTab = 'both';
  let _stnPollTimer = null;
  let splitBuilt = false;

  function buildSplitPanel() {
    if (splitBuilt) return;
    splitBuilt = true;
    const both = document.getElementById('panel-both');
    for (let i = 0; i < NUM_CHANNELS; i++) {
      const srcPanel = document.getElementById('panel-' + i);
      /* Build a column wrapper */
      const col = document.createElement('div');
      col.className = 'split-col';
      col.id = 'split-col-' + i;

      /* Column header showing frequency label */
      const hdr = document.createElement('div');
      hdr.className = 'split-col-header';
      hdr.innerHTML = '<span class="tab-dot" id="split-dot-' + i + '"></span>'
                    + srcPanel.querySelector('.stats-bar') ? '' : '';
      /* Get label from the tab button text */
      const tabBtn = document.getElementById('tab-btn-' + i);
      hdr.innerHTML = '<span class="tab-dot" id="split-dot-' + i + '"></span>'
                    + (tabBtn ? tabBtn.innerText.trim() : 'Channel ' + i);
      col.appendChild(hdr);

      /* Column body: clone the stats-bar, msg-bar, output from the real panel */
      const body = document.createElement('div');
      body.className = 'split-col-body';
      body.id = 'split-body-' + i;

      /* Clone stats-bar */
      const statsBar = srcPanel.querySelector('.stats-bar');
      if (statsBar) {
        const clone = statsBar.cloneNode(true);
        /* Suffix all IDs in the clone with -s (split) */
        clone.querySelectorAll('[id]').forEach(function(el) {
          el.id = el.id + '-s';
        });
        clone.id = 'split-stats-' + i;
        body.appendChild(clone);
      }

      /* Clone msg-bar — strip onclick attrs so cloned buttons don't fire stale handlers */
      const msgBar = srcPanel.querySelector('.msg-bar');
      if (msgBar) {
        const clone = msgBar.cloneNode(true);
        clone.querySelectorAll('[id]').forEach(function(node) {
          node.id = node.id + '-s';
        });
        /* Remove any inline onclick on cloned buttons (will be re-wired below) */
        clone.querySelectorAll('button[onclick]').forEach(function(node) {
          node.removeAttribute('onclick');
        });
        clone.id = 'split-msg-' + i;
        body.appendChild(clone);
      }

      /* Clone output */
      const output = srcPanel.querySelector('.output');
      if (output) {
        const clone = output.cloneNode(true);
        clone.id = 'split-output-' + i;
        body.appendChild(clone);
      }

      col.appendChild(body);
      both.appendChild(col);
    }

    /* MutationObserver: keep split output areas in sync with real output areas */
    for (let i = 0; i < NUM_CHANNELS; i++) {
      (function(ch) {
        const src  = document.getElementById('output-' + ch);
        const dest = document.getElementById('split-output-' + ch);
        if (!src || !dest) return;
        const obs = new MutationObserver(function() {
          dest.innerHTML = src.innerHTML;
          dest.scrollTop = dest.scrollHeight;
        });
        obs.observe(src, { childList: true, subtree: true, characterData: true });
      })(i);
    }

    /* Wire split-clone action buttons (clear/copy/download/audio) now that they exist */
    if (window._activeWs) {
      for (let i = 0; i < NUM_CHANNELS; i++) wireSplitButtons(i, window._activeWs);
    }
  }

  /* Wire the cloned action buttons in the split panel for one channel */
  function wireSplitButtons(ch, ws) {
    /* Clear / Copy / Download — re-wire onclick on the -s clones */
    const clearBtn = document.getElementById('clear-btn-' + ch + '-s');
    if (clearBtn) clearBtn.onclick = function() { clearOutput(ch); };
    const copyBtn = document.getElementById('copy-btn-' + ch + '-s');
    if (copyBtn) copyBtn.onclick = function() { copyOutput(ch); };
    const dlBtn = document.getElementById('dl-btn-' + ch + '-s');
    if (dlBtn) dlBtn.onclick = function() { downloadOutput(ch); };

    /* Per-channel history button */
    const splitHistBtn = document.getElementById('hist-ch-btn-' + ch + '-s');
    if (splitHistBtn) splitHistBtn.onclick = function() { openHistoryForChannel(ch); };

    /* Audio button */
    const splitAudioBtn = document.getElementById('audio-btn-' + ch + '-s');
    if (splitAudioBtn) {
      splitAudioBtn.onclick = function() {
        if (audioState[ch].enabled) audioDisable(ch, ws);
        else audioEnable(ch, ws);
      };
    }
  }

  function syncSplitStats(ch) {
    /* Copy text content of every stat-value from real panel to split clone */
    const suffixes = ['bb-val','nd-val','rate-val','dec-state','fec-val','err-val',
                      'act-dot','act-text','snr-val','sig-fill','snr-fill',
                      'fec-clean','fec-fec','fec-fail',
                      'msg-dot','msg-status','msg-station','msg-subject','msg-serial'];
    suffixes.forEach(function(name) {
      const src  = document.getElementById(name + '-' + ch);
      const dest = document.getElementById(name + '-' + ch + '-s');
      if (!src || !dest) return;
      if (name === 'msg-station') {
        /* Flag + name + pill markup, not just text.  Copied only when it
         * changed, so the flag image is not reloaded on every frame. */
        if (dest._identHtml !== src.innerHTML) { dest._identHtml = src.innerHTML; dest.innerHTML = src.innerHTML; }
        dest.className = src.className;
        return;
      }
      dest.textContent = src.textContent;
      dest.className   = src.className;
      if (src.style && src.style.width !== undefined) dest.style.width = src.style.width;
      if (src.style && src.style.marginLeft !== undefined) dest.style.marginLeft = src.style.marginLeft;
      if (src.style && src.style.background !== undefined) dest.style.background = src.style.background;
    });
    /* The message bar's own class (idle = dimmed) — its clone is renamed
     * split-msg-N, so the suffix loop above does not reach it. */
    const msgBar   = document.getElementById('msg-bar-' + ch);
    const msgClone = document.getElementById('split-msg-' + ch);
    if (msgBar && msgClone) msgClone.className = msgBar.className;
    /* Sync tab-dot -> split-dot */
    const tabDot   = document.getElementById('tab-dot-' + ch);
    const splitDot = document.getElementById('split-dot-' + ch);
    if (tabDot && splitDot) splitDot.className = tabDot.className;
  }

  function switchTab(ch) {
    /* Disable any active audio preview when switching tabs */
    if (window._activeWs) {
      for (let i = 0; i < NUM_CHANNELS; i++) {
        if (audioState[i] && audioState[i].enabled) audioDisable(i, window._activeWs);
      }
    }
    /* Hide all individual panels and deactivate all buttons */
    for (let i = 0; i < NUM_CHANNELS; i++) {
      const btn   = document.getElementById('tab-btn-' + i);
      const panel = document.getElementById('panel-' + i);
      if (btn)   btn.classList.remove('active');
      if (panel) panel.style.display = 'none';
    }
    const bothBtn    = document.getElementById('tab-btn-both');
    const bothPanel  = document.getElementById('panel-both');
    const latestBtn  = document.getElementById('tab-btn-latest');
    const latestPanel= document.getElementById('panel-latest');
    if (bothBtn)    bothBtn.classList.remove('active');
    if (bothPanel)  bothPanel.style.display = 'none';
    if (latestBtn)  latestBtn.classList.remove('active');
    if (latestPanel)latestPanel.style.display = 'none';
    const stnBtn     = document.getElementById('tab-btn-stations');
    const stnPanel   = document.getElementById('panel-stations');
    if (stnBtn)     stnBtn.classList.remove('active');
    if (stnPanel)   stnPanel.style.display = 'none';
    if (_stnPollTimer !== null) { clearInterval(_stnPollTimer); _stnPollTimer = null; }

    /* Stop any existing latest poll timer when leaving the tab */
    if (_latestPollTimer !== null) {
      clearInterval(_latestPollTimer);
      _latestPollTimer = null;
    }

    if (ch === 'both') {
      buildSplitPanel();
      /* Sync current state into split clones */
      for (let i = 0; i < NUM_CHANNELS; i++) syncSplitStats(i);
      if (bothBtn)   bothBtn.classList.add('active');
      if (bothPanel) bothPanel.style.display = 'flex';
    } else if (ch === 'latest') {
      if (latestBtn)   latestBtn.classList.add('active');
      if (latestPanel) latestPanel.style.display = 'flex';
      /* Fetch immediately on tab entry, then poll every 60 s */
      fetchLatest();
      _latestPollTimer = setInterval(fetchLatest, 60000);
    } else if (ch === 'stations') {
      if (stnBtn)   stnBtn.classList.add('active');
      if (stnPanel) stnPanel.style.display = 'flex';
      activeTab = ch;
      enterStationsTab();
      /* Refresh while visible (the header poll is only every 5 min). */
      _stnPollTimer = setInterval(function() { fetchStations(false); }, 60000);
    } else {
      const btn   = document.getElementById('tab-btn-' + ch);
      const panel = document.getElementById('panel-' + ch);
      if (btn)   btn.classList.add('active');
      if (panel) panel.style.display = 'flex';
    }
    activeTab = ch;
    /* The Stations tab is bookmarkable as #stations. */
    if (ch === 'stations') { if (location.hash !== '#stations') history.replaceState(null, '', '#stations'); }
    else if (location.hash === '#stations') clearHash();
  }

  /* After stats/msg updates, also refresh split clones if Both tab is active */
  function maybeSyncSplit(ch) {
    if (activeTab === 'both' && splitBuilt) syncSplitStats(ch);
  }

  /* ---------------------------------------------------------------
   * Live identification (message bar).
   *
   * The server sends WS frame 0x05 [0x05][ch][JSON ident] twice per
   * message: provisionally once the ZCZC header is decoded, and finally
   * when the message completes.  Until the first one arrives the bar
   * shows the B1 letter and "identifying…".  A new message start clears
   * the previous answer.
   *
   * Every 0x05 frame carries msg_id — "EA02", built by the server exactly
   * as liveMsgId[] is built here from the 0x03 frame (letter, subject,
   * two-digit serial).  A frame whose msg_id is not the message on screen
   * is a late answer for an earlier one and is ignored, as is a
   * provisional answer arriving after the final one.  Matching the letter
   * alone was not enough: when the worker is backlogged (startup replay),
   * the final for EA01 can arrive after EB02 has started — same letter —
   * and an unidentified answer has no letter to compare at all.
   *
   * If the page connects while a message is already complete, no 0x05
   * for it is coming; after a short wait the answer is looked up in
   * /api/latest (same freq/letter/subject/serial) instead, once.
   * --------------------------------------------------------------- */
  const liveIdent   = Array(NUM_CHANNELS).fill(null);
  const liveLetter  = Array(NUM_CHANNELS).fill('');
  const liveMsgKey  = Array(NUM_CHANNELS).fill('');
  const liveMsgId   = Array(NUM_CHANNELS).fill('');   /* server form: "EA02" */
  const liveFallbackT    = Array(NUM_CHANNELS).fill(null);
  const liveFallbackDone = Array(NUM_CHANNELS).fill('');

  function chFreqLabel(ch) { return (CH_FREQ_LABELS && CH_FREQ_LABELS[ch]) || ''; }

  function renderLiveStation(ch) {
    const node = chEl('msg-station', ch);
    if (!node) return;
    const letter = liveLetter[ch];
    const id = liveIdent[ch];
    let html;
    if (!letter)  html = '—';
    else if (!id) html = pendingHtml(letter);
    else          html = stationChipHtml(id, { letter: letter, chipPop: true });
    /* Only touch the DOM when the content changes: msg-info frames repeat,
     * and rewriting would reload the flag and drop an open pop-over. */
    if (node._identHtml !== html) {
      node._identHtml = html;
      node.innerHTML = html;
    }
  }

  /* Called by updateMsgInfo() with the current frame's fields. */
  function liveIdentOnMsgInfo(ch, inMsg, done, sta, sub, ser, isStart) {
    const letter = (inMsg || done) && sta ? String.fromCharCode(sta) : '';
    const key = letter ? letter + (sub ? String.fromCharCode(sub) : '?') + ser : '';
    liveMsgId[ch] = letter && sub
      ? letter + String.fromCharCode(sub) + (ser !== 0xFF ? String(ser).padStart(2, '0') : '')
      : '';
    if (isStart || key !== liveMsgKey[ch]) {
      /* A different message: forget the previous answer. */
      if (isStart || (liveMsgKey[ch] && key && key !== liveMsgKey[ch])) liveIdent[ch] = null;
      liveMsgKey[ch] = key;
    }
    liveLetter[ch] = letter;
    if (!inMsg && !done) liveIdent[ch] = null;
    renderLiveStation(ch);

    clearTimeout(liveFallbackT[ch]);
    if (done && !liveIdent[ch] && key && liveFallbackDone[ch] !== key) {
      liveFallbackT[ch] = setTimeout(function() { liveIdentFallback(ch, key, sta, sub, ser); }, 6000);
    }
  }

  function liveIdentFallback(ch, key, sta, sub, ser) {
    if (liveIdent[ch] || liveMsgKey[ch] !== key) return;
    liveFallbackDone[ch] = key;
    fetch(apiBase() + '/api/latest')
      .then(function(r) { return r.ok ? r.json() : []; })
      .then(function(list) {
        if (liveIdent[ch] || liveMsgKey[ch] !== key) return;
        const want = chFreqLabel(ch);
        const hit = (list || []).find(function(m) {
          return (m.freq || '').replace(/\s/g, '') === want
              && m.station === String.fromCharCode(sta)
              && m.subject === String.fromCharCode(sub)
              && (ser === 0xFF ? m.serial === null : m.serial === ser);
        });
        if (hit && hit.ident) { liveIdent[ch] = hit.ident; renderLiveStation(ch); maybeSyncSplit(ch); }
      })
      .catch(function() {});
  }

  /* WS frame 0x05. */
  let _latestRefreshT = null;
  function onIdentFrame(ch, id) {
    if (ch < 0 || ch >= NUM_CHANNELS || !id) return;
    if (typeof id.msg_id === 'string') {
      if (id.msg_id !== liveMsgId[ch]) return;                           /* stale */
    } else {
      const letter = id.key ? String(id.key).split(':')[1] : '';
      if (liveLetter[ch] && letter && letter !== liveLetter[ch]) return;
    }
    const cur = liveIdent[ch];
    if (cur && !cur.provisional && id.provisional) return;               /* final already shown */
    liveIdent[ch] = id;
    renderLiveStation(ch);
    maybeSyncSplit(ch);
    /* A final answer changes Latest / Stations; refresh them if visible. */
    if (!id.provisional) {
      clearTimeout(_latestRefreshT);
      _latestRefreshT = setTimeout(function() {
        if (activeTab === 'latest') fetchLatest();
        if (activeTab === 'stations') fetchStations(false);
      }, 1500);
    }
  }

  /* ---------------------------------------------------------------
   * /api/stations poll: receiver location + history-replay progress for
   * the header, and the data behind the Stations tab.
   *
   * Polled every 2 s while the server is still replaying history
   * (identifying every stored message in order on startup), otherwise
   * every 5 min so a receiver-location change shows up.  When replay
   * finishes, an open History list is reloaded so its "identifying…"
   * rows fill in.
   * --------------------------------------------------------------- */
  let _stationsData = null;
  let _stationsPollT = null;
  let _stationsFetchedAt = 0;
  let _replayWasDone = null;
  let _stationsUnavailable = false;

  function fetchStations(userInitiated) {
    clearTimeout(_stationsPollT);
    return fetch(apiBase() + '/api/stations')
      .then(function(r) {
        if (r.status === 404) { _stationsUnavailable = true; return null; }
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(function(d) {
        if (!d) { renderIdentHeader(); renderStations(); return; }
        _stationsUnavailable = false;
        _stationsData = d;
        _stationsFetchedAt = Date.now();
        const done = !d.replay || d.replay.done !== false;
        if (_replayWasDone === false && done) onReplayFinished();
        _replayWasDone = done;
        renderIdentHeader();
        if (activeTab === 'stations' || userInitiated) renderStations();
        _stationsPollT = setTimeout(fetchStations, done ? 300000 : 2000);
      })
      .catch(function() {
        renderIdentHeader();
        if (activeTab === 'stations') renderStations('error');
        _stationsPollT = setTimeout(fetchStations, 30000);
      });
  }

  function onReplayFinished() {
    const modal = document.getElementById('history-modal');
    if (modal && modal.style.display !== 'none') reloadHistoryKeepingView();
  }

  function renderIdentHeader() {
    const box = document.getElementById('ident-hdr');
    if (!box) return;
    const d = _stationsData;
    if (!d) { box.innerHTML = ''; return; }
    let html = '';
    const rp = d.replay;
    if (rp && rp.done === false) {
      const pct = rp.total > 0 ? Math.min(100, rp.processed / rp.total * 100) : 0;
      html += '<span class="hdr-badge hb-replay" title="Identifying the transmitter of every stored message, oldest first. '
            + 'Live messages are identified as soon as this finishes.">'
            + 'Identifying history ' + fmtInt(rp.processed || 0) + ' / ' + fmtInt(rp.total || 0)
            + '<span class="hb-bar"><i style="width:' + pct.toFixed(1) + '%"></i></span></span>';
    }
    const rx = d.receiver || {};
    const howTo = 'Set the receiver’s GPS position in UberSDR’s configuration, '
                + 'or set NAVTEX_RX_LAT and NAVTEX_RX_LON in this add-on’s environment.';
    if (!rx.valid) {
      html += '<span class="hdr-badge hb-warn" data-pop-title="Receiver location unknown" data-pop-text="'
            + escHtml('Without the receiver’s position, stations sharing a letter can only be told apart by '
            + 'schedule, so identification confidence is limited and distances are not shown. ' + howTo)
            + '">⚠︎ Location unknown</span>';
    } else if (rx.pos_suspect) {
      html += '<span class="hdr-badge hb-warn" data-pop-title="Receiver location looks wrong" data-pop-text="'
            + escHtml('Stations are being heard that do not fit the configured position ('
            + (rx.locator || (Number(rx.lat).toFixed(2) + ', ' + Number(rx.lon).toFixed(2)))
            + ', from ' + (rx.source === 'env' ? 'NAVTEX_RX_LAT/LON' : 'UberSDR') + '). '
            + 'If it is wrong, identifications will be too. ' + howTo)
            + '">⚠︎ Location suspect</span>';
    }
    if (box._html !== html) { box._html = html; box.innerHTML = html; }
  }

  /* ---------------------------------------------------------------
   * Stations tab: identified transmitters on a map and in a table.
   *
   * The map is Leaflet 1.9.4 from cdnjs, loaded only when this tab is
   * first opened.  Its base tiles come from mapTileLayer() below, shared
   * with the Coverage map and the message modal's map.  Many receivers sit on a
   * LAN with no internet access, so the map is strictly optional: if
   * Leaflet does not load within a few seconds the map box is hidden,
   * a one-line note says why, and the table — which needs nothing
   * external — carries on alone.
   *
   * Stations that share a site (Niton sends as E and K on 518 kHz and
   * as I on 490 kHz) get one marker listing each of them, so markers
   * never hide one another.
   * --------------------------------------------------------------- */
  const LEAFLET_JS  = 'https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.min.js';
  const LEAFLET_CSS = 'https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.min.css';
  let _leafletPromise = null;
  let _stnMap = null, _stnLayers = null, _stnMarkers = {}, _stnFitted = false;
  let _stnSort = { key: 'count', dir: -1 };
  let _stnRows = [];

  function loadLeaflet() {
    if (_leafletPromise) return _leafletPromise;
    _leafletPromise = new Promise(function(resolve, reject) {
      if (window.L && window.L.map) { resolve(window.L); return; }
      /* Inserted BEFORE the page's own <style> so the dark-theme
       * overrides there win over Leaflet's defaults at equal specificity. */
      const css = document.createElement('link');
      css.rel = 'stylesheet'; css.href = LEAFLET_CSS;
      document.head.insertBefore(css, document.head.querySelector('style'));
      const js = document.createElement('script');
      js.src = LEAFLET_JS; js.async = true;
      const t = setTimeout(function() { reject(new Error('timeout')); }, 8000);
      js.onload  = function() { clearTimeout(t); window.L ? resolve(window.L) : reject(new Error('no L')); };
      js.onerror = function() { clearTimeout(t); reject(new Error('blocked')); };
      document.head.appendChild(js);
    });
    return _leafletPromise;
  }

  /* Base tiles for every map on the page.
   *
   * OpenStreetMap's standard tiles by default.  CARTO's dark basemap now
   * answers every keyless request with an "API KEY REQUIRED" placeholder
   * (HTTP 200, so it cannot even be detected and fallen back from), and
   * Esri's terms for keyless use in a redistributed app are unclear, so
   * the light OSM tiles are darkened instead: the .map-tiles-dark CSS
   * filter applies to the tile layer only, which leaves the markers and
   * lines drawn over it in their real colours.
   *
   * An operator who would rather not lean on tile.openstreetmap.org (its
   * usage policy asks heavy users to run their own), or who has a tile
   * server on the LAN, sets NAVTEX_MAP_TILES and NAVTEX_MAP_ATTRIBUTION;
   * the server injects them as window._MAP_TILES / _MAP_ATTRIBUTION.
   * Nothing is prefetched beyond what Leaflet asks for itself. */
  function mapTileLayer(L) {
    const url = (typeof window._MAP_TILES === 'string' && window._MAP_TILES)
      || 'https://tile.openstreetmap.org/{z}/{x}/{y}.png';
    const attr = (typeof window._MAP_ATTRIBUTION === 'string' && window._MAP_ATTRIBUTION)
      || '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors';
    return L.tileLayer(url, { maxZoom: 19, attribution: attr, className: 'map-tiles-dark' });
  }

  function enterStationsTab() {
    if (_stationsData && Date.now() - _stationsFetchedAt < 30000) renderStations();
    else {
      const w = document.getElementById('stn-table-wrap');
      if (w && !_stationsData) w.innerHTML = '<div class="latest-empty">Loading…</div>';
      fetchStations(true);
    }
    loadLeaflet().then(function() {
      if (activeTab === 'stations') renderStations();
    }).catch(function() {
      const wrap = document.getElementById('stn-map-wrap');
      if (wrap) wrap.hidden = true;
      showStnNote('Map unavailable — the map library could not be loaded from cdnjs.cloudflare.com '
                + '(no internet access from this browser?). The station list below is unaffected.');
    });
  }

  function showStnNote(text) {
    const n = document.getElementById('stn-note');
    if (!n) return;
    n.hidden = !text;
    n.textContent = text || '';
  }

  /* Marker colour from how recently the station was last heard. */
  function recencyColour(iso) {
    const t = Date.parse(iso || '');
    if (!isFinite(t)) return '#6a7a8a';
    const age = (Date.now() - t) / 1000;
    return age <= 3600 ? '#4caf50' : age <= 86400 ? '#53d8fb' : age <= 7 * 86400 ? '#ffc107' : '#6a7a8a';
  }

  /* "Unheard stations nearby": anything within twice the station's
   * published range — MF NAVTEX routinely carries well past its nominal
   * range at night — or every station when the receiver position is
   * unknown and distance cannot be judged. */
  function unheardInRange(s, rxValid) {
    if (!rxValid || typeof s.dist_km !== 'number' || s.dist_km < 0) return true;
    return s.dist_km <= Math.max(500, (s.range_nm || 300) * 1.852 * 2);
  }

  function renderStations(errState) {
    const wrap = document.getElementById('stn-table-wrap');
    const sum  = document.getElementById('stn-summary');
    const upd  = document.getElementById('stn-last-updated');
    if (!wrap) return;
    if (_stationsUnavailable) {
      wrap.innerHTML = '<div class="latest-empty">Station identification is not available from this server.</div>';
      const mw = document.getElementById('stn-map-wrap'); if (mw) mw.hidden = true;
      return;
    }
    const d = _stationsData;
    if (!d) {
      wrap.innerHTML = '<div class="latest-empty">' + (errState ? 'Failed to load stations.' : 'Loading…') + '</div>';
      return;
    }
    const rx = d.receiver || {};
    const all = Array.isArray(d.stations) ? d.stations : [];
    const heard = all.filter(function(s) { return s.heard && s.heard.count > 0; });
    const showUnheard = !!(document.getElementById('stn-unheard-toggle') || {}).checked;

    const countries = new Set(heard.map(function(s) { return s.iso2 || s.country; }));
    let total = 0; heard.forEach(function(s) { total += s.heard.count; });
    if (sum) sum.textContent = heard.length + ' heard · ' + countries.size + ' countr' + (countries.size === 1 ? 'y' : 'ies')
                             + ' · ' + fmtInt(total) + ' messages';
    if (upd) upd.textContent = 'Updated ' + new Date(_stationsFetchedAt).toISOString().slice(11, 19) + 'Z';

    /* ---- table ---- */
    const rows = heard.slice();
    const k = _stnSort.key, dir = _stnSort.dir;
    const val = function(s) {
      switch (k) {
        case 'name':    return (s.name || '').toLowerCase();
        case 'freq':    return s.freq_khz * 100 + s.letter.charCodeAt(0);
        case 'country': return (s.country_name || '').toLowerCase();
        case 'dist':    return typeof s.dist_km === 'number' && s.dist_km >= 0 ? s.dist_km : null;
        case 'bearing': return typeof s.bearing === 'number' && s.bearing >= 0 ? s.bearing : null;
        case 'count':   return s.heard.count;
        case 'high':    return s.heard.count ? s.heard.high / s.heard.count : 0;
        case 'last':    return s.heard.last_utc || '';
        case 'snr':     return typeof s.heard.best_snr === 'number' ? s.heard.best_snr : null;
      }
      return 0;
    };
    rows.sort(function(a, b) {
      const x = val(a), y = val(b);
      if (x === null || y === null) return x === y ? 0 : x === null ? 1 : -1;
      return x < y ? -dir : x > y ? dir : b.heard.count - a.heard.count;
    });
    let extra = [];
    if (showUnheard) {
      extra = all.filter(function(s) { return !(s.heard && s.heard.count > 0) && unheardInRange(s, rx.valid); })
                 .sort(function(a, b) { return (a.dist_km || 0) - (b.dist_km || 0) || (a.name < b.name ? -1 : 1); });
    }
    _stnRows = rows.concat(extra);

    const cols = [
      ['name', 'Station', ''], ['freq', 'Freq / ID', ''], ['country', 'Country', ''],
      ['dist', 'Distance', 'num'], ['bearing', 'Bearing', 'num'], ['count', 'Messages', 'num'],
      ['high', 'High conf.', 'num'], ['last', 'Last heard', ''], ['snr', 'Best SNR', 'num']
    ];
    let html = '<table class="latest-table stn-table"><thead><tr>';
    cols.forEach(function(c) {
      const on = c[0] === k;
      html += '<th class="sortable' + (c[2] ? ' ' + c[2] : '') + (on ? ' sorted' : '') + '" data-sort="' + c[0] + '"'
            + ' tabindex="0" aria-sort="' + (on ? (dir < 0 ? 'descending' : 'ascending') : 'none') + '">'
            + c[1] + '<span class="sort-ind">' + (on ? (dir < 0 ? '▼' : '▲') : '') + '</span></th>';
    });
    if (LOGGING_ENABLED) html += '<th></th>';
    html += '</tr></thead><tbody>';
    if (!heard.length) {
      html += '<tr class="stn-sep"><td colspan="10">'
            + (d.replay && d.replay.done === false ? 'Identifying stored messages…' : 'No stations identified yet.')
            + '</td></tr>';
    }
    _stnRows.forEach(function(s, i) {
      if (i === rows.length && extra.length)
        html += '<tr class="stn-sep"><td colspan="10">Not heard'
              + (rx.valid ? ' — within twice their published range' : '') + ' (' + extra.length + ')</td></tr>';
      const h = s.heard;
      const isHeard = !!(h && h.count > 0);
      const lowOnly = isHeard && !stationTrusted(s);
      html += '<tr data-i="' + i + '"' + (!isHeard ? ' class="unheard"'
              : lowOnly ? ' class="lowonly" title="Only low-confidence attributions — possibly never really heard"' : '') + '>'
            + '<td><span class="stchip">' + flagHtml(s.iso2) + '<span class="st-name">' + escHtml(s.name) + '</span></span></td>'
            + '<td class="mono">' + s.freq_khz + ' ' + escHtml(s.letter) + '</td>'
            + '<td>' + escHtml(s.country_name || s.country || '') + '</td>'
            + '<td class="num">' + fmtKm(s.dist_km) + '</td>'
            + '<td class="num">' + fmtBearing(s.bearing) + '</td>'
            + '<td class="num">' + (isHeard ? fmtInt(h.count) : '—') + '</td>'
            + '<td class="num">' + (isHeard ? Math.round(h.high / h.count * 100) + '%' : '—') + '</td>'
            + '<td title="' + escHtml(isHeard ? (h.last_utc || '') : '') + '">'
            + (isHeard ? '<span class="rec-dot" style="background:' + recencyColour(h.last_utc) + '"></span>' + fmtAgo(h.last_utc) : '—') + '</td>'
            + '<td class="num ' + (isHeard ? 'lt-snr ' + snrCls(h.best_snr) : '') + '">'
            + (isHeard && typeof h.best_snr === 'number' ? h.best_snr.toFixed(1) + ' dB' : '—') + '</td>'
            + (LOGGING_ENABLED ? '<td>' + (isHeard ? '<button class="latest-view-btn" data-hist="' + escHtml(s.key) + '">History</button>' : '') + '</td>' : '')
            + '</tr>';
    });
    html += '</tbody></table>';
    wrap.innerHTML = html;

    renderStationsMap(heard, all, rx, showUnheard);
  }

  /* Table clicks: header sorts, History opens the filtered list, any
   * other click on a row shows that station on the map. */
  (function() {
    const wrap = document.getElementById('stn-table-wrap');
    if (!wrap) return;
    wrap.addEventListener('click', function(ev) {
      const th = ev.target.closest('th.sortable');
      if (th) {
        const key = th.getAttribute('data-sort');
        if (_stnSort.key === key) _stnSort.dir = -_stnSort.dir;
        else _stnSort = { key: key, dir: (key === 'name' || key === 'freq' || key === 'country' || key === 'dist' || key === 'bearing') ? 1 : -1 };
        renderStations();
        return;
      }
      const hb = ev.target.closest('[data-hist]');
      if (hb) { openHistoryForStation(hb.getAttribute('data-hist')); return; }
      const tr = ev.target.closest('tr[data-i]');
      if (!tr) return;
      const s = _stnRows[Number(tr.getAttribute('data-i'))];
      if (s) focusStationOnMap(s);
    });
  })();

  function siteKey(s) { return Number(s.lat).toFixed(2) + ',' + Number(s.lon).toFixed(2); }

  function focusStationOnMap(s) {
    if (!_stnMap) return;
    const mk = _stnMarkers[siteKey(s)];
    const wrap = document.getElementById('stn-map-wrap');
    if (wrap && !wrap.hidden) wrap.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
    _stnMap.flyTo([s.lat, s.lon], Math.max(_stnMap.getZoom(), 5), { duration: 0.6 });
    if (mk) setTimeout(function() { mk.openPopup(); }, 650);
  }

  function stationPopupHtml(site) {
    const first = site.list[0];
    let html = '<div class="pp-title">' + flagHtml(first.iso2) + escHtml(first.name) + '</div>'
             + '<div class="pp-sub">' + escHtml(first.country_name || first.country || '')
             + (first.navarea ? ' · NAVAREA ' + roman(first.navarea) : '')
             + (typeof first.dist_km === 'number' && first.dist_km >= 0
                ? ' · ' + fmtKm(first.dist_km) + ' ' + compassPoint(first.bearing) : '') + '</div>';
    site.list.forEach(function(s) {
      const h = s.heard;
      html += '<div class="pp-st"><div class="pp-grid">'
            + '<b>Freq / ID</b><span>' + s.freq_khz + ' kHz · ' + escHtml(s.letter)
            + (s.name !== first.name ? ' (' + escHtml(s.name) + ')' : '') + '</span>';
      if (h && h.count) {
        html += '<b>Messages</b><span>' + fmtInt(h.count) + ' · ' + Math.round(h.high / h.count * 100) + '% high</span>'
              + '<b>Last heard</b><span>' + fmtAgo(h.last_utc) + '</span>'
              + '<b>Best SNR</b><span>' + (typeof h.best_snr === 'number' ? h.best_snr.toFixed(1) + ' dB' : '—') + '</span>';
      } else {
        html += '<b>Messages</b><span style="color:#7d8aa3">not heard</span>';
      }
      html += '</div>';
      if (LOGGING_ENABLED && h && h.count)
        html += '<span class="pp-link" data-hist="' + escHtml(s.key) + '">Show in history ›</span>';
      html += '</div>';
    });
    return html;
  }

  /* Heard with at least one Medium+ attribution — really received, as
   * opposed to only ever a Low-confidence guess. */
  function stationTrusted(s) {
    const h = s && s.heard;
    if (!h || !h.count) return false;
    return typeof h.medium === 'number' ? (h.high + h.medium) > 0 : h.high > 0;
  }

  function renderStationsMap(heard, all, rx, showUnheard) {
    const L = window.L;
    const wrap = document.getElementById('stn-map-wrap');
    if (!L || !wrap || wrap.hidden) return;
    if (!_stnMap) {
      _stnMap = L.map('stn-map', { worldCopyJump: true, minZoom: 2, zoomSnap: 0.5 });
      mapTileLayer(L).addTo(_stnMap);
      _stnLayers = L.layerGroup().addTo(_stnMap);
      /* "Show in history" links inside popups. */
      document.getElementById('stn-map').addEventListener('click', function(ev) {
        const a = ev.target.closest('[data-hist]');
        if (a) openHistoryForStation(a.getAttribute('data-hist'));
      });
    }
    /* The panel may have been hidden when the map was created. */
    _stnMap.invalidateSize();
    _stnLayers.clearLayers();
    _stnMarkers = {};

    /* Unheard first, so heard markers draw on top. */
    if (showUnheard) {
      all.forEach(function(s) {
        if (s.heard && s.heard.count > 0) return;
        if (typeof s.lat !== 'number' || typeof s.lon !== 'number') return;
        L.circleMarker([s.lat, s.lon], { radius: 3.5, color: '#6a7a8a', weight: 1, opacity: 0.6,
                                         fillColor: '#6a7a8a', fillOpacity: 0.12 })
          .bindTooltip(escHtml(s.name) + ' · ' + s.freq_khz + ' ' + escHtml(s.letter) + ' (not heard)', { direction: 'top' })
          .addTo(_stnLayers);
      });
    }

    /* One marker per site. */
    const sites = new Map();
    heard.forEach(function(s) {
      if (typeof s.lat !== 'number' || typeof s.lon !== 'number') return;
      const k = siteKey(s);
      let site = sites.get(k);
      if (!site) { site = { list: [], count: 0, last: '' }; sites.set(k, site); }
      site.list.push(s);
      site.count += s.heard.count;
      if ((s.heard.last_utc || '') > site.last) site.last = s.heard.last_utc;
    });
    /* Section 10 applied to the map: a site whose every attribution is Low
     * is a guess, not a reception (Rio Gallegos at 13,405 km, mean
     * confidence 0.28, was what zoomed this map out to the whole world).
     * Such sites are drawn faint and dashed, and the initial view is
     * framed on the sites with at least one Medium+ message, plus the
     * receiver.  Their table rows are dimmed the same way. */
    const bounds = [], lowBounds = [];
    sites.forEach(function(site, k) {
      site.list.sort(function(a, b) { return b.heard.count - a.heard.count; });
      const s0 = site.list[0];
      const lowOnly = !site.list.some(stationTrusted);
      const r = 5 + 3.3 * Math.log10(Math.max(1, site.count));
      const col = recencyColour(site.last);
      const mk = L.circleMarker([s0.lat, s0.lon], lowOnly
          ? { radius: r, color: col, weight: 1, opacity: 0.45, dashArray: '3 3', fillColor: col, fillOpacity: 0.08 }
          : { radius: r, color: col, weight: 1.5, opacity: 0.95, fillColor: col, fillOpacity: 0.35 })
        .bindPopup(stationPopupHtml(site), { maxWidth: 280 })
        .bindTooltip(escHtml(s0.name) + ' · ' + fmtInt(site.count) + (lowOnly ? ' · low confidence only' : ''),
                     { direction: 'top', offset: [0, -r] })
        .addTo(_stnLayers);
      if (lowOnly) mk.bringToBack();
      _stnMarkers[k] = mk;
      (lowOnly ? lowBounds : bounds).push([s0.lat, s0.lon]);
    });
    if (!bounds.length) bounds.push.apply(bounds, lowBounds);   /* nothing trusted yet: show what there is */

    if (rx.valid && typeof rx.lat === 'number' && typeof rx.lon === 'number') {
      L.marker([rx.lat, rx.lon], {
        icon: L.divIcon({ className: '', html: '<div class="rx-icon"></div>', iconSize: [14, 14], iconAnchor: [7, 7] }),
        zIndexOffset: 1000, keyboard: false
      }).bindPopup('<div class="pp-title">Receiver</div><div class="pp-sub">'
                   + escHtml([rx.callsign, rx.location, rx.locator].filter(Boolean).join(' · ')) + '</div>'
                   + '<div class="pp-grid"><b>Position</b><span>' + Number(rx.lat).toFixed(3) + ', ' + Number(rx.lon).toFixed(3) + '</span>'
                   + '<b>Source</b><span>' + (rx.source === 'env' ? 'NAVTEX_RX_LAT / LON' : 'UberSDR') + '</span></div>')
        .addTo(_stnLayers);
      bounds.push([rx.lat, rx.lon]);
    }

    /* Frame everything once; later refreshes keep the user's view. */
    if (!_stnFitted) {
      _stnFitted = true;
      if (bounds.length >= 2) _stnMap.fitBounds(bounds, { padding: [30, 30], maxZoom: 7 });
      else if (bounds.length === 1) _stnMap.setView(bounds[0], 5);
      else _stnMap.setView([45, 0], 2);
    }
  }

  /* ---- Output actions: clear / copy / download ---- */
  function clearOutput(ch) {
    const out = document.getElementById('output-' + ch);
    if (!out) return;
    out.innerHTML = '<span class="dim">Cleared.</span>';
    firstChar[ch] = true;
    lineStart[ch] = true;
    rawLines[ch] = [];
  }

  function copyOutput(ch) {
    const text = getOutputText(ch);
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).catch(function() {
        fallbackCopy(text);
      });
    } else {
      fallbackCopy(text);
    }
  }

  function fallbackCopy(text) {
    const ta = document.createElement('textarea');
    ta.value = text;
    ta.style.position = 'fixed';
    ta.style.opacity = '0';
    document.body.appendChild(ta);
    ta.select();
    try { document.execCommand('copy'); } catch(e) {}
    document.body.removeChild(ta);
  }

  function downloadOutput(ch) {
    const text = getOutputText(ch);
    const blob = new Blob([text], { type: 'text/plain' });
    const url  = URL.createObjectURL(blob);
    const a    = document.createElement('a');
    a.href     = url;
    a.download = 'navtex-ch' + ch + '-' + new Date().toISOString().slice(0,19).replace(/:/g,'-') + '.txt';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  }

  /* ---- DOM element helpers ---- */
  function el(id) { return document.getElementById(id); }
  function chEl(name, ch) { return el(name + '-' + ch); }

  /* ---- Stats update ---- */
  function updateStats(ch, s) {
    if (ch < 0 || ch >= NUM_CHANNELS) return;
    const bbEl     = chEl('bb-val',   ch);
    const ndEl     = chEl('nd-val',   ch);
    const rateEl   = chEl('rate-val', ch);
    const actDot   = chEl('act-dot',  ch);
    const actTxt   = chEl('act-text', ch);
    const sigFill  = chEl('sig-fill', ch);
    const snrVal   = chEl('snr-val',  ch);
    const snrFill  = chEl('snr-fill', ch);
    const decState = chEl('dec-state',ch);
    const fecVal   = chEl('fec-val',  ch);
    const errVal   = chEl('err-val',  ch);
    const fecClean = chEl('fec-clean',ch);
    const fecFec   = chEl('fec-fec',  ch);
    const fecFail  = chEl('fec-fail', ch);
    const tabDot   = chEl('tab-dot',  ch);
    /* Guard: if core elements are missing, bail out */
    if (!bbEl || !rateEl || !decState) return;

    if (s.bb !== undefined) {
      if (isFinite(s.bb)) {
        bbEl.textContent = s.bb.toFixed(1);
        const pct = Math.max(0, Math.min(100, (s.bb + 120) / 70 * 100));
        sigFill.style.marginLeft = pct + '%';
      } else {
        bbEl.textContent = '\u2014';
        sigFill.style.marginLeft = '100%';
      }
    }
    if (s.nd !== undefined)
      ndEl.textContent = isFinite(s.nd) ? s.nd.toFixed(1) : '\u2014';

    if (s.bb !== undefined && s.nd !== undefined && isFinite(s.bb) && isFinite(s.nd)) {
      const snr = s.bb - s.nd;
      snrVal.textContent = snr.toFixed(1) + ' dB';
      snrVal.className = 'stat-value ' + snrCls(snr);
      snrFill.style.width = snrBarPct(snr) + '%';
      snrFill.style.background = snrBarColour(snr);
    } else if (s.bb !== undefined) {
      snrVal.textContent = '\u2014'; snrVal.className = 'stat-value';
      snrFill.style.width = '0%';
    }

    if (s.rate !== undefined)
      rateEl.textContent = (s.rate / 1000).toFixed(1) + ' kHz';

    if (s.act !== undefined) {
      if (s.act) { actDot.className = 'act-dot active'; actTxt.textContent = 'Active'; }
      else        { actDot.className = 'act-dot idle';   actTxt.textContent = 'Idle';   }
      /* Alert sound: fire on idle -> active transition */
      if (s.act && !prevActState[ch]) {
        const alertToggle = document.getElementById('alert-sound-toggle');
        if (alertToggle && alertToggle.checked) playAlertDing();
      }
      prevActState[ch] = s.act ? 1 : 0;
    }

    if (s.decState !== undefined) {
      const newState = s.decState;
      /* Only (re)start the debounce timer when the incoming state differs from
       * what is already pending.  This means a stable state fires the timer
       * exactly once (after 1 s of stability) rather than being perpetually
       * reset by the 100 ms stats stream. */
      if (newState !== decStatePending[ch]) {
        decStatePending[ch] = newState;
        if (decStateTimer[ch] !== null) { clearTimeout(decStateTimer[ch]); decStateTimer[ch] = null; }
        decStateTimer[ch] = setTimeout(function() {
          decStateTimer[ch] = null;
          const st   = decStatePending[ch];
          decStateCommitted[ch] = st;
          const name = STATE_NAMES[st] !== undefined ? STATE_NAMES[st] : '?';
          const cls  = STATE_CLASS[st]  !== undefined ? STATE_CLASS[st]  : '';
          const dsEl = chEl('dec-state', ch);
          const tdEl = chEl('tab-dot',   ch);
          if (dsEl) { dsEl.textContent = name; dsEl.className = 'stat-value ' + cls; }
          if (tdEl) { tdEl.className = 'tab-dot ' + (TAB_DOT_CLASS[st] || ''); }
          const splitDot = document.getElementById('split-dot-' + ch);
          if (splitDot) splitDot.className = tdEl ? tdEl.className : 'tab-dot';
          const splitDecState = document.getElementById('dec-state-' + ch + '-s');
          if (splitDecState) { splitDecState.textContent = name; splitDecState.className = 'stat-value ' + cls; }
        }, 1000);
      }
      /* If newState === decStatePending[ch], the timer is already running and
       * will fire after 1 s of stability — do nothing. */
    }

    if (s.decState === 2 && s.total > 0) {
      const fecPct = s.fec    / s.total * 100;
      const errPct = s.failed / s.total * 100;
      const clnPct = s.clean  / s.total * 100;
      fecVal.textContent = fecPct.toFixed(0) + '%';
      fecVal.className = 'stat-value ' + (fecPct > 30 ? 'warn' : 'good');
      errVal.textContent = errPct.toFixed(0) + '%';
      errVal.className = 'stat-value ' + (errPct > 10 ? 'bad' : errPct > 3 ? 'warn' : 'good');
      fecClean.style.width = clnPct.toFixed(1) + '%';
      fecFec.style.width   = fecPct.toFixed(1) + '%';
      fecFail.style.width  = errPct.toFixed(1) + '%';
    } else if (s.decState !== undefined && s.decState !== 2) {
      fecVal.textContent = '\u2014'; fecVal.className = 'stat-value dim';
      errVal.textContent = '\u2014'; errVal.className = 'stat-value dim';
      fecClean.style.width = '0%';
      fecFec.style.width   = '0%';
      fecFail.style.width  = '0%';
    }
    maybeSyncSplit(ch);
  }

  /* ---- Message info update ---- */
  function updateMsgInfo(ch, inMsg, done, sta, sub, ser) {
    if (ch < 0 || ch >= NUM_CHANNELS) return;
    const prev = prevMsgState[ch] || { inMsg: false, done: false };

    /* Detect message start: inMsg transitions to true (regardless of prev done state —
     * a new ZCZC always starts a new message even immediately after a previous NNNN) */
    if (inMsg && !prev.inMsg) {
      insertDivider(ch, 'start');
    }
    /* Detect message end: transition to done=true */
    if (done && !prev.done) {
      insertDivider(ch, 'end');
    }

    const isStart = inMsg && !prev.inMsg;

    /* Update previous state */
    prevMsgState[ch] = { inMsg: inMsg, done: done };

    const msgBar  = chEl('msg-bar',    ch);
    const dot     = chEl('msg-dot',    ch);
    const statTxt = chEl('msg-status', ch);
    const station = chEl('msg-station',ch);
    const subject = chEl('msg-subject',ch);
    const serial  = chEl('msg-serial', ch);
    if (!msgBar || !dot || !statTxt || !station || !subject || !serial) return;

    if (inMsg || done) {
      msgBar.className = 'msg-bar';
      if (done) { dot.className = 'msg-dot complete';  statTxt.textContent = 'Complete'; }
      else      { dot.className = 'msg-dot receiving'; statTxt.textContent = 'Receiving\u2026'; }
      const subCh = sub ? String.fromCharCode(sub) : '';
      const subName = subCh ? (SUBJECT[subCh] || '') : '';
      subject.textContent = subCh
        ? (subCh + (subName ? ' \u2013 ' + subName : ''))
        : '\u2014';
      serial.textContent = (ser !== 0xFF) ? String(ser).padStart(2, '0') : '\u2014';
    } else {
      msgBar.className = 'msg-bar idle';
      dot.className = 'msg-dot';
      statTxt.textContent = 'Idle';
      subject.textContent = '\u2014';
      serial.textContent  = '\u2014';
    }
    /* Station: letter, then flag + name + confidence once identified. */
    liveIdentOnMsgInfo(ch, inMsg, done, sta, sub, ser, isStart);
    maybeSyncSplit(ch);
  }

  /* ---- Audio preview ---- */
  const audioState = {};
  for (let i = 0; i < NUM_CHANNELS; i++) {
    audioState[i] = { enabled: false, ctx: null, queue: [], nextTime: 0, sampleRate: 12000 };
  }
  const AUDIO_AHEAD = 0.1;

  function audioFlush(ch) {
    const a = audioState[ch];
    if (!a.ctx || a.queue.length === 0) return;
    const now = a.ctx.currentTime;
    if (a.nextTime < now) a.nextTime = now + 0.05;
    while (a.queue.length > 0) {
      const samples = a.queue.shift();
      const buf = a.ctx.createBuffer(1, samples.length, a.sampleRate);
      buf.copyToChannel(samples, 0);
      const src = a.ctx.createBufferSource();
      src.buffer = buf;
      src.connect(a.ctx.destination);
      src.start(a.nextTime);
      a.nextTime += buf.duration;
    }
  }

  function setAudioBtnState(ch, active) {
    /* Update both the real button and the split-clone (if it exists) */
    const ids = ['audio-btn-' + ch, 'audio-btn-' + ch + '-s'];
    ids.forEach(function(id) {
      const b = document.getElementById(id);
      if (!b) return;
      if (active) {
        b.className = 'audio-btn active';
        b.textContent = '\uD83D\uDD0A';
      } else {
        b.className = 'audio-btn';
        b.textContent = '\uD83D\uDD0A';
      }
    });
  }

  function audioEnable(ch, ws) {
    /* Only one channel can preview at a time — disable any other active channel first */
    for (let i = 0; i < NUM_CHANNELS; i++) {
      if (i !== ch && audioState[i].enabled) audioDisable(i, ws);
    }
    const a = audioState[ch];
    if (a.enabled) return;
    a.ctx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: a.sampleRate });
    a.nextTime = 0; a.queue = [];
    a.enabled = true;
    setAudioBtnState(ch, true);
    ws.send(JSON.stringify({ type: 'audio_preview', enable: true, channel: ch }));
  }

  function audioDisable(ch, ws) {
    const a = audioState[ch];
    if (!a.enabled) return;
    a.enabled = false; a.queue = [];
    if (a.ctx) { a.ctx.close(); a.ctx = null; }
    setAudioBtnState(ch, false);
    ws.send(JSON.stringify({ type: 'audio_preview', enable: false, channel: ch }));
  }

  /* ---- WebSocket connection ---- */
  const BASE_PATH = (typeof window._BASE_PATH === 'string') ? window._BASE_PATH : '';

  function connect() {
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    const ws = new WebSocket(proto + '://' + location.host + BASE_PATH + '/ws');
    ws.binaryType = 'arraybuffer';

    ws.onopen = function() {
      el('status-dot').className = 'connected';
      el('status-text').textContent = 'Connected';
      window._activeWs = ws;
      /* Wire audio buttons (real panels) and split-clone buttons if already built */
      for (let i = 0; i < NUM_CHANNELS; i++) {
        (function(ch) {
          const btn = chEl('audio-btn', ch);
          btn.onclick = function() {
            if (audioState[ch].enabled) audioDisable(ch, ws);
            else audioEnable(ch, ws);
          };
          /* Wire split-clone buttons if split panel already exists */
          if (splitBuilt) wireSplitButtons(ch, ws);
          /* Re-subscribe if audio was active before reconnect */
          if (audioState[ch].enabled)
            ws.send(JSON.stringify({ type: 'audio_preview', enable: true, channel: ch }));
        })(i);
      }
    };

    ws.onclose = function() {
      el('status-dot').className = 'disconnected';
      el('status-text').textContent = 'Disconnected \u2014 reconnecting\u2026';
      setTimeout(connect, 3000);
    };

    ws.onerror = function() { ws.close(); };

    ws.onmessage = function(e) {
      if (e.data instanceof ArrayBuffer) {
        const v    = new DataView(e.data);
        const type = v.getUint8(0);
        const ch   = v.getUint8(1);

        /* 0x02 stats frame (22 bytes) */
        if (type === 0x02 && e.data.byteLength >= 22) {
          const bb    = v.getFloat32(2,  true);
          const nd    = v.getFloat32(6,  true);
          const flags = v.getUint8(10);
          const rate  = v.getUint16(11, true) * 1000;
          const state = v.getUint8(13);
          const clean = v.getUint8(14);
          const fec   = v.getUint8(15);
          const fail  = v.getUint8(16);
          updateStats(ch, {
            bb:       (flags & 0x01) ? bb : undefined,
            nd:       (flags & 0x01) ? nd : undefined,
            rate:     rate,
            act:      (flags & 0x02) ? 1 : 0,
            decState: state,
            clean:    clean,
            fec:      fec,
            failed:   fail,
            total:    clean + fec + fail
          });
          return;
        }

        /* 0x03 msg-info frame (6 bytes) */
        if (type === 0x03 && e.data.byteLength >= 6) {
          const flags = v.getUint8(2);
          const inMsg = !!(flags & 0x01);
          const done  = !!(flags & 0x02);
          const sta   = v.getUint8(3);
          const sub   = v.getUint8(4);
          const ser   = v.getUint8(5);
          updateMsgInfo(ch, inMsg, done, sta, sub, ser);
          return;
        }

        /* 0x05 identification frame: [0x05][ch][utf8 JSON ident] —
         * provisional at header decode, final at message end. */
        if (type === 0x05 && e.data.byteLength > 2) {
          try {
            const txt = new TextDecoder('utf-8').decode(new Uint8Array(e.data, 2));
            onIdentFrame(ch, JSON.parse(txt));
          } catch (err) { /* malformed frame: ignore */ }
          return;
        }

        /* 0x04 audio frame */
        if (type === 0x04) {
          const a = audioState[ch];
          if (a.enabled && a.ctx) {
            const n = (e.data.byteLength - 2) / 2;
            if (n > 0) {
              const f32 = new Float32Array(n);
              for (let i = 0; i < n; i++)
                f32[i] = v.getInt16(2 + i * 2, true) / 32768.0;
              a.queue.push(f32);
              audioFlush(ch);
            }
          }
          return;
        }
        return;
      }

      /* Text frame: channel digit + decoded character, e.g. "0A" */
      if (typeof e.data === 'string' && e.data.length >= 2) {
        const ch  = parseInt(e.data[0], 10);
        const chr = e.data.slice(1);
        if (ch >= 0 && ch < NUM_CHANNELS) {
          appendChar(ch, chr);
        }
      }
    };
  }

  /* ---- Timestamp-aware character appender ---- */
  /*
   * Per-channel state: track whether we are at the start of a new line
   * so we can prepend a UTC timestamp when timestamps are enabled.
   * A "new line" starts after \r or \n, or at the very first character.
   */
  const lineStart = Array(NUM_CHANNELS).fill(true);

  /* Per-channel array of entries for completed lines + current partial.
   * Each entry is one of:
   *   {stamp:'HH:MM:SSZ', text:'...'}          — a text line
   *   {divider: true, kind: 'start'|'end'}      — a message boundary marker
   */
  const rawLines = [];
  for (let i = 0; i < NUM_CHANNELS; i++) rawLines.push([]);

  /* Previous msg state per channel — used to detect start/end transitions */
  const prevMsgState = [];
  for (let i = 0; i < NUM_CHANNELS; i++) prevMsgState.push({ inMsg: false, done: false });

  function utcStamp() {
    const d = new Date();
    return d.toISOString().slice(11, 19) + 'Z'; /* HH:MM:SSZ */
  }

  /* Insert a divider element into the output area and record it in rawLines */
  function insertDivider(ch, kind) {
    rawLines[ch].push({ divider: true, kind: kind });
    const out = chEl('output', ch);
    if (!out) return;
    const d = document.createElement('div');
    d.className = 'msg-divider msg-' + kind;
    d.textContent = kind === 'start' ? 'Message Start' : 'Message End';
    out.appendChild(d);
    out.scrollTop = out.scrollHeight;
  }

  /* Re-render the output area for channel ch from rawLines */
  function reRenderOutput(ch) {
    const out = chEl('output', ch);
    if (!out) return;
    const tsEnabled = document.getElementById('ts-toggle') && document.getElementById('ts-toggle').checked;
    out.innerHTML = '';
    const lines = rawLines[ch];
    if (lines.length === 0) return;
    for (let i = 0; i < lines.length; i++) {
      const entry = lines[i];
      if (entry.divider) {
        const d = document.createElement('div');
        d.className = 'msg-divider msg-' + entry.kind;
        d.textContent = entry.kind === 'start' ? 'Message Start' : 'Message End';
        out.appendChild(d);
      } else if (tsEnabled) {
        const lineDiv = document.createElement('div');
        lineDiv.className = 'output-line';
        const stamp = document.createElement('span');
        stamp.className = 'ts-stamp';
        stamp.textContent = entry.stamp + ' ';
        lineDiv.appendChild(stamp);
        const text = document.createElement('span');
        text.className = 'output-text';
        text.textContent = entry.text;
        lineDiv.appendChild(text);
        out.appendChild(lineDiv);
      } else {
        /* Plain text: text + newline (except last partial line) */
        const isLast = (i === lines.length - 1);
        out.appendChild(document.createTextNode(entry.text + (isLast ? '' : '\n')));
      }
    }
    out.scrollTop = out.scrollHeight;
  }

  /* Wire the timestamp toggle to re-render all channels */
  (function() {
    const tog = document.getElementById('ts-toggle');
    if (tog) tog.addEventListener('change', function() {
      for (let i = 0; i < NUM_CHANNELS; i++) reRenderOutput(i);
    });
  })();

  function appendChar(ch, chr) {
    const out = chEl('output', ch);
    if (!out) return;

    /* Clear placeholder on first real character */
    if (firstChar[ch]) {
      out.innerHTML = '';
      firstChar[ch] = false;
      rawLines[ch] = [{ stamp: utcStamp(), text: '' }];
    }

    /* Ensure at least one line entry exists */
    if (rawLines[ch].length === 0) {
      rawLines[ch].push({ stamp: utcStamp(), text: '' });
    }

    const tsEnabled = document.getElementById('ts-toggle') && document.getElementById('ts-toggle').checked;

    if (chr === '\n' || chr === '\r') {
      /* Append newline to current last line's text, then start a new line entry */
      rawLines[ch][rawLines[ch].length - 1].text += chr;
      rawLines[ch].push({ stamp: utcStamp(), text: '' });
      lineStart[ch] = true;
    } else {
      /* Append character to current last line */
      rawLines[ch][rawLines[ch].length - 1].text += chr;
      lineStart[ch] = false;
    }

    /* Render the new character into the DOM */
    if (tsEnabled) {
      if (chr === '\n' || chr === '\r') {
        /* Start a new .output-line div for the next line.
         * Find the last non-divider entry for the stamp (the divider inserted
         * by updateMsgInfo may now be the last rawLines entry). */
        let newStamp = utcStamp();
        for (let ri = rawLines[ch].length - 1; ri >= 0; ri--) {
          if (!rawLines[ch][ri].divider) { newStamp = rawLines[ch][ri].stamp; break; }
        }
        const lineDiv = document.createElement('div');
        lineDiv.className = 'output-line';
        const stamp = document.createElement('span');
        stamp.className = 'ts-stamp';
        stamp.textContent = newStamp + ' ';
        lineDiv.appendChild(stamp);
        const text = document.createElement('span');
        text.className = 'output-text';
        lineDiv.appendChild(text);
        out.appendChild(lineDiv);
      } else {
        /* Find or create the last .output-line div.
         * We cannot use :last-child because a msg-divider may be the last
         * child after a ZCZC/NNNN boundary.  Walk backwards instead. */
        let lineDiv = null;
        {
          let node = out.lastElementChild;
          while (node) {
            if (node.classList && node.classList.contains('output-line')) {
              lineDiv = node;
              break;
            }
            node = node.previousElementSibling;
          }
        }
        if (!lineDiv) {
          /* Find the last non-divider rawLines entry for the stamp */
          let stampVal = utcStamp();
          for (let ri = rawLines[ch].length - 1; ri >= 0; ri--) {
            if (!rawLines[ch][ri].divider) { stampVal = rawLines[ch][ri].stamp; break; }
          }
          lineDiv = document.createElement('div');
          lineDiv.className = 'output-line';
          const stamp = document.createElement('span');
          stamp.className = 'ts-stamp';
          stamp.textContent = stampVal + ' ';
          lineDiv.appendChild(stamp);
          const text = document.createElement('span');
          text.className = 'output-text';
          lineDiv.appendChild(text);
          out.appendChild(lineDiv);
        }
        const textSpan = lineDiv.querySelector('.output-text');
        if (textSpan) textSpan.textContent += chr;
      }
    } else {
      /* Timestamps off: plain text node */
      out.appendChild(document.createTextNode(chr));
    }

    out.scrollTop = out.scrollHeight;
  }

  /* Extract plain text from output for copy/download.
   * Dividers are always stripped. Timestamps are included when the toggle is on.
   * Never falls back to innerText/textContent — those include divider text. */
  function getOutputText(ch) {
    const lines = rawLines[ch];
    if (!lines || lines.length === 0) return '';
    const tsEnabled = document.getElementById('ts-toggle') && document.getElementById('ts-toggle').checked;
    return lines
      .filter(function(e) { return !e.divider; })
      .map(function(e) {
        const t = e.text || '';
        if (tsEnabled && e.stamp && t.trim().length > 0) {
          /* Prepend timestamp only to non-empty lines */
          return e.stamp + ' ' + t;
        }
        return t;
      })
      .join('');
  }

  /* ── History modal ─────────────────────────────────────────────────── */
  /*
   * The list can hold tens of thousands of entries (a few months of two
   * busy frequencies is ~32k), so the work is arranged to keep every
   * keystroke and click well under a frame or two:
   *
   *   - Everything a filter or sort needs is derived ONCE per load in
   *     prepHistEntry(): the lower-cased search haystack, letter, subject,
   *     level, country, station key, distance, path and a sortable time
   *     string are stored on the entry as _fields.  Filtering is then a
   *     single pass of plain comparisons, with no parsing, decoding or
   *     string building per entry.
   *   - Free-text search is debounced (150 ms) so typing does not re-filter
   *     on every character.
   *   - The server already returns newest-first, which is the default sort,
   *     so the default view skips sorting entirely; any other sort is one
   *     Array.sort over the filtered subset with precomputed keys.
   *   - Only one page (50 rows) is ever in the DOM.
   *   - Country / station facet counts are computed once per load.
   */
  let _histData = [];   /* cached list from /api/history */
  var _histFiltered = [];
  var _histPage     = 0;
  var _HIST_PAGE_SZ = 50;
  let _histSort     = { key: 'time', dir: -1 };
  let _histFacets   = { countries: [], stations: [] };
  let _histHasPending = false;
  let _histLoadSeq  = 0;
  let _histFilterT  = null;
  let _histHashMuted = false;
  let _histPendingRetries = 0;

  /* Current filter state, mirrored to and from the URL hash. */
  const HIST_DEFAULTS = { q: '', type: 'msg', subject: '', freq: '', country: '', station: '',
                          level: '', dist: '', path: '', sort: 'time', dir: 'desc' };
  let _histState = Object.assign({}, HIST_DEFAULTS);

  function prepHistEntry(m) {
    const isMsg = (m.type || 'msg') === 'msg';
    const id    = (m.id || '').toUpperCase();
    const st    = isMsg ? m.st : null;
    const has   = !!(st && st.key);
    m._msg    = isMsg;
    m._letter = isMsg && id !== 'UNKNOWN' ? (id[0] || '') : '';
    m._subj   = isMsg && id !== 'UNKNOWN' ? (id[1] || '') : '';
    m._lvl    = !isMsg ? '' : m.st_pending ? 'pending' : has ? identLevel(st) : 'none';
    m._iso    = has ? (st.iso2 || '') : '';
    m._skey   = has ? st.key : '';
    m._sname  = has ? String(st.name || '').toLowerCase() : '';
    m._dist   = has && typeof st.dist_km === 'number' && st.dist_km >= 0 ? st.dist_km : -1;
    m._path   = has && typeof st.night === 'boolean' ? (st.night ? 'night' : 'day') : '';
    /* Shown time: when the message STARTED (the facts talk about the start
     * — "Started 3 min 45 s into G's 21:00 slot"), from the server's index.
     * The file name, and so m.time, is the END.  A message that started
     * before midnight and ended after it keeps its end time, so the time
     * never contradicts the Date column. */
    m._shownTime = (isMsg && m.start && m.time && m.start <= m.time) ? m.start : (m.time || '');
    m._ts     = (m.date || '') + (isMsg ? m._shownTime : '');   /* sorts as it reads */
    m._ser    = parseInt(m.serial, 10);
    if (!isFinite(m._ser)) m._ser = -1;
    m._nav    = has && st.navarea ? Number(st.navarea) : 0;
    const dec = isMsg ? decodeNavtexId(m.id || '') : null;
    let hay = (m.freq || '') + ' ' + (m.date || '') + ' ' + (m.time || '') + ' ' + (m.id || '')
            + ' ' + (m.serial || '');
    if (dec) hay += ' ' + dec.station + ' ' + dec.subject;
    if (has) {
      hay += ' ' + (st.name || '') + ' ' + (st.country_name || '');
      if (st.navarea) hay += ' navarea ' + st.navarea + ' navarea ' + roman(st.navarea);
    }
    m._hay = hay.toLowerCase();
  }

  function buildHistFacets() {
    const cMap = new Map(), sMap = new Map();
    for (let i = 0; i < _histData.length; i++) {
      const m = _histData[i];
      if (!m._skey) continue;
      const st = m.st;
      let c = cMap.get(m._iso);
      if (!c) { c = { iso: m._iso, name: st.country_name || m._iso.toUpperCase(), count: 0 }; cMap.set(m._iso, c); }
      c.count++;
      let s = sMap.get(m._skey);
      if (!s) {
        const parts = m._skey.split(':');
        s = { key: m._skey, name: st.name || parts[2] || m._skey, iso: m._iso,
              country: c.name, khz: parts[0] || '', letter: parts[1] || '', count: 0 };
        sMap.set(m._skey, s);
      }
      s.count++;
    }
    const byCount = function(a, b) { return b.count - a.count || (a.name < b.name ? -1 : 1); };
    _histFacets.countries = Array.from(cMap.values()).sort(byCount);
    _histFacets.stations  = Array.from(sMap.values()).sort(byCount);
  }

  /* ---- URL hash: "#history?country=gb&level=high" ---- */
  function histStateFromHash() {
    const h = location.hash || '';
    if (h.indexOf('#history') !== 0) return null;
    const qi = h.indexOf('?');
    const p = new URLSearchParams(qi >= 0 ? h.slice(qi + 1) : '');
    const st = Object.assign({}, HIST_DEFAULTS);
    Object.keys(HIST_DEFAULTS).forEach(function(k) { if (p.has(k)) st[k] = p.get(k); });
    return st;
  }
  function histStateToHash() {
    if (_histHashMuted) return;
    const p = new URLSearchParams();
    Object.keys(HIST_DEFAULTS).forEach(function(k) {
      if (_histState[k] !== HIST_DEFAULTS[k]) p.set(k, _histState[k]);
    });
    const qs = p.toString();
    const h = '#history' + (qs ? '?' + qs : '');
    if (location.hash !== h) history.replaceState(null, '', h);
  }
  function clearHash() {
    if (location.hash) history.replaceState(null, '', location.pathname + location.search);
  }

  /* Read the toolbar controls into _histState. */
  function readHistControls() {
    _histState.q       = document.getElementById('hist-search').value || '';
    _histState.type    = document.getElementById('hist-type-filter').value || '';
    _histState.subject = document.getElementById('hist-subject-filter').value || '';
    _histState.freq    = document.getElementById('hist-freq-filter').value || '';
    _histState.level   = document.getElementById('hist-level-filter').value || '';
    _histState.dist    = document.getElementById('hist-dist-filter').value || '';
    _histState.path    = document.getElementById('hist-path-filter').value || '';
    _histState.sort    = _histSort.key;
    _histState.dir     = _histSort.dir < 0 ? 'desc' : 'asc';
  }
  /* And the other way, e.g. after loading state from the hash. */
  function writeHistControls() {
    document.getElementById('hist-search').value         = _histState.q;
    document.getElementById('hist-type-filter').value    = _histState.type;
    document.getElementById('hist-subject-filter').value = _histState.subject;
    document.getElementById('hist-level-filter').value   = _histState.level;
    document.getElementById('hist-dist-filter').value    = _histState.dist;
    document.getElementById('hist-path-filter').value    = _histState.path;
    const fs = document.getElementById('hist-freq-filter');
    fs.value = _histState.freq;
    if (fs.value !== _histState.freq) {   /* option not loaded yet: add it so the state survives */
      const o = document.createElement('option');
      o.value = o.textContent = _histState.freq;
      fs.appendChild(o); fs.value = _histState.freq;
    }
    _histSort = { key: _histState.sort || 'time', dir: _histState.dir === 'asc' ? 1 : -1 };
  }

  function openHistory(presetFreq, presetState) {
    const modal = document.getElementById('history-modal');
    if (!modal) return;
    modal.style.display = 'flex';
    /* Reset UI, then apply whichever preset applies */
    _histState = Object.assign({}, HIST_DEFAULTS, presetState || {});
    if (presetFreq) _histState.freq = presetFreq;
    writeHistControls();
    buildHistDropdowns();
    histStateToHash();
    loadHistory(false);
  }

  /* Fetch /api/history.  keepView: a background reload (replay finished),
   * which keeps the current page instead of returning to the first. */
  function loadHistory(keepView) {
    const seq = ++_histLoadSeq;
    if (!keepView) {
      document.getElementById('hist-empty').style.display   = 'none';
      document.getElementById('hist-loading').style.display = 'block';
      document.getElementById('hist-pagination').style.display = 'none';
      document.getElementById('hist-tbody').innerHTML = '';
    }
    fetch(apiBase() + '/api/history')
      .then(function(r) { return r.json(); })
      .then(function(data) {
        if (seq !== _histLoadSeq) return;     /* superseded by a newer load */
        _histData = Array.isArray(data) ? data : [];
        _histHasPending = false;
        for (let i = 0; i < _histData.length; i++) {
          prepHistEntry(_histData[i]);
          if (_histData[i].st_pending) _histHasPending = true;
        }
        buildHistFacets();
        document.getElementById('hist-loading').style.display = 'none';
        /* Populate frequency filter options */
        const freqs = [...new Set(_histData.map(function(m) { return m.freq; }))].sort();
        const sel = document.getElementById('hist-freq-filter');
        /* Remove old dynamic options (keep first "All frequencies") */
        while (sel.options.length > 1) sel.remove(1);
        freqs.forEach(function(f) {
          const opt = document.createElement('option');
          opt.value = f;
          opt.textContent = f;
          sel.appendChild(opt);
        });
        sel.value = _histState.freq;
        buildHistDropdowns();
        applyHistFilters(keepView);
        /* Entries still being identified.  During replay the stations
         * poll reloads the list when replay finishes; outside replay
         * (live messages queued behind a slow identification) retry a
         * few times on a backoff rather than polling an 11 MB list. */
        if (!_histHasPending) _histPendingRetries = 0;
        else if (!_stationsData) fetchStations(false);
        else if (_stationsData.replay && _stationsData.replay.done !== false && _histPendingRetries < 3) {
          _histPendingRetries++;
          setTimeout(function() {
            const modal = document.getElementById('history-modal');
            if (modal && modal.style.display !== 'none') reloadHistoryKeepingView();
          }, 5000 * _histPendingRetries);
        }
      })
      .catch(function(err) {
        if (seq !== _histLoadSeq) return;
        document.getElementById('hist-loading').style.display = 'none';
        document.getElementById('hist-empty').style.display   = 'block';
        document.getElementById('hist-empty').textContent     = 'Failed to load history: ' + err;
      });
  }

  function reloadHistoryKeepingView() { loadHistory(true); }

  function closeHistory() {
    const modal = document.getElementById('history-modal');
    if (modal) modal.style.display = 'none';
    closeAllDropdowns();
    if ((location.hash || '').indexOf('#history') === 0) clearHash();
  }

  /* Open history modal pre-filtered to the frequency of channel ch */
  function openHistoryForChannel(ch) {
    const label = (CH_FREQ_LABELS && CH_FREQ_LABELS[ch]) ? CH_FREQ_LABELS[ch] : '';
    openHistory(label);
  }

  /* Open history filtered to one station (from the Stations view). */
  function openHistoryForStation(key) {
    openHistory('', { station: key });
  }

  function clearHistFilters() {
    const keepSort = { sort: _histState.sort, dir: _histState.dir };
    _histState = Object.assign({}, HIST_DEFAULTS, keepSort);
    writeHistControls();
    buildHistDropdowns();
    applyHistFilters(false);
  }

  function updateHistCount() {
    const countEl = document.getElementById('hist-count');
    if (!countEl) return;
    let totalMsgs = 0, filteredMsgs = 0;
    for (let i = 0; i < _histData.length; i++) if (_histData[i]._msg) totalMsgs++;
    for (let i = 0; i < _histFiltered.length; i++) if (_histFiltered[i]._msg) filteredMsgs++;
    if (filteredMsgs === totalMsgs) {
      countEl.textContent = '(' + fmtInt(totalMsgs) + ' messages)';
    } else {
      countEl.textContent = '(' + fmtInt(filteredMsgs) + ' / ' + fmtInt(totalMsgs) + ' messages)';
    }
  }

  /* Toolbar change handlers. */
  function filterHistory() {
    clearTimeout(_histFilterT);
    readHistControls();
    applyHistFilters(false);
  }
  function filterHistoryDebounced() {
    clearTimeout(_histFilterT);
    _histFilterT = setTimeout(filterHistory, 150);
  }

  function histFiltersActive() {
    const s = _histState;
    return !!(s.q || s.subject || s.freq || s.country || s.station || s.level || s.dist || s.path
              || s.type !== HIST_DEFAULTS.type);
  }

  const DIST_RANGES = { 'lt300': [0, 300], '300-1000': [300, 1000], '1000-3000': [1000, 3000], 'gt3000': [3000, Infinity] };

  function applyHistFilters(keepPage) {
    const s       = _histState;
    const q       = s.q.trim().toLowerCase();
    const type    = s.type;
    const freq    = s.freq;
    const subject = s.subject.toUpperCase();
    const country = s.country;
    const station = s.station;
    const level   = s.level;
    const path    = s.path;
    const dr      = DIST_RANGES[s.dist] || null;
    /* "NAVAREA 1" / "navarea i" names one area exactly.  As a plain
     * substring it would also match NAVAREA 10-19 (or II, III, IV, IX...),
     * so a query that is just a NAVAREA is matched on the number instead. */
    let navq = 0;
    const nm = /^navarea\s+([0-9]+|[ivxl]+)$/.exec(q);
    if (nm) {
      navq = /^[0-9]+$/.test(nm[1]) ? Number(nm[1]) : 0;
      if (!navq) for (let n = 1; n < 40; n++) if (roman(n).toLowerCase() === nm[1]) { navq = n; break; }
      if (!navq) navq = -1;          /* not a NAVAREA that exists: match nothing */
    }
    const out = [];
    const data = _histData;
    for (let i = 0; i < data.length; i++) {
      const m = data[i];
      if (type && (m._msg ? 'msg' : 'raw') !== type) continue;
      if (freq && m.freq !== freq) continue;
      /* Station-level filters only make sense for messages: a raw log
       * spans a whole day of every station, so it is excluded by them. */
      if (subject && m._subj !== subject) continue;
      if (country && m._iso !== country) continue;
      if (station && m._skey !== station) continue;
      if (level && m._lvl !== level) continue;
      if (path && m._path !== path) continue;
      if (dr && !(m._dist >= dr[0] && m._dist < dr[1])) continue;
      if (navq) { if (m._nav !== navq) continue; }
      else if (q && m._hay.indexOf(q) === -1) continue;
      out.push(m);
    }
    sortHist(out);
    _histFiltered = out;
    if (!keepPage) _histPage = 0;
    updateHistCount();
    updateHistChrome();
    histStateToHash();
    renderHistTable(_histFiltered);
  }

  /* Missing values (no SNR, unidentified, unknown distance) always sort
   * last, whichever the direction, so they never crowd the top. */
  function sortHist(arr) {
    const k = _histSort.key, dir = _histSort.dir;
    if (k === 'time' && dir < 0) return;          /* server order */
    if (k === 'time') { arr.reverse(); return; }
    let get;
    switch (k) {
      case 'date':    get = function(m) { return m.date || ''; }; break;
      case 'freq':    get = function(m) { return m.freq || ''; }; break;
      case 'id':      get = function(m) { return m._msg ? (m.id || '') : null; }; break;
      case 'serial':  get = function(m) { return m._ser >= 0 ? m._ser : null; }; break;
      case 'station': get = function(m) { return m._sname || null; }; break;
      case 'subject': get = function(m) { return m._subj || null; }; break;
      case 'dist':    get = function(m) { return m._dist >= 0 ? m._dist : null; }; break;
      case 'snr':     get = function(m) { return (m._msg && typeof m.snr === 'number') ? m.snr : null; }; break;
      case 'dur':     get = function(m) { return (m._msg && typeof m.duration_s === 'number') ? m.duration_s : null; }; break;
      default: return;
    }
    /* Decorate once, then sort on the cached key (ties keep newest first). */
    for (let i = 0; i < arr.length; i++) { arr[i]._sk = get(arr[i]); arr[i]._si = i; }
    arr.sort(function(a, b) {
      const x = a._sk, y = b._sk;
      if (x === null || y === null) {
        if (x === y) return a._si - b._si;
        return x === null ? 1 : -1;
      }
      if (x < y) return -dir;
      if (x > y) return dir;
      return a._si - b._si;
    });
  }

  function setHistSort(key) {
    /* Date and time are one ordering.  Normalised BEFORE the comparison,
     * or a click on Date (key 'date' vs the stored 'time') would always
     * look like a new column and could never flip to ascending. */
    if (key === 'date') key = 'time';
    if (_histSort.key === key) _histSort.dir = -_histSort.dir;
    else _histSort = { key: key, dir: (key === 'time' || key === 'snr' || key === 'dur') ? -1 : 1 };
    _histState.sort = _histSort.key;
    _histState.dir  = _histSort.dir < 0 ? 'desc' : 'asc';
    applyHistFilters(false);
  }

  /* Sort indicators, "Clear filters" visibility, replay note. */
  function updateHistChrome() {
    document.querySelectorAll('#hist-table thead th.sortable').forEach(function(th) {
      /* Date and Time are one ordering; the indicator sits on Date. */
      let k = th.getAttribute('data-sort');
      if (k === 'date') k = 'time'; else if (k === 'time') k = '-';
      const on = (k === _histSort.key);
      th.classList.toggle('sorted', on);
      th.querySelector('.sort-ind').textContent = on ? (_histSort.dir < 0 ? '▼' : '▲') : '';
      th.setAttribute('aria-sort', on ? (_histSort.dir < 0 ? 'descending' : 'ascending') : 'none');
    });
    const cb = document.getElementById('hist-clear-btn');
    if (cb) cb.hidden = !histFiltersActive();
    const note = document.getElementById('hist-replay-note');
    if (note) {
      const rp = _stationsData && _stationsData.replay;
      if (_histHasPending) {
        note.hidden = false;
        note.textContent = 'Stations are still being identified'
          + (rp && rp.done === false ? ' (' + fmtInt(rp.processed) + ' / ' + fmtInt(rp.total) + ')' : '')
          + ' — the list refreshes when this finishes.';
      } else note.hidden = true;
    }
  }

  (function() {
    document.querySelectorAll('#hist-table thead th.sortable').forEach(function(th) {
      th.addEventListener('click', function() { setHistSort(th.getAttribute('data-sort')); });
    });
  })();

  /* ---------------------------------------------------------------
   * Country / Station dropdowns.
   *
   * Native <select> cannot show images, and the flags are the point of
   * these two, so they are small custom dropdowns: a button plus a
   * panel positioned (fixed) under it so the dialog's overflow cannot
   * clip it.  Keyboard: type to search (station list), arrows + Enter,
   * Escape closes.
   * --------------------------------------------------------------- */
  function buildHistDropdowns() {
    buildDropdown('hist-country-dd', {
      allLabel: 'All countries',
      search: false,
      value: _histState.country,
      items: function() {
        return _histFacets.countries.map(function(c) {
          return { value: c.iso, html: flagHtml(c.iso) + '<span class="dd-name">' + escHtml(c.name) + '</span>',
                   count: c.count, text: c.name };
        });
      },
      label: function(v) {
        const c = _histFacets.countries.find(function(x) { return x.iso === v; });
        return c ? flagHtml(c.iso) + '<span class="dd-label">' + escHtml(c.name) + '</span>'
                 : '<span class="dd-label">' + (v ? escHtml(v.toUpperCase()) : 'All countries') + '</span>';
      },
      onPick: function(v) {
        _histState.country = v;
        /* A station from another country would leave nothing to show. */
        if (v && _histState.station) {
          const s = _histFacets.stations.find(function(x) { return x.key === _histState.station; });
          if (s && s.iso !== v) _histState.station = '';
        }
        buildHistDropdowns();
        applyHistFilters(false);
      }
    });
    buildDropdown('hist-station-dd', {
      allLabel: 'All stations',
      search: true,
      value: _histState.station,
      items: function() {
        /* Grouped by country (countries by message count), stations by count. */
        const list = [];
        _histFacets.countries.forEach(function(c) {
          if (_histState.country && c.iso !== _histState.country) return;
          const sts = _histFacets.stations.filter(function(s) { return s.iso === c.iso; });
          if (!sts.length) return;
          list.push({ group: true, html: flagHtml(c.iso) + escHtml(c.name), text: c.name });
          sts.forEach(function(s) {
            list.push({ value: s.key, sub: true, count: s.count, text: s.name + ' ' + s.khz + ' ' + s.letter,
                        html: '<span class="dd-name">' + escHtml(s.name) + '</span>'
                            + '<span class="dd-meta">· ' + escHtml(s.khz) + ' ' + escHtml(s.letter) + '</span>' });
          });
        });
        return list;
      },
      label: function(v) {
        const s = _histFacets.stations.find(function(x) { return x.key === v; });
        if (s) return flagHtml(s.iso) + '<span class="dd-label">' + escHtml(s.name) + ' · ' + escHtml(s.khz) + ' ' + escHtml(s.letter) + '</span>';
        if (v) { const p = v.split(':'); return '<span class="dd-label">' + escHtml((p[2] || v) + ' · ' + (p[0] || '') + ' ' + (p[1] || '')) + '</span>'; }
        return '<span class="dd-label">All stations</span>';
      },
      onPick: function(v) {
        _histState.station = v;
        buildHistDropdowns();
        applyHistFilters(false);
      }
    });
  }

  let _openDd = null;
  function closeAllDropdowns() {
    if (_openDd) {
      _openDd.classList.remove('open');
      const b = _openDd.querySelector('.dd-btn');
      if (b) b.setAttribute('aria-expanded', 'false');
      _openDd = null;
    }
  }

  function buildDropdown(id, cfg) {
    const root = document.getElementById(id);
    if (!root) return;
    root._cfg = cfg;
    if (!root._built) {
      root._built = true;
      root.innerHTML = '<button type="button" class="dd-btn" aria-haspopup="listbox" aria-expanded="false"></button>'
                     + '<div class="dd-panel" role="listbox">'
                     + (cfg.search ? '<input class="dd-search" type="text" placeholder="Search stations…">' : '')
                     + '<div class="dd-list"></div></div>';
      const btn = root.querySelector('.dd-btn');
      const panel = root.querySelector('.dd-panel');
      const search = root.querySelector('.dd-search');
      btn.addEventListener('click', function(ev) {
        ev.stopPropagation();
        if (root.classList.contains('open')) { closeAllDropdowns(); return; }
        closeAllDropdowns();
        root.classList.add('open'); _openDd = root;
        btn.setAttribute('aria-expanded', 'true');
        if (search) search.value = '';
        renderDropdownList(root, '');
        /* Position under the button, inside the viewport. */
        const r = btn.getBoundingClientRect();
        const w = Math.min(Math.max(r.width, 300), window.innerWidth - 16);
        panel.style.width = w + 'px';
        panel.style.left = Math.max(8, Math.min(r.left, window.innerWidth - w - 8)) + 'px';
        const below = window.innerHeight - r.bottom - 12;
        if (below >= 220 || below >= r.top) {
          panel.style.top = (r.bottom + 4) + 'px'; panel.style.bottom = 'auto';
          panel.style.maxHeight = Math.min(340, below) + 'px';
        } else {
          panel.style.top = 'auto'; panel.style.bottom = (window.innerHeight - r.top + 4) + 'px';
          panel.style.maxHeight = Math.min(340, r.top - 12) + 'px';
        }
        if (search && window.matchMedia('(pointer: fine)').matches) search.focus();
      });
      if (search) {
        search.addEventListener('input', function() { renderDropdownList(root, search.value); });
        search.addEventListener('click', function(ev) { ev.stopPropagation(); });
      }
      panel.addEventListener('click', function(ev) {
        const it = ev.target.closest('.dd-item');
        if (!it) return;
        ev.stopPropagation();
        closeAllDropdowns();
        root._cfg.onPick(it.getAttribute('data-value') || '');
      });
      root.addEventListener('keydown', function(ev) {
        if (!root.classList.contains('open')) return;
        if (ev.key === 'Tab') { closeAllDropdowns(); return; }   /* focus leaves: so does the panel */
        const items = Array.from(root.querySelectorAll('.dd-item'));
        let i = items.findIndex(function(x) { return x.classList.contains('kb'); });
        if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') {
          ev.preventDefault();
          if (i >= 0) items[i].classList.remove('kb');
          i = ev.key === 'ArrowDown' ? Math.min(items.length - 1, i + 1) : Math.max(0, i - 1);
          if (items[i]) {
            items[i].classList.add('kb'); items[i].scrollIntoView({ block: 'nearest' });
            if (!items[i].id) items[i].id = root.id + '-opt-' + i;
            panel.setAttribute('aria-activedescendant', items[i].id);
          }
        } else if (ev.key === 'Enter') {
          ev.preventDefault();
          const it = items[i >= 0 ? i : 0];
          if (it) { closeAllDropdowns(); root._cfg.onPick(it.getAttribute('data-value') || ''); }
        } else if (ev.key === 'Escape') {
          ev.stopPropagation(); closeAllDropdowns(); btn.focus();
        }
      });
    }
    const btn = root.querySelector('.dd-btn');
    btn.innerHTML = cfg.label(cfg.value);
    btn.classList.toggle('dd-set', !!cfg.value);
    if (root.classList.contains('open')) renderDropdownList(root, (root.querySelector('.dd-search') || {}).value || '');
  }

  function renderDropdownList(root, filterText) {
    const cfg = root._cfg;
    const list = root.querySelector('.dd-list');
    const f = (filterText || '').trim().toLowerCase();
    let items = cfg.items();
    if (f) {
      /* Station names first: "nit" is Niton, not every station in UNITED
       * Kingdom.  Only when no station matches does a country name count,
       * and then only as the start of a word ("swe" -> Sweden; "den" no
       * longer lists all of SweDEN).  A group header is kept when one of
       * its stations is. */
      const wordStart = function(text) {
        return text.toLowerCase().split(/[\s,()-]+/).some(function(w) { return w.indexOf(f) === 0; });
      };
      const filterBy = function(stationHit, groupHit) {
        const keep = [];
        let group = null, groupAll = false;
        items.forEach(function(it) {
          if (it.group) { group = it; groupAll = groupHit(it); return; }
          if (groupAll || stationHit(it)) {
            if (group) { keep.push(group); group = null; }
            keep.push(it);
          }
        });
        return keep;
      };
      let keep = filterBy(function(it) { return it.text.toLowerCase().indexOf(f) >= 0; },
                          function() { return false; });
      if (!keep.length)
        keep = filterBy(function() { return false; }, function(g) { return wordStart(g.text); });
      items = keep;
    }
    let html = f ? '' : '<div class="dd-item' + (!cfg.value ? ' sel' : '') + '" role="option" aria-selected="'
                      + (!cfg.value) + '" data-value="">'
                      + '<span class="dd-name">' + escHtml(cfg.allLabel) + '</span></div>';
    items.forEach(function(it) {
      if (it.group) { html += '<div class="dd-group" role="presentation">' + it.html + '</div>'; return; }
      html += '<div class="dd-item' + (it.sub ? ' dd-sub' : '') + (it.value === cfg.value ? ' sel' : '')
            + '" role="option" aria-selected="' + (it.value === cfg.value)
            + '" data-value="' + escHtml(it.value) + '">' + it.html
            + '<span class="dd-count">' + fmtInt(it.count) + '</span></div>';
    });
    if (!items.length && f) html += '<div class="dd-empty">No match.</div>';
    if (!items.length && !f) html += '<div class="dd-empty">No identified messages yet.</div>';
    list.innerHTML = html;
  }

  document.addEventListener('click', function(ev) {
    if (_openDd && !_openDd.contains(ev.target)) closeAllDropdowns();
  });
  window.addEventListener('resize', closeAllDropdowns);

  function histPagePrev() {
    if (_histPage > 0) { _histPage--; renderHistTable(_histFiltered); }
  }
  function histPageNext() {
    const pages = Math.ceil(_histFiltered.length / _HIST_PAGE_SZ);
    if (_histPage < pages - 1) { _histPage++; renderHistTable(_histFiltered); }
  }

  function renderHistTable(rows) {
    /* Keep a reference for pagination nav */
    _histFiltered = rows;

    const tbody   = document.getElementById('hist-tbody');
    const empty   = document.getElementById('hist-empty');
    const pagDiv  = document.getElementById('hist-pagination');
    const prevBtn = document.getElementById('hist-prev-btn');
    const nextBtn = document.getElementById('hist-next-btn');
    const pageInfo= document.getElementById('hist-page-info');

    tbody.innerHTML = '';

    if (rows.length === 0) {
      empty.style.display   = 'block';
      empty.textContent     = _histData.length ? 'No entries match these filters.' : 'No entries found.';
      pagDiv.style.display  = 'none';
      return;
    }
    empty.style.display = 'none';

    /* Pagination slice */
    const totalPages = Math.ceil(rows.length / _HIST_PAGE_SZ);
    if (_histPage >= totalPages) _histPage = totalPages - 1;
    const start = _histPage * _HIST_PAGE_SZ;
    const page  = rows.slice(start, start + _HIST_PAGE_SZ);

    /* Show/hide pagination bar */
    if (totalPages > 1) {
      pagDiv.style.display  = 'flex';
      prevBtn.disabled      = (_histPage === 0);
      nextBtn.disabled      = (_histPage >= totalPages - 1);
      pageInfo.textContent  = 'Page ' + (_histPage + 1) + ' / ' + fmtInt(totalPages)
                            + ' (' + fmtInt(rows.length) + ' entries)';
    } else {
      pagDiv.style.display  = 'none';
    }

    function fmtDur(s) {
      if (s === null || s === undefined) return '—';
      const m = Math.floor(s / 60), sec = s % 60;
      return m > 0 ? m + 'm ' + sec + 's' : sec + 's';
    }

    /* Built as one HTML string: 50 rows of createElement calls was the
     * slowest part of paging, and every value below is escaped. */
    let html = '';
    page.forEach(function(m, idx) {
      const isRaw = !m._msg;
      const dec = isRaw ? { station: '—', subject: '—', serial: '—' } : decodeNavtexId(m.id || '');
      html += '<tr data-idx="' + (start + idx) + '">';

      /* Date */
      html += '<td class="hist-date">' + escHtml(m.date || '—') + '</td>';

      /* Time — only for message files; raw logs span a whole day */
      if (isRaw) {
        html += '<td class="hist-time"><span style="font-size:0.7rem;background:#1a3a5c;color:#53d8fb;'
              + 'border-radius:3px;padding:1px 5px;letter-spacing:0.5px;">RAW LOG</span></td>';
      } else {
        const t = (m._shownTime || '').replace(/Z$/i, '');
        html += '<td class="hist-time">' + escHtml(t.length === 6
          ? t.slice(0,2) + ':' + t.slice(2,4) + ':' + t.slice(4,6)
          : (m.time || '—')) + '</td>';
      }

      /* Frequency */
      html += '<td class="hist-freq">' + escHtml(m.freq || '—') + '</td>';

      /* ID (raw code, e.g. "EA42") — blank for raw logs */
      html += '<td class="hist-id">' + escHtml(isRaw ? '—' : ((m.id && m.id !== 'unknown') ? m.id.toUpperCase() : '—')) + '</td>';

      /* Serial — from server-parsed field, or decoded from id */
      let ser = '—';
      if (!isRaw) { const s2 = m.serial || dec.serial || ''; ser = (s2 && s2 !== '—') ? s2 : '—'; }
      html += '<td style="font-family:monospace;color:#a0b8d8">' + escHtml(ser) + '</td>';

      /* Station — flag + name + confidence pill, or "identifying…" */
      html += '<td class="hist-station">';
      if (isRaw)             html += '—';
      else if (m.st_pending) html += pendingHtml('');
      else if (m.st)         html += stationChipHtml(m.st, { rel: m.rel });
      else                   html += '<span style="color:#7d8aa3">' + escHtml(dec.station) + '</span>';
      html += '</td>';

      /* Subject */
      html += '<td>' + escHtml(isRaw ? '—' : dec.subject) + '</td>';

      /* Distance (+ bearing, night-path mark) */
      html += '<td class="hist-dist">';
      if (m._dist >= 0) {
        html += fmtKm(m._dist);
        if (m.st && typeof m.st.bearing === 'number' && m.st.bearing >= 0)
          html += '<span class="dist-brg">' + compassPoint(m.st.bearing) + '</span>';
        if (m._path === 'night') html += '<span class="dist-night" title="Night path">☾</span>';
      } else html += '<span style="color:#446">—</span>';
      html += '</td>';

      /* SNR — colour-coded, blank for raw logs or missing metrics */
      if (!isRaw && m.snr !== null && m.snr !== undefined) {
        /* Recorded before audio protocol version 4, so the stored figure was on
         * the old noise-density scale and has been converted for display. */
        html += '<td class="hist-snr ' + snrCls(m.snr) + '"'
              + (m.snr_rescaled ? ' title="Recorded on the pre-v4 noise-density scale; '
                                + 'converted for comparison with newer messages."' : '')
              + '>' + Number(m.snr).toFixed(1) + ' dB</td>';
      } else {
        html += '<td class="hist-snr dim">—</td>';
      }

      /* Duration */
      html += '<td class="hist-dur">' + (isRaw ? '—' : fmtDur(m.duration_s)) + '</td>';

      /* View / Download button */
      html += '<td><button class="hist-view-btn">' + (isRaw ? 'Download' : 'View') + '</button></td>';
      html += '</tr>';
    });
    tbody.innerHTML = html;
  }

  /* One delegated handler for the table: the View/Download button or a
   * click anywhere on a message row opens it. */
  (function() {
    const tbody = document.getElementById('hist-tbody');
    if (!tbody) return;
    tbody.addEventListener('click', function(ev) {
      const tr = ev.target.closest('tr[data-idx]');
      if (!tr) return;
      const m = _histFiltered[Number(tr.getAttribute('data-idx'))];
      if (!m) return;
      if (!m._msg) {
        if (ev.target.closest('.hist-view-btn')) downloadRawLog(m.rel, m.freq, m.date);
        return;
      }
      const t = (m._shownTime || '').replace(/Z$/i, '');
      const tFmt = t.length === 6
        ? t.slice(0,2) + ':' + t.slice(2,4) + ':' + t.slice(4,6) + ' UTC'
        : (m.time || '—');
      const idStr = (m.id && m.id !== 'unknown') ? m.id.toUpperCase() : '—';
      const stName = (m.st && m.st.key && m.st.name) ? m.st.name : decodeNavtexId(m.id || '').station;
      const title = (m.freq || '') + ' · ' + (m.date || '') + ' ' + tFmt + ' · ' + idStr + ' · ' + stName;
      viewMessage(m.rel, title);
    });
  })();

  function downloadRawLog(rel, freq, date) {
    const base = (window._BASE_PATH || '').replace(/\/$/, '');
    const url = base + '/api/history/file?rel=' + encodeURIComponent(rel);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'navtex-raw-' + (freq || 'log').replace(/\s/g, '') + '-' + (date || 'log') + '.log';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
  }

  /* ---- Metrics panel renderer ---- */
  function renderMetricsPanel(m) {
    const panel = document.getElementById('msg-metrics-panel');
    if (!panel) return;
    if (!m) { panel.className = 'msg-metrics-panel hidden'; return; }

    function fmt(v, digits) {
      return (v === null || v === undefined) ? '\u2014' : Number(v).toFixed(digits !== undefined ? digits : 1);
    }
    function fmtPct(v) { return (v === null || v === undefined) ? '\u2014' : Number(v).toFixed(1) + '%'; }
    function fecClass(pct) {
      if (pct === null || pct === undefined) return '';
      return pct > 30 ? 'warn' : 'good';
    }
    function errClass(pct) {
      if (pct === null || pct === undefined) return '';
      return pct > 10 ? 'bad' : pct > 3 ? 'warn' : 'good';
    }

    /* Format UTC timestamps nicely */
    function fmtUtc(iso) {
      if (!iso) return '\u2014';
      return iso.replace('T', ' ').replace('Z', ' UTC');
    }
    function fmtDuration(s) {
      if (s === null || s === undefined) return '\u2014';
      const m = Math.floor(s / 60), sec = s % 60;
      return m > 0 ? m + 'm ' + sec + 's' : sec + 's';
    }

    /* Build FEC quality bar */
    const cleanPct  = m.avg_chars_clean_pct  || 0;
    const fecPct    = m.avg_chars_fec_pct    || 0;
    const failPct   = m.avg_chars_failed_pct || 0;

    panel.innerHTML =
      '<div class="mm-item">'
        + '<span class="mm-label">Start (UTC)</span>'
        + '<span class="mm-value">' + fmtUtc(m.start_utc) + '</span>'
      + '</div>'
      + '<div class="mm-item">'
        + '<span class="mm-label">End (UTC)</span>'
        + '<span class="mm-value">' + fmtUtc(m.end_utc) + '</span>'
      + '</div>'
      + '<div class="mm-item">'
        + '<span class="mm-label">Duration</span>'
        + '<span class="mm-value">' + fmtDuration(m.duration_s) + '</span>'
      + '</div>'
      + (m.avg_snr_db !== null && m.avg_snr_db !== undefined
        ? '<div class="mm-item">'
            + '<span class="mm-label">Avg SNR</span>'
            + '<span class="mm-value ' + snrClass(m.avg_snr_db) + '">' + fmt(m.avg_snr_db, 1) + ' dB</span>'
          + '</div>'
        : '')
      + (m.avg_bb_power_dbfs !== null && m.avg_bb_power_dbfs !== undefined
        ? '<div class="mm-item">'
            + '<span class="mm-label">Avg Signal</span>'
            + '<span class="mm-value">' + fmt(m.avg_bb_power_dbfs, 1) + ' dBFS</span>'
          + '</div>'
        : '')
      + (m.avg_noise_density_dbfs !== null && m.avg_noise_density_dbfs !== undefined
        ? '<div class="mm-item">'
            + '<span class="mm-label">Avg Noise</span>'
            + '<span class="mm-value">' + fmt(m.avg_noise_density_dbfs, 1) + ' dBFS</span>'
          + '</div>'
        : '')
      + (m.avg_chars_clean_pct !== null && m.avg_chars_clean_pct !== undefined
        ? '<div class="mm-item" style="min-width:160px">'
            + '<span class="mm-label">Decode Quality (clean / fec / err)</span>'
            + '<span class="mm-value">'
              + '<span class="' + '' + '">' + fmtPct(cleanPct) + '</span>'
              + ' / <span class="' + fecClass(fecPct) + '">' + fmtPct(fecPct) + '</span>'
              + ' / <span class="' + errClass(failPct) + '">' + fmtPct(failPct) + '</span>'
            + '</span>'
            + '<div class="mm-fec-bar">'
              + '<div class="mm-fec-clean" style="width:' + cleanPct.toFixed(1) + '%"></div>'
              + '<div class="mm-fec-fec"   style="width:' + fecPct.toFixed(1)   + '%"></div>'
              + '<div class="mm-fec-fail"  style="width:' + failPct.toFixed(1)  + '%"></div>'
            + '</div>'
          + '</div>'
        : '')
      + '<div class="mm-item">'
        + '<span class="mm-label">Char Count</span>'
        + '<span class="mm-value">' + (m.char_count !== null && m.char_count !== undefined ? m.char_count : '\u2014') + '</span>'
      + '</div>'
      + '<div class="mm-item">'
        + '<span class="mm-label">Samples</span>'
        + '<span class="mm-value dim">' + (m.sample_count || '\u2014') + '</span>'
      + '</div>';

    panel.className = 'msg-metrics-panel';
  }

  function viewMessage(path, title) {
    const modal   = document.getElementById('msg-modal');
    const body    = document.getElementById('msg-modal-body');
    const ttl     = document.getElementById('msg-modal-title');
    const copyBtn = document.getElementById('msg-modal-copy-btn');
    const dlBtn   = document.getElementById('msg-modal-dl-btn');
    const panel   = document.getElementById('msg-metrics-panel');
    if (!modal || !body) return;
    ttl.textContent  = title || 'Message';
    body.textContent = 'Loading\u2026';
    if (panel) panel.className = 'msg-metrics-panel hidden';
    renderIdentPanel(null);
    /* Reset button state while loading */
    if (copyBtn) { copyBtn.onclick = null; copyBtn.style.opacity = '0.4'; }
    if (dlBtn)   { dlBtn.onclick   = null; dlBtn.style.opacity   = '0.4'; }
    modal.style.display = 'flex';
    const base = (window._BASE_PATH || '').replace(/\/$/, '');

    /* Fetch message text and metrics sidecar in parallel.
     * Both endpoints accept a relative path (rel=) — never an absolute path. */
    const textPromise    = fetch(base + '/api/history/file?rel='    + encodeURIComponent(path))
      .then(function(r) { if (!r.ok) throw new Error('HTTP ' + r.status); return r.text(); });
    const metricsPromise = fetch(base + '/api/history/metrics?rel=' + encodeURIComponent(path))
      .then(function(r) { return r.ok ? r.json() : null; })
      .catch(function()  { return null; });
    /* The identification is also in the sidecar (metrics.ident), but
     * /api/ident is authoritative while a message is still queued. */
    const identPromise = fetch(base + '/api/ident?rel=' + encodeURIComponent(path))
      .then(function(r) { return r.ok ? r.json() : null; })
      .catch(function()  { return null; });

    Promise.all([textPromise, metricsPromise, identPromise])
      .then(function(results) {
        const text    = results[0];
        const metrics = results[1];
        const ident   = results[2] || (metrics && metrics.ident) || null;
        if (ident) _identCache.set(path, ident);

        body.textContent = text;
        renderIdentPanel(ident);
        renderMetricsPanel(metrics);

        /* Derive a download filename from the path (last component) */
        const fname = path.split('/').pop() || 'navtex-message.txt';
        /* Wire copy button */
        if (copyBtn) {
          copyBtn.style.opacity = '1';
          copyBtn.onclick = function() {
            if (navigator.clipboard && navigator.clipboard.writeText) {
              navigator.clipboard.writeText(text).catch(function() { fallbackCopy(text); });
            } else {
              fallbackCopy(text);
            }
          };
        }
        /* Wire download button */
        if (dlBtn) {
          dlBtn.style.opacity = '1';
          dlBtn.onclick = function() {
            const blob = new Blob([text], { type: 'text/plain' });
            const url  = URL.createObjectURL(blob);
            const a    = document.createElement('a');
            a.href     = url;
            a.download = fname;
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
            URL.revokeObjectURL(url);
          };
        }
      })
      .catch(function(err) {
        body.textContent = 'Error loading message: ' + err;
        if (panel) panel.className = 'msg-metrics-panel hidden';
        if (copyBtn) copyBtn.style.opacity = '0.4';
        if (dlBtn)   dlBtn.style.opacity   = '0.4';
      });
  }

  /* ---------------------------------------------------------------
   * Keyboard, page-wide.
   *
   *  - Enter or Space on a sortable column header sorts, as a click does
   *    (headers carry tabindex=0 and aria-sort).
   *  - Escape closes the topmost modal: the message view, then History.
   *    (Metrics has its own handler, which first closes an open dropdown.)
   *    An open facts popover or dropdown takes the Escape first.
   * --------------------------------------------------------------- */
  (function() {
    document.addEventListener('keydown', function(ev) {
      if ((ev.key === 'Enter' || ev.key === ' ') && ev.target && ev.target.matches &&
          ev.target.matches('th.sortable')) {
        ev.preventDefault();
        ev.target.click();
      }
    });
    window.addEventListener('keydown', function(ev) {
      if (ev.key !== 'Escape') return;
      const pop = document.getElementById('ident-pop');
      if ((pop && pop.style.display === 'block') || _openDd) return;
      const shown = function(id) { const m = document.getElementById(id); return m && m.style.display !== 'none'; };
      if (shown('msg-modal'))          { closeMsgModal(); ev.preventDefault(); }
      else if (shown('history-modal') && !shown('metrics-modal')) { closeHistory(); ev.preventDefault(); }
    }, true);
  })();

  function closeMsgModal() {
    const modal = document.getElementById('msg-modal');
    if (modal) modal.style.display = 'none';
    dropIdentMap();
  }

  /* ═══════════════════════════════════════════════════════════════════
   * Metrics view
   *
   * The Metrics modal is a tabbed view.  "Activity" is the original pair
   * of bar charts from /api/metrics (every logged message, fixed 24 h /
   * 30 d windows).  The other five tabs are built entirely in the browser
   * from /api/analytics: one compact, columnar JSON document holding a
   * row per identified message (time, frequency, station, subject,
   * confidence, distance, bearing, day/night path, SNR, duration, failed
   * characters).  The server never aggregates; every chart is a plain
   * loop over typed arrays here, so changing a filter costs a few
   * milliseconds even with 30,000+ rows, and no request.
   *
   * Flow:  open -> fetch /api/metrics and /api/analytics in parallel
   *        -> mxDecode() turns the columns into typed arrays (once)
   *        -> mxSelect() applies the filter bar, giving an index list
   *        -> the active tab's renderer aggregates over that list.
   * Inactive tabs are only marked stale and rendered when shown.
   *
   * Every chart sits in a "card" with a Table toggle and a CSV button.
   * Both read the same dataset object the chart was drawn from, so the
   * exported CSV is exactly the aggregate on screen.  The charts are
   * hand-drawn SVG (and one canvas, for the scatter of up to 30k
   * points), so there is no chart library to fail to load; only the
   * coverage map needs Leaflet, and without it the map hides itself and
   * the polar chart and figures beside it carry the same information.
   *
   * Filter and option state lives in the URL hash ("#metrics?tab=coverage
   * &range=7d&c=gb|ie"), so a view can be bookmarked or shared.  Only
   * values that differ from MX_DEFAULTS are written.
   * ═══════════════════════════════════════════════════════════════════ */
  const MX_TABS = ['activity', 'coverage', 'distance', 'stations', 'breakdown', 'trends'];
  const MX_DEFAULTS = {
    tab: 'activity', range: '30d', from: '', to: '', freq: '', lvl: 'auto', c: '', s: '', path: '',
    pct: 'max', fp: 'dn',                 /* coverage: reach statistic, footprints shown (d/n/a) */
    colour: 'freq',                       /* distance: scatter colouring */
    hsort: 'count',                       /* stations: heatmap row order */
    g: 'station', m: 'msgs', k: 'none',   /* breakdown: group, metric, split */
    bucket: 'day', split: '0'             /* trends */
  };
  const MX_FREQ_COLS = ['#53d8fb', '#ffeb3b', '#d55181', '#9085e9'];
  const MX_LVL_COLS  = ['#56627a', '#c98a92', '#ffc107', '#4caf50'];
  const MX_LVL_NAMES = ['Unidentified', 'Low', 'Medium', 'High'];
  const MX_DAY_COL = '#f2b33d', MX_NIGHT_COL = '#7b8cff', MX_ALL_COL = '#53d8fb';
  const MX_WEEKDAYS = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];
  const MX_SURFACE = '#0a1628';        /* card background: ring and gap colour */
  const MX_GRID = '#16304e', MX_AXIS = '#2a4a7f';

  let _mxState = Object.assign({}, MX_DEFAULTS);
  let _mx = null;              /* decoded analytics (mxDecode) */
  let _mxErr = '';             /* why analytics could not be loaded */
  let _mxFetch = null;         /* in-flight analytics request */
  let _mxActivity = null;      /* /api/metrics payload, or {error} */
  let _mxSel = null;           /* current selection (mxSelect) */
  let _mxStale = {};           /* tab -> must re-render before showing */
  let _mxData = {};            /* chart id -> dataset {name, cols, rows, empty} */
  let _mxTable = {};           /* chart id -> showing the Table view */
  let _mxMap = null, _mxMapLayers = null, _mxMapFit = '';
  let _mxHeatAll = false, _mxBarsAll = false;
  let _mxRank = { key: 'n', dir: -1 };
  let _mxTimer = null;
  let _mxWired = false;

  /* ---- small helpers ---- */
  function mxEl(id) { return document.getElementById(id); }
  function mxKhz(f) { return (f % 1 ? Number(f).toFixed(1) : String(f)) + ' kHz'; }
  function mxNum(v, dp) { return (typeof v === 'number' && isFinite(v)) ? v.toFixed(dp || 0) : '—'; }
  function mxPct(v) { return (typeof v === 'number' && isFinite(v)) ? Math.round(v * 100) + '%' : '—'; }
  function mxDb(v) { return (typeof v === 'number' && isFinite(v)) ? (v > 0 ? '+' : '') + v.toFixed(1) + ' dB' : '—'; }
  function mxCompact(v) {
    if (!isFinite(v)) return '';
    const a = Math.abs(v);
    if (a >= 1000) return (v / 1000).toFixed(a >= 10000 ? 0 : 1).replace(/\.0$/, '') + 'K';
    return String(Math.round(v * 100) / 100);
  }
  function mxUtcDate(t) { return new Date(t * 1000).toISOString().slice(0, 10); }
  function mxUtcStamp(t) { return new Date(t * 1000).toISOString().slice(0, 16).replace('T', ' ') + 'Z'; }
  /* Sorted-array quantile with linear interpolation (p in 0..1). */
  function mxQ(sorted, p) {
    const n = sorted.length;
    if (!n) return NaN;
    if (p >= 1) return sorted[n - 1];
    const x = (n - 1) * p, i = Math.floor(x);
    return i + 1 < n ? sorted[i] + (sorted[i + 1] - sorted[i]) * (x - i) : sorted[i];
  }
  function mxSorted(arr) { return Float64Array.from(arr).sort(); }
  function mxMedian(arr) { return mxQ(mxSorted(arr), 0.5); }
  /* "Nice" axis: ticks at 1/2/2.5/5 x 10^n covering min(0, min)..max.
   * The axis always includes zero; it only goes below zero when a value
   * does (a median SNR can be negative), so bars still grow from zero. */
  function mxTicks(max, n, min) {
    if (!(max > 0)) max = (min < 0) ? 0 : 1;
    const lo0 = (min < 0) ? min : 0;
    const raw = (max - lo0) / (n || 4), mag = Math.pow(10, Math.floor(Math.log10(raw)));
    const step = [1, 2, 2.5, 5, 10].map(function(k) { return k * mag; }).find(function(s) { return s >= raw; });
    const lo = lo0 < 0 ? Math.floor(lo0 / step + 1e-9) * step : 0;
    const top = Math.ceil(max / step - 1e-9) * step, ticks = [];
    for (let v = lo; v <= top + step / 2; v += step) ticks.push(Math.round(v * 1e6) / 1e6);
    return { lo: lo, top: top, ticks: ticks };
  }
  /* Smallest finite value over a set of series (for mxTicks' min). */
  function mxMinOf(series) {
    let m = 0;
    series.forEach(function(s) { s.vals.forEach(function(v) { if (v < m) m = v; }); });
    return m;
  }
  function mxFreqCol(f) {
    const i = _mx ? _mx.freqs.indexOf(f) : -1;
    return MX_FREQ_COLS[(i < 0 ? 0 : i) % MX_FREQ_COLS.length];
  }
  function mxStationLabel(si, opts) {
    const s = _mx.stations[si];
    if (!s) return flagHtml('') + '<span>Unidentified</span>';
    return flagHtml(s.iso2) + '<span>' + escHtml(s.name) + '</span>'
         + (opts && opts.noMeta ? '' : '<em>' + escHtml(s.freq_khz + ' ' + s.letter) + '</em>');
  }
  function mxSubjectName(c) {
    return (typeof SUBJECT === 'object' && SUBJECT[c]) ? SUBJECT[c] : '';
  }

  /* ---- shared tooltip ---- */
  function mxTipShow(html, x, y) {
    const t = mxEl('mx-tip');
    if (!t) return;
    t.innerHTML = html;
    t.style.display = 'block';
    const w = t.offsetWidth, h = t.offsetHeight;
    let left = x + 14, top = y + 14;
    if (left + w > window.innerWidth - 6) left = Math.max(6, x - w - 14);
    if (top + h > window.innerHeight - 6) top = Math.max(6, y - h - 14);
    t.style.left = left + 'px'; t.style.top = top + 'px';
  }
  function mxTipHide() { const t = mxEl('mx-tip'); if (t) t.style.display = 'none'; }
  /* Tooltip row: line key in the series colour, the value strong, the
   * series name after it (the reader already knows the series and is
   * looking for the number). */
  function mxTipRow(col, val, name) {
    return '<div class="mx-tr"><i style="border-color:' + col + '"></i><b>' + escHtml(val) + '</b><span>'
         + escHtml(name) + '</span></div>';
  }
  function mxTipHead(html) { return '<div class="mx-th">' + html + '</div>'; }

  /* ═══ Data: fetch, decode, select ═══ */

  /* Turn the columnar JSON into typed arrays.  null -> NaN for the
   * optional numeric columns, so the aggregation loops can test with
   * "v === v" instead of type checks. */
  function mxDecode(d) {
    const c = (d && d.cols) || {};
    const n = (c.t || []).length;
    function f32(a) {
      const o = new Float32Array(n);
      for (let i = 0; i < n; i++) { const v = a ? a[i] : null; o[i] = (typeof v === 'number') ? v : NaN; }
      return o;
    }
    const t = new Float64Array(n), f = new Float64Array(n), s = new Int16Array(n);
    const b2 = new Uint8Array(n), lvl = new Int8Array(n), night = new Uint8Array(n);
    const freqSet = new Set();
    for (let i = 0; i < n; i++) {
      t[i] = +c.t[i] || 0;
      f[i] = +(c.f && c.f[i]) || 0;
      freqSet.add(f[i]);
      const si = c.s ? c.s[i] : -1;
      s[i] = (typeof si === 'number') ? si : -1;
      const b = c.b2 && c.b2[i];
      b2[i] = b ? String(b).charCodeAt(0) : 63;
      lvl[i] = Math.max(0, Math.min(3, (c.lvl && c.lvl[i]) | 0));
      night[i] = (c.night && c.night[i]) ? 1 : 0;
    }
    const stations = (d.stations || []).map(function(x) {
      return { key: x.key, name: x.name || '?', iso2: x.iso2 || '', country: x.country_name || '',
               navarea: x.navarea, freq_khz: x.freq_khz, letter: x.letter || '', lat: x.lat, lon: x.lon };
    });
    return {
      n: n, t: t, f: f, s: s, b2: b2, lvl: lvl, night: night,
      conf: f32(c.conf), dist: f32(c.dist), brg: f32(c.brg), snr: f32(c.snr), dur: f32(c.dur), fail: f32(c.fail),
      freqs: Array.from(freqSet).sort(function(a, b) { return a - b; }),
      stations: stations, receiver: d.receiver || {}, generated: d.generated_utc || '',
      replay: d.replay || null,
      fetchedAt: Date.now()
    };
  }

  function mxLoad(force) {
    if (_mx && !force && Date.now() - _mx.fetchedAt < 60000) return Promise.resolve(_mx);
    if (_mxFetch) return _mxFetch;
    const t0 = performance.now();
    _mxFetch = fetch(apiBase() + '/api/analytics')
      .then(function(r) { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
      .then(function(d) {
        _mx = mxDecode(d); _mxErr = '';
        _mx.loadMs = Math.round(performance.now() - t0);
        mxFillFreqSelect();
        return _mx;
      })
      .catch(function(e) { _mxErr = String(e && e.message || e); return _mx; })
      .then(function(x) { _mxFetch = null; return x; });
    return _mxFetch;
  }

  /* UTC [lo, hi] seconds for the date-range control. */
  function mxRangeBounds() {
    const st = _mxState, now = Date.now() / 1000;
    if (st.range === '24h') return [now - 86400, Infinity];
    if (st.range === '7d')  return [now - 7 * 86400, Infinity];
    if (st.range === '30d') return [now - 30 * 86400, Infinity];
    if (st.range === 'custom') {
      const lo = Date.parse((st.from || '') + 'T00:00:00Z'), hi = Date.parse((st.to || '') + 'T00:00:00Z');
      return [isFinite(lo) ? lo / 1000 : -Infinity, isFinite(hi) ? hi / 1000 + 86399.999 : Infinity];
    }
    return [-Infinity, Infinity];
  }
  function mxList(v) { return v ? String(v).split('|').filter(Boolean) : []; }

  /* Minimum confidence for DISTANCE-derived numbers.
   *
   * A Low attribution is a guess, and a guess about which of several
   * same-letter transmitters sent a message is exactly what stretches a
   * coverage footprint: one misread letter turns a Split message into
   * "Buenos Aires, 11,338 km".  So with the confidence control on Auto
   * (the default), everything built from distances — footprint, reach by
   * bearing, median distance, distance by hour, SNR vs distance, maximum
   * distance in Breakdown — uses Medium or better, and the single
   * "furthest station" figure, which one bad row can set on its own, uses
   * High only.  Counts (messages, stations, heatmap, activity) keep every
   * message.  Choosing a level explicitly applies it to everything alike,
   * through mxSelect(), so these return 0 (no further filtering).
   * kind: 'dist' or 'far'. */
  function mxDistMin(kind) {
    if (_mxState.lvl !== 'auto') return 0;
    return kind === 'far' ? 3 : 2;
  }
  /* Short wording of that basis, for card subtitles and tiles. */
  function mxDistBasis(kind) {
    const l = _mxState.lvl === 'auto' ? mxDistMin(kind) : (Number(_mxState.lvl) || 0);
    return l >= 3 ? 'High confidence' : l === 2 ? 'Medium+ confidence' : l === 1 ? 'Low+ confidence' : 'any confidence';
  }

  /* Apply the filter bar.  Returns the matching row indices, plus the
   * per-station counts over every filter EXCEPT country/station — the
   * counts the country and station dropdowns show, so picking one
   * country does not make the others vanish from the list. */
  function mxSelect() {
    const D = _mx, st = _mxState;
    const b = mxRangeBounds(), lo = b[0], hi = b[1];
    const fq = st.freq === '' ? NaN : Number(st.freq);
    const ml = st.lvl === 'auto' ? 0 : (Number(st.lvl) || 0);
    const pv = st.path === 'day' ? 0 : st.path === 'night' ? 1 : -1;
    const cset = new Set(mxList(st.c)), sset = new Set(mxList(st.s));
    const ns = D.stations.length;
    const allow = new Uint8Array(ns);
    for (let k = 0; k < ns; k++) {
      const s = D.stations[k];
      allow[k] = ((!cset.size || cset.has(s.iso2)) && (!sset.size || sset.has(s.key))) ? 1 : 0;
    }
    const filt = cset.size > 0 || sset.size > 0;
    const facet = new Int32Array(ns);
    const idx = new Int32Array(D.n);
    let m = 0, unid = 0;
    for (let i = 0; i < D.n; i++) {
      const t = D.t[i];
      if (t < lo || t > hi) continue;
      if (fq === fq && D.f[i] !== fq) continue;
      if (D.lvl[i] < ml) continue;
      if (pv >= 0 && D.night[i] !== pv) continue;
      const s = D.s[i];
      if (s >= 0) facet[s]++; else unid++;
      if (filt && (s < 0 || !allow[s])) continue;
      idx[m++] = i;
    }
    return { idx: idx.subarray(0, m), facet: facet, unid: unid };
  }

  /* ═══ State <-> URL hash ═══ */
  function mxStateFromHash() {
    const h = location.hash || '';
    if (h.indexOf('#metrics') !== 0) return null;
    const qi = h.indexOf('?');
    const p = new URLSearchParams(qi >= 0 ? h.slice(qi + 1) : '');
    const st = Object.assign({}, MX_DEFAULTS);
    let stored = '';
    try { stored = localStorage.getItem('navtex.mx.tab') || ''; } catch (e) { /* private mode */ }
    if (MX_TABS.indexOf(stored) >= 0) st.tab = stored;
    Object.keys(MX_DEFAULTS).forEach(function(k) { if (p.has(k)) st[k] = p.get(k); });
    if (MX_TABS.indexOf(st.tab) < 0) st.tab = MX_DEFAULTS.tab;
    return st;
  }
  function mxStateToHash() {
    const p = new URLSearchParams();
    Object.keys(MX_DEFAULTS).forEach(function(k) {
      if (k === 'tab' || _mxState[k] !== MX_DEFAULTS[k]) p.set(k, _mxState[k]);
    });
    const h = '#metrics?' + p.toString();
    if (location.hash !== h) history.replaceState(null, '', h);
    try { localStorage.setItem('navtex.mx.tab', _mxState.tab); } catch (e) { /* ignore */ }
  }
  function mxFiltersActive() {
    return ['range', 'from', 'to', 'freq', 'lvl', 'c', 's', 'path'].some(function(k) {
      return _mxState[k] !== MX_DEFAULTS[k];
    });
  }

  /* ═══ Open / close / wiring ═══ */
  function openMetrics(preset) {
    const modal = mxEl('metrics-modal');
    if (!modal) return;
    _mxState = preset || mxStateFromHash() || (function() {
      const st = Object.assign({}, MX_DEFAULTS);
      try { const t = localStorage.getItem('navtex.mx.tab'); if (MX_TABS.indexOf(t) >= 0) st.tab = t; } catch (e) { /* */ }
      return st;
    })();
    modal.style.display = 'flex';
    mxWire();
    mxSyncControls();
    mxStateToHash();
    MX_TABS.forEach(function(t) { _mxStale[t] = true; });
    _mxActivity = null;
    mxShowTab();
    fetch(apiBase() + '/api/metrics')
      .then(function(r) { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
      .then(function(d) { _mxActivity = d; })
      .catch(function(e) { _mxActivity = { error: String(e && e.message || e) }; })
      .then(function() { _mxStale.activity = true; if (_mxState.tab === 'activity') mxShowTab(); });
    const body = mxEl('metrics-body');
    if (_mx && body) body.classList.add('mx-busy');
    mxLoad(false).then(function() {
      if (body) body.classList.remove('mx-busy');
      if (mxEl('metrics-modal').style.display === 'none') return;
      mxRefresh();
    });
  }

  function closeMetrics() {
    const modal = mxEl('metrics-modal');
    if (modal) modal.style.display = 'none';
    mxTipHide();
    closeAllDropdowns();
    if ((location.hash || '').indexOf('#metrics') === 0) clearHash();
  }

  function mxWire() {
    if (_mxWired) return;
    _mxWired = true;
    const modal = mxEl('metrics-modal');
    modal.addEventListener('click', function(ev) {
      const t = ev.target.closest('[data-mx-tab],[data-mx-range],[data-mx-path],[data-mx-table],[data-mx-csv],'
                                + '[data-mx-set],[data-mx-rsort],[data-mx-more],[data-mx-reset]');
      if (!t) return;
      if (t.hasAttribute('data-mx-tab')) {
        _mxState.tab = t.getAttribute('data-mx-tab');
        mxStateToHash(); mxSyncControls(); mxShowTab();
      } else if (t.hasAttribute('data-mx-range')) {
        _mxState.range = t.getAttribute('data-mx-range');
        if (_mxState.range === 'custom' && !_mxState.from && _mx && _mx.n) {
          /* Start the custom range on the last 7 days of data. */
          let mx = 0; for (let i = 0; i < _mx.n; i++) if (_mx.t[i] > mx) mx = _mx.t[i];
          _mxState.to = mxUtcDate(mx); _mxState.from = mxUtcDate(mx - 6 * 86400);
        }
        mxChanged();
      } else if (t.hasAttribute('data-mx-path')) {
        _mxState.path = t.getAttribute('data-mx-path'); mxChanged();
      } else if (t.hasAttribute('data-mx-table')) {
        const id = t.getAttribute('data-mx-table');
        _mxTable[id] = !_mxTable[id];
        _mxStale[_mxState.tab] = true; mxShowTab();
      } else if (t.hasAttribute('data-mx-csv')) {
        mxCsv(t.getAttribute('data-mx-csv'));
      } else if (t.hasAttribute('data-mx-set')) {
        /* Tab-local toggle buttons: data-mx-set="key" data-v="value" */
        const k = t.getAttribute('data-mx-set'), v = t.getAttribute('data-v');
        if (k === 'fp') {
          /* Footprint layers toggle independently; keep at least one. */
          let fp = _mxState.fp;
          fp = fp.indexOf(v) >= 0 ? fp.replace(v, '') : fp + v;
          _mxState.fp = fp || v;
        } else _mxState[k] = v;
        mxStateToHash(); _mxStale[_mxState.tab] = true; mxShowTab();
      } else if (t.hasAttribute('data-mx-rsort')) {
        const k = t.getAttribute('data-mx-rsort');
        _mxRank = { key: k, dir: _mxRank.key === k ? -_mxRank.dir : (k === 'name' || k === 'country' ? 1 : -1) };
        _mxStale.stations = true; mxShowTab();
      } else if (t.hasAttribute('data-mx-more')) {
        const k = t.getAttribute('data-mx-more');
        if (k === 'heat') _mxHeatAll = !_mxHeatAll; else _mxBarsAll = !_mxBarsAll;
        _mxStale[_mxState.tab] = true; mxShowTab();
      } else if (t.hasAttribute('data-mx-reset')) {
        mxResetFilters();
      }
    });
    modal.addEventListener('change', function(ev) {
      const t = ev.target;
      if (t.hasAttribute('data-mx-opt')) {
        _mxState[t.getAttribute('data-mx-opt')] = t.value;
        if (t.getAttribute('data-mx-opt') === 'g') _mxBarsAll = false;
        mxStateToHash(); _mxStale[_mxState.tab] = true; mxShowTab();
      }
    });
    mxEl('mx-freq').addEventListener('change', function() { _mxState.freq = this.value; mxChanged(); });
    mxEl('mx-lvl').addEventListener('change', function() { _mxState.lvl = this.value; mxChanged(); });
    mxEl('mx-from').addEventListener('change', function() { _mxState.from = this.value; mxChanged(); });
    mxEl('mx-to').addEventListener('change', function() { _mxState.to = this.value; mxChanged(); });
    mxEl('mx-reset').addEventListener('click', mxResetFilters);
    mxEl('metrics-body').addEventListener('scroll', function() { mxTipHide(); closeAllDropdowns(); }, { passive: true });
    modal.querySelector('.mx-dialog').addEventListener('scroll', function() { mxTipHide(); closeAllDropdowns(); }, { passive: true });
    document.addEventListener('keydown', function(ev) {
      if (ev.key !== 'Escape' || mxEl('metrics-modal').style.display === 'none') return;
      if (_openDd) { closeAllDropdowns(); return; }   /* first Escape closes an open dropdown */
      closeMetrics();
    });
    let rt = null;
    window.addEventListener('resize', function() {
      if (mxEl('metrics-modal').style.display === 'none') return;
      clearTimeout(rt);
      rt = setTimeout(function() {
        MX_TABS.forEach(function(t) { _mxStale[t] = true; });
        mxShowTab();
      }, 150);
    });
  }

  function mxResetFilters() {
    ['range', 'from', 'to', 'freq', 'lvl', 'c', 's', 'path'].forEach(function(k) { _mxState[k] = MX_DEFAULTS[k]; });
    mxChanged();
  }

  /* A filter changed: update the controls and hash at once, re-select
   * and re-render after a short pause so dragging through a list of
   * options does not aggregate for every intermediate value. */
  function mxChanged() {
    mxSyncControls();
    mxStateToHash();
    clearTimeout(_mxTimer);
    _mxTimer = setTimeout(mxRefresh, 90);
  }
  function mxRefresh() {
    if (_mx) _mxSel = mxSelect();
    MX_TABS.forEach(function(t) { _mxStale[t] = true; });
    mxBuildDropdowns();
    mxShowTab();
  }

  function mxFillFreqSelect() {
    const sel = mxEl('mx-freq');
    if (!sel || !_mx) return;
    sel.innerHTML = '<option value="">All frequencies</option>' + _mx.freqs.map(function(f) {
      return '<option value="' + f + '">' + mxKhz(f) + '</option>';
    }).join('');
    sel.value = _mxState.freq;
  }

  function mxSyncControls() {
    const st = _mxState;
    document.querySelectorAll('#mx-tabs [data-mx-tab]').forEach(function(b) {
      const on = b.getAttribute('data-mx-tab') === st.tab;
      b.classList.toggle('active', on); b.setAttribute('aria-selected', on ? 'true' : 'false');
      /* Phone width: the tab strip scrolls; keep the current tab in view. */
      if (on && b.parentNode.scrollWidth > b.parentNode.clientWidth)
        b.parentNode.scrollLeft = Math.max(0, b.offsetLeft - (b.parentNode.clientWidth - b.offsetWidth) / 2);
    });
    document.querySelectorAll('#mx-range [data-mx-range]').forEach(function(b) {
      b.classList.toggle('on', b.getAttribute('data-mx-range') === st.range);
    });
    document.querySelectorAll('#mx-path [data-mx-path]').forEach(function(b) {
      b.classList.toggle('on', b.getAttribute('data-mx-path') === st.path);
    });
    mxEl('mx-custom').hidden = st.range !== 'custom';
    mxEl('mx-from').value = st.from; mxEl('mx-to').value = st.to;
    const fs = mxEl('mx-freq');
    if (fs.querySelector('option[value="' + CSS.escape(st.freq) + '"]')) fs.value = st.freq;
    mxEl('mx-lvl').value = st.lvl;
    mxEl('mx-reset').hidden = !mxFiltersActive();
    mxEl('mx-controls').hidden = st.tab === 'activity';
    mxBuildDropdowns();
  }

  /* ---- Country / station multi-select ----
   * Same look as the history dropdowns (buildDropdown), but each item
   * toggles a check mark and the panel stays open; the list is grouped
   * like the history Station list.  Values are joined with "|". */
  function mxBuildDropdowns() {
    const D = _mx, sel = _mxSel;
    const counts = sel ? sel.facet : null;
    const countries = new Map();   /* iso2 -> {iso, name, count} */
    if (D && counts) {
      D.stations.forEach(function(s, k) {
        if (!counts[k]) return;
        let c = countries.get(s.iso2);
        if (!c) { c = { iso: s.iso2, name: s.country || s.iso2.toUpperCase(), count: 0 }; countries.set(s.iso2, c); }
        c.count += counts[k];
      });
    }
    const clist = Array.from(countries.values()).sort(function(a, b) { return b.count - a.count; });
    mxMultiDd('mx-country-dd', {
      key: 'c', allLabel: 'All countries', plural: 'countries', search: false,
      items: function() {
        return clist.map(function(c) {
          return { value: c.iso, count: c.count, text: c.name,
                   html: flagHtml(c.iso) + '<span class="dd-name">' + escHtml(c.name) + '</span>' };
        });
      },
      one: function(v) {
        const c = countries.get(v);
        return flagHtml(v) + '<span class="dd-label">' + escHtml(c ? c.name : v.toUpperCase()) + '</span>';
      }
    });
    const cset = new Set(mxList(_mxState.c));
    mxMultiDd('mx-station-dd', {
      key: 's', allLabel: 'All stations', plural: 'stations', search: true,
      items: function() {
        const list = [];
        clist.forEach(function(c) {
          if (cset.size && !cset.has(c.iso)) return;
          const sts = [];
          D.stations.forEach(function(s, k) { if (s.iso2 === c.iso && counts[k]) sts.push(k); });
          sts.sort(function(a, b) { return counts[b] - counts[a]; });
          list.push({ group: true, html: flagHtml(c.iso) + escHtml(c.name), text: c.name });
          sts.forEach(function(k) {
            const s = D.stations[k];
            list.push({ value: s.key, sub: true, count: counts[k], text: s.name + ' ' + c.name + ' ' + s.freq_khz + ' ' + s.letter,
                        html: '<span class="dd-name">' + escHtml(s.name) + '</span>'
                            + '<span class="dd-meta">· ' + escHtml(s.freq_khz + ' ' + s.letter) + '</span>' });
          });
        });
        return list;
      },
      one: function(v) {
        const s = D ? D.stations.find(function(x) { return x.key === v; }) : null;
        if (s) return flagHtml(s.iso2) + '<span class="dd-label">' + escHtml(s.name + ' · ' + s.freq_khz + ' ' + s.letter) + '</span>';
        const p = v.split(':');
        return '<span class="dd-label">' + escHtml(p[2] || v) + '</span>';
      }
    });
  }

  function mxMultiDd(id, cfg) {
    const root = mxEl(id);
    if (!root) return;
    root._cfg = cfg;
    if (!root._built) {
      root._built = true;
      root.innerHTML = '<button type="button" class="dd-btn" aria-haspopup="listbox" aria-expanded="false"></button>'
                     + '<div class="dd-panel" role="listbox" aria-multiselectable="true">'
                     + (cfg.search ? '<input class="dd-search" type="text" placeholder="Search stations…">' : '')
                     + '<div class="dd-list"></div></div>';
      const btn = root.querySelector('.dd-btn'), panel = root.querySelector('.dd-panel');
      const search = root.querySelector('.dd-search');
      btn.addEventListener('click', function(ev) {
        ev.stopPropagation();
        if (root.classList.contains('open')) { closeAllDropdowns(); return; }
        closeAllDropdowns();
        root.classList.add('open'); _openDd = root;
        btn.setAttribute('aria-expanded', 'true');
        if (search) search.value = '';
        mxMultiList(root, '');
        const r = btn.getBoundingClientRect();
        const w = Math.min(Math.max(r.width, 300), window.innerWidth - 16);
        panel.style.width = w + 'px';
        panel.style.left = Math.max(8, Math.min(r.left, window.innerWidth - w - 8)) + 'px';
        const below = window.innerHeight - r.bottom - 12;
        if (below >= 220 || below >= r.top) {
          panel.style.top = (r.bottom + 4) + 'px'; panel.style.bottom = 'auto';
          panel.style.maxHeight = Math.min(360, below) + 'px';
        } else {
          panel.style.top = 'auto'; panel.style.bottom = (window.innerHeight - r.top + 4) + 'px';
          panel.style.maxHeight = Math.min(360, r.top - 12) + 'px';
        }
        if (search && window.matchMedia('(pointer: fine)').matches) search.focus();
      });
      if (search) {
        search.addEventListener('input', function() { mxMultiList(root, search.value); });
        search.addEventListener('click', function(ev) { ev.stopPropagation(); });
      }
      panel.addEventListener('click', function(ev) {
        /* Stop here: the list is re-rendered below, which detaches the
         * clicked node, and the document handler would then think the
         * click landed outside and close the panel. */
        ev.stopPropagation();
        const it = ev.target.closest('.dd-item');
        if (!it) return;
        mxMultiToggle(root, it.getAttribute('data-value') || '');
      });
      root.addEventListener('keydown', function(ev) {
        if (!root.classList.contains('open')) return;
        if (ev.key === 'Tab') { closeAllDropdowns(); return; }   /* focus leaves: so does the panel */
        const items = Array.from(root.querySelectorAll('.dd-item'));
        let i = items.findIndex(function(x) { return x.classList.contains('kb'); });
        if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') {
          ev.preventDefault();
          if (i >= 0) items[i].classList.remove('kb');
          i = ev.key === 'ArrowDown' ? Math.min(items.length - 1, i + 1) : Math.max(0, i - 1);
          if (items[i]) {
            items[i].classList.add('kb'); items[i].scrollIntoView({ block: 'nearest' });
            if (!items[i].id) items[i].id = root.id + '-opt-' + i;
            panel.setAttribute('aria-activedescendant', items[i].id);
          }
        } else if (ev.key === 'Enter' && i >= 0) {
          ev.preventDefault();
          const v = items[i].getAttribute('data-value') || '';
          mxMultiToggle(root, v);
          const again = root.querySelector('.dd-item[data-value="' + CSS.escape(v) + '"]');
          if (again) again.classList.add('kb');
        } else if (ev.key === 'Escape') {
          ev.stopPropagation(); closeAllDropdowns(); btn.focus();
        }
      });
    }
    const vals = mxList(_mxState[cfg.key]);
    const btn = root.querySelector('.dd-btn');
    btn.innerHTML = !vals.length ? '<span class="dd-label">' + cfg.allLabel + '</span>'
                  : vals.length === 1 ? cfg.one(vals[0])
                  : '<span class="dd-label">' + vals.length + ' ' + cfg.plural + '</span>';
    btn.classList.toggle('dd-set', vals.length > 0);
    if (root.classList.contains('open')) mxMultiList(root, (root.querySelector('.dd-search') || {}).value || '');
  }

  function mxMultiToggle(root, v) {
    const cfg = root._cfg;
    let vals = mxList(_mxState[cfg.key]);
    if (!v) vals = [];
    else if (vals.indexOf(v) >= 0) vals = vals.filter(function(x) { return x !== v; });
    else vals.push(v);
    _mxState[cfg.key] = vals.join('|');
    /* Stations outside the chosen countries would select nothing. */
    if (cfg.key === 'c' && _mxState.c && _mxState.s && _mx) {
      const cs = new Set(mxList(_mxState.c));
      _mxState.s = mxList(_mxState.s).filter(function(k) {
        const s = _mx.stations.find(function(x) { return x.key === k; });
        return !s || cs.has(s.iso2);
      }).join('|');
    }
    mxChanged();
  }

  function mxMultiList(root, filterText) {
    const cfg = root._cfg, sel = new Set(mxList(_mxState[cfg.key]));
    const f = (filterText || '').trim().toLowerCase();
    let items = cfg.items();
    if (f) {
      const keep = []; let pg = null;
      items.forEach(function(it) {
        if (it.group) { pg = it; pg.all = it.text.toLowerCase().indexOf(f) >= 0; return; }
        if ((pg && pg.all) || it.text.toLowerCase().indexOf(f) >= 0) {
          if (pg && keep.indexOf(pg) < 0) keep.push(pg);
          keep.push(it);
        }
      });
      items = keep;
    }
    let html = f ? '' : '<div class="dd-item' + (!sel.size ? ' sel' : '') + '" data-value="">'
                      + '<span class="dd-name">' + escHtml(cfg.allLabel) + '</span></div>';
    items.forEach(function(it) {
      if (it.group) { html += '<div class="dd-group">' + it.html + '</div>'; return; }
      html += '<div class="dd-item' + (it.sub ? ' dd-sub' : '') + (sel.has(it.value) ? ' sel' : '')
            + '" role="option" aria-selected="' + sel.has(it.value) + '" data-value="' + escHtml(it.value) + '">'
            + '<span class="dd-check"></span>' + it.html
            + '<span class="dd-count">' + fmtInt(it.count) + '</span></div>';
    });
    if (!items.length) html += '<div class="dd-empty">' + (f ? 'No match.' : 'Nothing identified in this range.') + '</div>';
    root.querySelector('.dd-list').innerHTML = html;
  }

  /* ═══ Tabs ═══ */
  const MX_RENDER = {};

  function mxShowTab() {
    const tab = _mxState.tab;
    MX_TABS.forEach(function(t) { const p = mxEl('mx-pane-' + t); if (p) p.hidden = t !== tab; });
    mxTipHide();
    const note = mxEl('mx-note'), cnt = mxEl('mx-count');
    note.hidden = true; note.className = 'mx-note';
    if (tab === 'activity') {
      cnt.textContent = '';
      note.hidden = false;
      note.textContent = 'Every logged message, per hour for the last 24 hours and per day for the last 30 days. '
                       + 'The filters on the other tabs do not apply here.';
      if (_mxStale.activity) { _mxStale.activity = false; renderMetricsCharts(_mxActivity); }
      return;
    }
    const pane = mxEl('mx-pane-' + tab);
    if (!_mx) {
      cnt.textContent = '';
      pane.innerHTML = _mxErr
        ? '<div class="mx-empty">Analytics unavailable: ' + escHtml(_mxErr) + '.<br>'
          + 'This view needs <code>/api/analytics</code> from the NAVTEX server.</div>'
        : '<div class="mx-empty">Loading analytics…</div>';
      _mxStale[tab] = true;
      return;
    }
    if (!_mxSel) _mxSel = mxSelect();
    const n = _mxSel.idx.length;
    cnt.textContent = fmtInt(n) + ' of ' + fmtInt(_mx.n) + ' messages';
    const rx = _mx.receiver || {};
    const rp = _mx.replay;
    if (rp && rp.done === false) {
      note.hidden = false;
      note.textContent = 'Still identifying history (' + fmtInt(rp.processed || 0) + ' / ' + fmtInt(rp.total || 0)
                       + ') — these figures cover the messages identified so far. Reopen Metrics when it finishes.';
    } else if (!rx.valid && tab !== 'stations' && tab !== 'breakdown') {
      note.hidden = false; note.className = 'mx-note warn';
      note.textContent = 'Receiver location unknown — distances and bearings need it. Set the position in UberSDR, '
                       + 'or NAVTEX_RX_LAT / NAVTEX_RX_LON for this add-on.';
    } else if (rx.pos_suspect && tab === 'coverage') {
      note.hidden = false; note.className = 'mx-note warn';
      note.textContent = 'Receiver location looks wrong (stations heard far beyond their range) — distances may be off.';
    }
    if (!_mxStale[tab]) return;
    _mxStale[tab] = false;
    _mxData = {};
    const t0 = performance.now();
    if (!n && tab !== 'coverage') {
      pane.innerHTML = mxNoMatch();
    } else {
      MX_RENDER[tab](pane, _mxSel.idx);
    }
    pane.setAttribute('data-ms', Math.round(performance.now() - t0));
  }

  function mxNoMatch() {
    if (_mx && !_mx.n) return '<div class="mx-card"><div class="mx-empty">No identified messages logged yet.</div></div>';
    return '<div class="mx-card"><div class="mx-empty">No messages match these filters.'
         + (mxFiltersActive() ? '<br><span class="pp-link" data-mx-reset>Reset filters</span>' : '')
         + '</div></div>';
  }

  /* Card: title, subtitle, tab-local controls, then Table + CSV. */
  function mxCard(id, title, sub, ctl, cls) {
    return '<section class="mx-card' + (cls ? ' ' + cls : '') + '" id="mxc-' + id + '">'
         + '<div class="mx-card-head"><div><div class="mx-card-title">' + title + '</div>'
         + (sub ? '<div class="mx-card-sub">' + sub + '</div>' : '') + '</div>'
         + '<div class="mx-card-ctl">' + (ctl || '')
         + '<button class="mx-mini' + (_mxTable[id] ? ' on' : '') + '" data-mx-table="' + id + '" aria-pressed="'
         + (!!_mxTable[id]) + '" title="Show the numbers as a table">Table</button>'
         + '<button class="mx-mini" data-mx-csv="' + id + '" title="Download this chart\'s data as CSV">CSV</button>'
         + '</div></div><div class="mx-card-body" id="mxb-' + id + '"></div></section>';
  }
  function mxSelectHtml(key, label, opts) {
    return '<label class="mx-lab">' + label + ' <select class="mx-sel" data-mx-opt="' + key + '">'
         + opts.map(function(o) {
             return '<option value="' + o[0] + '"' + (_mxState[key] === o[0] ? ' selected' : '') + '>' + o[1] + '</option>';
           }).join('') + '</select></label>';
  }
  function mxToggleHtml(key, v, label, on, col) {
    return '<button class="mx-mini' + (on ? ' on' : '') + '" data-mx-set="' + key + '" data-v="' + v + '" aria-pressed="' + on + '">'
         + (col ? '<span style="display:inline-block;width:8px;height:8px;border-radius:2px;margin-right:5px;background:' + col + '"></span>' : '')
         + label + '</button>';
  }

  /* Store a chart's dataset and draw it (or its table, or its empty
   * state).  dataset.cols: [{label, num?, fmt?}], dataset.rows: arrays of
   * raw values in column order — exactly what the CSV contains.  A large
   * dataset can give "n" and a "rows" getter, built only when the Table
   * or CSV is asked for (the scatter has one row per message). */
  function mxMount(id, ds, draw) {
    _mxData[id] = ds;
    const body = mxEl('mxb-' + id);
    if (!body) return;
    if (!(ds.n !== undefined ? ds.n : ds.rows.length)) { body.innerHTML = '<div class="mx-empty">' + (ds.empty || 'No messages match these filters.') + '</div>'; return; }
    if (_mxTable[id]) { body.innerHTML = mxTableHtml(ds); return; }
    draw(body);
  }
  function mxTableHtml(ds) {
    let h = '<div class="mx-tbl-wrap"><table class="mx-tbl"><thead><tr>';
    ds.cols.forEach(function(c) { h += '<th' + (c.num ? ' class="num"' : '') + '>' + escHtml(c.label) + '</th>'; });
    h += '</tr></thead><tbody>';
    /* The page stays responsive with a bounded table; the CSV has every row. */
    const LIMIT = 2000, all = ds.rows;
    all.slice(0, LIMIT).forEach(function(r) {
      h += '<tr>';
      ds.cols.forEach(function(c, j) {
        const v = r[j];
        const txt = c.fmt ? c.fmt(v) : (typeof v === 'number' ? (isFinite(v) ? String(Math.round(v * 1000) / 1000) : '—')
                                                              : (v === null || v === undefined || v === '' ? '—' : String(v)));
        h += '<td' + (c.num ? ' class="num"' : '') + '>' + escHtml(txt) + '</td>';
      });
      h += '</tr>';
    });
    h += '</tbody></table></div>';
    if (all.length > LIMIT)
      h += '<div class="mx-card-sub" style="margin-top:6px">First ' + fmtInt(LIMIT) + ' of ' + fmtInt(all.length)
         + ' rows — the CSV has them all.</div>';
    return h;
  }
  function mxCsv(id) {
    const ds = _mxData[id];
    if (!ds) return;
    function q(v) {
      if (v === null || v === undefined || (typeof v === 'number' && !isFinite(v))) return '';
      if (typeof v === 'number') return String(Math.round(v * 1000) / 1000);
      let s = String(v);
      if (/^[=+@\t\r]/.test(s)) s = "'" + s;   /* keep spreadsheets from running it as a formula */
      return /[",\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
    }
    const lines = [ds.cols.map(function(c) { return q(c.label); }).join(',')];
    ds.rows.forEach(function(r) { lines.push(r.map(q).join(',')); });
    const st = _mxState;
    const range = st.range === 'custom' ? (st.from || 'start') + '_' + (st.to || 'now') : st.range;
    const blob = new Blob([lines.join('\r\n') + '\r\n'], { type: 'text/csv;charset=utf-8' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'navtex-' + id + '-' + range + '.csv';
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(function() { URL.revokeObjectURL(a.href); }, 2000);
  }

  /* ═══ Chart primitives ═══ */

  function mxLegend(items) {
    return '<div class="mx-legend">' + items.map(function(it) {
      return '<span class="mx-lg"><i class="' + (it.kind || '') + '" style="' + (it.kind === 'ln' || it.kind === 'ln dash'
             ? 'border-color:' : 'background:') + it.col + '"></i>' + escHtml(it.name) + '</span>';
    }).join('') + '</div>';
  }

  /* Line chart over categorical x positions (hours, days, weeks).
   *   cfg.xs       x labels                cfg.series [{name, col, vals, dash}]
   *   cfg.yFmt     tick/tooltip format     cfg.shade  optional 0..1 per x (night share)
   *   cfg.tipHead  (i) -> tooltip header html; cfg.tipExtra (i) -> trailing line
   * A crosshair snaps to the nearest x and the tooltip lists every series. */
  function mxLineChart(host, cfg) {
    const W = Math.max(280, host.clientWidth | 0), H = cfg.height || 220;
    const M = { l: 46, r: 12, t: 10, b: 26 };
    const n = cfg.xs.length, pw = W - M.l - M.r, ph = H - M.t - M.b, band = pw / Math.max(1, n);
    let max = 0;
    cfg.series.forEach(function(s) { s.vals.forEach(function(v) { if (v > max) max = v; }); });
    const T = mxTicks(cfg.yMax || max, 4, mxMinOf(cfg.series));
    const X = function(i) { return M.l + (i + 0.5) * band; };
    const Y = function(v) { return M.t + ph - ((v - T.lo) / (T.top - T.lo)) * ph; };
    let svg = '<svg viewBox="0 0 ' + W + ' ' + H + '" width="' + W + '" height="' + H + '" role="img" aria-label="'
            + escHtml(cfg.aria || '') + '">';
    if (cfg.shade) cfg.shade.forEach(function(v, i) {
      if (v > 0) svg += '<rect x="' + (M.l + i * band) + '" y="' + M.t + '" width="' + band + '" height="' + ph
                      + '" fill="' + MX_NIGHT_COL + '" fill-opacity="' + (0.04 + 0.2 * v).toFixed(3) + '"/>';
    });
    T.ticks.forEach(function(v) {
      const y = Y(v);
      svg += '<line x1="' + M.l + '" x2="' + (W - M.r) + '" y1="' + y + '" y2="' + y + '" stroke="'
           + (v === 0 ? MX_AXIS : MX_GRID) + '" stroke-width="1"/>'
           + '<text x="' + (M.l - 6) + '" y="' + (y + 3) + '" text-anchor="end">' + escHtml(cfg.yFmt ? cfg.yFmt(v, true) : mxCompact(v)) + '</text>';
    });
    const every = Math.max(1, Math.ceil(n / Math.max(1, Math.floor(pw / ((cfg.xLabW || 5) * 7 + 10)))));
    cfg.xs.forEach(function(lab, i) {
      if (i % every) return;
      svg += '<text x="' + X(i) + '" y="' + (H - 8) + '" text-anchor="middle">' + escHtml(lab) + '</text>';
    });
    cfg.series.forEach(function(s) {
      let d = '', pen = false;
      s.vals.forEach(function(v, i) {
        if (!(v === v) || v === null) { pen = false; return; }
        d += (pen ? 'L' : 'M') + X(i).toFixed(1) + ',' + Y(v).toFixed(1); pen = true;
      });
      svg += '<path d="' + d + '" fill="none" stroke="' + s.col + '" stroke-width="2" stroke-linejoin="round" stroke-linecap="round"'
           + (s.dash ? ' stroke-dasharray="5 4"' : '') + '/>';
      /* Markers where the line has few points, or where a value stands alone. */
      s.vals.forEach(function(v, i) {
        if (!(v === v) || v === null) return;
        const lone = !(s.vals[i - 1] === s.vals[i - 1] && s.vals[i - 1] !== undefined && s.vals[i - 1] !== null)
                  && !(s.vals[i + 1] === s.vals[i + 1] && s.vals[i + 1] !== undefined && s.vals[i + 1] !== null);
        if (n <= 31 || lone)
          svg += '<circle cx="' + X(i).toFixed(1) + '" cy="' + Y(v).toFixed(1) + '" r="3.5" fill="' + s.col
               + '" stroke="' + MX_SURFACE + '" stroke-width="2"/>';
      });
    });
    svg += '<line class="mx-xh" x1="0" x2="0" y1="' + M.t + '" y2="' + (M.t + ph) + '" stroke="#a0b8d8" stroke-width="1" visibility="hidden"/>';
    svg += '<rect x="' + M.l + '" y="' + M.t + '" width="' + pw + '" height="' + ph + '" fill="transparent" class="mx-hit"/>';
    svg += '</svg>';
    const leg = [];
    if (cfg.series.length > 1 || cfg.legendAlways)
      cfg.series.forEach(function(s) { leg.push({ name: s.name, col: s.col, kind: s.dash ? 'ln dash' : 'ln' }); });
    if (cfg.shade) leg.push({ name: cfg.shadeName || 'Night-path share', col: '', kind: 'shade' });
    host.innerHTML = (leg.length ? mxLegend(leg) : '') + '<div class="mx-svgwrap">' + svg + '</div>';
    const svgEl = host.querySelector('svg'), xh = svgEl.querySelector('.mx-xh');
    function at(ev) {
      const r = svgEl.getBoundingClientRect();
      const px = (ev.clientX - r.left) * (W / r.width);
      return Math.max(0, Math.min(n - 1, Math.floor((px - M.l) / band)));
    }
    svgEl.querySelector('.mx-hit').addEventListener('pointermove', function(ev) {
      const i = at(ev);
      xh.setAttribute('x1', X(i)); xh.setAttribute('x2', X(i)); xh.setAttribute('visibility', 'visible');
      let h = mxTipHead(cfg.tipHead ? cfg.tipHead(i) : escHtml(cfg.xs[i]));
      cfg.series.forEach(function(s) {
        const v = s.vals[i];
        h += mxTipRow(s.col, (v === v && v !== null && v !== undefined) ? (cfg.yFmt ? cfg.yFmt(v) : mxCompact(v)) : '—', s.name);
      });
      if (cfg.tipExtra) h += '<div class="mx-tm">' + cfg.tipExtra(i) + '</div>';
      mxTipShow(h, ev.clientX, ev.clientY);
    });
    svgEl.querySelector('.mx-hit').addEventListener('pointerleave', function() {
      xh.setAttribute('visibility', 'hidden'); mxTipHide();
    });
  }

  /* Column chart: stacked (additive metric) or grouped side by side.
   * Bars <= 24 px, 4 px rounded top, 2 px surface gap between stacked
   * segments.  Hover anywhere in a column's band for its tooltip. */
  function mxRoundTop(x, y, w, h, r) {
    r = Math.max(0, Math.min(r, h, w / 2));
    return 'M' + x + ',' + (y + h) + 'V' + (y + r) + 'Q' + x + ',' + y + ' ' + (x + r) + ',' + y
         + 'H' + (x + w - r) + 'Q' + (x + w) + ',' + y + ' ' + (x + w) + ',' + (y + r) + 'V' + (y + h) + 'Z';
  }
  function mxColChart(host, cfg) {
    const W = Math.max(280, host.clientWidth | 0), H = cfg.height || 220;
    const M = { l: 46, r: 12, t: 10, b: 26 };
    const n = cfg.xs.length, ns = cfg.series.length, pw = W - M.l - M.r, ph = H - M.t - M.b, band = pw / Math.max(1, n);
    const stacked = cfg.stacked && ns > 1;
    let max = 0;
    for (let i = 0; i < n; i++) {
      let tot = 0;
      cfg.series.forEach(function(s) { const v = s.vals[i] || 0; if (stacked) tot += v; else if (v > tot) tot = v; });
      if (tot > max) max = tot;
    }
    const T = mxTicks(cfg.yMax || max, 4, stacked ? 0 : mxMinOf(cfg.series));
    const Y = function(v) { return M.t + ph - ((v - T.lo) / (T.top - T.lo)) * ph; };
    let svg = '<svg viewBox="0 0 ' + W + ' ' + H + '" width="' + W + '" height="' + H + '" role="img">';
    svg += '<rect class="mx-band" x="0" y="' + M.t + '" width="' + band + '" height="' + ph + '" fill="#a0b8d8" fill-opacity="0.07" visibility="hidden"/>';
    T.ticks.forEach(function(v) {
      const y = Y(v);
      svg += '<line x1="' + M.l + '" x2="' + (W - M.r) + '" y1="' + y + '" y2="' + y + '" stroke="' + (v === 0 ? MX_AXIS : MX_GRID) + '"/>'
           + '<text x="' + (M.l - 6) + '" y="' + (y + 3) + '" text-anchor="end">' + escHtml(cfg.yFmt ? cfg.yFmt(v, true) : mxCompact(v)) + '</text>';
    });
    const every = Math.max(1, Math.ceil(n / Math.max(1, Math.floor(pw / ((cfg.xLabW || 5) * 7 + 10)))));
    for (let i = 0; i < n; i++) {
      const x0 = M.l + i * band;
      if (i % every === 0)
        svg += '<text x="' + (x0 + band / 2) + '" y="' + (H - 8) + '" text-anchor="middle">' + escHtml(cfg.xs[i]) + '</text>';
      if (stacked || (ns === 1 && T.lo === 0)) {
        const bw = Math.max(2, Math.min(24, band * 0.72)), bx = x0 + (band - bw) / 2;
        let cum = 0; let top = -1;
        cfg.series.forEach(function(s, k) { if ((s.vals[i] || 0) > 0) top = k; });
        cfg.series.forEach(function(s, k) {
          const v = s.vals[i] || 0;
          if (v <= 0) return;
          const y1 = Y(cum), y0 = Y(cum + v);
          let h = y1 - y0;
          const gap = cum > 0 ? 2 : 0;         /* surface gap above the segment below */
          h = Math.max(0.5, h - gap);
          svg += k === top
            ? '<path d="' + mxRoundTop(bx, y0, bw, h, 4) + '" fill="' + s.col + '"/>'
            : '<rect x="' + bx + '" y="' + y0 + '" width="' + bw + '" height="' + h + '" fill="' + s.col + '"/>';
          cum += v;
        });
      } else {
        const gw = Math.min(band * 0.8, ns * 24 + (ns - 1) * 2), bw = Math.max(1.5, (gw - (ns - 1) * 2) / ns);
        const gx = x0 + (band - gw) / 2;
        cfg.series.forEach(function(s, k) {
          const v = s.vals[i];
          if (!(v === v) || v === 0 || v === null || v === undefined) return;
          const y0 = Y(v);
          /* Below zero: a plain bar hanging from the baseline. */
          svg += v > 0 ? '<path d="' + mxRoundTop(gx + k * (bw + 2), y0, bw, Y(0) - y0, 4) + '" fill="' + s.col + '"/>'
                       : '<rect x="' + (gx + k * (bw + 2)) + '" y="' + Y(0) + '" width="' + bw + '" height="' + (y0 - Y(0)) + '" fill="' + s.col + '"/>';
        });
      }
    }
    svg += '<rect x="' + M.l + '" y="' + M.t + '" width="' + pw + '" height="' + ph + '" fill="transparent" class="mx-hit"/></svg>';
    const leg = ns > 1 ? mxLegend(cfg.series.map(function(s) { return { name: s.name, col: s.col }; })) : '';
    host.innerHTML = leg + '<div class="mx-svgwrap">' + svg + '</div>';
    const svgEl = host.querySelector('svg'), bandEl = svgEl.querySelector('.mx-band');
    const hit = svgEl.querySelector('.mx-hit');
    hit.addEventListener('pointermove', function(ev) {
      const r = svgEl.getBoundingClientRect();
      const i = Math.max(0, Math.min(n - 1, Math.floor(((ev.clientX - r.left) * (W / r.width) - M.l) / band)));
      bandEl.setAttribute('x', M.l + i * band); bandEl.setAttribute('visibility', 'visible');
      let h = mxTipHead(cfg.tipHead ? cfg.tipHead(i) : escHtml(cfg.xs[i]));
      let tot = 0;
      cfg.series.slice().reverse().forEach(function(s) {
        const v = s.vals[i]; tot += (v || 0);
        h += mxTipRow(s.col, (v === v && v !== undefined && v !== null) ? (cfg.yFmt ? cfg.yFmt(v) : mxCompact(v)) : '—', s.name);
      });
      if (stacked) h += '<div class="mx-tm">Total ' + (cfg.yFmt ? cfg.yFmt(tot) : mxCompact(tot)) + '</div>';
      if (cfg.tipExtra) h += '<div class="mx-tm">' + cfg.tipExtra(i) + '</div>';
      mxTipShow(h, ev.clientX, ev.clientY);
    });
    hit.addEventListener('pointerleave', function() { bandEl.setAttribute('visibility', 'hidden'); mxTipHide(); });
  }

  /* Horizontal bars, for categories with names (stations, countries…).
   * HTML rather than SVG so labels can carry flags and ellipsis. */
  function mxHBars(host, cfg) {
    const ns = cfg.series.length, stacked = cfg.stacked && ns > 1;
    let max = 0;
    cfg.cats.forEach(function(c, i) {
      let tot = 0;
      cfg.series.forEach(function(s) { const v = s.vals[i] || 0; if (stacked) tot += v; else if (v > tot) tot = v; });
      if (tot > max) max = tot;
    });
    /* Negative values (median SNR) get a zero line inside the track:
     * bars below zero extend left of it, the rest right. */
    const lo = stacked ? 0 : mxMinOf(cfg.series);
    const span = (max - lo) > 0 ? (max - lo) : 1;
    function seg(v) {
      const a = v < 0 ? v : 0, b = v < 0 ? 0 : v;
      return 'margin-left:' + ((a - lo) / span * 100).toFixed(2) + '%;width:' + ((b - a) / span * 100).toFixed(2) + '%';
    }
    let h = ns > 1 ? mxLegend(cfg.series.map(function(s) { return { name: s.name, col: s.col }; })) : '';
    h += '<div class="mx-hb">';
    cfg.cats.forEach(function(c, i) {
      h += '<div class="mx-hb-row" data-i="' + i + '"><div class="mx-hb-lab">' + c.html + '</div><div class="mx-hb-track">';
      if (stacked || ns === 1) {
        let tot = 0; cfg.series.forEach(function(s) { tot += s.vals[i] || 0; });
        h += '<div class="mx-hb-bar' + (ns === 1 && cfg.series[0].vals[i] < 0 ? ' neg' : '') + '" style="' + (ns === 1 ? seg(cfg.series[0].vals[i] || 0) : seg(tot)) + '">';
        cfg.series.forEach(function(s) {
          const v = s.vals[i] || 0;
          if (v > 0 || (ns === 1 && v < 0)) h += '<span style="flex:' + Math.abs(v) + ' 1 0;background:' + s.col + '"></span>';
        });
        h += '</div>';
      } else {
        cfg.series.forEach(function(s) {
          const v = s.vals[i];
          const ok = v === v && v !== null && v !== undefined && v !== 0;
          h += '<div class="mx-hb-bar thin' + (ok && v < 0 ? ' neg' : '') + '" style="' + seg(ok ? v : 0) + '">'
             + (ok ? '<span style="flex:1 1 0;background:' + s.col + '"></span>' : '') + '</div>';
        });
      }
      h += '</div><div class="mx-hb-val">' + escHtml(cfg.valText(i)) + '</div></div>';
    });
    h += '</div>';
    if (cfg.more) h += cfg.more;
    host.innerHTML = h;
    host.querySelectorAll('.mx-hb-row').forEach(function(row) {
      row.addEventListener('pointermove', function(ev) {
        const i = +row.getAttribute('data-i');
        let t = mxTipHead(cfg.cats[i].html);
        cfg.series.slice().reverse().forEach(function(s) {
          const v = s.vals[i];
          t += mxTipRow(s.col, (v === v && v !== undefined && v !== null) ? cfg.fmt(v) : '—', s.name);
        });
        if (cfg.tipExtra) t += '<div class="mx-tm">' + cfg.tipExtra(i) + '</div>';
        mxTipShow(t, ev.clientX, ev.clientY);
      });
      row.addEventListener('pointerleave', mxTipHide);
    });
  }

  /* Polar "reach" chart: one closed outline per series, the radius of
   * each 10° sector its reach in km.  North up, bearings clockwise. */
  function mxPolar(host, cfg) {
    const S = Math.max(240, Math.min(400, host.clientWidth | 0)), c = S / 2, R = c - 28;
    const nsec = cfg.series[0].vals.length, step = 360 / nsec;
    let max = 0;
    cfg.series.forEach(function(s) { s.vals.forEach(function(v) { if (v > max) max = v; }); });
    const T = mxTicks(max, 3);
    function pt(deg, r) {
      const a = deg * Math.PI / 180;
      return [(c + r * Math.sin(a)).toFixed(1), (c - r * Math.cos(a)).toFixed(1)];
    }
    let svg = '<svg viewBox="0 0 ' + S + ' ' + S + '" width="' + S + '" height="' + S + '" role="img" aria-label="Reception reach by bearing">';
    T.ticks.forEach(function(v) {
      if (!v) return;
      svg += '<circle cx="' + c + '" cy="' + c + '" r="' + (v / T.top * R) + '" fill="none" stroke="' + MX_GRID + '"/>';
    });
    for (let d = 0; d < 360; d += 30) {
      const p = pt(d, R), q = pt(d, R + 12);
      svg += '<line x1="' + c + '" y1="' + c + '" x2="' + p[0] + '" y2="' + p[1] + '" stroke="' + MX_GRID + '"/>';
      const lab = d === 0 ? 'N' : d === 90 ? 'E' : d === 180 ? 'S' : d === 270 ? 'W' : d + '°';
      svg += '<text x="' + q[0] + '" y="' + (+q[1] + 3) + '" text-anchor="middle">' + lab + '</text>';
    }
    T.ticks.forEach(function(v) {
      if (!v) return;
      const p = pt(45, v / T.top * R);
      svg += '<text x="' + (+p[0] + 3) + '" y="' + p[1] + '" style="font-size:9px">' + escHtml(mxCompact(v)) + ' km</text>';
    });
    svg += '<path class="mx-wedge" d="" fill="#a0b8d8" fill-opacity="0.09"/>';
    cfg.series.forEach(function(s) {
      let d = '';
      for (let k = 0; k < nsec; k++) {
        const v = s.vals[k], r = (v > 0) ? v / T.top * R : 0;
        if (r <= 0) { d += (d ? 'L' : 'M') + c + ',' + c; continue; }
        const a = pt(k * step, r), b = pt((k + 1) * step, r);
        d += (d ? 'L' : 'M') + a[0] + ',' + a[1] + 'A' + r.toFixed(1) + ',' + r.toFixed(1) + ' 0 0 1 ' + b[0] + ',' + b[1];
      }
      svg += '<path d="' + d + 'Z" fill="' + s.col + '" fill-opacity="0.12" stroke="' + s.col + '" stroke-width="2" stroke-linejoin="round"/>';
    });
    svg += '<circle cx="' + c + '" cy="' + c + '" r="3.5" fill="#e94560" stroke="' + MX_SURFACE + '" stroke-width="2"/>';
    svg += '<circle class="mx-hit" cx="' + c + '" cy="' + c + '" r="' + (R + 6) + '" fill="transparent"/></svg>';
    const leg = cfg.series.length > 1 ? mxLegend(cfg.series.map(function(s) { return { name: s.name, col: s.col }; })) : '';
    host.innerHTML = leg + '<div class="mx-svgwrap mx-polar">' + svg + '</div>';
    const svgEl = host.querySelector('svg'), wedge = svgEl.querySelector('.mx-wedge');
    const hit = svgEl.querySelector('.mx-hit');
    hit.addEventListener('pointermove', function(ev) {
      const r = svgEl.getBoundingClientRect(), k2 = S / r.width;
      const dx = (ev.clientX - r.left) * k2 - c, dy = (ev.clientY - r.top) * k2 - c;
      const deg = (Math.atan2(dx, -dy) * 180 / Math.PI + 360) % 360;
      const k = Math.min(nsec - 1, Math.floor(deg / step));
      const a = pt(k * step, R), b = pt((k + 1) * step, R);
      wedge.setAttribute('d', 'M' + c + ',' + c + 'L' + a[0] + ',' + a[1] + 'A' + R + ',' + R + ' 0 0 1 ' + b[0] + ',' + b[1] + 'Z');
      let h = mxTipHead('Bearing ' + (k * step) + '–' + ((k + 1) * step) + '°');
      cfg.series.forEach(function(s) { h += mxTipRow(s.col, s.vals[k] > 0 ? fmtKm(s.vals[k]) : '—', s.name); });
      if (cfg.tipExtra) h += '<div class="mx-tm">' + cfg.tipExtra(k) + '</div>';
      mxTipShow(h, ev.clientX, ev.clientY);
    });
    hit.addEventListener('pointerleave', function() { wedge.setAttribute('d', ''); mxTipHide(); });
  }

  /* Scatter on a canvas: SVG with 30,000 circles would be slow to build
   * and to hover.  Points are bucketed into a 12 px grid so the pointer
   * only has to be NEAREST a point (within 14 px), not on it. */
  function mxScatter(host, cfg) {
    const W = Math.max(280, host.clientWidth | 0), H = cfg.height || 300;
    const M = { l: 46, r: 12, t: 10, b: 34 };
    const pw = W - M.l - M.r, ph = H - M.t - M.b;
    const xs = cfg.x, ys = cfg.y, n = xs.length;
    let xmax = 0, ymin = Infinity, ymax = -Infinity;
    for (let i = 0; i < n; i++) { if (xs[i] > xmax) xmax = xs[i]; if (ys[i] < ymin) ymin = ys[i]; if (ys[i] > ymax) ymax = ys[i]; }
    const TX = mxTicks(xmax, 5);
    const ylo = Math.floor(Math.min(0, ymin) / 5) * 5, TY = mxTicks(ymax - ylo, 4);
    const X = function(v) { return M.l + v / TX.top * pw; };
    const Y = function(v) { return M.t + ph - (v - ylo) / TY.top * ph; };
    const dpr = window.devicePixelRatio || 1;
    host.innerHTML = mxLegend(cfg.legend.map(function(l) { return { name: l.name, col: l.col, kind: 'dot' }; }))
                   + '<div class="mx-scatter"><canvas width="' + Math.round(W * dpr) + '" height="' + Math.round(H * dpr)
                   + '" style="height:' + H + 'px" role="img" aria-label="' + escHtml(cfg.aria || '') + '"></canvas><div class="mx-hl"></div></div>';
    const cv = host.querySelector('canvas'), ctx = cv.getContext('2d'), hl = host.querySelector('.mx-hl');
    ctx.scale(dpr, dpr);
    ctx.font = "10px 'Courier New', monospace";
    ctx.fillStyle = '#6a7f98'; ctx.lineWidth = 1;
    TY.ticks.forEach(function(v) {
      const y = Math.round(Y(v + ylo)) + 0.5;
      ctx.strokeStyle = (v + ylo === 0) ? MX_AXIS : MX_GRID;
      ctx.beginPath(); ctx.moveTo(M.l, y); ctx.lineTo(W - M.r, y); ctx.stroke();
      ctx.textAlign = 'right'; ctx.fillText(mxCompact(v + ylo), M.l - 6, y + 3);
    });
    TX.ticks.forEach(function(v) {
      const x = Math.round(X(v)) + 0.5;
      ctx.strokeStyle = MX_GRID;
      ctx.beginPath(); ctx.moveTo(x, M.t); ctx.lineTo(x, M.t + ph); ctx.stroke();
      ctx.textAlign = 'center'; ctx.fillText(mxCompact(v), x, H - 20);
    });
    ctx.textAlign = 'center'; ctx.fillText(cfg.xTitle || '', M.l + pw / 2, H - 5);
    ctx.save(); ctx.translate(11, M.t + ph / 2); ctx.rotate(-Math.PI / 2); ctx.fillText(cfg.yTitle || '', 0, 0); ctx.restore();
    /* Most common colour first, so rarer groups stay visible on top. */
    const groups = new Map();
    for (let i = 0; i < n; i++) {
      const g = cfg.col[i]; let a = groups.get(g); if (!a) { a = []; groups.set(g, a); } a.push(i);
    }
    const order = Array.from(groups.keys()).sort(function(a, b) { return groups.get(b).length - groups.get(a).length; });
    const grid = new Map(), C = 12;
    ctx.globalAlpha = n > 4000 ? 0.45 : 0.75;
    order.forEach(function(col) {
      ctx.fillStyle = col;
      ctx.beginPath();
      groups.get(col).forEach(function(i) {
        const x = X(xs[i]), y = Y(ys[i]);
        ctx.moveTo(x + 2.6, y); ctx.arc(x, y, 2.6, 0, Math.PI * 2);
        const key = Math.floor(x / C) * 4096 + Math.floor(y / C);
        let cell = grid.get(key); if (!cell) { cell = []; grid.set(key, cell); } cell.push(i);
      });
      ctx.fill();
    });
    ctx.globalAlpha = 1;
    cv.addEventListener('pointermove', function(ev) {
      const r = cv.getBoundingClientRect(), px = (ev.clientX - r.left) * (W / r.width), py = (ev.clientY - r.top) * (H / r.height);
      const gx = Math.floor(px / C), gy = Math.floor(py / C);
      let best = -1, bd = 14 * 14;
      for (let a = -1; a <= 1; a++) for (let b = -1; b <= 1; b++) {
        const cell = grid.get((gx + a) * 4096 + (gy + b));
        if (!cell) continue;
        for (let j = 0; j < cell.length; j++) {
          const i = cell[j], dx = X(xs[i]) - px, dy = Y(ys[i]) - py, d = dx * dx + dy * dy;
          if (d < bd) { bd = d; best = i; }
        }
      }
      if (best < 0) { hl.style.display = 'none'; mxTipHide(); return; }
      hl.style.display = 'block';
      hl.style.left = (X(xs[best]) / W * 100) + '%'; hl.style.top = Y(ys[best]) + 'px';
      mxTipShow(cfg.tip(best), ev.clientX, ev.clientY);
    });
    cv.addEventListener('pointerleave', function() { hl.style.display = 'none'; mxTipHide(); });
  }

  /* One-hue sequential ramp for the heatmap: dark navy -> accent cyan,
   * on a square-root scale so a few busy cells do not wash out the rest. */
  function mxSeq(v) {
    const a = [19, 41, 74], b = [83, 216, 251];
    const k = Math.sqrt(Math.max(0, Math.min(1, v)));
    return 'rgb(' + a.map(function(x, j) { return Math.round(x + (b[j] - x) * k); }).join(',') + ')';
  }

  /* ═══ Map (coverage) ═══ */

  /* Point at distance d km along initial bearing brg from (lat, lon) on
   * a sphere — the footprint outline is drawn from these, so it follows
   * great circles rather than straight Mercator lines.  Longitudes are
   * left unwrapped so an outline crossing 180° stays continuous. */
  function mxDest(lat, lon, brg, d) {
    const R = 6371.0088, r = Math.PI / 180, dd = d / R, th = brg * r, p1 = lat * r;
    const p2 = Math.asin(Math.sin(p1) * Math.cos(dd) + Math.cos(p1) * Math.sin(dd) * Math.cos(th));
    const l2 = lon * r + Math.atan2(Math.sin(th) * Math.sin(dd) * Math.cos(p1), Math.cos(dd) - Math.sin(p1) * Math.sin(p2));
    return [p2 / r, l2 / r];
  }
  function mxFootprintLatLngs(rx, vals) {
    const out = [], nsec = vals.length, step = 360 / nsec;
    for (let k = 0; k < nsec; k++) {
      const v = vals[k];
      if (!(v > 0)) { out.push([rx.lat, rx.lon]); continue; }
      for (let j = 0; j <= 4; j++) out.push(mxDest(rx.lat, rx.lon, k * step + j * step / 4, v));
    }
    return out;
  }

  /* ═══ Coverage tab ═══ */

  const MX_PCTS = [['max', 'Furthest'], ['p90', '90th percentile'], ['p75', '75th percentile'], ['p50', 'Median']];
  function mxPctOf(key) { return key === 'p90' ? 0.9 : key === 'p75' ? 0.75 : key === 'p50' ? 0.5 : 1; }

  MX_RENDER.coverage = function(pane, idx) {
    const D = _mx, rx = D.receiver || {};
    /* Skeleton once: the Leaflet map must survive re-renders. */
    if (!pane._built) {
      pane._built = true;
      pane.innerHTML = '<div class="mx-tiles" id="mx-cov-tiles"></div>'
        + '<div class="mx-cov-grid"><div id="mx-cov-mapcard"></div><div id="mx-cov-polarcard"></div></div>';
      pane.querySelector('#mx-cov-mapcard').innerHTML = mxCard('coverage-map', 'Coverage map',
          'Stations heard and the reception footprint around the receiver', '', 'mx-cov-map')
        .replace('<div class="mx-card-body" id="mxb-coverage-map"></div>',
                 '<div class="mx-card-body" id="mxb-coverage-map"></div><div class="mx-map" id="mx-map"></div>'
               + '<div class="mx-map-note" id="mx-map-note" hidden></div>');
    }
    const pct = mxPctOf(_mxState.pct), fp = _mxState.fp;
    const nsec = 36;
    const secDay = [], secNight = [], secAll = [];
    for (let k = 0; k < nsec; k++) { secDay.push([]); secNight.push([]); secAll.push([]); }
    const per = new Map();     /* station -> {n, high, maxd} */
    const isoSet = new Set();
    const dDay = [], dNight = [];
    let far = -1, farD = -1, unid = 0;
    const dMin = mxDistMin('dist'), fMin = mxDistMin('far');
    for (let j = 0; j < idx.length; j++) {
      const i = idx[j], s = D.s[i], d = D.dist[i], b = D.brg[i], lv = D.lvl[i];
      if (s >= 0) {
        let p = per.get(s); if (!p) { p = { n: 0, high: 0, trusted: 0, maxd: NaN }; per.set(s, p); }
        p.n++; if (lv === 3) p.high++;
        if (lv >= dMin) p.trusted++;
        if (lv >= dMin && d === d && !(d <= p.maxd)) p.maxd = d;
        isoSet.add(D.stations[s].iso2);
      } else unid++;
      if (!(d === d) || !(b === b) || s < 0) continue;
      if (lv >= fMin && d > farD) { farD = d; far = s; }
      if (lv < dMin) continue;
      const k = Math.min(nsec - 1, Math.floor(((b % 360) + 360) % 360 / (360 / nsec)));
      secAll[k].push(d);
      if (D.night[i]) { secNight[k].push(d); dNight.push(d); } else { secDay[k].push(d); dDay.push(d); }
    }
    function reach(list) {
      return list.map(function(a) { return a.length ? mxQ(mxSorted(a), pct) : 0; });
    }
    const rDay = reach(secDay), rNight = reach(secNight), rAll = reach(secAll);

    /* Key numbers */
    const medDay = mxMedian(dDay), medNight = mxMedian(dNight);
    const farSt = far >= 0 ? D.stations[far] : null;
    function tile(l, v, s) {
      return '<div class="mx-tile"><div class="mx-tile-l">' + l + '</div><div class="mx-tile-v">' + v + '</div>'
           + (s ? '<div class="mx-tile-s">' + s + '</div>' : '') + '</div>';
    }
    mxEl('mx-cov-tiles').innerHTML =
        tile('Messages', fmtInt(idx.length), unid ? fmtInt(unid) + ' unidentified' : idx.length ? 'all identified' : '')
      + tile('Stations heard', fmtInt(per.size), '')
      + tile('Countries', fmtInt(isoSet.size), '')
      + tile('Furthest station', farSt ? flagHtml(farSt.iso2) + escHtml(fmtKm(farD)) : '—',
             (farSt ? escHtml(farSt.name + ' · ' + farSt.freq_khz + ' ' + farSt.letter) + '<br>'
                    : (rx.valid ? '' : 'receiver location unknown<br>'))
             + escHtml(mxDistBasis('far')))
      + tile('Median distance', '<span style="color:' + MX_DAY_COL + '">&#9679;</span>' + escHtml(fmtKm(medDay))
             + ' <small>day</small>',
             '<span style="color:' + MX_NIGHT_COL + '">&#9679;</span> ' + escHtml(fmtKm(medNight)) + ' night<br>'
             + escHtml(mxDistBasis('dist')));
    mxEl('mxc-coverage-map').querySelector('.mx-card-title').textContent = 'Coverage map';
    const covSub = mxEl('mxc-coverage-map').querySelector('.mx-card-sub');
    if (covSub) covSub.textContent = 'Stations heard, and the reception footprint around the receiver from '
                                   + mxDistBasis('dist') + ' messages';

    /* Polar card (rebuilt each time; it is cheap) */
    const ctl = '<select class="mx-sel" data-mx-opt="pct" title="Reach statistic per 10° sector">'
              + MX_PCTS.map(function(o) { return '<option value="' + o[0] + '"' + (_mxState.pct === o[0] ? ' selected' : '') + '>' + o[1] + '</option>'; }).join('')
              + '</select>'
              + mxToggleHtml('fp', 'd', 'Day', fp.indexOf('d') >= 0, MX_DAY_COL)
              + mxToggleHtml('fp', 'n', 'Night', fp.indexOf('n') >= 0, MX_NIGHT_COL)
              + mxToggleHtml('fp', 'a', 'All', fp.indexOf('a') >= 0, MX_ALL_COL);
    mxEl('mx-cov-polarcard').innerHTML = mxCard('coverage-reach', 'Reach by bearing',
      (MX_PCTS.find(function(o) { return o[0] === _mxState.pct; }) || MX_PCTS[0])[1].toLowerCase()
      + ' distance heard in each 10° sector, from the receiver · ' + mxDistBasis('dist'), ctl);
    const series = [];
    if (fp.indexOf('d') >= 0) series.push({ name: 'Day path', col: MX_DAY_COL, vals: rDay });
    if (fp.indexOf('n') >= 0) series.push({ name: 'Night path', col: MX_NIGHT_COL, vals: rNight });
    if (fp.indexOf('a') >= 0) series.push({ name: 'All', col: MX_ALL_COL, vals: rAll });
    const rows = [];
    const anyDist = secAll.some(function(a) { return a.length; });
    if (anyDist) for (let k = 0; k < nsec; k++)
      rows.push([k * 10, k * 10 + 10, secDay[k].length, rDay[k] || null, secNight[k].length, rNight[k] || null,
                 secAll[k].length, rAll[k] || null]);
    mxMount('coverage-reach', {
      cols: [{ label: 'Bearing from', num: true }, { label: 'Bearing to', num: true },
             { label: 'Day messages', num: true }, { label: 'Day reach km', num: true, fmt: mxKmCell },
             { label: 'Night messages', num: true }, { label: 'Night reach km', num: true, fmt: mxKmCell },
             { label: 'All messages', num: true }, { label: 'All reach km', num: true, fmt: mxKmCell }],
      rows: rows,
      empty: rx.valid ? 'No messages with a distance match these filters.'
                      : 'Receiver location unknown — no distances or bearings to plot.'
    }, function(body) {
      mxPolar(body, { series: series, tipExtra: function(k) {
        return fmtInt(secDay[k].length) + ' day · ' + fmtInt(secNight[k].length) + ' night messages';
      } });
    });

    /* Map card: dataset = the stations plotted */
    const plotted = Array.from(per.keys()).sort(function(a, b) { return per.get(b).n - per.get(a).n; });
    const mrows = plotted.map(function(k) {
      const s = D.stations[k], p = per.get(k);
      return [s.name, s.freq_khz, s.letter, s.country, s.iso2, s.lat, s.lon, p.n, p.high, p.maxd === p.maxd ? p.maxd : null];
    });
    _mxData['coverage-map'] = { rows: mrows, cols: [{ label: 'Station' }, { label: 'kHz', num: true }, { label: 'Letter' },
      { label: 'Country' }, { label: 'ISO2' }, { label: 'Lat', num: true }, { label: 'Lon', num: true },
      { label: 'Messages', num: true }, { label: 'High confidence', num: true }, { label: 'Distance km', num: true, fmt: mxKmCell }] };
    const mbody = mxEl('mxb-coverage-map'), mapEl = mxEl('mx-map');
    const tblOn = !!_mxTable['coverage-map'];
    mxEl('mxc-coverage-map').querySelector('[data-mx-table]').classList.toggle('on', tblOn);
    mbody.innerHTML = tblOn ? (mrows.length ? mxTableHtml(_mxData['coverage-map']) : '<div class="mx-empty">No identified stations match these filters.</div>') : '';
    const note = mxEl('mx-map-note');
    if (tblOn) { mapEl.hidden = true; note.hidden = true; return; }
    if (_mxMapFailed) {
      mapEl.hidden = true; note.hidden = false;
      note.textContent = 'Map unavailable — the map library could not be loaded from cdnjs.cloudflare.com. '
                       + 'Use Table for the station list; the reach chart shows the footprint.';
      return;
    }
    mapEl.hidden = false;
    note.hidden = !(idx.length === 0 || !rx.valid);
    note.textContent = idx.length === 0 ? 'No messages match these filters.'
                     : 'Receiver location unknown — the footprint cannot be drawn; markers show stations heard.';
    loadLeaflet().then(function() {
      if (_mxState.tab !== 'coverage') { _mxStale.coverage = true; return; }
      mxDrawCoverageMap(per, rx, series);
    }).catch(function() {
      _mxMapFailed = true; _mxStale.coverage = true;
      if (_mxState.tab === 'coverage') mxShowTab();
    });
  };
  let _mxMapFailed = false;
  function mxKmCell(v) { return (typeof v === 'number' && isFinite(v)) ? fmtInt(v) : '—'; }

  function mxDrawCoverageMap(per, rx, series) {
    const L = window.L, D = _mx;
    if (!_mxMap) {
      /* Whole zoom levels: at a fractional zoom the tiles are scaled by a
       * non-integer factor and faint seams show at their edges. */
      _mxMap = L.map('mx-map', { worldCopyJump: true, minZoom: 2, zoomSnap: 1 });
      mapTileLayer(L).addTo(_mxMap);
      _mxMapLayers = L.layerGroup().addTo(_mxMap);
    }
    _mxMap.invalidateSize();
    _mxMapLayers.clearLayers();
    const bounds = [];
    const hasRx = rx.valid && typeof rx.lat === 'number' && typeof rx.lon === 'number';
    if (hasRx) series.forEach(function(s) {
      if (!s.vals.some(function(v) { return v > 0; })) return;
      const ll = mxFootprintLatLngs(rx, s.vals);
      L.polygon(ll, { color: s.col, weight: 2, opacity: 0.9, fillColor: s.col, fillOpacity: 0.1, interactive: false })
        .addTo(_mxMapLayers);
      ll.forEach(function(p) { bounds.push(p); });
    });
    let maxN = 1; per.forEach(function(p) { if (p.n > maxN) maxN = p.n; });
    Array.from(per.keys()).sort(function(a, b) { return per.get(b).n - per.get(a).n; }).forEach(function(k) {
      const s = D.stations[k], p = per.get(k);
      if (typeof s.lat !== 'number' || typeof s.lon !== 'number') return;
      const r = 4 + 10 * Math.sqrt(p.n / maxN);
      /* Same basis as the footprint: a station with no message at the
       * distance views' confidence (Medium+ by default) is drawn faint and
       * does not pull the view out — otherwise Canada and Iran sat on a map
       * whose footprint never reached them. */
      const faint = !(p.trusted > 0);
      L.circleMarker([s.lat, s.lon], faint
          ? { radius: r, color: '#53d8fb', weight: 1, opacity: 0.35, dashArray: '3 3', fillColor: '#53d8fb', fillOpacity: 0.05 }
          : { radius: r, color: '#53d8fb', weight: 1.5, opacity: 0.95, fillColor: '#53d8fb', fillOpacity: 0.3 })
        .bindPopup('<div class="pp-title">' + flagHtml(s.iso2) + escHtml(s.name) + '</div>'
          + '<div class="pp-sub">' + escHtml(s.freq_khz + ' kHz · ' + s.letter + ' · ' + s.country) + '</div>'
          + '<div class="pp-grid"><b>Messages</b><span>' + fmtInt(p.n) + '</span>'
          + '<b>High conf.</b><span>' + mxPct(p.high / p.n) + '</span>'
          + '<b>Distance</b><span>' + escHtml(fmtKm(p.maxd)) + '</span></div>', { maxWidth: 260 })
        .bindTooltip(escHtml(s.name) + ' · ' + fmtInt(p.n) + (faint ? ' · below ' + escHtml(mxDistBasis('dist')) : ''),
                     { direction: 'top', offset: [0, -r] })
        .addTo(_mxMapLayers);
      if (!faint) bounds.push([s.lat, s.lon]);
    });
    if (hasRx) {
      L.marker([rx.lat, rx.lon], {
        icon: L.divIcon({ className: '', html: '<div class="rx-icon"></div>', iconSize: [14, 14], iconAnchor: [7, 7] }),
        zIndexOffset: 1000, keyboard: false
      }).bindTooltip('Receiver' + (rx.locator ? ' · ' + escHtml(rx.locator) : ''), { direction: 'top' }).addTo(_mxMapLayers);
      bounds.push([rx.lat, rx.lon]);
    }
    /* Re-frame only when what is selected changes, not on every
     * statistic or layer toggle, so a zoomed-in view is kept. */
    const st = _mxState, fitKey = [st.range, st.from, st.to, st.freq, st.lvl, st.c, st.s, st.path].join('/');
    if (fitKey !== _mxMapFit) {
      _mxMapFit = fitKey;
      if (bounds.length >= 2) _mxMap.fitBounds(bounds, { padding: [24, 24], maxZoom: 7 });
      else if (bounds.length === 1) _mxMap.setView(bounds[0], 5);
      else _mxMap.setView([50, 0], 3);
    }
  }

  /* ═══ Distance tab ═══ */
  MX_RENDER.distance = function(pane, idx) {
    const D = _mx;
    const dMin = mxDistMin('dist'), basis = mxDistBasis('dist');
    pane.innerHTML = mxCard('distance-hour', 'Distance by UTC hour',
        'Furthest, 90th percentile and median distance of messages started in each hour (' + basis
        + '); shading = share on a night path')
      + mxCard('snr-distance', 'SNR vs distance', 'One dot per message with a distance and an SNR (' + basis + ')',
               mxSelectHtml('colour', 'Colour', [['freq', 'Frequency'], ['conf', 'Confidence']]));
    const hours = []; for (let h = 0; h < 24; h++) hours.push([]);
    const nightN = new Int32Array(24), allN = new Int32Array(24);
    for (let j = 0; j < idx.length; j++) {
      const i = idx[j], h = Math.floor((D.t[i] % 86400) / 3600);
      allN[h]++; if (D.night[i]) nightN[h]++;
      const d = D.dist[i]; if (d === d && D.lvl[i] >= dMin) hours[h].push(d);
    }
    const mx = [], p90 = [], med = [], shade = [], rows = [];
    for (let h = 0; h < 24; h++) {
      const s = mxSorted(hours[h]);
      mx.push(s.length ? s[s.length - 1] : NaN); p90.push(mxQ(s, 0.9)); med.push(mxQ(s, 0.5));
      shade.push(allN[h] ? nightN[h] / allN[h] : 0);
      if (s.length || allN[h]) rows.push([String(h).padStart(2, '0') + ':00', s.length, mx[h], p90[h], med[h], allN[h] ? nightN[h] / allN[h] : null]);
    }
    const anyDist = hours.some(function(a) { return a.length; });
    const xs = []; for (let h = 0; h < 24; h++) xs.push(String(h).padStart(2, '0'));
    mxMount('distance-hour', {
      cols: [{ label: 'Hour (UTC)' }, { label: 'Messages with distance', num: true }, { label: 'Max km', num: true, fmt: mxKmCell },
             { label: 'P90 km', num: true, fmt: mxKmCell }, { label: 'Median km', num: true, fmt: mxKmCell },
             { label: 'Night share', num: true, fmt: mxPct }],
      rows: anyDist ? rows : [],
      empty: D.receiver.valid ? 'No messages with a distance match these filters.' : 'Receiver location unknown — no distances to plot.'
    }, function(body) {
      mxLineChart(body, {
        xs: xs, xLabW: 2, aria: 'Distance by UTC hour',
        series: [{ name: 'Furthest', col: '#53d8fb', vals: mx },
                 { name: '90th percentile', col: '#d55181', vals: p90 },
                 { name: 'Median', col: '#ffeb3b', vals: med }],
        yFmt: function(v, tick) { return tick ? mxCompact(v) : fmtKm(v); },
        shade: shade, shadeName: 'Night-path share',
        tipHead: function(i) { return escHtml(xs[i] + ':00–' + xs[i] + ':59 UTC'); },
        tipExtra: function(i) { return fmtInt(hours[i].length) + ' messages · ' + mxPct(shade[i]) + ' night path'; }
      });
    });

    /* Scatter */
    const px = [], py = [], pi = [], pc = [];
    const byConf = _mxState.colour === 'conf';
    for (let j = 0; j < idx.length; j++) {
      const i = idx[j], d = D.dist[i], s = D.snr[i];
      if (!(d === d) || !(s === s) || D.lvl[i] < dMin) continue;
      px.push(d); py.push(s); pi.push(i);
      pc.push(byConf ? MX_LVL_COLS[D.lvl[i]] : mxFreqCol(D.f[i]));
    }
    const legend = byConf ? [3, 2, 1, 0].map(function(l) { return { name: MX_LVL_NAMES[l], col: MX_LVL_COLS[l] }; })
                          : D.freqs.map(function(f) { return { name: mxKhz(f), col: mxFreqCol(f) }; });
    let scRows = null;
    mxMount('snr-distance', {
      cols: [{ label: 'Start (UTC)' }, { label: 'kHz', num: true }, { label: 'Station' }, { label: 'Distance km', num: true },
             { label: 'SNR dB', num: true }, { label: 'Confidence' }],
      n: pi.length,
      get rows() {
        return scRows || (scRows = pi.map(function(i) {
          const s = D.stations[D.s[i]];
          return [mxUtcStamp(D.t[i]), D.f[i], s ? s.name : '', D.dist[i], D.snr[i], MX_LVL_NAMES[D.lvl[i]]];
        }));
      },
      empty: 'No messages with both a distance and an SNR match these filters.'
    }, function(body) {
      mxScatter(body, {
        x: px, y: py, col: pc, legend: legend, xTitle: 'distance (km)', yTitle: 'SNR (dB)', aria: 'SNR versus distance',
        tip: function(k) {
          const i = pi[k], s = D.stations[D.s[i]];
          return mxTipHead(s ? flagHtml(s.iso2) + escHtml(s.name + ' · ' + s.freq_khz + ' ' + s.letter) : 'Unidentified')
               + mxTipRow(pc[k], fmtKm(px[k]), 'distance') + mxTipRow(pc[k], mxDb(py[k]), 'SNR')
               + '<div class="mx-tm">' + escHtml(mxUtcStamp(D.t[i]) + ' · ' + MX_LVL_NAMES[D.lvl[i]]
               + (D.night[i] ? ' · night path' : '')) + '</div>';
        }
      });
    });
  };

  /* ═══ Stations tab ═══ */

  /* Per-station aggregate over the selection, shared by the heatmap and
   * the ranked table. */
  function mxStationAgg(idx) {
    const D = _mx, per = new Map();
    for (let j = 0; j < idx.length; j++) {
      const i = idx[j], s = D.s[i];
      if (s < 0) continue;
      let p = per.get(s);
      if (!p) { p = { s: s, n: 0, high: 0, snr: [], dist: NaN, first: Infinity, last: -Infinity, hours: new Int32Array(24) }; per.set(s, p); }
      p.n++; if (D.lvl[i] === 3) p.high++;
      const v = D.snr[i]; if (v === v) p.snr.push(v);
      const d = D.dist[i]; if (d === d && !(d <= p.dist)) p.dist = d;
      if (D.t[i] < p.first) p.first = D.t[i];
      if (D.t[i] > p.last) p.last = D.t[i];
      p.hours[Math.floor((D.t[i] % 86400) / 3600)]++;
    }
    const list = Array.from(per.values());
    list.forEach(function(p) { p.medSnr = mxMedian(p.snr); p.hiPct = p.high / p.n; });
    return list;
  }

  MX_RENDER.stations = function(pane, idx) {
    const D = _mx;
    const list = mxStationAgg(idx);
    pane.innerHTML = mxCard('station-hours', 'Stations by UTC hour', 'Messages per station in each hour of the day',
        mxSelectHtml('hsort', 'Rows', [['count', 'By messages'], ['dist', 'By distance'], ['name', 'By name']]))
      + mxCard('station-rank', 'Station ranking', 'Click a column to sort');
    /* Heatmap */
    const rows = list.slice();
    const hs = _mxState.hsort;
    rows.sort(hs === 'dist' ? function(a, b) { return (a.dist === a.dist ? a.dist : 1e9) - (b.dist === b.dist ? b.dist : 1e9); }
            : hs === 'name' ? function(a, b) { return D.stations[a.s].name.localeCompare(D.stations[b.s].name); }
            : function(a, b) { return b.n - a.n; });
    const LIMIT = 30, shown = _mxHeatAll ? rows : rows.slice(0, LIMIT);
    let cmax = 1;
    rows.forEach(function(p) { for (let h = 0; h < 24; h++) if (p.hours[h] > cmax) cmax = p.hours[h]; });
    mxMount('station-hours', {
      cols: [{ label: 'Station' }, { label: 'kHz', num: true }, { label: 'Letter' }, { label: 'Country' }]
        .concat(Array.from({ length: 24 }, function(_, h) { return { label: String(h).padStart(2, '0') + 'Z', num: true }; }))
        .concat([{ label: 'Total', num: true }]),
      rows: rows.map(function(p) {
        const s = D.stations[p.s];
        return [s.name, s.freq_khz, s.letter, s.country].concat(Array.from(p.hours)).concat([p.n]);
      }),
      empty: 'No identified stations match these filters.'
    }, function(body) {
      let h = '<div class="mx-heat-wrap"><table class="mx-heat"><thead><tr><th></th>';
      for (let k = 0; k < 24; k++) h += '<th>' + (k % 3 === 0 ? String(k).padStart(2, '0') : '') + '</th>';
      h += '<th></th></tr></thead><tbody>';
      shown.forEach(function(p, r) {
        const s = D.stations[p.s];
        h += '<tr><td class="mx-heat-lab">' + flagHtml(s.iso2) + escHtml(s.name) + '<em>' + escHtml(s.freq_khz + ' ' + s.letter) + '</em></td>';
        for (let k = 0; k < 24; k++) {
          const v = p.hours[k];
          h += '<td class="c" data-r="' + r + '" data-h="' + k + '"' + (v ? ' style="background:' + mxSeq(v / cmax) + '"' : '') + '></td>';
        }
        h += '<td class="mx-heat-n">' + fmtInt(p.n) + '</td></tr>';
      });
      h += '</tbody></table></div>';
      h += '<div class="mx-scale"><span>0</span><i style="background:linear-gradient(90deg,' + mxSeq(0.001) + ',' + mxSeq(0.25)
         + ',' + mxSeq(1) + ')"></i><span>' + fmtInt(cmax) + ' messages / hour cell</span></div>';
      if (rows.length > LIMIT)
        h += '<div class="mx-more"><button class="mx-mini" data-mx-more="heat">'
           + (_mxHeatAll ? 'Show top ' + LIMIT : 'Show all ' + fmtInt(rows.length) + ' stations') + '</button></div>';
      body.innerHTML = h;
      body.querySelector('tbody').addEventListener('pointermove', function(ev) {
        const td = ev.target.closest('td.c');
        if (!td) { mxTipHide(); return; }
        const p = shown[+td.getAttribute('data-r')], k = +td.getAttribute('data-h'), s = D.stations[p.s];
        const hh = String(k).padStart(2, '0');
        mxTipShow(mxTipHead(flagHtml(s.iso2) + escHtml(s.name + ' · ' + s.freq_khz + ' ' + s.letter))
          + mxTipRow(mxSeq(p.hours[k] / cmax), fmtInt(p.hours[k]), 'messages ' + hh + ':00–' + hh + ':59 UTC'), ev.clientX, ev.clientY);
      });
      body.querySelector('tbody').addEventListener('pointerleave', mxTipHide);
    });

    /* Ranked table */
    const R = _mxRank;
    const keyf = {
      name: function(p) { return D.stations[p.s].name; }, country: function(p) { return D.stations[p.s].country; },
      n: function(p) { return p.n; }, hi: function(p) { return p.hiPct; }, snr: function(p) { return p.medSnr; },
      dist: function(p) { return p.dist; }, first: function(p) { return p.first; }, last: function(p) { return p.last; }
    }[R.key] || function(p) { return p.n; };
    const ranked = list.slice().sort(function(a, b) {
      const x = keyf(a), y = keyf(b);
      const xn = (x === x && x !== undefined), yn = (y === y && y !== undefined);
      if (!xn || !yn) return xn === yn ? 0 : xn ? -1 : 1;      /* missing values always last */
      return (typeof x === 'string' ? x.localeCompare(y) : x - y) * R.dir;
    });
    const cols = [{ label: 'Station' }, { label: 'kHz', num: true }, { label: 'Letter' }, { label: 'Country' },
      { label: 'Messages', num: true }, { label: 'High confidence %', num: true }, { label: 'Median SNR dB', num: true },
      { label: 'Distance km', num: true }, { label: 'First heard (UTC)' }, { label: 'Last heard (UTC)' }];
    const ds = {
      cols: cols,
      rows: ranked.map(function(p) {
        const s = D.stations[p.s];
        return [s.name, s.freq_khz, s.letter, s.country, p.n, Math.round(p.hiPct * 1000) / 10,
                p.medSnr === p.medSnr ? Math.round(p.medSnr * 10) / 10 : null, p.dist === p.dist ? Math.round(p.dist) : null,
                mxUtcStamp(p.first), mxUtcStamp(p.last)];
      }),
      empty: 'No identified stations match these filters.'
    };
    mxMount('station-rank', ds, function(body) {
      function th(k, label, num) {
        const on = R.key === k;
        return '<th data-mx-rsort="' + k + '" class="' + (num ? 'num' : '') + (on ? ' sorted' : '') + '"'
             + ' aria-sort="' + (on ? (R.dir > 0 ? 'ascending' : 'descending') : 'none') + '">' + label
             + (on ? (R.dir > 0 ? ' ▲' : ' ▼') : '') + '</th>';
      }
      let h = '<div class="mx-tbl-wrap"><table class="mx-tbl"><thead><tr><th class="num">#</th>' + th('name', 'Station')
            + th('country', 'Country') + th('n', 'Messages', 1) + th('hi', 'High conf.', 1) + th('snr', 'Median SNR', 1)
            + th('dist', 'Distance', 1) + th('first', 'First heard') + th('last', 'Last heard') + '</tr></thead><tbody>';
      ranked.forEach(function(p, r) {
        const s = D.stations[p.s];
        h += '<tr><td class="num">' + (r + 1) + '</td><td>' + flagHtml(s.iso2) + escHtml(s.name) + '<em>'
           + escHtml(s.freq_khz + ' ' + s.letter) + '</em></td><td>' + escHtml(s.country) + '</td><td class="num">' + fmtInt(p.n)
           + '</td><td class="num">' + mxPct(p.hiPct) + '</td><td class="num">' + mxDb(p.medSnr) + '</td><td class="num">'
           + escHtml(fmtKm(p.dist)) + '</td><td>' + escHtml(mxUtcStamp(p.first)) + '</td><td>' + escHtml(mxUtcStamp(p.last)) + '</td></tr>';
      });
      body.innerHTML = h + '</tbody></table></div>';
    });
  };

  /* ═══ Breakdown tab ═══ */
  const MX_GROUPS = [['station', 'Station'], ['country', 'Country'], ['navarea', 'NAVAREA'], ['freq', 'Frequency'],
                     ['subject', 'Subject'], ['hour', 'UTC hour'], ['weekday', 'Weekday']];
  const MX_METRICS = [['msgs', 'Messages'], ['stations', 'Unique stations'], ['maxdist', 'Max distance'],
                      ['snr', 'Median SNR'], ['high', 'High-confidence %']];
  const MX_SPLITS = [['none', 'None'], ['freq', 'Frequency'], ['conf', 'Confidence'], ['path', 'Day / night']];

  /* Split series definitions: key per row and a colour / name per key. */
  function mxSplitDef(kind) {
    const D = _mx;
    if (kind === 'freq') return { keys: D.freqs.slice(), of: function(i) { return D.f[i]; },
                                  name: function(k) { return mxKhz(k); }, col: function(k) { return mxFreqCol(k); } };
    if (kind === 'conf') return { keys: [3, 2, 1, 0], of: function(i) { return D.lvl[i]; },
                                  name: function(k) { return MX_LVL_NAMES[k]; }, col: function(k) { return MX_LVL_COLS[k]; } };
    if (kind === 'path') return { keys: [0, 1], of: function(i) { return D.night[i]; },
                                  name: function(k) { return k ? 'Night path' : 'Day path'; }, col: function(k) { return k ? MX_NIGHT_COL : MX_DAY_COL; } };
    return { keys: [0], of: function() { return 0; }, name: function() { return 'All'; }, col: function() { return '#53d8fb'; } };
  }

  /* Accumulator for one (category, split) cell; metric read at the end. */
  function mxAcc() { return { n: 0, high: 0, st: new Set(), maxd: NaN, snr: [] }; }
  function mxAccAdd(a, i) {
    const D = _mx;
    a.n++; if (D.lvl[i] === 3) a.high++;
    if (D.s[i] >= 0) a.st.add(D.s[i]);
    const d = D.dist[i]; if (d === d && D.lvl[i] >= mxDistMin('dist') && !(d <= a.maxd)) a.maxd = d;
    const v = D.snr[i]; if (v === v) a.snr.push(v);
  }
  function mxAccVal(a, m) {
    if (!a || !a.n) return NaN;
    return m === 'msgs' ? a.n : m === 'stations' ? a.st.size : m === 'maxdist' ? a.maxd
         : m === 'snr' ? mxMedian(a.snr) : a.high / a.n * 100;
  }
  /* The chart's formatter for a table cell ("34%", "3,906 km", "5.6 dB"),
   * with a dash for no value.  Tables show what the chart shows; the CSV
   * keeps the raw numbers. */
  function mxMetricCell(m) {
    const f = mxMetricFmt(m);
    return function(v) { return (typeof v === 'number' && isFinite(v)) ? f(v) : '—'; };
  }

  function mxMetricFmt(m) {
    return m === 'msgs' || m === 'stations' ? function(v) { return fmtInt(v); }
         : m === 'maxdist' ? function(v, tick) { return tick ? mxCompact(v) : fmtKm(v); }
         : m === 'snr' ? function(v, tick) { return tick ? mxCompact(v) : mxDb(v); }
         : function(v) { return Math.round(v) + '%'; };
  }

  MX_RENDER.breakdown = function(pane, idx) {
    const D = _mx, st = _mxState;
    const g = st.g, m = st.m, k = st.k;
    const ctl = mxSelectHtml('g', 'Group', MX_GROUPS) + mxSelectHtml('m', 'Metric', MX_METRICS) + mxSelectHtml('k', 'Split', MX_SPLITS);
    const gname = (MX_GROUPS.find(function(x) { return x[0] === g; }) || MX_GROUPS[0])[1];
    const mname = (MX_METRICS.find(function(x) { return x[0] === m; }) || MX_METRICS[0])[1];
    const additive = m === 'msgs' || (m === 'stations' && k === 'freq');
    pane.innerHTML = '<div class="mx-card mx-opts" style="padding:10px 14px">' + ctl + '</div>'
      + mxCard('breakdown', escHtml(mname + ' by ' + (/^(UTC|NAVAREA)/.test(gname) ? gname : gname.toLowerCase())),
               [k === 'none' ? '' : (additive ? 'Stacked by ' : 'Side by side by ') + (MX_SPLITS.find(function(x) { return x[0] === k; }) || ['', ''])[1].toLowerCase(),
                m === 'maxdist' ? 'distances from ' + mxDistBasis('dist') + ' messages' : ''].filter(Boolean).join(' · '));
    /* Category key per row */
    const catOf = {
      station: function(i) { return D.s[i]; },
      country: function(i) { const s = D.s[i]; return s >= 0 ? D.stations[s].iso2 || '?' : ''; },
      navarea: function(i) { const s = D.s[i]; return s >= 0 ? (D.stations[s].navarea || 0) : -1; },
      freq: function(i) { return D.f[i]; },
      /* Garbled decodes leave digits and punctuation in the subject
       * position; fold them into one "not a letter" row. */
      subject: function(i) { const c = D.b2[i]; return (c >= 65 && c <= 90) ? c : 63; },
      hour: function(i) { return Math.floor((D.t[i] % 86400) / 3600); },
      weekday: function(i) { return (new Date(D.t[i] * 1000).getUTCDay() + 6) % 7; }
    }[g] || function(i) { return D.s[i]; };
    const sp = mxSplitDef(k);
    const cells = new Map(), totals = new Map();
    for (let j = 0; j < idx.length; j++) {
      const i = idx[j], c = catOf(i);
      let row = cells.get(c);
      if (!row) { row = new Map(); cells.set(c, row); totals.set(c, mxAcc()); }
      const sk = sp.of(i);
      let a = row.get(sk); if (!a) { a = mxAcc(); row.set(sk, a); }
      mxAccAdd(a, i); mxAccAdd(totals.get(c), i);
    }
    /* Category order: natural for hour/weekday, else by the metric. */
    let cats = Array.from(cells.keys());
    const ordinal = g === 'hour' || g === 'weekday';
    if (ordinal) {
      cats = g === 'hour' ? Array.from({ length: 24 }, function(_, h) { return h; }) : [0, 1, 2, 3, 4, 5, 6];
    } else {
      cats.sort(function(a, b) {
        const x = mxAccVal(totals.get(a), m), y = mxAccVal(totals.get(b), m);
        const xn = x === x, yn = y === y;
        if (!xn || !yn) return xn === yn ? 0 : xn ? -1 : 1;
        return y - x;
      });
    }
    function catText(c) {
      if (g === 'station') return c >= 0 ? D.stations[c].name + ' · ' + D.stations[c].freq_khz + ' ' + D.stations[c].letter : 'Unidentified';
      if (g === 'country') { if (!c) return 'Unidentified'; const s = D.stations.find(function(x) { return x.iso2 === c; }); return s ? s.country : c; }
      if (g === 'navarea') return c > 0 ? 'NAVAREA ' + roman(c) : c === 0 ? 'NAVAREA ?' : 'Unidentified';
      if (g === 'freq') return mxKhz(c);
      if (g === 'subject') {
        if (c === 63) return '? — not a letter (garbled)';
        const ch = String.fromCharCode(c); const nm = mxSubjectName(ch); return ch + (nm ? ' — ' + nm : '');
      }
      if (g === 'hour') return String(c).padStart(2, '0') + ':00';
      return MX_WEEKDAYS[c];
    }
    function catHtml(c) {
      if (g === 'station' && c >= 0) return mxStationLabel(c);
      if (g === 'country' || g === 'station') return flagHtml(c || '') + '<span>' + escHtml(catText(c)) + '</span>';
      return '<span>' + escHtml(catText(c)) + '</span>';
    }
    const keys = sp.keys.filter(function(sk) {
      for (const row of cells.values()) if (row.has(sk)) return true;
      return false;
    });
    const fmt = mxMetricFmt(m);
    const rowsOut = [];
    cats.forEach(function(c) {
      const row = cells.get(c);
      keys.forEach(function(sk) {
        const v = mxAccVal(row ? row.get(sk) : null, m);
        if (k === 'none' || v === v) rowsOut.push(k === 'none' ? [catText(c), v === v ? v : null, row ? totals.get(c).n : 0]
                                                               : [catText(c), sp.name(sk), v === v ? v : null, row && row.get(sk) ? row.get(sk).n : 0]);
      });
    });
    const ds = {
      cols: k === 'none' ? [{ label: gname }, { label: mname, num: true, fmt: mxMetricCell(m) }, { label: 'Messages', num: true }]
                         : [{ label: gname }, { label: (MX_SPLITS.find(function(x) { return x[0] === k; }) || ['', 'Split'])[1] },
                            { label: mname, num: true, fmt: mxMetricCell(m) }, { label: 'Messages', num: true }],
      rows: rowsOut.filter(function(r) { return r[r.length - 1] > 0 || ordinal; }),
      empty: m === 'maxdist' && !D.receiver.valid ? 'Receiver location unknown — no distances.' : 'No messages match these filters.'
    };
    if (!idx.length) ds.rows = [];
    /* Metric = Messages: the trailing Messages column would repeat it. */
    if (m === 'msgs') {
      ds.cols.pop();
      ds.rows = ds.rows.map(function(r) { return r.slice(0, -1); });
    }
    const series = keys.map(function(sk) {
      return { name: sp.name(sk), col: sp.col(sk), vals: cats.map(function(c) {
        const row = cells.get(c); return mxAccVal(row ? row.get(sk) : null, m);
      }) };
    });
    mxMount('breakdown', ds, function(body) {
      if (ordinal) {
        mxColChart(body, {
          xs: cats.map(function(c) { return g === 'hour' ? String(c).padStart(2, '0') : MX_WEEKDAYS[c]; }),
          xLabW: g === 'hour' ? 2 : 3, series: series, stacked: additive, yFmt: fmt, height: 240,
          tipHead: function(i) { return escHtml(catText(cats[i]) + (g === 'hour' ? ' UTC' : '')); },
          tipExtra: function(i) { const t = totals.get(cats[i]); return fmtInt(t ? t.n : 0) + ' messages'; }
        });
        return;
      }
      const LIMIT = 25, showN = _mxBarsAll ? cats.length : Math.min(LIMIT, cats.length);
      mxHBars(body, {
        cats: cats.slice(0, showN).map(function(c) { return { html: catHtml(c) }; }),
        series: series.map(function(s) { return { name: s.name, col: s.col, vals: s.vals.slice(0, showN) }; }),
        stacked: additive, fmt: fmt,
        valText: function(i) { return fmt(mxAccVal(totals.get(cats[i]), m)); },
        tipExtra: function(i) {
          const t = totals.get(cats[i]);
          return (k !== 'none' ? 'Overall ' + fmt(mxAccVal(t, m)) + ' · ' : '') + fmtInt(t.n) + ' messages';
        },
        more: cats.length > LIMIT ? '<div class="mx-more"><button class="mx-mini" data-mx-more="bars">'
            + (_mxBarsAll ? 'Show top ' + LIMIT : 'Show all ' + fmtInt(cats.length)) + '</button></div>' : ''
      });
    });
  };

  /* ═══ Trends tab ═══ */
  MX_RENDER.trends = function(pane, idx) {
    const D = _mx, st = _mxState;
    const week = st.bucket === 'week', split = st.split === '1';
    const ctl = '<span class="mx-lab">Bucket</span>' + mxToggleHtml('bucket', 'day', 'Day', !week) + mxToggleHtml('bucket', 'week', 'Week', week)
              + '<span class="mx-lab" style="margin-left:8px">Frequency</span>' + mxToggleHtml('split', '0', 'Combined', !split)
              + mxToggleHtml('split', '1', 'Split', split);
    pane.innerHTML = '<div class="mx-card mx-opts" style="padding:10px 14px">' + ctl + '</div><div class="mx-grid2">'
      + mxCard('trend-msgs', 'Messages', 'per ' + (week ? 'ISO week (Mon–Sun, UTC)' : 'UTC day'))
      + mxCard('trend-stations', 'Unique stations heard', 'distinct identified stations per ' + (week ? 'week' : 'day'))
      + mxCard('trend-dist', 'Max distance', 'furthest message per ' + (week ? 'week' : 'day') + ' · '
               + mxDistBasis('dist'))
      + mxCard('trend-high', 'High-confidence share', 'messages identified with high confidence')
      + '</div>';
    /* Bucket start (UTC seconds) per row; weeks start Monday 00:00. */
    function bucketOf(t) {
      const day = Math.floor(t / 86400);
      return week ? (day - ((day + 3) % 7)) * 86400 : day * 86400;   /* 1970-01-01 was a Thursday */
    }
    let lo = Infinity, hi = -Infinity;
    for (let j = 0; j < idx.length; j++) { const b = bucketOf(D.t[idx[j]]); if (b < lo) lo = b; if (b > hi) hi = b; }
    const stepS = week ? 7 * 86400 : 86400;
    const nb = idx.length ? Math.min(2000, Math.round((hi - lo) / stepS) + 1) : 0;
    const keys = split ? D.freqs.slice() : [null];
    const acc = keys.map(function() { return Array.from({ length: nb }, mxAcc); });
    for (let j = 0; j < idx.length; j++) {
      const i = idx[j], b = Math.round((bucketOf(D.t[i]) - lo) / stepS);
      if (b < 0 || b >= nb) continue;
      mxAccAdd(acc[split ? keys.indexOf(D.f[i]) : 0][b], i);
    }
    /* Partial buckets.  The first and last day (or week) of a range are
     * usually cut short — the range starts at "now − 30 d", logging began
     * mid-day, or today is not over — and drawn like full ones they read as
     * an outage.  A bucket counts as partial when the data in it starts
     * more than an hour after the bucket does, or the bucket is not over /
     * the data stops more than an hour before its end.  Marked with "*" on
     * the axis and said so in the tooltip and the subtitle. */
    let tMin = Infinity, tMax = -Infinity;
    for (let j = 0; j < idx.length; j++) { const t = D.t[idx[j]]; if (t < tMin) tMin = t; if (t > tMax) tMax = t; }
    const nowS = Date.now() / 1000;
    const partial = [];
    for (let b = 0; b < nb; b++) {
      const b0 = lo + b * stepS, b1 = b0 + stepS;
      partial.push((b === 0 && tMin - b0 > 3600) || (b === nb - 1 && (b1 > nowS || b1 - tMax > 3600)));
    }
    const xs = [], full = [], isoD = [];
    for (let b = 0; b < nb; b++) {
      const d = new Date((lo + b * stepS) * 1000);
      xs.push(String(d.getUTCDate()).padStart(2, '0') + '/' + String(d.getUTCMonth() + 1).padStart(2, '0')
              + (partial[b] ? '*' : ''));
      isoD.push(d.toISOString().slice(0, 10));
      full.push((week ? 'Week of ' : '') + isoD[b] + (partial[b] ? ' — partial ' + (week ? 'week' : 'day') : ''));
    }
    if (partial.some(Boolean))
      pane.querySelectorAll('.mx-card-sub').forEach(function(el) {
        el.textContent += ' · * partial ' + (week ? 'week' : 'day');
      });
    function sname(k) { return k === null ? 'All frequencies' : mxKhz(k); }
    function scol(k) { return k === null ? '#53d8fb' : mxFreqCol(k); }
    function mk(id, metric, draw) {
      const series = keys.map(function(k, si) {
        return { name: sname(k), col: scol(k), vals: acc[si].map(function(a) { return mxAccVal(a, metric); }) };
      });
      const rows = [];
      for (let b = 0; b < nb; b++) keys.forEach(function(k, si) {
        const v = series[si].vals[b];
        rows.push(split ? [isoD[b], sname(k), v === v ? v : (metric === 'msgs' || metric === 'stations' ? 0 : null), acc[si][b].n]
                        : [isoD[b], v === v ? v : (metric === 'msgs' || metric === 'stations' ? 0 : null), acc[si][b].n]);
      });
      const label = (MX_METRICS.find(function(x) { return x[0] === metric; }) || ['', metric])[1];
      const cols = split ? [{ label: week ? 'Week starting' : 'Date' }, { label: 'Frequency' }, { label: label, num: true, fmt: mxMetricCell(metric) }, { label: 'Messages', num: true }]
                         : [{ label: week ? 'Week starting' : 'Date' }, { label: label, num: true, fmt: mxMetricCell(metric) }, { label: 'Messages', num: true }];
      let trows = nb ? rows : [];
      if (metric === 'msgs') { cols.pop(); trows = trows.map(function(r) { return r.slice(0, -1); }); }
      mxMount(id, {
        cols: cols,
        rows: trows,
        empty: metric === 'maxdist' && !D.receiver.valid ? 'Receiver location unknown — no distances.' : 'No messages match these filters.'
      }, function(body) { draw(body, series); });
    }
    const common = { xs: xs, xLabW: 5, height: 200, tipHead: function(i) { return escHtml(full[i]); } };
    mk('trend-msgs', 'msgs', function(body, series) {
      mxColChart(body, Object.assign({}, common, { series: series, stacked: true, yFmt: function(v) { return fmtInt(v); } }));
    });
    mk('trend-stations', 'stations', function(body, series) {
      mxLineChart(body, Object.assign({}, common, { series: series, yFmt: function(v) { return fmtInt(v); } }));
    });
    mk('trend-dist', 'maxdist', function(body, series) {
      mxLineChart(body, Object.assign({}, common, { series: series, yFmt: function(v, tick) { return tick ? mxCompact(v) : fmtKm(v); } }));
    });
    mk('trend-high', 'high', function(body, series) {
      mxLineChart(body, Object.assign({}, common, { series: series, yMax: 100, yFmt: function(v) { return Math.round(v) + '%'; } }));
    });
  };

  /* ═══ Activity tab: the original two charts from /api/metrics ═══ */
  function renderMetricsCharts(data) {
    const body = document.getElementById('mx-pane-activity');
    if (!body) return;
    if (!data) { body.innerHTML = '<div class="metrics-empty">Loading\u2026</div>'; return; }
    if (data.error) {
      body.innerHTML = '<div class="metrics-empty">Failed to load metrics: ' + escHtml(data.error) + '</div>';
      return;
    }
    /* Frequencies in ascending order, so each keeps the same colour here
     * as on the analytics tabs (which colour by sorted frequency) however
     * the server happened to list its log directories. */
    const order  = (data.freqs || []).map(function(f, i) { return i; })
      .sort(function(a, b) { return parseFloat(data.freqs[a]) - parseFloat(data.freqs[b]); });
    function reorder(bs) {
      return (bs || []).map(function(b) {
        return { label: b.label, counts: order.map(function(i) { return (b.counts || [])[i] || 0; }) };
      });
    }
    const freqs  = order.map(function(i) { return data.freqs[i]; });
    const hours  = reorder(data.hours);
    const days   = reorder(data.days);

    if (freqs.length === 0) {
      body.innerHTML = '<div class="metrics-empty">No message data found.</div>';
      return;
    }

    /* Find max count across all buckets for scaling */
    function maxCount(buckets) {
      var m = 1;
      buckets.forEach(function(b) {
        (b.counts || []).forEach(function(c) { if (c > m) m = c; });
      });
      return m;
    }

    function buildLegend(freqs) {
      var html = '<div class="metrics-legend">';
      freqs.forEach(function(f, i) {
        html += '<div class="metrics-legend-item">'
              + '<div class="metrics-legend-dot freq-' + i + '"></div>'
              + '<span>' + f + '</span></div>';
      });
      return html + '</div>';
    }

    function buildChart(buckets, maxVal, fmtLabel, freqs) {
      /* Y-axis: max, mid, 0 */
      var mid = maxVal > 0 ? Math.round(maxVal / 2) : 0;
      var yaxisHtml = '<div class="metrics-yaxis">'
        + '<span class="metrics-ytick">' + maxVal + '</span>'
        + '<span class="metrics-ytick">' + mid + '</span>'
        + '<span class="metrics-ytick">0</span>'
        + '</div>';

      /* Bars row */
      var barsHtml = '<div class="metrics-bars-row">';
      buckets.forEach(function(b) {
        barsHtml += '<div class="metrics-col-bars"><div class="metrics-bar-group">';
        (b.counts || []).forEach(function(c, fi) {
          var h = maxVal > 0 ? Math.round(c / maxVal * 100) : 0;
          var freqName = (freqs && freqs[fi]) ? freqs[fi] : ('Freq ' + fi);
          var tip = freqName + '\n' + b.label + ': ' + c;
          barsHtml += '<div class="metrics-bar freq-' + fi
                    + '" style="height:' + h + 'px" title="' + tip + '"></div>';
        });
        barsHtml += '</div></div>';
      });
      barsHtml += '</div>';

      /* Labels row — one cell per bucket, same flex proportions */
      var labelsHtml = '<div class="metrics-labels-row">';
      buckets.forEach(function(b) {
        var lbl = fmtLabel ? fmtLabel(b.label) : b.label;
        labelsHtml += '<div class="metrics-col-label"><span class="metrics-label">' + lbl + '</span></div>';
      });
      labelsHtml += '</div>';

      var innerHtml = '<div class="metrics-chart-inner">' + barsHtml + labelsHtml + '</div>';
      return '<div class="metrics-chart-wrap">' + yaxisHtml + innerHtml + '</div>';
    }

    /* Format "YYYY-MM-DD" -> "DD/MM" */
    function fmtDayLabel(s) {
      var parts = s.split('-');
      return parts.length === 3 ? parts[2] + '/' + parts[1] : s;
    }

    var maxH = maxCount(hours);
    var maxD = maxCount(days);

    body.innerHTML =
      buildLegend(freqs)
      + '<div>'
        + '<div class="metrics-section-title">Messages per hour &mdash; last 24 hours</div>'
        + buildChart(hours, maxH, null, freqs)
      + '</div>'
      + '<div>'
        + '<div class="metrics-section-title">Messages per day &mdash; last 30 days</div>'
        + buildChart(days, maxD, fmtDayLabel, freqs)
      + '</div>';
  }

  /* Default to 'Both' split view on load — or what the URL hash names:
   * "#stations", or "#history?…" for a bookmarked, filtered history view. */
  switchTab((location.hash === '#stations') ? 'stations' : 'both');
  fetchStations(false);
  function openHistoryFromHash() {
    const st = histStateFromHash();
    if (st && LOGGING_ENABLED) openHistory('', st);
  }
  openHistoryFromHash();
  /* "#metrics?…": a bookmarked metrics view (tab, filters, options). */
  if (LOGGING_ENABLED && (location.hash || '').indexOf('#metrics') === 0) openMetrics();
  /* Fires for a hash typed or pasted by the user (and back/forward),
   * never for the page's own replaceState() updates. */
  window.addEventListener('hashchange', function() {
    if (location.hash === '#stations') { switchTab('stations'); return; }
    if ((location.hash || '').indexOf('#metrics') === 0) {
      if (!LOGGING_ENABLED) return;
      const ms = mxStateFromHash();
      if (document.getElementById('metrics-modal').style.display === 'none') openMetrics(ms);
      else { _mxState = ms; mxSyncControls(); mxRefresh(); }
      return;
    }
    const st = histStateFromHash();
    if (!st || !LOGGING_ENABLED) return;
    const modal = document.getElementById('history-modal');
    if (modal && modal.style.display === 'none') { openHistoryFromHash(); return; }
    /* Already open: apply the new filters to the loaded list. */
    _histState = st;
    writeHistControls();
    buildHistDropdowns();
    applyHistFilters(false);
  });
  connect();
</script>
</body>
</html>
)HTML";

    /* Inject window._BASE_PATH, and the map tile source, before </head>.
     *
     * NAVTEX_MAP_TILES / NAVTEX_MAP_ATTRIBUTION let an operator point every
     * map on the page at their own tile server (a Leaflet URL template such
     * as "https://tiles.example.lan/{z}/{x}/{y}.png", and the HTML credit
     * line it requires).  Unset or empty, the page uses OpenStreetMap's
     * standard tiles — see mapTileLayer() in the page script.  Read on
     * every page build rather than cached, so it is simply the process
     * environment that counts. */
    std::string inject = "<script>window._BASE_PATH = " + navtex_js_string(base_path) + ";";
    {
        const char *tiles = getenv("NAVTEX_MAP_TILES");
        const char *attr  = getenv("NAVTEX_MAP_ATTRIBUTION");
        if (tiles && *tiles) inject += "window._MAP_TILES = " + navtex_js_string(tiles) + ";";
        if (attr && *attr)   inject += "window._MAP_ATTRIBUTION = " + navtex_js_string(attr) + ";";
    }
    inject += "</script>\n";
    const std::string head_close = "</head>";
    auto pos = html.find(head_close);
    if (pos != std::string::npos)
        html.insert(pos, inject);

    return html;
}