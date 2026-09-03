/* -*- c++ -*- */
/*
 * navtex_snr.h — the one place the SNR scale and its thresholds are defined.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * WHY THIS FILE EXISTS
 * --------------------
 * Audio protocol version 4 changed the units of the noise figure the server
 * sends, and every SNR number this program shows, stores or colours moved with
 * it.  The thresholds used to be four copies of `snr > 45 ? 'good' : snr > 35`
 * scattered through navtex_html.h, plus a bar scaled `(snr - 25) / 35`;
 * rescaling those independently is how they drift apart.  They are defined once
 * here now, and the page's JavaScript is generated from these values rather
 * than repeating them.
 *
 * THE SCALE CHANGE
 * ----------------
 * The header carries a baseband power and a noise figure, and the SNR every
 * consumer shows is the difference.
 *
 *   version 2:  noise = radiod's NOISE_DENSITY, a power spectral DENSITY in
 *               dBFS/Hz.  Subtracting it from a power gives S/N0 in dB*Hz, not
 *               an SNR -- it reads 10*log10(bandwidth) too high.
 *   version 4:  noise = channelNoisePower(NoiseDensity, FilterBandwidthHz()),
 *               which is NoiseDensity + 10*log10(passband): a power in dBFS, in
 *               the same units as the signal, so the subtraction is a real SNR.
 *
 * (ka9q_ubersdr/websocket.go:1695-1699 chooses between them on the negotiated
 * version; radiod_status.go:295,318 define FilterBandwidthHz and
 * channelNoisePower.)
 *
 * THE PASSBAND IS OURS, AND THE SHIFT WAS MEASURED
 * ------------------------------------------------
 * FilterBandwidthHz() is radiod's HighEdge - LowEdge, and those edges are the
 * ones we ask for: run_channel() puts bandwidthLow=50 and bandwidthHigh=2700 in
 * the WebSocket URL and the server honours both when both are present
 * (websocket.go:621-633, applied at :748-765).  2700 - 50 = 2650 Hz, so the
 * predicted shift is 10*log10(2650) = 34.23 dB.  That is NOT the [usb] preset's
 * 50..3000 (2950 Hz, 34.70 dB), which would only stand if we sent no bandwidth
 * parameters -- and every commit that has ever touched the URL sends these two.
 *
 * Measured rather than assumed: two sessions on 517500 Hz at the same moment,
 * one negotiating version 2 and one version 4, 60 s and ~3000 packets each
 * (m9psy.tunnel.ubersdr.org, 2026-09-03).  The baseband statistics came back
 * identical to the last 0.01 dB in both, which is what makes the pair a
 * controlled comparison, and the noise figure moved by:
 *
 *     median -146.92 -> -112.70 dBFS   = +34.22 dB
 *     mean   -146.87 -> -112.65 dBFS   = +34.22 dB
 *     p05    -149.21 -> -114.98 dBFS   = +34.23 dB
 *
 * against the predicted 34.23.  No clamp is visible on either scale: the
 * version 4 noise figure ranged -104 to -126 dBFS across every capture, nowhere
 * near a floor.
 *
 * THE THRESHOLDS COME FROM THE MEASURED DISTRIBUTION
 * --------------------------------------------------
 * Subtracting the shift from the old numbers would carry forward whatever the
 * old ones were fitted to, so the boundaries below are set from what NAVTEX
 * actually reads on the version 4 scale, captured on this program's own
 * frequencies and its own 2650 Hz passband (m9psy, 2026-09-03, 5 minutes and
 * ~15000 packets each):
 *
 *   490 kHz, idle:            SNR median -6.59, p05 -9.76, p95 -4.07, max +2.32
 *   518 kHz, signal present:  SNR median 15.85, p05 12.03, p95 18.62, max 25.12
 *                             (baseband -95.6 dBFS, 23 dB above the 490 floor)
 *
 * The two are cleanly separated: the 518 histogram is empty between +3 and +9
 * dB, and 99% of its packets sit in +12..+21.  So:
 *
 *   WARN at +3 dB   -- the top of the idle distribution.  Below this nothing is
 *                      being received; 100% of the idle capture is below it.
 *   GOOD at +12 dB  -- the 5th percentile of a real received transmission, so
 *                      "good" means as strong as a genuine NAVTEX signal, and
 *                      the empty band between the two distributions is "warn".
 *   BAR -10..+25 dB -- the full measured range, floor to peak.
 *
 * As a cross-check, converting the old calibration by the measured shift gives
 * 45 - 34.23 = 10.8, 35 - 34.23 = 0.8 and a bar floor of -9.2.  Two independent
 * routes -- the operator's old fit carried across, and a fresh fit to live data
 * -- agree to within about 2 dB, which is the best evidence available that
 * neither is badly wrong.
 *
 * These are calibrated on ONE receiver.  A site with a different antenna or
 * noise environment may want them moved; this is the only place to move them.
 */

#ifndef NAVTEX_SNR_H
#define NAVTEX_SNR_H

#include <cmath>
#include <string>

/* The demodulator passband this program asks for, in Hz: bandwidthHigh -
 * bandwidthLow from the WebSocket URL in run_channel().  Change one and the
 * other must change with it. */
static const double NAVTEX_PASSBAND_HZ = 2700.0 - 50.0;

/* How far an SNR fell when the noise figure stopped being a density:
 * 10*log10(2650) = 34.23 dB, measured at 34.22.  Needed only for converting
 * records stored before the migration -- a live version 4 reading arrives on
 * the new scale already and is never shifted. */
static inline double navtex_snr_scale_shift_db()
{
    return 10.0 * std::log10(NAVTEX_PASSBAND_HZ);
}

/* Boundaries on the version 4 scale, from the measured distributions above. */
static const double NAVTEX_SNR_GOOD_DB    =  12.0; /* p05 of a real signal   */
static const double NAVTEX_SNR_WARN_DB    =   3.0; /* top of the idle floor  */
static const double NAVTEX_SNR_BAR_MIN_DB = -10.0; /* measured floor         */
static const double NAVTEX_SNR_BAR_SPAN_DB = 35.0; /* to +25, measured peak  */

static inline double navtex_snr_good_db()    { return NAVTEX_SNR_GOOD_DB; }
static inline double navtex_snr_warn_db()    { return NAVTEX_SNR_WARN_DB; }
static inline double navtex_snr_bar_min_db() { return NAVTEX_SNR_BAR_MIN_DB; }

/* Convert an SNR recorded on the version 2 scale to the version 4 one. */
static inline double navtex_snr_legacy_to_v4(double legacy_snr_db)
{
    return legacy_snr_db - navtex_snr_scale_shift_db();
}

/* Convert a noise DENSITY in dBFS/Hz, as versions 1-3 reported it, to the noise
 * POWER in the passband in dBFS that version 4 reports. */
static inline double navtex_noise_density_to_power(double density_dbfs_per_hz)
{
    return density_dbfs_per_hz + navtex_snr_scale_shift_db();
}

/* "good", "warn" or "bad" for an SNR already on the version 4 scale, or "dim"
 * when there is no reading.  The page classifies in JavaScript, generated from
 * the same two boundaries; this exists so the classification is testable. */
static inline std::string navtex_snr_class(double snr_db)
{
    if (!std::isfinite(snr_db)) return "dim";
    if (snr_db > NAVTEX_SNR_GOOD_DB) return "good";
    if (snr_db > NAVTEX_SNR_WARN_DB) return "warn";
    return "bad";
}

/* The SNR bar's fill, 0-100. */
static inline double navtex_snr_bar_pct(double snr_db)
{
    double pct = (snr_db - NAVTEX_SNR_BAR_MIN_DB) / NAVTEX_SNR_BAR_SPAN_DB * 100.0;
    if (!std::isfinite(pct) || pct < 0.0) return 0.0;
    if (pct > 100.0) return 100.0;
    return pct;
}

/* ------------------------------------------------------------------ */
/* Stored records                                                       */
/* ------------------------------------------------------------------ */

/*
 * Message metrics are written to a .json sidecar next to each saved message and
 * read back by the history list and the metrics modal, so a log directory that
 * survives this upgrade holds records from both scales in one field.
 *
 * Every sidecar written from now on carries the audio protocol version it was
 * recorded on.  A sidecar without that key is pre-migration by definition --
 * nothing but save_message() has ever written one -- and is converted on the
 * way out of the API, using the passband above, which every build that could
 * have written it requested.
 *
 * Converting rather than refusing to judge is what this storage allows: the
 * records are this program's own, the passband is fixed in its own source and
 * in its whole git history, and the shift is measured rather than assumed.  The
 * conversion is declared in the JSON it comes back in (snr_rescaled_from_density)
 * and the history table says so on hover, so a converted figure is never passed
 * off as a native one.
 */
static const int NAVTEX_AUDIO_PROTOCOL_VERSION = 4;

/* The sidecar key that says which scale a record is on. */
#define NAVTEX_SIDECAR_VERSION_KEY "audio_protocol_version"

#endif /* NAVTEX_SNR_H */
