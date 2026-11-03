# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""What a finished render is worth looking at.

Levels rather than waveforms: which track carries which section, whether
the climax is actually the loudest thing in the piece, whether the master
clips, how many voices are alive at once. These are the numbers a render
is checked against - a bounce through a real host is compared section by
section with them, so their arithmetic is part of the contract.

numpy only, and generously: this is the offline tier.
"""

import numpy as np


def db(value):
    """Amplitude as dBFS, floored so silence prints as a number."""
    return 20 * np.log10(max(float(value), 1e-9))


def section_table(master, out):
    """Per-track RMS in every section, as a table of dBFS."""
    sections = master.composition.SECTIONS
    out("\nper-track section RMS (dBFS):")
    out("  %-14s " % "track"
        + " ".join("%8s" % name.split()[0] for name, _, _ in sections))
    for track in master.composition.TRACKS:
        values = master.section_rms[track["name"]]
        # A track that is silent in a section prints as a dot rather than a
        # number: -180 dB in a column of -25s reads as data, and it is not.
        cells = " ".join("%8.1f" % db(value) if value > 1e-6 else "       ."
                         for value in values)
        out("  %-14s %s" % (track["name"], cells))


def section_profile(master, out):
    """Level, mid/high energy and peak for each section of the piece.

    The hp150 column is the energy above 150 Hz, so sub bass does not
    dominate the numbers the way it does not dominate the ear: two sections
    can measure the same overall and be nothing alike, and this is the
    column that says so.
    """
    mono = master.data.mean(axis=1)
    out("\nsection profile:")
    for (name, _first, _last), (start, end) in zip(master.composition.SECTIONS,
                                                   master.bounds):
        segment = master.data[start:end]
        spectrum = np.fft.rfft(mono[start:end])
        spectrum[np.fft.rfftfreq(end - start, 1.0 / master.sample_rate)
                 < 150.0] = 0.0
        band = np.fft.irfft(spectrum, n=end - start)
        out("  %-14s rms=%6.1f dBFS  hp150=%6.1f dBFS  peak=%6.1f dBFS"
            % (name, db(np.sqrt((segment ** 2).mean())),
               db(np.sqrt((band ** 2).mean())),
               db(np.abs(segment).max())))


def busiest_beat(composition):
    """``(tracks, beat)`` where the most tracks sound at once.

    Every simultaneous track is a sidecar in a real host, so this is the
    number that decides whether a piece will play at all on a given
    machine. Sampled every half beat, which is finer than any track's
    shortest note.
    """
    worst, worst_beat, beat = 0, 0.0, 0.0
    while beat < composition.TOTAL_BEATS:
        count = composition.active_track_count(beat)
        if count > worst:
            worst, worst_beat = count, beat
        beat += 0.5
    return worst, worst_beat


def report(master, out=print):
    """Print the whole report; returns whether the render is usable.

    Usable means it did not clip and did not ask for more simultaneous
    tracks than the piece says it may have.
    """
    composition = master.composition
    section_table(master, out)

    peak = master.peak
    out("\nmaster peak %.3f (%.1f dBFS)" % (peak, db(peak)))

    section_profile(master, out)

    worst, worst_beat = busiest_beat(composition)
    limit = getattr(composition, "ACTIVE_LIMIT", None)
    out("\nmax simultaneous tracks: %d (at beat %.1f)%s"
        % (worst, worst_beat,
           " - limit %d" % limit if limit else " - no limit"))

    return peak < 1.0 and (limit is None or worst <= limit)
