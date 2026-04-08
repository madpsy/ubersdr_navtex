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
                                  const std::string &base_path = "")
{
    /* ---- Tab buttons (individual channels) ---- */
    std::string tab_buttons;
    for (size_t i = 0; i < channels.size(); i++) {
        char btn[512];
        snprintf(btn, sizeof(btn),
            "    <button class=\"tab-btn%s\" id=\"tab-btn-%zu\" onclick=\"switchTab(%zu)\">"
            "<span class=\"tab-dot\" id=\"tab-dot-%zu\"></span>"
            "%s <span class=\"tab-name\">%s</span></button>\n",
            i == 0 ? " active" : "",
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
        std::string disp = (i == 0) ? "flex" : "none";

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
  <label class="ts-label" style="margin-left:auto;display:flex;align-items:center;gap:6px;cursor:pointer;font-size:0.78rem;color:#888;user-select:none">
    <input type="checkbox" id="ts-toggle" checked style="accent-color:#53d8fb;width:14px;height:14px;cursor:pointer">
    Timestamps
  </label>
</header>
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
  const STATE_NAMES = ['Searching', 'Syncing', 'Locked'];
  const STATE_CLASS = ['dim', 'warn', 'good'];
  const TAB_DOT_CLASS = ['searching', 'syncing', 'locked'];

  /* Per-channel debounce timers for decoder state display */
  const decStateTimer  = Array(NUM_CHANNELS).fill(null);
  const decStatePending = Array(NUM_CHANNELS).fill(null);

  const NUM_CHANNELS = )HTML" + std::to_string(channels.size()) + R"HTML(;

  /* Per-channel first-char flag */
  const firstChar = Array(NUM_CHANNELS).fill(true);

  /* ---- Tab switching ---- */
  let activeTab = 0;
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
    firstChar[ch] = false;
    lineStart[ch] = true;
  }

  function copyOutput(ch) {
    const out = document.getElementById('output-' + ch);
    if (!out) return;
    const text = out.innerText || out.textContent || '';
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
    const out = document.getElementById('output-' + ch);
    if (!out) return;
    const text = out.innerText || out.textContent || '';
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
      /* Debounce: only commit the new state after it has been stable for 1 s */
      decStatePending[ch] = s.decState;
      if (decStateTimer[ch] !== null) clearTimeout(decStateTimer[ch]);
      decStateTimer[ch] = setTimeout(function() {
        decStateTimer[ch] = null;
        const st   = decStatePending[ch];
        const name = STATE_NAMES[st] || '?';
        const cls  = STATE_CLASS[st] || '';
        decState.textContent = name;
        decState.className = 'stat-value ' + cls;
        if (tabDot) {
          tabDot.className = 'tab-dot ' + (TAB_DOT_CLASS[st] || '');
        }
        /* Sync split-clone dot if split panel is built */
        const splitDot = document.getElementById('split-dot-' + ch);
        if (splitDot) splitDot.className = tabDot ? tabDot.className : 'tab-dot';
        /* Sync split-clone dec-state text */
        const splitDecState = document.getElementById('dec-state-' + ch + '-s');
        if (splitDecState) {
          splitDecState.textContent = name;
          splitDecState.className   = 'stat-value ' + cls;
        }
      }, 1000);
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
    const msgBar  = chEl('msg-bar',    ch);
    const dot     = chEl('msg-dot',    ch);
    const statTxt = chEl('msg-status', ch);
    const station = chEl('msg-station',ch);
    const subject = chEl('msg-subject',ch);
    const serial  = chEl('msg-serial', ch);

    if (inMsg || done) {
      msgBar.className = 'msg-bar';
      if (done) { dot.className = 'msg-dot complete';  statTxt.textContent = 'Complete'; }
      else      { dot.className = 'msg-dot receiving'; statTxt.textContent = 'Receiving\u2026'; }
      station.textContent = sta ? String.fromCharCode(sta) : '\u2014';
      const subCh = sub ? String.fromCharCode(sub) : '';
      subject.textContent = subCh
        ? (subCh + (SUBJECT[subCh] ? ' \u2013 ' + SUBJECT[subCh] : ''))
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

  function utcStamp() {
    const d = new Date();
    return d.toISOString().slice(11, 19) + 'Z'; /* HH:MM:SSZ */
  }

  function appendChar(ch, chr) {
    const out = chEl('output', ch);
    if (!out) return;

    /* Clear placeholder on first real character */
    if (firstChar[ch]) { out.innerHTML = ''; firstChar[ch] = false; }

    const tsEnabled = document.getElementById('ts-toggle') && document.getElementById('ts-toggle').checked;

    /* If we're at the start of a line and timestamps are on, create a new line div */
    if (lineStart[ch] && tsEnabled) {
      const lineDiv = document.createElement('div');
      lineDiv.className = 'output-line';

      const stamp = document.createElement('span');
      stamp.className = 'ts-stamp';
      stamp.textContent = utcStamp();
      lineDiv.appendChild(stamp);

      const text = document.createElement('span');
      text.className = 'output-text';
      lineDiv.appendChild(text);

      out.appendChild(lineDiv);
      lineStart[ch] = false;
    }

    /* Append the character to the current line */
    if (tsEnabled) {
      /* Find the last .output-text span in the output */
      const spans = out.querySelectorAll('.output-text');
      const target = spans.length > 0 ? spans[spans.length - 1] : null;
      if (target) {
        target.textContent += chr;
      } else {
        /* Fallback: no line div yet (shouldn't happen) */
        out.appendChild(document.createTextNode(chr));
      }
    } else {
      /* Timestamps off: plain text node, same as before */
      out.appendChild(document.createTextNode(chr));
    }

    /* Track line boundaries */
    if (chr === '\n' || chr === '\r') {
      lineStart[ch] = true;
    }

    out.scrollTop = out.scrollHeight;
  }

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