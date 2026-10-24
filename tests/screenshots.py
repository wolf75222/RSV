#!/usr/bin/env python3
"""Makes the README's GIF and screenshots, from the fake Slurm (made-up jobs, user "demo").

    python3 tests/screenshots.py                      # with ./build/rsv, here
    python3 tests/screenshots.py --ssh romeo --dir '~/RSV'   # on ROMEO (built there), drawn here

Needs pyte and Pillow where it runs (pip install pyte pillow), only the standard library on the remote side.
Writes img/rsv.gif, img/history.png, img/log.png, img/cluster.png, img/nodes.png, img/node.png and img/hardware.png.
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
              "squeue -h -O JobID >/dev/null; {grow} $BIN --refresh 5; rm -rf {state}").format(
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
    p.add_argument("--rows", type=int, default=40)
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
    shot(2.0, "j")                  # 1 node, 2 GPUs, an error in red in its last lines
    shot(2.0, "j")                  # 4 CPU nodes
    shot(3.0, "j", 1.0)             # an array of 200 tasks on one row, its progress
    shot(2.6, "enter", 0.8)         # its tasks
    shot(1.6, "left,j", 0.8)        # folded again; a job waiting for another one
    shot(1.0, "g", 0.5)
    shot(1.2, "l", 1.0)             # log, followed
    for _ in range(3):
        shot(0.6, None, 0.6)
    shot(1.2, "q,j,l", 1.0)         # another log: its error in red
    shot(1.6, "e", 0.6)             # e: the next error
    shot(1.8, "/,l,o,s,s,enter", 0.6)   # / search, n next
    log_png = t.snapshot()
    shot(2.4, "q,b", 1.2)           # the script it was submitted with
    shot(1.2, "q,a", 2.0)           # history: what each job used, CPU and GPU hours, what was wasted
    shot(2.8, "j,j,j", 0.6)
    history_png = t.snapshot()
    t.send("q")
    t.sleep(0.3)
    t.send("p")
    t.sleep(26)                     # a few readings for the curves (--refresh 5)
    shot(3.0, None, 0.2)            # cluster: overview
    cluster_png = t.snapshot()
    shot(2.6, "2", 0.8)             # queue
    shot(3.0, "3", 0.8)             # nodes: why down, the map
    nodes_png = t.snapshot()
    shot(2.6, "down,down,down,right,right,enter", 1.5)   # zoom on a node: its jobs
    zoom_png = t.snapshot()
    shot(3.0, "esc,4", 1.5)         # the hardware
    hardware_png = t.snapshot()
    shot(2.6, "q,o", 2.0)           # the project: colleagues' jobs, read-only
    shot(1.0, "o", 1.5)
    shot(2.4, " , ,c", 0.8)         # two jobs marked, c lists them
    shot(1.0, "esc,esc", 0.6)
    shot(2.6, "?", 0.8)             # help
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
    render_png(cluster_png, os.path.join(args.out, "cluster.png"))
    render_png(nodes_png, os.path.join(args.out, "nodes.png"))
    render_png(zoom_png, os.path.join(args.out, "node.png"))
    render_png(hardware_png, os.path.join(args.out, "hardware.png"))
    for name in ("rsv.gif", "history.png", "log.png", "cluster.png", "nodes.png", "node.png", "hardware.png"):
        path = os.path.join(args.out, name)
        print("%s: %d KB" % (path, os.path.getsize(path) // 1024))


if __name__ == "__main__":
    main()
