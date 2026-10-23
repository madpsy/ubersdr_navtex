#!/usr/bin/env python3
"""Convert the real Dalgety Bay NAVTEX logs into the harness CSV layout.

    python3 -I test/ident/real_to_csv.py [--history H] [--logs DIR] [--calib calib.json]
                                         [--proxy real_proxy.csv] [--out data/real.csv]

history.json is the index; every message is read from its .json sidecar beside
the .txt (sidecars are authoritative: exact start_utc, snr, chars).  Sidecars
missing from the index are added too.

  rx              dalgety_bay (56.0403, -3.3554), rx_valid 1
  start_utc       sidecar start_utc (when ZCZC was seen), unix seconds
  snr_db          sidecar avg_snr_db.  Sidecars written before audio protocol
                  version 4 (until 2026-09-03 ~13:00 UTC) use a different SNR
                  scale (~+34 dB); --legacy-snr drop (default) leaves snr_db
                  empty for them, 'shift' subtracts --legacy-shift, 'keep' keeps it.
  truth           empty (no ground truth)
  proxy_truth     text-derived station index (or -1 = transmitter not in the
                  table) from --proxy (rel,station_index,label_confidence,...)
                  and/or calib.json "proxy_labels"; empty if unlabelled.
                  NEVER an input to identification - evaluation only.
  rel             the message's path under navtex_logs (join key)
  proxy_conf      label_confidence of the proxy label
"""
import argparse
import calendar
import csv
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# Working data (the receiver's logs under real/, calibration and proxy labels
# under ident/) lives outside git: NAVTEX_IDENT_WORK, default test/ident/work.
SCR = os.environ.get('NAVTEX_IDENT_WORK', os.path.join(HERE, 'work'))
RX = ('dalgety_bay', 56.0403, -3.3554)


def load_json(path):
    t = open(path, encoding='utf-8', errors='replace').read()
    try:
        return json.loads(t)
    except ValueError:
        # the addon writes a lone '"' (or '\') unescaped as station/subject: "subject": """,
        t = re.sub(r'(:\s*)"(.)"(\s*,?\s*\n)', lambda m: m.group(1) + json.dumps(m.group(2)) + m.group(3), t)
        return json.loads(t)


def iso_to_unix(s):
    return calendar.timegm(time.strptime(s[:19], '%Y-%m-%dT%H:%M:%S'))


def clean_char(c):
    if c is None or c == '':
        return ''
    c = str(c)[0]
    return '?' if c in ',"\n\r' else c


def load_proxy(args):
    lab = {}
    if args.proxy and os.path.exists(args.proxy):
        for r in csv.DictReader(open(args.proxy, encoding='utf-8')):
            v = r.get('station_index', r.get('proxy_truth', ''))
            if v not in ('', None):
                lab[r['rel']] = (int(v), r.get('label_confidence', ''))
        sys.stderr.write('proxy: %d labels from %s\n' % (len(lab), args.proxy))
    if args.calib and os.path.exists(args.calib):
        try:
            cj = json.load(open(args.calib))
        except ValueError:
            cj = {}
        pl = cj.get('proxy_labels') if isinstance(cj, dict) else None
        n = 0
        if isinstance(pl, dict):
            for k, v in pl.items():
                if isinstance(v, dict):
                    v, c = v.get('station_index', v.get('station')), v.get('label_confidence', '')
                else:
                    c = ''
                if v is not None and k not in lab:
                    lab[k] = (int(v), c); n += 1
        elif isinstance(pl, list):
            for e in pl:
                k = e.get('rel')
                v = e.get('station_index', e.get('station'))
                if k and v is not None and k not in lab:
                    lab[k] = (int(v), e.get('label_confidence', '')); n += 1
        sys.stderr.write('calib.json: %d additional proxy labels\n' % n)
    return lab


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--history', default=os.path.join(SCR, 'real', 'history.json'))
    p.add_argument('--logs', default=os.path.join(SCR, 'real', 'navtex_logs'))
    p.add_argument('--calib', default=os.path.join(SCR, 'ident', 'calib.json'))
    p.add_argument('--proxy', default=os.path.join(SCR, 'ident', 'real_proxy.csv'))
    p.add_argument('--legacy-snr', choices=('drop', 'shift', 'keep'), default='drop')
    p.add_argument('--legacy-shift', type=float, default=34.0)
    p.add_argument('--out', default=os.path.join(HERE, 'data', 'real.csv'))
    a = p.parse_args()

    rels = []
    if os.path.exists(a.history):
        for e in json.load(open(a.history)):
            if e.get('type') == 'msg' and e.get('rel'):
                rels.append(e['rel'])
    seen = set(rels)
    for root, _, files in os.walk(a.logs):
        for f in files:
            if f.endswith('.json'):
                rel = os.path.relpath(os.path.join(root, f[:-5] + '.txt'), a.logs)
                if rel not in seen:
                    rels.append(rel); seen.add(rel)
    proxy = load_proxy(a)
    rows, missing, legacy = [], 0, 0
    for rel in rels:
        sc = os.path.join(a.logs, rel[:-4] + '.json')
        if not os.path.exists(sc):
            missing += 1
            continue
        j = load_json(sc)
        if not j.get('start_utc'):
            missing += 1
            continue
        snr = j.get('avg_snr_db')
        if snr is not None and (j.get('audio_protocol_version') or 0) < 4:
            legacy += 1
            snr = None if a.legacy_snr == 'drop' else (snr - a.legacy_shift if a.legacy_snr == 'shift' else snr)
        chf = j.get('avg_chars_failed_pct')
        ser = j.get('serial')
        pt = proxy.get(rel)
        rows.append([RX[0], 1, '%.4f' % RX[1], '%.4f' % RX[2], int(j['freq_hz']),
                     clean_char(j.get('station')), clean_char(j.get('subject')),
                     -1 if ser is None else int(ser), iso_to_unix(j['start_utc']),
                     int(j.get('duration_s') or 0),
                     '' if snr is None else '%.2f' % snr, '' if chf is None else '%.1f' % chf,
                     '', '', '' if pt is None else pt[0], '', '', rel, '' if pt is None else pt[1]])
    rows.sort(key=lambda r: (r[8], r[4]))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w', newline='') as fh:
        w = csv.writer(fh, lineterminator='\n')
        w.writerow(['rx', 'rx_valid', 'rx_lat', 'rx_lon', 'freq_hz', 'b1', 'b2', 'serial', 'start_utc',
                    'duration_s', 'snr_db', 'chars_failed_pct', 'truth', 'tags', 'proxy_truth', 'dist_km',
                    'tx_name', 'rel', 'proxy_conf'])
        w.writerows(rows)
    sys.stderr.write('wrote %s: %d rows (%d legacy-SNR sidecars, snr %s; %d without sidecar; %d proxy-labelled)\n'
                     % (a.out, len(rows), legacy, a.legacy_snr, missing, sum(1 for r in rows if r[14] != '')))


if __name__ == '__main__':
    main()
