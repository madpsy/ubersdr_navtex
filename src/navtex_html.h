/* -*- c++ -*- */
/*
 * navtex_html.h — embedded HTML/CSS/JS for the dual-frequency NAVTEX web UI.
 * Generated as a C++ inline function; included by navtex_rx_from_ubersdr.cpp.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once
#include <string>
#include <vector>

/* Forward declaration — ChannelContext is defined in the including TU */
struct ChannelContext;

static std::string make_html_page(const std::string &sdr_url,
                                  const std::vector<ChannelContext> &channels,
                                  const std::string &base_path = "",
                                  bool logging_enabled = false)
{
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
    /* "Both" tab — pushed to far right via margin-left:auto */
    tab_buttons +=
        "    <button class=\"tab-btn tab-btn-both\" id=\"tab-btn-both\" onclick=\"switchTab('both')\">"
        "&#x25A6; <span class=\"tab-name\">Both</span></button>\n";

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
            "      <div class=\"stat\"><span class=\"stat-label\">Noise (dBFS/Hz)</span>"
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
                   "<span class=\"act-dot\" id=\"act-dot-" + s + "\"></span>"
                   "<span id=\"act-text-" + s + "\">Idle</span></span></div>\n"
            "      <div class=\"stat\"><span class=\"stat-label\">SNR (dB)</span>"
                   "<span class=\"stat-value\" id=\"snr-val-" + s + "\">&mdash;</span></div>\n"
            "      <div class=\"bar-wrap\"><span class=\"stat-label\">Signal Level</span>"
                   "<div class=\"bar-track\"><div class=\"bar-fill\" id=\"sig-fill-" + s + "\"></div></div></div>\n"
            "      <div class=\"bar-wrap\"><span class=\"stat-label\">SNR Level</span>"
                   "<div class=\"bar-track\"><div class=\"snr-fill\" id=\"snr-fill-" + s + "\"></div></div></div>\n"
            "      <div class=\"bar-wrap\"><span class=\"stat-label\">Quality (clean/fec/err)</span>"
                   "<div class=\"bar-track fec-track\">"
                   "<div class=\"fec-clean\" id=\"fec-clean-" + s + "\" style=\"width:0%\"></div>"
                   "<div class=\"fec-fec\"   id=\"fec-fec-"   + s + "\" style=\"width:0%\"></div>"
                   "<div class=\"fec-fail\"  id=\"fec-fail-"  + s + "\" style=\"width:0%\"></div>"
                   "</div></div>\n"
            "      <button class=\"audio-btn\" id=\"audio-btn-" + s + "\" data-ch=\"" + s + "\">"
                   "&#x1F50A; Audio Preview</button>\n"
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
  margin-left: auto;  /* push to far right */
  border-color: #2a4a6a;
  color: #7ab8d8;
}
.tab-btn-both:hover { color: #53d8fb; }
.tab-btn-both.active { color: #53d8fb; }
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
.bar-wrap { flex: 1; min-width: 100px; display: flex; flex-direction: column; gap: 3px; }
.bar-track {
  height: 7px; background: #0d0d1a; border-radius: 4px; overflow: hidden;
  border: 1px solid #0f3460;
}
.fec-track { display: flex; }
.bar-fill {
  height: 100%; width: 0%;
  background: linear-gradient(90deg, #1a6b1a, #4caf50, #ffeb3b, #e94560);
  border-radius: 4px;
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
  max-width: 860px;
  max-height: 85vh;
  box-shadow: 0 8px 40px rgba(0,0,0,0.7);
  overflow: hidden;
}
.hist-dialog-msg { max-width: 700px; }
.hist-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 14px 18px 12px;
  border-bottom: 1px solid #1a3a5c;
  flex-shrink: 0;
}
.hist-title { font-size: 1rem; font-weight: 600; color: #c8dff0; letter-spacing: 0.02em; }
.hist-close {
  background: transparent; border: none; color: #557; cursor: pointer;
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
  background: #0a1628; color: #557; font-weight: 600;
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
  <div style="margin-left:auto;display:flex;align-items:center;gap:12px">
    <label class="ts-label" style="display:flex;align-items:center;gap:6px;cursor:pointer;font-size:0.78rem;color:#888;user-select:none">
      <input type="checkbox" id="ts-toggle" checked style="accent-color:#53d8fb;width:14px;height:14px;cursor:pointer">
      Timestamps
    </label>
)HTML" + (logging_enabled ? R"HTML(    <button class="history-btn" id="history-btn" onclick="openHistory()">&#x1F4DC; History</button>
)HTML" : "") + R"HTML(  </div>
</header>

<!-- History modal -->
<div id="history-modal" class="hist-overlay" style="display:none" onclick="if(event.target===this)closeHistory()">
  <div class="hist-dialog">
    <div class="hist-header">
      <span class="hist-title">Message History</span>
      <button class="hist-close" onclick="closeHistory()" title="Close">&times;</button>
    </div>
    <div class="hist-toolbar">
      <input type="text" id="hist-search" placeholder="Search&hellip;" oninput="filterHistory()" class="hist-search-input">
      <select id="hist-type-filter" onchange="filterHistory()" class="hist-freq-select">
        <option value="">Messages &amp; Raw Logs</option>
        <option value="msg">Messages only</option>
        <option value="raw">Raw Logs only</option>
      </select>
      <select id="hist-freq-filter" onchange="filterHistory()" class="hist-freq-select">
        <option value="">All frequencies</option>
      </select>
    </div>
    <div class="hist-list-wrap">
      <table class="hist-table" id="hist-table">
        <thead>
          <tr>
            <th>Date</th>
            <th>Time (UTC)</th>
            <th>Frequency</th>
            <th>ID</th>
            <th>Serial</th>
            <th>Station</th>
            <th>Subject</th>
            <th></th>
          </tr>
        </thead>
        <tbody id="hist-tbody"></tbody>
      </table>
      <div id="hist-empty" class="hist-empty" style="display:none">No messages found.</div>
      <div id="hist-loading" class="hist-empty">Loading&hellip;</div>
    </div>
  </div>
</div>

<!-- Message detail modal -->
<div id="msg-modal" class="hist-overlay" style="display:none" onclick="if(event.target===this)closeMsgModal()">
  <div class="hist-dialog hist-dialog-msg">
    <div class="hist-header">
      <span class="hist-title" id="msg-modal-title">Message</span>
      <div style="display:flex;align-items:center;gap:6px;margin-left:auto">
        <button class="icon-btn" id="msg-modal-copy-btn" title="Copy to clipboard" aria-label="Copy">&#x1F4CB;</button>
        <button class="icon-btn" id="msg-modal-dl-btn" title="Download as text file" aria-label="Download">&#x2B07;</button>
        <button class="hist-close" onclick="closeMsgModal()" title="Close">&times;</button>
      </div>
    </div>
    <pre id="msg-modal-body" class="msg-modal-body"></pre>
  </div>
</div>
<div class="tab-bar">
)HTML" + tab_buttons + R"HTML(</div>
<div class="tab-content">
)HTML" + panels + R"HTML(
  <div class="tab-panel" id="panel-both" style="display:none">
  </div>
</div>
<script>
  const SUBJECT = {
    A:'Nav Warning', B:'Met Warning', C:'Ice', D:'SAR',
    E:'Met Forecast', F:'Pilot Service', G:'AIS', H:'LORAN',
    I:'Omega', J:'Satnav', K:'Other Nav', L:'Nav Warning (LORAN)',
    T:'Test', X:'Special', Z:'No msg'
  };
  function decodeNavtexId(id) {
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

  /* Per-channel first-char flag */
  const firstChar = Array(NUM_CHANNELS).fill(true);

  /* Per-channel debounce timers for decoder state display */
  const decStateTimer     = Array(NUM_CHANNELS).fill(null);
  const decStatePending   = Array(NUM_CHANNELS).fill(null);
  const decStateCommitted = Array(NUM_CHANNELS).fill(null); /* last value shown in UI */

  /* ---- Tab switching ---- */
  let activeTab = 'both';
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
                      'act-text','snr-val','sig-fill','snr-fill',
                      'fec-clean','fec-fec','fec-fail',
                      'msg-dot','msg-status','msg-station','msg-subject','msg-serial'];
    suffixes.forEach(function(name) {
      const src  = document.getElementById(name + '-' + ch);
      const dest = document.getElementById(name + '-' + ch + '-s');
      if (!src || !dest) return;
      dest.textContent = src.textContent;
      dest.className   = src.className;
      if (src.style && src.style.width !== undefined) dest.style.width = src.style.width;
      if (src.style && src.style.background !== undefined) dest.style.background = src.style.background;
    });
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
    const bothBtn   = document.getElementById('tab-btn-both');
    const bothPanel = document.getElementById('panel-both');
    if (bothBtn)   bothBtn.classList.remove('active');
    if (bothPanel) bothPanel.style.display = 'none';

    if (ch === 'both') {
      buildSplitPanel();
      /* Sync current state into split clones */
      for (let i = 0; i < NUM_CHANNELS; i++) syncSplitStats(i);
      if (bothBtn)   bothBtn.classList.add('active');
      if (bothPanel) bothPanel.style.display = 'flex';
    } else {
      const btn   = document.getElementById('tab-btn-' + ch);
      const panel = document.getElementById('panel-' + ch);
      if (btn)   btn.classList.add('active');
      if (panel) panel.style.display = 'flex';
    }
    activeTab = ch;
  }

  /* After stats/msg updates, also refresh split clones if Both tab is active */
  function maybeSyncSplit(ch) {
    if (activeTab === 'both' && splitBuilt) syncSplitStats(ch);
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
        const pct = Math.max(0, Math.min(100, (s.bb + 120) / 120 * 100));
        sigFill.style.width = pct + '%';
      } else {
        bbEl.textContent = '\u2014';
        sigFill.style.width = '0%';
      }
    }
    if (s.nd !== undefined)
      ndEl.textContent = isFinite(s.nd) ? s.nd.toFixed(1) : '\u2014';

    if (s.bb !== undefined && s.nd !== undefined && isFinite(s.bb) && isFinite(s.nd)) {
      const snr = s.bb - s.nd;
      snrVal.textContent = snr.toFixed(1) + ' dB';
      snrVal.className = 'stat-value ' + (snr > 45 ? 'good' : snr > 35 ? 'warn' : 'bad');
      const snrPct = Math.max(0, Math.min(100, (snr - 25) / 35 * 100));
      snrFill.style.width = snrPct + '%';
      snrFill.style.background = snr > 45 ? '#4caf50' : snr > 35 ? '#ffeb3b' : '#e94560';
    } else if (s.bb !== undefined) {
      snrVal.textContent = '\u2014'; snrVal.className = 'stat-value';
      snrFill.style.width = '0%';
    }

    if (s.rate !== undefined)
      rateEl.textContent = (s.rate / 1000).toFixed(1) + ' kHz';

    if (s.act !== undefined) {
      if (s.act) { actDot.className = 'act-dot active'; actTxt.textContent = 'Active'; }
      else        { actDot.className = 'act-dot';        actTxt.textContent = 'Idle';   }
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
      station.textContent = sta ? String.fromCharCode(sta) : '\u2014';
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
      station.textContent = '\u2014';
      subject.textContent = '\u2014';
      serial.textContent  = '\u2014';
    }
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
        b.textContent = '\uD83D\uDD0A Audio On';
      } else {
        b.className = 'audio-btn';
        b.textContent = '\uD83D\uDD0A Audio Preview';
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
  let _histData = [];   /* cached list from /api/history */

  function openHistory() {
    const modal = document.getElementById('history-modal');
    if (!modal) return;
    modal.style.display = 'flex';
    /* Reset UI */
    document.getElementById('hist-search').value = '';
    document.getElementById('hist-freq-filter').value = '';
    document.getElementById('hist-type-filter').value = '';
    document.getElementById('hist-empty').style.display   = 'none';
    document.getElementById('hist-loading').style.display = 'block';
    document.getElementById('hist-tbody').innerHTML = '';
    /* Fetch list */
    const base = (window._BASE_PATH || '').replace(/\/$/, '');
    fetch(base + '/api/history')
      .then(function(r) { return r.json(); })
      .then(function(data) {
        _histData = Array.isArray(data) ? data : [];
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
        renderHistTable(_histData);
      })
      .catch(function(err) {
        document.getElementById('hist-loading').style.display = 'none';
        document.getElementById('hist-empty').style.display   = 'block';
        document.getElementById('hist-empty').textContent     = 'Failed to load history: ' + err;
      });
  }

  function closeHistory() {
    const modal = document.getElementById('history-modal');
    if (modal) modal.style.display = 'none';
  }

  function filterHistory() {
    const q    = (document.getElementById('hist-search').value || '').toLowerCase();
    const freq = (document.getElementById('hist-freq-filter').value || '');
    const type = (document.getElementById('hist-type-filter').value || '');
    const filtered = _histData.filter(function(m) {
      if (type && (m.type || 'msg') !== type) return false;
      if (freq && m.freq !== freq) return false;
      if (q) {
        const dec = decodeNavtexId(m.id || '');
        const serial = m.serial || dec.serial || '';
        const hay = (m.freq + ' ' + (m.date || '') + ' ' + (m.time || '') + ' '
                   + (m.id || '') + ' ' + serial + ' '
                   + dec.station + ' ' + dec.subject).toLowerCase();
        if (hay.indexOf(q) === -1) return false;
      }
      return true;
    });
    renderHistTable(filtered);
  }

  function renderHistTable(rows) {
    const tbody = document.getElementById('hist-tbody');
    const empty = document.getElementById('hist-empty');
    tbody.innerHTML = '';
    if (rows.length === 0) {
      empty.style.display   = 'block';
      empty.textContent     = 'No entries found.';
      return;
    }
    empty.style.display = 'none';
    rows.forEach(function(m) {
      const isRaw = (m.type === 'raw');
      const dec = isRaw ? { station: '—', subject: '—', serial: '—' } : decodeNavtexId(m.id || '');
      const tr = document.createElement('tr');

      /* Date */
      const tdDate = document.createElement('td');
      tdDate.className = 'hist-date';
      tdDate.textContent = m.date || '—';
      tr.appendChild(tdDate);

      /* Time — only for message files; raw logs span a whole day */
      const tdTime = document.createElement('td');
      tdTime.className = 'hist-time';
      if (isRaw) {
        const badge = document.createElement('span');
        badge.style.cssText = 'font-size:0.7rem;background:#1a3a5c;color:#53d8fb;'
                            + 'border-radius:3px;padding:1px 5px;letter-spacing:0.5px;';
        badge.textContent = 'RAW LOG';
        tdTime.appendChild(badge);
      } else {
        const t = (m.time || '').replace(/Z$/i, '');
        tdTime.textContent = t.length === 6
          ? t.slice(0,2) + ':' + t.slice(2,4) + ':' + t.slice(4,6) + ' UTC'
          : (m.time || '—');
      }
      tr.appendChild(tdTime);

      /* Frequency */
      const tdFreq = document.createElement('td');
      tdFreq.className = 'hist-freq';
      tdFreq.textContent = m.freq || '—';
      tr.appendChild(tdFreq);

      /* ID (raw code, e.g. "EA42") — blank for raw logs */
      const tdId = document.createElement('td');
      tdId.className = 'hist-id';
      tdId.textContent = isRaw ? '—' : ((m.id && m.id !== 'unknown') ? m.id.toUpperCase() : '—');
      tr.appendChild(tdId);

      /* Serial — from server-parsed field, or decoded from id */
      const tdSerial = document.createElement('td');
      tdSerial.style.fontFamily = 'monospace';
      tdSerial.style.color = '#a0b8d8';
      if (!isRaw) {
        const ser = m.serial || dec.serial || '';
        tdSerial.textContent = (ser && ser !== '—') ? ser : '—';
      } else {
        tdSerial.textContent = '—';
      }
      tr.appendChild(tdSerial);

      /* Station */
      const tdStation = document.createElement('td');
      tdStation.textContent = isRaw ? '—' : dec.station;
      tr.appendChild(tdStation);

      /* Subject */
      const tdSubject = document.createElement('td');
      tdSubject.textContent = isRaw ? '—' : dec.subject;
      tr.appendChild(tdSubject);

      /* View / Download button */
      const tdBtn = document.createElement('td');
      const btn = document.createElement('button');
      btn.className = 'hist-view-btn';
      if (isRaw) {
        btn.textContent = 'Download';
        btn.onclick = function() { downloadRawLog(m.path, m.freq, m.date); };
      } else {
        btn.textContent = 'View';
        const t = (m.time || '').replace(/Z$/i, '');
        const tFmt = t.length === 6
          ? t.slice(0,2) + ':' + t.slice(2,4) + ':' + t.slice(4,6) + ' UTC'
          : (m.time || '—');
        const idStr = (m.id && m.id !== 'unknown') ? m.id.toUpperCase() : '—';
        const title = (m.freq || '') + ' · ' + (m.date || '') + ' ' + tFmt + ' · ' + idStr
                    + ' · ' + dec.station;
        btn.onclick = function() { viewMessage(m.path, title); };
      }
      tdBtn.appendChild(btn);
      tr.appendChild(tdBtn);
      tbody.appendChild(tr);
    });
  }

  function downloadRawLog(path, freq, date) {
    const base = (window._BASE_PATH || '').replace(/\/$/, '');
    const url = base + '/api/history/file?path=' + encodeURIComponent(path);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'navtex-raw-' + (freq || 'log').replace(/\s/g, '') + '-' + (date || 'log') + '.log';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
  }

  function viewMessage(path, title) {
    const modal   = document.getElementById('msg-modal');
    const body    = document.getElementById('msg-modal-body');
    const ttl     = document.getElementById('msg-modal-title');
    const copyBtn = document.getElementById('msg-modal-copy-btn');
    const dlBtn   = document.getElementById('msg-modal-dl-btn');
    if (!modal || !body) return;
    ttl.textContent  = title || 'Message';
    body.textContent = 'Loading\u2026';
    /* Reset button state while loading */
    if (copyBtn) { copyBtn.onclick = null; copyBtn.style.opacity = '0.4'; }
    if (dlBtn)   { dlBtn.onclick   = null; dlBtn.style.opacity   = '0.4'; }
    modal.style.display = 'flex';
    const base = (window._BASE_PATH || '').replace(/\/$/, '');
    fetch(base + '/api/history/file?path=' + encodeURIComponent(path))
      .then(function(r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.text();
      })
      .then(function(text) {
        body.textContent = text;
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
        if (copyBtn) copyBtn.style.opacity = '0.4';
        if (dlBtn)   dlBtn.style.opacity   = '0.4';
      });
  }

  function closeMsgModal() {
    const modal = document.getElementById('msg-modal');
    if (modal) modal.style.display = 'none';
  }

  /* Default to 'Both' split view on load */
  switchTab('both');
  connect();
</script>
</body>
</html>
)HTML";

    /* Inject window._BASE_PATH before </head> */
    const std::string inject =
        "<script>window._BASE_PATH = \"" + base_path + "\";</script>\n";
    const std::string head_close = "</head>";
    auto pos = html.find(head_close);
    if (pos != std::string::npos)
        html.insert(pos, inject);

    return html;
}