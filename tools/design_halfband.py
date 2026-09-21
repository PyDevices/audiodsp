#!/usr/bin/env python3
"""Design (and check) audiodsp_shaper.c's polyphase half-band coefficients.

    tools/design_halfband.py --verify      check the shipped four
    tools/design_halfband.py --design      search for them again

`audioshaper.Waveshaper` interpolates and decimates with a two-path
polyphase all-pass half-band,

    H(w) = ( A0(e^{j2w}) + e^{-jw} A1(e^{j2w}) ) / 2

where each branch is a cascade of first-order all-passes
(a + z^-1)/(1 + a z^-1) running at half the half-band's output rate. The
coefficients are not quoted from anywhere: they are the result of the
minimax search below, and `--verify` is what says the numbers in the C are
the numbers this script produces.

The stopband edge is 0.5833*pi because of the *last* decimation stage of a
48 kHz chain: it runs at 96 kHz, folds everything above 24 kHz down, and has
to be clear of it by 20 kHz -- so the transition band is 20..28 kHz and the
passband reaches 0.4167*pi. Earlier stages of a 4x or 8x chain have far more
room and reuse the same four numbers rather than carrying a second table.

numpy only; it is a desktop design tool, never imported by the runtime.
"""

import argparse
import math

import numpy as np

#: What src/shared/audiodsp_shaper.c ships, branch 0 then branch 1.
SHIPPED = ((0.0903576217, 0.5794053552), (0.3127017166, 0.8516878519))

PASSBAND_EDGE = 0.4167
STOPBAND_EDGE = 0.5833


def response(branch0, branch1, w):
    """|H| and phase of the two-path half-band at angular frequencies w."""
    z = np.exp(-2j * w)
    a0 = np.ones_like(z)
    for a in branch0:
        a0 = a0 * (a + z) / (1.0 + a * z)
    a1 = np.ones_like(z)
    for a in branch1:
        a1 = a1 * (a + z) / (1.0 + a * z)
    return 0.5 * (a0 + np.exp(-1j * w) * a1)


def verify(branch0=SHIPPED[0], branch1=SHIPPED[1]):
    w = np.linspace(1e-6, math.pi - 1e-6, 200001)
    magnitude = np.abs(response(branch0, branch1, w))
    passband = magnitude[w <= PASSBAND_EDGE * math.pi]
    stopband = magnitude[w >= STOPBAND_EDGE * math.pi]
    quarter = magnitude[np.argmin(abs(w - math.pi / 2))]
    print("branch 0        %s" % (", ".join("%.10f" % a for a in branch0),))
    print("branch 1        %s" % (", ".join("%.10f" % a for a in branch1),))
    print("passband ripple %+.5f / %+.5f dB (to %.4f pi)"
          % (20 * math.log10(passband.max()), 20 * math.log10(passband.min()),
             PASSBAND_EDGE))
    print("stopband peak   %.2f dB (from %.4f pi)"
          % (20 * math.log10(stopband.max()), STOPBAND_EDGE))
    print("gain at pi/2    %.4f (the half-band condition wants 0.7071)"
          % quarter)
    for a in tuple(branch0) + tuple(branch1):
        if not 0.0 < a < 1.0:
            raise SystemExit("coefficient %r is outside (0, 1): unstable" % a)
    print("all poles at |z| = sqrt(a) < 1")


def _nelder_mead(cost, start, step, iterations):
    n = len(start)
    points = [np.array(start, float)]
    for i in range(n):
        moved = np.array(start, float)
        moved[i] += step
        points.append(moved)
    points = np.array(points)
    values = np.array([cost(p) for p in points])
    for _ in range(iterations):
        order = np.argsort(values)
        points, values = points[order], values[order]
        if abs(values[-1] - values[0]) < 1e-14:
            break
        centre = points[:-1].mean(axis=0)
        reflected = centre + (centre - points[-1])
        value = cost(reflected)
        if value < values[0]:
            expanded = centre + 2.0 * (centre - points[-1])
            expanded_value = cost(expanded)
            if expanded_value < value:
                points[-1], values[-1] = expanded, expanded_value
            else:
                points[-1], values[-1] = reflected, value
        elif value < values[-2]:
            points[-1], values[-1] = reflected, value
        else:
            contracted = centre + 0.5 * (points[-1] - centre)
            contracted_value = cost(contracted)
            if contracted_value < values[-1]:
                points[-1], values[-1] = contracted, contracted_value
            else:
                points[1:] = points[0] + 0.5 * (points[1:] - points[0])
                values[1:] = [cost(p) for p in points[1:]]
    order = np.argsort(values)
    return points[order][0], values[order][0]


def design(sections=2, trials=40, seed=0):
    """Minimax search for `sections` all-passes in each of the two branches."""
    grid = np.linspace(STOPBAND_EDGE * math.pi, math.pi, 4001)
    generator = np.random.default_rng(seed)

    def cost(parameters):
        coefficients = np.tanh(parameters) ** 2          # R -> (0, 1)
        return np.max(np.abs(response(coefficients[:sections],
                                      coefficients[sections:], grid)))

    best = (None, 1e9)
    for _ in range(trials):
        start = np.arctanh(np.sqrt(
            np.sort(generator.uniform(0.05, 0.95, sections * 2))))
        for step in (0.5, 0.2, 0.08, 0.03):
            start, value = _nelder_mead(cost, start, step, 4000)
        if value < best[1]:
            best = (start, value)
    coefficients = np.tanh(best[0]) ** 2
    branch0 = sorted(coefficients[:sections])
    branch1 = sorted(coefficients[sections:])
    print("stopband peak   %.2f dB" % (20 * math.log10(best[1])))
    print("branch 0        %s" % ", ".join("%.10ff," % a for a in branch0))
    print("branch 1        %s" % ", ".join("%.10ff," % a for a in branch1))
    return branch0, branch1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--design", action="store_true",
                        help="run the search again instead of checking")
    parser.add_argument("--sections", type=int, default=2,
                        help="all-passes per branch (default 2)")
    parser.add_argument("--verify", action="store_true",
                        help="check the coefficients audiodsp_shaper.c ships")
    arguments = parser.parse_args()
    if arguments.design:
        design(sections=arguments.sections)
    else:
        verify()


if __name__ == "__main__":
    main()
