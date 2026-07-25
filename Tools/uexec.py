#!/usr/bin/env python3
"""
uexec.py — run Python inside the running Unreal editor from the command line.

Transport: Remote Control HTTP (stock plugin, localhost:30010) calling
UPythonScriptLibrary::ExecutePythonCommandEx. No third-party deps.

Usage:
  python uexec.py --ping                        # bridge + FableKit liveness
  python uexec.py -c "import fable; fable.info('/Game/X/BP_Y')"
  python uexec.py -c "print(unreal.FableBP.ping())"
  python uexec.py -f script.py                  # editor executes the file (same machine)
  python uexec.py -c "..." -o out.json          # tee log output to a file

Notes:
  - "import unreal" is available implicitly in the editor; "import fable" loads
    Content/Python/fable.py helpers.
  - Multi-line code is fine with -c (ExecuteFile mode runs it as a script).
  - Big dumps: prefer fable.dump(..., out=r"path.json") server-side, then read the file.
"""
import argparse
import json
import os
import sys
import urllib.request
import urllib.error

PYLIB_CDO = "/Script/PythonScriptPlugin.Default__PythonScriptLibrary"

# The plugin's Content/Python (holds fable.py). Injected onto the editor's
# sys.path before each run so `import fable` works even before the plugin's
# content mount registers it (or in projects where that never happens).
PLUGIN_PY = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Content", "Python"))


def ws_call(host, port, payload, timeout):
    """Remote Control WebSocket fallback (default port 30020). Same object/call
    semantics as the HTTP route; used when the HTTP listener lost its bind race
    (e.g. a previous editor instance still held 30010 during startup)."""
    import base64
    import os
    import socket
    import struct

    s = socket.create_connection((host, port), timeout=min(timeout, 30))
    s.settimeout(timeout)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET / HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n" % (host, port, key)).encode())
    hdr = b""
    while b"\r\n\r\n" not in hdr:
        chunk = s.recv(4096)
        if not chunk:
            raise ConnectionError("websocket handshake closed")
        hdr += chunk
    if b" 101 " not in hdr.split(b"\r\n", 1)[0]:
        raise ConnectionError("websocket handshake refused: %s" % hdr[:120])

    msg = json.dumps({"MessageName": "http", "Id": 1, "Parameters": {
        "Url": "/remote/object/call", "Verb": "PUT", "Body": payload}}).encode()
    mask = os.urandom(4)
    ln = len(msg)
    if ln < 126:
        frame = struct.pack("!BB", 0x81, 0x80 | ln)
    elif ln < 65536:
        frame = struct.pack("!BBH", 0x81, 0x80 | 126, ln)
    else:
        frame = struct.pack("!BBQ", 0x81, 0x80 | 127, ln)
    frame += mask + bytes(b ^ mask[i % 4] for i, b in enumerate(msg))
    s.sendall(frame)

    def recv_exact(n):
        buf = b""
        while len(buf) < n:
            c = s.recv(n - len(buf))
            if not c:
                raise ConnectionError("websocket closed mid-frame")
            buf += c
        return buf

    data = b""
    while True:
        b1, b2 = recv_exact(2)
        opcode = b1 & 0x0F
        ln = b2 & 0x7F
        if ln == 126:
            ln = struct.unpack("!H", recv_exact(2))[0]
        elif ln == 127:
            ln = struct.unpack("!Q", recv_exact(8))[0]
        pl = recv_exact(ln) if ln else b""
        if opcode == 0x9:  # ping -> pong
            s.sendall(struct.pack("!BB", 0x8A, 0x80) + os.urandom(4))
            continue
        if opcode in (0x1, 0x0):
            data += pl
            if b1 & 0x80:
                break
        elif opcode == 0x8:
            raise ConnectionError("websocket closed by server")
    s.close()
    resp = json.loads(data.decode("utf-8", errors="replace"))
    body = resp.get("ResponseBody", resp)
    if isinstance(body, str):
        body = json.loads(body)
    return body


def call_remote(host, port, payload, timeout):
    url = "http://%s:%d/remote/object/call" % (host, port)
    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=body, method="PUT",
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8", errors="replace"))
    except urllib.error.HTTPError:
        raise  # real HTTP response (4xx/5xx) — not a transport failure
    except (urllib.error.URLError, ConnectionError, OSError):
        # HTTP transport broken (refused/reset/half-bound listener) ->
        # try the RC WebSocket (port+10 by default)
        return ws_call(host, port + 10, payload, timeout)


def run_python(host, port, code, timeout):
    payload = {
        "objectPath": PYLIB_CDO,
        "functionName": "ExecutePythonCommandEx",
        "parameters": {
            "PythonCommand": code,
            "ExecutionMode": "ExecuteFile",
        },
    }
    return call_remote(host, port, payload, timeout)


def main():
    ap = argparse.ArgumentParser(description="Execute Python in the running Unreal editor.")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("-c", "--code", help="python code to run (multi-line ok)")
    g.add_argument("-f", "--file", help="path to a .py file; the editor reads it directly")
    g.add_argument("--ping", action="store_true", help="check bridge + FableKit plugin")
    ap.add_argument("-o", "--out", help="also write raw response JSON to this file")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=30010)
    ap.add_argument("--timeout", type=float, default=900.0,
                    help="seconds; first-time BP loads can be slow (default 900)")
    ap.add_argument("--raw", action="store_true", help="print raw response JSON only")
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    if args.ping:
        code = "import unreal; print(unreal.FableBP.ping())"
    elif args.file:
        code = args.file.replace("\\", "/")
    elif args.code:
        code = args.code
    else:
        ap.error("one of --ping, -c, -f is required")

    try:
        if os.path.isdir(PLUGIN_PY):
            boot = ("import sys\n_p = r'%s'\nif _p not in sys.path:\n"
                    "    sys.path.insert(0, _p)\n" % PLUGIN_PY.replace("\\", "/"))
            try:
                run_python(args.host, args.port, boot, min(args.timeout, 30.0))
            except Exception:
                pass  # non-fatal; explicit imports will surface the problem
        resp = run_python(args.host, args.port, code, args.timeout)
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", errors="replace")
        print("HTTP %d from Remote Control: %s" % (e.code, detail[:2000]), file=sys.stderr)
        return 2
    except (urllib.error.URLError, ConnectionError, TimeoutError) as e:
        print("Cannot reach the editor at %s:%d — is the editor running with the "
              "RemoteControl plugin? (console: WebControl.StartServer)  %s"
              % (args.host, args.port, e), file=sys.stderr)
        return 2

    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(resp, f, indent=1)

    if args.raw:
        print(json.dumps(resp, indent=1))
        return 0

    ok = bool(resp.get("ReturnValue", False))
    had_error_log = False
    for entry in resp.get("LogOutput", []) or []:
        etype = entry.get("Type", "Info")
        line = entry.get("Output", "").rstrip("\n")
        if etype == "Error":
            had_error_log = True
            print("[ERR] " + line, file=sys.stderr)
        elif etype == "Warning":
            print("[WRN] " + line)
        else:
            print(line)

    result = resp.get("CommandResult", "")
    if not ok and result and result not in ("None", "none"):
        print("---- CommandResult ----", file=sys.stderr)
        print(result, file=sys.stderr)

    return 0 if (ok and not had_error_log) else 1


if __name__ == "__main__":
    sys.exit(main())
