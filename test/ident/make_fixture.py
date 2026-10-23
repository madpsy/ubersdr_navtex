#!/usr/bin/env python3
"""Write test/testdata/ident_fixture.csv: a small, deterministic sample of the held-out
synthetic set for the ctest regression (test/ident_regression.cpp).

    python3 -I test/ident/make_fixture.py [--src test/ident/data/holdout.csv] [--rows 3000]

For a subset of receivers (taken in the order of an FNV-1a hash of their names until
--rows is reached), a contiguous chronological window of --window rows at an offset
fixed by the same hash, so learning in observe() still sees a real stream.  The window
must be longer than the identifier's position-check warm-up (POS_MIN_OBS = 100
messages at the configured position): before that nothing is "high".  B1/B2 are written as character codes so that garbage letters
(',' '"' ...) need no CSV quoting.
"""
import argparse, csv, os

HERE = os.path.dirname(os.path.abspath(__file__))


def fnv1a(s):
    h = 2166136261
    for b in s.encode():
        h ^= b
        h = (h * 16777619) & 0xffffffff
    return h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', default=os.path.join(HERE, 'data', 'holdout.csv'))
    ap.add_argument('--rows', type=int, default=4000)
    ap.add_argument('--window', type=int, default=200)
    ap.add_argument('--out', default=os.path.join(HERE, '..', 'testdata', 'ident_fixture.csv'))
    a = ap.parse_args()
    by = {}
    for r in csv.DictReader(open(a.src, newline='')):
        by.setdefault(r['rx'], []).append(r)
    out = []
    for nm in sorted(by, key=lambda n: (fnv1a(n), n)):
        if len(out) >= a.rows:
            break
        rows = sorted(by[nm], key=lambda r: int(r['start_utc']))     # stable, like eval.cpp
        cnt = len(rows)
        q = min(cnt, a.window, a.rows - len(out))
        off = fnv1a(nm) % (cnt - q + 1) if cnt > q else 0
        out.extend(rows[off:off + q])
    out.sort(key=lambda r: r['rx'])                                    # grouped per receiver
    cols = ['rx', 'rx_valid', 'rx_lat', 'rx_lon', 'freq_hz', 'b1', 'b2', 'serial', 'start_utc', 'duration_s',
            'snr_db', 'chars_failed_pct', 'truth']
    with open(a.out, 'w', newline='') as f:
        f.write('# NAVTEX identification regression fixture: %d rows sampled from %s (see make_fixture.py)\n'
                % (len(out), os.path.basename(a.src)))
        f.write(','.join(cols) + '\n')
        for r in out:
            v = [r[c] for c in cols]
            v[5] = str(ord(r['b1'])) if r['b1'] else '0'
            v[6] = str(ord(r['b2'])) if r['b2'] else '0'
            f.write(','.join(v) + '\n')
    print('%d rows -> %s' % (len(out), a.out))


if __name__ == '__main__':
    main()
