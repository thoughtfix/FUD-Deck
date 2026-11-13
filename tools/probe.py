#!/usr/bin/env python3
"""Talk to the FUD-Deck probe firmware over serial.

  probe.py [--port P] [--log FILE] CMD [ARGS...]   send one command, stream until DONE
  probe.py [--port P] listen [SECS]                just print what the board says
  probe.py [--port P] shot out.png                 capture the screen as PNG
  probe.py ports                                   list candidate ports
  probe.py send FIFO LOG CMD...                    send one line through a running serve, print
                                                   the output until "DONE" (or 10 s)
  probe.py shotlog FIFO LOG out.png                take a screenshot through a running serve
  probe.py [--port P] serve LOG FIFO               hold the port open (no reboots), append all
                                                   output to LOG, send each line written to FIFO

Port defaults to the first usbmodem (native USB) or SLAB/usbserial (CP2102) port.
Answers to guided questions (y/n/s/q) are read from this terminal while it runs.
Needs pyserial (PlatformIO's Python has it: ~/.platformio/penv/bin/python).
"""
import glob
import os
import select
import struct
import sys
import time
import zlib

import serial

TIMEOUTS = {"auto": 90, "guided": 600, "watch": 400, "shot": 60}


def ports():
    pats = ["/dev/cu.usbmodem*", "/dev/cu.SLAB_USBtoUART*", "/dev/cu.usbserial*", "/dev/ttyACM*", "/dev/ttyUSB*"]
    out = []
    for p in pats:
        out += sorted(glob.glob(p))
    return out


def open_port(port):
    # Set the control lines before opening, or the open itself resets the board
    # (native USB-JTAG and the CP2102 auto-reset circuit both react to DTR/RTS).
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.05
    s.dtr = False
    s.rts = False
    s.open()
    return s


def wait_ready(s, log, timeout=60):
    """macOS pulses DTR on open, which reboots the board. If it rebooted, wait for READY."""
    buf = b""
    end = time.time() + 1.0
    while time.time() < end:
        buf += s.read(4096)
    if b"ESP-ROM" not in buf and b"[probe] FUD PROBE" not in buf:
        return
    print("(board rebooted on port open, waiting for boot tests)", file=sys.stderr)
    end = time.time() + timeout
    while time.time() < end:
        if b"[probe] READY" in buf:
            if log:
                log.write(buf.decode(errors="replace"))
            return
        buf += s.read(4096)


def stream(s, cmd, timeout, log):
    wait_ready(s, log)
    s.write((cmd + "\n").encode())
    s.flush()
    verb = cmd.split()[0]
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        buf += s.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode(errors="replace").rstrip("\r")
            print(text, flush=True)
            if log:
                log.write(text + "\n")
            if text.startswith("[probe] DONE " + verb):
                return 0
        if sys.stdin.isatty() and select.select([sys.stdin], [], [], 0)[0]:
            ans = sys.stdin.readline().strip()
            if ans:
                s.write((ans + "\n").encode())
    print(f"(timeout after {timeout}s)", file=sys.stderr)
    return 1


def rgb332(v):
    r, g, b = (v >> 5) & 7, (v >> 2) & 7, v & 3
    return (r * 255 // 7, g * 255 // 7, b * 255 // 3)


def shot(s, path):
    s.reset_input_buffer()
    s.write(b"shot\n")
    buf = b""
    end = time.time() + TIMEOUTS["shot"]
    while time.time() < end:
        buf += s.read(65536)
        i = buf.find(b"[shot] BEGIN")
        if i >= 0 and b"[shot] END" in buf[i:]:
            break
    else:
        raise SystemExit("no [shot] reply")
    body = buf[i:buf.index(b"[shot] END", i)].decode(errors="replace").splitlines()
    w, h = (int(v) for v in body[0].split()[2:4])
    rows = {}
    for line in body[1:]:
        j, k = line.find("R"), line.find(":")
        if j >= 0 and k > j and line[j + 1:k].isdigit() and len(line[k + 1:]) >= 2 * w:
            rows[int(line[j + 1:k])] = line[k + 1:k + 1 + 2 * w]
    missing = [y for y in range(h) if y not in rows]
    if missing:
        raise SystemExit(f"short capture: {len(missing)} of {h} rows missing")
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        r = rows[y]
        for x in range(w):
            raw.extend(rgb332(int(r[2 * x:2 * x + 2], 16)))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
    print(f"wrote {path} ({w}x{h})")


def serve(s, logpath, fifo):
    """Keep the port open so the board is not rebooted by every check."""
    if not os.path.exists(fifo):
        os.mkfifo(fifo)
    fd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
    pending = b""
    with open(logpath, "a", buffering=1) as log:
        log.write(f"\n=== serve {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        while True:
            try:
                data = s.read(4096)
            except (serial.SerialException, OSError):
                # Board unplugged or reset off the bus. Wait for it and reopen.
                log.write(f"\n=== port lost {time.strftime('%H:%M:%S')}, waiting\n")
                try:
                    s.close()
                except Exception:
                    pass
                while not os.path.exists(s.port):
                    time.sleep(0.5)
                time.sleep(1.0)
                s = open_port(s.port)
                log.write(f"=== port back {time.strftime('%H:%M:%S')}\n")
                continue
            if data:
                log.write(data.decode(errors="replace"))
            r, _, _ = select.select([fd], [], [], 0)
            if r:
                chunk = os.read(fd, 1024)
                if not chunk:  # writer closed: reopen so the next writer works
                    os.close(fd)
                    fd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
                pending += chunk
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    if line.strip():
                        log.write(f"\n>>> {line.decode(errors='replace')}\n")
                        s.write(line + b"\n")


def send_via(fifo, logpath, cmd, timeout=10, quiet=False):
    """Send a command through serve and return the log text it produced."""
    start = os.path.getsize(logpath)
    with open(fifo, "w") as f:
        f.write(cmd + "\n")
    verb = cmd.split()[0]
    end = time.time() + timeout
    text = ""
    while time.time() < end:
        with open(logpath, "r", errors="replace") as f:
            f.seek(start)
            text = f.read()
        if f"DONE {verb}" in text:
            break
        time.sleep(0.1)
    if not quiet:
        print(text.strip())
    return text


def shot_from_text(text, path):
    i = text.rfind("[shot] BEGIN")
    if i < 0 or "[shot] END" not in text[i:]:
        raise SystemExit("no complete [shot] block")
    body = text[i:text.index("[shot] END", i)].splitlines()
    w, h = (int(v) for v in body[0].split()[2:4])
    rows = {}
    for line in body[1:]:
        j, k = line.find("R"), line.find(":")
        if j >= 0 and k > j and line[j + 1:k].isdigit() and len(line[k + 1:]) >= 2 * w:
            rows[int(line[j + 1:k])] = line[k + 1:k + 1 + 2 * w]
    missing = [y for y in range(h) if y not in rows]
    if missing:
        raise SystemExit(f"short capture: {len(missing)} of {h} rows missing")
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        r = rows[y]
        for x in range(w):
            raw.extend(rgb332(int(r[2 * x:2 * x + 2], 16)))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
    print(f"wrote {path} ({w}x{h})")


def main():
    args = sys.argv[1:]
    if args and args[0] == "send":
        send_via(args[1], args[2], " ".join(args[3:]))
        return 0
    if args and args[0] == "shotlog":
        text = send_via(args[1], args[2], "shot", timeout=60, quiet=True)
        shot_from_text(text, args[3])
        return 0
    port, logpath = None, None
    while args and args[0].startswith("--"):
        flag = args.pop(0)
        if flag == "--port":
            port = args.pop(0)
        elif flag == "--log":
            logpath = args.pop(0)
        else:
            raise SystemExit(__doc__)
    if not args:
        raise SystemExit(__doc__)
    if args[0] == "ports":
        print("\n".join(ports()) or "no ports")
        return 0
    port = port or (ports() or [None])[0]
    if not port:
        raise SystemExit("no serial port found")
    s = open_port(port)
    log = open(logpath, "a") if logpath else None
    if log:
        log.write(f"\n=== {time.strftime('%Y-%m-%d %H:%M:%S')} {port} {' '.join(args)}\n")
    try:
        if args[0] == "shot":
            shot(s, args[1] if len(args) > 1 else "probe-shot.png")
            return 0
        if args[0] == "serve":
            serve(s, args[1], args[2])
            return 0
        if args[0] == "listen":
            secs = float(args[1]) if len(args) > 1 else 10
            end = time.time() + secs
            while time.time() < end:
                data = s.read(4096)
                if data:
                    text = data.decode(errors="replace")
                    sys.stdout.write(text)
                    sys.stdout.flush()
                    if log:
                        log.write(text)
            return 0
        cmd = " ".join(args)
        return stream(s, cmd, TIMEOUTS.get(args[0], 30), log)
    finally:
        if log:
            log.close()
        s.close()


if __name__ == "__main__":
    sys.exit(main())
