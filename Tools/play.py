#!/usr/bin/env python3
"""
play.py — drive a live PIE session from the shell, and WAIT IN THE SHELL.

    python play.py "W 2.0 | LOOK 90 0 | LMB tap | WAIT 0.5 | E tap"
    python play.py --status
    python play.py --actions                 # every InputAction + whether it is actually BOUND
    python play.py --keys gamepad            # FKey names matching a pattern
    python play.py --shot Saved/shots/a.png  # capture, works with the editor MINIMISED
    python play.py --stop                    # abort a running sequence, release every key

WHY THIS EXISTS
---------------
A bridge call OWNS the game thread for its whole duration, so `time.sleep()` inside one blocks the
tick: nothing moves while you wait, and a multi-step script written that way collapses onto a single
starved frame. UFablePlay::RunSequence therefore runs the script on the game-thread ticker and
returns at once, and the waiting has to happen HERE, between calls. That is the entire contract of
this file, and it is the mistake that has cost this lane the most time.

☠ NOTHING HERE TOUCHES THE MACHINE. No SetCursorPos, no SendKeys, no SetForegroundWindow. Every verb
lands inside the editor process at APlayerController::InputKey or FSlateApplication.

SEQUENCE GRAMMAR (steps separated by '|', case-insensitive)
    W 2.0            hold W for 2 s          W / W TAP        tap
    W DOWN / W UP    press / release         WAIT 0.5         idle
    LOOK 90 0        mouse delta dx dy       SCROLL -2        wheel
    AXIS Gamepad_LeftX -1.0 1.5              CLICK 640 360    (CLICK R x y, DCLICK x y)
    MOVE 640 360     pointer move            TYPE hello       type into keyboard focus
    CALL player Func Args                    SAY marker       note in the transcript
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uexec  # noqa: E402  — same transport, same Remote Control endpoint


class Bridge:
    def __init__(self, host, port, timeout):
        self.host, self.port, self.timeout = host, port, timeout

    def py(self, code):
        """Run one statement in the editor and return whatever it printed, as text."""
        resp = uexec.run_python(self.host, self.port, code, self.timeout)
        lines, errs = [], []
        for entry in resp.get("LogOutput", []) or []:
            (errs if entry.get("Type") == "Error" else lines).append(entry.get("Output", "").rstrip("\n"))
        if errs:
            raise RuntimeError("\n".join(errs))
        return "\n".join(lines).strip()

    def call(self, expr):
        """Call one unreal.FablePlay.* expression and parse its JSON reply."""
        raw = self.py("import unreal\nprint(unreal.FablePlay.%s)" % expr)
        try:
            return json.loads(raw)
        except ValueError:
            return {"ok": False, "error": "unparsed reply", "raw": raw}


def lit(s):
    """A python string literal safe to paste into the code we send."""
    return json.dumps(s)


def run_sequence(bridge, script, poll=0.25, timeout=120.0):
    """Start the script, then wait HERE — one poll per bridge call, sleeping between."""
    started = bridge.call("run_sequence(%s)" % lit(script))
    if not started.get("ok"):
        print("could not start: %s" % started.get("error", started), file=sys.stderr)
        return 1
    print("running %d step(s)" % started.get("steps", 0))

    deadline = time.time() + timeout
    while True:
        # ☠ SLEEP FIRST, AND SLEEP HERE. Polling in a tight loop starves the very ticker that is
        # running the script, and sleeping inside the editor would stop the game outright.
        time.sleep(poll)
        state = bridge.call("poll_sequence()")
        for line in state.get("transcript", []):
            print("  %6.2fs  %s" % (line.get("t", 0.0), line.get("step", "")))
        if not state.get("running"):
            print("finished at step %d/%d" % (state.get("step", 0), state.get("total", 0)))
            return 0
        if time.time() > deadline:
            print("timed out after %.0fs — stopping" % timeout, file=sys.stderr)
            print(json.dumps(bridge.call("stop_sequence()")))
            return 1


def main():
    ap = argparse.ArgumentParser(description="Drive a live PIE session; wait in the shell.")
    ap.add_argument("script", nargs="?", help="sequence script, steps separated by '|'")
    ap.add_argument("--status", action="store_true", help="is PIE up, and what is in it")
    ap.add_argument("--actions", action="store_true", help="InputActions + BOUND/unbound + keys")
    ap.add_argument("--keys", metavar="PATTERN", help="FKey names matching PATTERN")
    ap.add_argument("--key-state", metavar="KEY", help="what the engine believes about one key")
    ap.add_argument("--action-state", metavar="PATH", help="what Enhanced Input believes about one action")
    ap.add_argument("--shot", metavar="PNG", help="capture to PNG (works with the editor minimised)")
    ap.add_argument("--no-ui", action="store_true", help="--shot: world only, skip the HUD composite")
    ap.add_argument("--stop", action="store_true", help="abort the running sequence, release keys")
    ap.add_argument("--release", action="store_true", help="release every key this harness holds")
    ap.add_argument("--poll", type=float, default=0.25, help="seconds between polls (default 0.25)")
    ap.add_argument("--timeout", type=float, default=120.0, help="give up after N seconds")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=30010)
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    bridge = Bridge(args.host, args.port, 120.0)
    show = lambda d: print(json.dumps(d, indent=1))

    try:
        if args.status:
            show(bridge.call("status()"))
        elif args.actions:
            reply = bridge.call("list_input_actions(0)")
            print("applied contexts: %s" % (reply.get("appliedContexts") or "NONE"))
            print("%d of %d actions are bound\n" % (reply.get("bound", 0), reply.get("count", 0)))
            for a in reply.get("actions", []):
                mark = "BOUND  " if a.get("bound") else "unbound"
                print("  %s %-28s %s" % (mark, a.get("name", ""), ",".join(a.get("keys") or [])))
        elif args.keys is not None:
            for k in bridge.call("list_keys(%s)" % lit(args.keys)).get("keys", []):
                flags = "".join(c for c, on in
                                (("A", k.get("analog")), ("G", k.get("gamepad")), ("M", k.get("mouse"))) if on)
                print("  %-30s %-28s %s" % (k.get("key", ""), k.get("display", ""), flags))
        elif args.key_state:
            show(bridge.call("key_state(%s)" % lit(args.key_state)))
        elif args.action_state:
            show(bridge.call("action_state(%s)" % lit(args.action_state)))
        elif args.shot:
            show(bridge.call("capture_screen(%s, 1280, 720, %s)"
                             % (lit(args.shot), "False" if args.no_ui else "True")))
        elif args.stop:
            show(bridge.call("stop_sequence()"))
        elif args.release:
            show(bridge.call("release_all_keys(True)"))
        elif args.script:
            return run_sequence(bridge, args.script, args.poll, args.timeout)
        else:
            ap.print_help()
            return 2
    except RuntimeError as e:
        print(e, file=sys.stderr)
        return 1
    except Exception as e:
        # A timeout here is usually CONTENTION, not a dead editor: another agent is holding the game
        # thread with a long job and the bridge answers one caller at a time. Check the newest
        # Saved/Logs/Deliria*.log — if it is still advancing, wait and retry rather than diagnosing.
        print("no answer from the editor at %s:%d (HTTP, then the RC WebSocket on %d): %s"
              % (args.host, args.port, args.port + 10, e), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
