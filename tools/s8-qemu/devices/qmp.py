#!/usr/bin/env python3
"""Tiny QMP client for the running s8-qemu VM (stdlib only).

  qmp.py [-s SOCK] screendump OUT.ppm          console screenshot (works with -display none)
  qmp.py [-s SOCK] pmemsave ADDR SIZE OUT      guest-physical memory to a file (hex or decimal)
  qmp.py [-s SOCK] hmp "savevm name"           any human-monitor command
  qmp.py [-s SOCK] touch X Y [HOLD_MS]         tap at absolute pointer position (0..32767 each axis)
  qmp.py [-s SOCK] drag X0 Y0 X1 Y1            press at 0, move to 1, release
  qmp.py [-s SOCK] key QCODE [HOLD_MS]         press/release a key (p=power h=home pgup pgdn)
  qmp.py [-s SOCK] set PATH PROP VALUE         qom-set (e.g. set /machine/peripheral-anon/device[N] soc 50)
  qmp.py [-s SOCK] raw COMMAND [JSON-ARGS]     execute any QMP command, print the return value
Default socket: /root/work/s8-qemu/run/qmp.sock
"""
import json, os, socket, sys, time

SOCK = "/root/work/s8-qemu/run/qmp.sock"


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.f = self.s.makefile("rw", encoding="utf-8")
        json.loads(self.f.readline())  # greeting
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r:
                return r["return"]
            if "error" in r:
                raise SystemExit("QMP error: %s" % r["error"]["desc"])


def num(x):
    return int(x, 0)


def btn(q, down):
    q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])


def pos(q, x, y):
    q.cmd("input-send-event", events=[
        {"type": "abs", "data": {"axis": "x", "value": x}},
        {"type": "abs", "data": {"axis": "y", "value": y}}])


def main():
    global SOCK
    a = sys.argv[1:]
    if a[:1] == ["-s"]:
        SOCK, a = a[1], a[2:]
    if not a:
        sys.exit(__doc__)
    q = Qmp(SOCK)
    c = a[0]
    if c == "screendump":
        q.cmd("screendump", filename=os.path.abspath(a[1]))
    elif c == "pmemsave":
        q.cmd("pmemsave", val=num(a[1]), size=num(a[2]), filename=os.path.abspath(a[3]))
    elif c == "hmp":
        print(q.cmd("human-monitor-command", **{"command-line": a[1]}), end="")
    elif c == "touch":
        pos(q, num(a[1]), num(a[2])); btn(q, True)
        time.sleep(float(a[3]) / 1000 if len(a) > 3 else 0.15)
        btn(q, False)
    elif c == "drag":
        pos(q, num(a[1]), num(a[2])); btn(q, True); time.sleep(0.1)
        for i in range(1, 11):
            pos(q, num(a[1]) + (num(a[3]) - num(a[1])) * i // 10,
                num(a[2]) + (num(a[4]) - num(a[2])) * i // 10)
            time.sleep(0.03)
        btn(q, False)
    elif c == "key":
        k = {"type": "qcode", "data": a[1]}
        q.cmd("input-send-event", events=[{"type": "key", "data": {"down": True, "key": k}}])
        time.sleep(float(a[2]) / 1000 if len(a) > 2 else 0.15)
        q.cmd("input-send-event", events=[{"type": "key", "data": {"down": False, "key": k}}])
    elif c == "set":
        v = a[3]
        try:
            v = int(v, 0)
        except ValueError:
            pass
        q.cmd("qom-set", path=a[1], property=a[2], value=v)
    elif c == "raw":
        print(json.dumps(q.cmd(a[1], **(json.loads(a[2]) if len(a) > 2 else {})), indent=1))
    else:
        sys.exit(__doc__)


main()
