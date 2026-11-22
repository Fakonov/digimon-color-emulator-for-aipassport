"""Per-second deltas of the device log: frames, instructions, hot-block calls.

Reading the cumulative counters as deltas is the only way to see what a slow
second was actually made of: interpreted instructions, collapsed wait-loop
passes, or just very few frames.
"""
import io
import re
import sys

PAT = re.compile(
    r"frames=(\d+) guest_fps=(\d+)\.(\d+).*?insns=(\d+) K .*?"
    r"interp=(\d+) hotins=(\d+) collapse=(\d+)")
COLLAPSE_PASS_INSN = 74


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "dbgtest/press_diag.log"
    text = io.open(path, encoding="utf-8", errors="replace").read()
    rows = []
    for line in text.splitlines():
        m = PAT.search(line)
        if m:
            f, fps_i, fps_f, ins, interp, hot, coll = m.groups()
            rows.append((int(f), int(fps_i), int(ins), int(interp), int(hot), int(coll)))
    prev = None
    for cur in rows:
        if prev:
            df = cur[0] - prev[0]
            di = (cur[2] - prev[2]) * 1000
            dh = cur[4] - prev[4]
            dc = cur[5] - prev[5]
            per_frame = di / df if df else 0
            print("d_frames=%3d fps=%3d interp=%8d hot=%7d coll=%6d "
                  "collapsed_insn~=%8d insn/frame=%8d"
                  % (df, cur[1], di, dh, dc, dc * COLLAPSE_PASS_INSN, int(per_frame)))
        prev = cur
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
