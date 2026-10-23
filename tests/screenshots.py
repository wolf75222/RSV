#!/usr/bin/env python3
"""Makes the README's GIF and screenshots, from the fake Slurm (made-up jobs, user "demo").

    python3 tests/screenshots.py                      # with ./build/rsv, here
    python3 tests/screenshots.py --ssh romeo --dir '~/RSV'   # on ROMEO (built there), drawn here

Needs pyte and Pillow where it runs (pip install pyte pillow), only the standard library on the remote side.
Writes img/rsv.gif, img/history.png and img/log.png.
"""

import argparse
import os
import shlex
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tui import Term, render_png, save_gif  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def command(args):
    """The shell command that starts RSV on the fake Slurm, with a log that grows (for the follow mode)"""
    root = args.dir if args.ssh else REPO
    state = "$S"   # set first: USER becomes "demo" below
    log = state + "/home/runs/lbm/lbm-812345.out"
    grow = ("(i=60; while [ $i -lt 400 ] && [ -d %s ]; do sleep 0.4; i=$((i+1)); "
            "printf 'step %%6d  t=%%.4e  residual=%%.3e  MLUPS=%%7.1f\\n' $((i*200)) "
            "$(awk \"BEGIN{print $i*2.5e-5, 1/($i+1)^1.5, 2400+($i*37)%%90}\") >> %s; done) >/dev/null 2>&1 &"
            % (state, log))
    # ROOT and BIN first: HOME changes below, and ~ with it
    script = ("S=/tmp/rsv-screenshots-$(id -un); ROOT={root}; BIN={binary}; rm -rf {state}; "
              "export PATH=$ROOT/tests/fake-slurm/bin:$PATH USER=demo FAKE_SLURM_STATE={state} "
              "FAKE_SLURM_HOME={state}/home HOME={state}/home RSV_COLOR=truecolor; "
              "squeue -h -O JobID >/dev/null; {grow} $BIN; rm -rf {state}").format(
        state=state, root=root, grow=grow, binary=args.binary or (root + "/build/rsv"))
    if args.ssh:
        return ["ssh", "-q", "-tt", args.ssh, "bash -c " + shlex.quote(script)]
    return ["bash", "-c", script]


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--ssh", help="host to run RSV on (its fake Slurm), drawn here")
    p.add_argument("--dir", default="~/RSV", help="the RSV folder on that host")
    p.add_argument("--binary", help="rsv to run (default: <dir>/build/rsv)")
    p.add_argument("--out", default=os.path.join(REPO, "img"))
    p.add_argument("--cols", type=int, default=140)
    p.add_argument("--rows", type=int, default=36)
    args = p.parse_args()

    t = Term(command(args), cols=args.cols, rows=args.rows)
    frames = []

    def shot(seconds, keys=None, settle=0.8):
        if keys:
            for k in keys.split(","):
                t.send(k)
                t.sleep(0.15)
        t.sleep(settle)
        frames.append((t.snapshot(), int(seconds * 1000)))

    if not t.wait_for("lbm-d3q19", 20):
        print(t.text())
        sys.exit("RSV didn't start")
    t.sleep(2.5)
    shot(3.0)                       # main view, 2 GPU nodes
    shot(1.8, "j")                  # 1 node, 2 GPUs
    shot(2.4, "j")                  # 4 CPU nodes
    shot(2.6, "j,j,j", 1.2)         # pending array: reason, expected start
    shot(1.6, "j")                  # dependency
    shot(1.0, "g", 0.5)
    shot(1.2, "l", 1.0)             # log, followed
    for _ in range(4):
        shot(0.6, None, 0.6)
    log_png = t.snapshot()
    shot(1.0, "q,a", 2.0)           # history with efficiency
    shot(2.6, "j,j,j", 0.6)
    history_png = t.snapshot()
    shot(2.6, "q,p", 1.5)           # partitions
    shot(2.4, "q,u", 1.5)           # limits
    shot(2.6, "q,?", 0.8)           # help
    shot(1.5, "q,/,s,w,e,e,p", 0.8)  # filter
    t.send("\x1b")
    t.sleep(0.3)
    t.send("q")
    t.wait_exit(5)
    t.close()

    os.makedirs(args.out, exist_ok=True)
    images = [render_png(cells) for cells, _ in frames]
    save_gif(images, os.path.join(args.out, "rsv.gif"), [d for _, d in frames])
    render_png(history_png, os.path.join(args.out, "history.png"))
    render_png(log_png, os.path.join(args.out, "log.png"))
    for name in ("rsv.gif", "history.png", "log.png"):
        path = os.path.join(args.out, name)
        print("%s: %d KB" % (path, os.path.getsize(path) // 1024))


if __name__ == "__main__":
    main()
