# NAVTEX transmitter identification — test harness

Given (channel, B1, start time, receiver position) decide which physical
transmitter in `src/navtex_stations.h` sent a message, with a calibrated
confidence.  This directory holds the data generators, the evaluator and the
variants being compared.  `data/` and `build/` are generated, not committed.

```
ident_api.h          the interface every variant implements (IdentQuery -> IdentResult)
variants/<name>.h    one self-contained, header-only C++11 variant per file
synth.py             synthetic world + receiver simulator -> CSV (has ground truth)
real_to_csv.py       real Dalgety Bay logs -> data/real.csv (proxy truth, no ground truth)
eval.cpp             evaluator (CSV -> cached .bin -> metrics JSON)
run_eval.sh          incremental build of one variant + run
quick.sh             inner loop: 20k-row train sample + all real data, < 1 s
```

## Data

```sh
python3 -I test/ident/synth.py --seed 1 --days 60 --out test/ident/data/train.csv    # ~350k rows, ~20 s
python3 -I test/ident/synth.py --seed 7 --days 3  --out test/ident/data/smoke.csv    # ~15k rows
python3 -I test/ident/real_to_csv.py                                                  # data/real.csv, 32,651 rows
```

A holdout set is just another seed (and preferably another start date), e.g.
`synth.py --seed 2 --start 2026-11-15 --out test/ident/data/holdout.csv`; `run_eval.sh <v> all`
picks it up automatically when it exists.  `synth.py -h` documents every rate.

CSV columns (shared by both generators):

| column | meaning |
|---|---|
| rx, rx_valid, rx_lat, rx_lon | receiver name and location (`rx_valid 0` = location unknown to the identifier) |
| freq_hz, b1, b2, serial | channel and header as decoded (`b1` may be garbage or empty, serial -1 if unreadable) |
| start_utc, duration_s | unix seconds when ZCZC was seen, ZCZC..NNNN |
| snr_db, chars_failed_pct | decoder metrics, empty if not reported |
| truth | station index, -1 = not in the table / noise, empty = unknown (real data) |
| tags | `|`-separated scenario tags (synthetic) |
| proxy_truth | real data only: text-derived station index (-1 = not in table) — evaluation only |
| dist_km, tx_name | diagnostics only (true distance / transmitter); never inputs |
| rel, proxy_conf | real data only: log path (join key for proxy files), proxy label confidence |

### The synthetic world (`synth.py`)

Deterministic for a given seed.  It builds a TRUE world that differs from the table, then simulates
44 receivers (coastal and inland, every NAVAREA incl. the Mediterranean/Black Sea, North Sea,
Japan/Korea/China, US East coast and Chile/Argentina, Dalgety Bay itself; 7 with location unknown).

* **Table imperfection**: 6% of transmitters 25–400 km off their table position, 1.5% sign-flipped
  coordinate, 15% start 3–15 min early (seen in the real data for 518 I/J/Q/W at Dalgety Bay), 5%
  late, 3% a different slot entirely, 5% skip one slot; empty schedules are canonical
  ((letter−A)×10 min + k×4 h) or occasionally something else; `active=0` stations transmit with
  P 0.5, `active=1` are silent with P 0.05, `active=-1` transmit with P 0.8; 10% have a 2–25 day
  outage; 10 transmitters exist that are not in the table at all (truth -1).  Identical table rows
  (Cabo La Nao is listed twice per letter) are one transmitter; the evaluator treats them as equivalent.
* **Traffic**: each transmitter keeps a set of active warnings (subject mix of the real data: A 77%,
  E 11%, B 7%, ...) with per-subject serial counters and lifetimes; every slot broadcasts the
  active set back to back (8–25 s phasing, 2–12 s gaps), so later messages start minutes into the
  slot; 25% of slots may overrun to 1.5× the slot length; unscheduled A/B/D/L broadcasts at
  0.5/day/transmitter; durations lognormal around the real Dalgety Bay distribution.
* **Propagation** (SNR on the decoder's protocol-v4 scale): groundwave from the stated range
  (floored at 200 nm; real Oostende "55 nm" is heard at 700 km) with distance and per-path terrain
  loss; MF skywave whose daytime D-layer absorption follows the solar elevation at the path
  midpoint (computed from date/time, NOAA formulae); per-path day-to-day ionospheric variation;
  fading; rare anomalous +15–30 dB enhancements; co-channel overlap (signals within 8 dB destroy
  each other, otherwise the stronger captures); probabilistic decode around −3 dB; receivers are
  online in multi-hour sessions (15–45% duty, Dalgety Bay 97%).
* **Decoder imperfection**: header corruption rising near the SNR floor (other letter, non-letter,
  empty), unreadable serials, noise headers (truth -1).

Tags: dx, night_dx, anomalous_dx, offslot, late_slot, overrun, garbled, b1_nonletter, unknown_tx,
noise, pos_error, sign_flip, sched_diff, inactive_on_air, unknown_active, rx_unknown, collision
(≥2 table stations with that freq+letter within 3000 km), capture, inland, hf, jp424.
A `<out>.world.json` beside each CSV records the true world.

### Held-out set (`synth_holdout.py`)

An independent, adversarial generator (shares no code with `synth.py`) for `data/holdout.csv`:

```sh
python3 -I test/ident/synth_holdout.py --out test/ident/data/holdout.csv   # ~390k rows, ~2.5 min
python3 -I test/ident/synth_holdout.py --calib                              # virtual Dalgety Bay statistics only
```

Different world model on purpose: P.368-like groundwave with a Millington mixed land/sea correction,
P.1147-like night skywave (geomagnetic-latitude loss, D-layer absorption at the path control points,
season), night/tropical atmospheric noise, one geomagnetic storm and sporadic DX nights per epoch,
Rician fading, SINR power-sum interference.  Three 10-day epochs (Dec 2026, Jun 2027, Sep 2027), 79
receivers that are not in train: 15 on the bisector of two same-letter table stations, far inland, 11
ships/rigs moving along routes, 9 location-unknown, 2 with a wrong configured position, one on the legacy
+30 dB SNR scale.  Table errors that differ from train's: letter differs from the table (Arkhangelsk
sends L), adjacent-slot shifts, sequential chains (Swedish H/I/J), latitude sign flips, wrong site,
the real schedule errors, decommissioned and new stations, and 26 unlisted transmitters.  Per-station
clock offsets up to ±60 s; vital warnings sent on receipt and repeated off-slot.  Extra tags: ship,
storm, sporadic_dx, equinox/winter/summer, clock_offset, late_start, chain, adj_shift, letter_diff,
garble_adjacent, vital_repeat, long_overrun, rx_pos_wrong, snr_legacy, leakage, equidistant, unsched,
decommissioned, new_station, wrong_site.  `<out>.world.json` records the true world.

### Real data (`real_to_csv.py`)

Reads every sidecar `.json` (exact `start_utc`, SNR, chars).  Sidecars written before audio protocol
v4 (until 2026-09-03 ~13:00 UTC, 3,647 messages) use an SNR scale ~34 dB higher; by default their
`snr_db` is left empty (`--legacy-snr shift|keep` to change that).  One sidecar has an unescaped
`"` as its subject (a bug in the addon's JSON writer) and is repaired on read.  Proxy labels come
from `real_proxy.csv` (and `calib.json` `proxy_labels` if present).

## Evaluating

```sh
test/ident/run_eval.sh <variant> all            # train + holdout (if present) + real, one JSON line each
test/ident/run_eval.sh <variant> all --tags     # plus one line per scenario tag
test/ident/quick.sh <variant>                   # 20k-row train sample + real, for the inner loop
test/ident/run_eval.sh <variant> data/smoke.csv data/train.csv@50000
```

`file.csv@N` evaluates a fixed N-row sample (per receiver, a contiguous chronological window, so
learning variants still see real streams).  Each CSV is converted once into `data/<name>.bin`
(sorted by receiver then time; rebuilt automatically when the CSV is newer) and afterwards read in
one `fread`.  The evaluator core is compiled once into `build/_core/`; a variant change recompiles
only the variant and relinks (~0.3 s).  The single-command build also works:
`g++ -std=c++11 -O2 -DVARIANT='"variants/<name>.h"' eval.cpp -o eval`.

The real proxy file defaults to `$NAVTEX_IDENT_WORK/ident/real_proxy.csv` (`NAVTEX_IDENT_WORK` defaults to `test/ident/work/`, which git ignores) (override with `IDENT_PROXY=` or
`--proxy F`; keyed by `rel`, or by `freq_hz`+`start_utc`); `--proxy-min-conf X` restricts the proxy
metrics to labels with `label_confidence >= X`.

Every receiver gets a fresh `make_identifier()` instance; `identify()` then `observe()` per message,
chronologically.

### Metrics

`correct` = the variant's station is the same transmitter as the truth (duplicates equivalent); for
`station == -1` it means `truth == -1`.

| metric | definition |
|---|---|
| acc | top-1 accuracy over messages whose truth is in the table |
| wrong_hi / n_hi | P(wrong \| attributed, conf ≥ 0.90) — **gate: ≤ 0.01** |
| wrong_med / n_med | P(wrong \| attributed, conf ≥ 0.60) |
| coverage_hi | #(conf ≥ 0.9 and correct) / #all |
| brier | mean (conf − correct)² over all messages |
| ece | expected calibration error, 10 equal-width bins, over all messages |
| unk_conf | mean confidence on messages whose truth is -1 |
| abstain, abstain_prec | share with station -1, and P(truth -1 \| abstained) |
| acc_amb | accuracy where ≥ 2 table transmitters share the freq+letter |
| score | coverage_hi − 2·brier − ece (rank only among gate_ok variants) |
| stability | per (freq, B1), share of attributions going to the modal transmitter |
| far_hi | share of conf ≥ 0.9 attributions more than 2500 km from the receiver |
| conf_hist | 10 confidence bins + abstentions |
| proxy {…} | the truth metrics on real messages with proxy labels |

**Abstentions** must report confidence 0, and are scored in brier/ece as a *certain* claim that the
message is not attributable: they cost 1 when the truth is in the table and 0 when it is -1.  (Scoring
them at their literal confidence 0 would charge 1 for a *correct* abstention and nothing for a wrong
one.)  They never count towards wrong_hi, wrong_med or coverage_hi.  So abstain only when the message
is probably not from a table transmitter; otherwise give the best guess with a low confidence.

`violations` appears when a variant returns an out-of-range station, a confidence outside [0,1], or
a non-zero confidence with station -1.

## Production (`src/navtex_ident.h`)

The server uses `src/navtex_ident.h`: an ensemble of two members built differently,
`bayes_physical` (`src/navtex_ident_bayes.h`) and `rules_calibrated`
(`src/navtex_ident_rules.h`, with the B1-coordination graft from `letter_recovery`),
plus a check of the configured receiver position against what is heard
(`rx_pos_suspect`).  The header's top comment documents the combiner classes, the
"high" gate, the calibration and the measured numbers.

```sh
test/ident/run_eval.sh production data/train.csv data/holdout.csv data/real.csv   # the production header itself
touch test/ident/variants/production.h      # run_eval.sh only tracks the variant file: force a rebuild after editing src/
python3 -I test/ident/make_fixture.py       # regenerate test/testdata/ident_fixture.csv (ctest ident_regression)
```

Sealed sets: `holdout2.csv` (`synth_holdout.py --seed 31337 --epochs 2027-01-15,2027-04-20,2027-10-10 --days 8`)
was evaluated once and failed the gate (1.22%), which led to the 1200 km rule; it no
longer counts as unseen.  `holdout3.csv` (`--seed 4711 --epochs 2027-02-20,2027-07-25,2027-11-05 --days 7`)
was generated afterwards and evaluated once on the final version: wrong_hi 0.62%, coverage_hi 10.5%.
