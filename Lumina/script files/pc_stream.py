#!/usr/bin/env python3
"""
pc_stream.py - send video/images to the ESP32 PC-monitor display (WiFi first, USB optional).

Needs: Python 3.8+, ffmpeg in PATH.  USB mode also needs:  pip install pyserial

  python pc_stream.py ping   --wifi 192.168.5.50
  python pc_stream.py make   movie.mp4 movie.mjv --size 480x272 --fps 20 --quality 5
        -> writes a full-frame MJV2 clip; copy it to the SD card folder /media
  python pc_stream.py send   movie.mjv --wifi 192.168.5.50 --sd        (save to SD + play)
  python pc_stream.py send   movie.mjv --wifi 192.168.5.50 --sd-only   (save only)
  python pc_stream.py play   movie.mp4 --wifi 192.168.5.50 --size 480x272 --fps 20
  python pc_stream.py play   screen    --wifi 192.168.5.50             (live desktop, Windows gdigrab)
  python pc_stream.py play   movie.mp4 --usb COM5                      (cable)
  python pc_stream.py list-ports
  python pc_stream.py sim    --port 8081                              (fake device for testing)

Wire protocol: 'P''M''U''X' | type | flags | u32 len (LE) | payload   (see main.cpp).
Replies are text lines starting with "@PMUX ".
"""
import argparse, os, socket, struct, subprocess, sys, threading, time

MAGIC = b"PMUX"


def pkt(t, flags=0, payload=b""):
    return MAGIC + bytes([ord(t), flags]) + struct.pack("<I", len(payload)) + payload


class Link:
    """Byte link to the device over TCP or USB serial, with "@PMUX" line replies."""

    def __init__(self, wifi=None, usb=None):
        self.buf = b""
        if wifi:
            host, _, port = wifi.partition(":")
            self.s = socket.create_connection((host, int(port or 8081)), timeout=5)
            self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.ser = None
            self.window = 2
        else:
            try:
                import serial
            except ImportError:
                sys.exit("USB mode needs pyserial:  pip install pyserial")
            self.ser = serial.Serial(usb, 2000000, timeout=0.05, write_timeout=10)
            self.s = None
            self.window = 1          # USB CDC RX buffer is small: one frame in flight

    def send(self, data):
        if self.s:
            self.s.sendall(data)
        else:
            self.ser.write(data)

    def _read(self, timeout):
        if self.s:
            self.s.settimeout(timeout)
            try:
                d = self.s.recv(4096)
            except socket.timeout:
                return b""
            if not d:
                raise ConnectionError("device closed the connection")
            return d
        end = time.time() + timeout
        while time.time() < end:
            d = self.ser.read(256)
            if d:
                return d
        return b""

    def line(self, timeout=5.0):
        """Next "@PMUX ..." reply line (text after the prefix), or None on timeout."""
        end = time.time() + timeout
        while True:
            while b"\n" in self.buf:
                ln, self.buf = self.buf.split(b"\n", 1)
                ln = ln.strip()
                if ln.startswith(b"@PMUX "):
                    return ln[6:].decode("ascii", "replace")
            left = end - time.time()
            if left <= 0:
                return None
            self.buf += self._read(min(left, 0.5))

    def close(self):
        try:
            (self.s or self.ser).close()
        except Exception:
            pass


def open_link(a):
    if a.wifi:
        return Link(wifi=a.wifi)
    if a.usb:
        return Link(usb=a.usb)
    sys.exit("give --wifi HOST[:8081] or --usb PORT")


def parse_size(s):
    w, h = s.lower().split("x")
    return int(w), int(h)


def ffmpeg_cmd(src, w, h, fps, q, realtime):
    cmd = ["ffmpeg", "-v", "error", "-nostdin"]
    if src == "screen":
        cmd += ["-f", "gdigrab", "-framerate", str(fps), "-i", "desktop"]
    else:
        if realtime:
            cmd += ["-re"]
        cmd += ["-i", src]
    vf = "fps=%d,scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2" % (fps, w, h, w, h)
    cmd += ["-an", "-vf", vf, "-c:v", "mjpeg", "-q:v", str(q), "-pix_fmt", "yuvj420p", "-f", "image2pipe", "-"]
    return cmd


def jpeg_frames(stream):
    """Yield complete JPEG images from a concatenated MJPEG byte stream."""
    buf = b""
    while True:
        d = stream.read(65536)
        if not d:
            break
        buf += d
        while True:
            a = buf.find(b"\xff\xd8")
            if a < 0:
                buf = b""
                break
            b = buf.find(b"\xff\xd9", a + 2)
            if b < 0:
                buf = buf[a:]
                break
            yield buf[a:b + 2]
            buf = buf[b + 2:]


def jpeg_size(j):
    i = 2
    while i + 9 < len(j):
        if j[i] != 0xFF:
            i += 1
            continue
        m = j[i + 1]
        if m in (0xC0, 0xC1, 0xC2):
            h, w = struct.unpack(">HH", j[i + 5:i + 9])
            return w, h
        i += 2 + struct.unpack(">H", j[i + 2:i + 4])[0]
    raise ValueError("bad JPEG")


def pack_mjv2(frames, w, h, delay_ms):
    out = [b"MJV2", struct.pack("<HHHH", w, h, delay_ms, 1), struct.pack("<I", len(frames))]
    for f in frames:
        out.append(struct.pack("<HHHHI", 0, 0, w, h, len(f)))
    out += frames
    return b"".join(out)


def cmd_make(a):
    w, h = parse_size(a.size)
    p = subprocess.Popen(ffmpeg_cmd(a.src, w, h, a.fps, a.quality, False), stdout=subprocess.PIPE)
    frames = []
    total = 0
    for j in jpeg_frames(p.stdout):
        frames.append(j)
        total += len(j)
        if len(frames) % 50 == 0:
            print("\r%d frames, %.1f MB" % (len(frames), total / 1048576), end="", flush=True)
        if a.max_sec and len(frames) >= a.max_sec * a.fps:
            p.kill()
            break
    p.wait()
    if not frames:
        sys.exit("ffmpeg produced no frames")
    data = pack_mjv2(frames, w, h, round(1000 / a.fps))
    with open(a.out, "wb") as f:
        f.write(data)
    print("\nwrote %s: %d frames %dx%d @%d fps, %.1f MB (avg %.1f KB/frame)" %
          (a.out, len(frames), w, h, a.fps, len(data) / 1048576, total / len(frames) / 1024))
    print("Copy it to the SD card folder /media  (or: send %s --wifi IP --sd-only)" % a.out)


def cmd_ping(a):
    L = open_link(a)
    L.send(pkt("P"))
    print(L.line() or "no reply")
    L.close()


def cmd_send(a):
    data = open(a.file, "rb").read()
    name = os.path.basename(a.file)
    flags = 4 if a.sd_only else (3 if a.sd else 1)
    payload = bytes([len(name.encode())]) + name.encode() + data
    L = open_link(a)
    t0 = time.time()
    L.send(pkt("M", flags, payload))
    r = L.line(timeout=max(30, len(data) / 100000))
    dt = time.time() - t0
    print("%s  (%.1f MB in %.1fs = %.0f KB/s)" % (r, len(data) / 1048576, dt, len(data) / 1024 / max(dt, .001)))
    L.close()
    return 0 if r and r.startswith("OK") else 1


def cmd_play(a):
    w, h = parse_size(a.size)
    L = open_link(a)
    L.send(pkt("B", 0, struct.pack("<HHH", w, h, a.fps)))
    r = L.line(6)
    print("device:", r)
    if not r or not r.startswith("OK"):
        sys.exit(1)
    inflight = 0
    sent = dropped = 0
    t_start = time.time()
    try:
        while True:                       # one pass; with --loop restart ffmpeg
            p = subprocess.Popen(ffmpeg_cmd(a.src, w, h, a.fps, a.quality, a.src != "screen"), stdout=subprocess.PIPE)
            for j in jpeg_frames(p.stdout):
                while inflight:           # collect acks that already arrived
                    ln = L.line(0 if inflight < L.window else 2.0)
                    if ln is None:
                        break
                    if ln.startswith("A"):
                        inflight -= 1
                    elif ln.startswith("ERR"):
                        print("device:", ln)
                if inflight >= L.window:  # device busy: drop this frame (keeps real-time pace)
                    dropped += 1
                    continue
                L.send(pkt("F", 0, struct.pack("<HHHH", 0, 0, w, h) + j))
                inflight += 1
                sent += 1
                if sent % 100 == 0:
                    el = time.time() - t_start
                    print("\r%d sent, %d dropped, %.1f fps" % (sent, dropped, sent / el), end="", flush=True)
            p.wait()
            if not a.loop or a.src == "screen":
                break
    except KeyboardInterrupt:
        pass
    finally:
        try:
            L.send(pkt("E"))
            L.line(2)
        except Exception:
            pass
        L.close()
    print("\nfinished: %d frames sent, %d dropped" % (sent, dropped))


def cmd_ports(a):
    try:
        from serial.tools import list_ports
    except ImportError:
        sys.exit("pip install pyserial")
    for p in list_ports.comports():
        print(p.device, "-", p.description)


def cmd_sim(a):
    """Fake device: accepts the protocol on TCP, answers like the firmware. For testing without hardware."""
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", a.port))
    srv.listen(1)
    print("sim listening on", a.port, flush=True)
    stats = {"F": 0, "M": 0, "bytes": 0}
    while True:
        c, _ = srv.accept()
        _sim_conn(c, a, stats)
        print("sim conn done:", stats, flush=True)


def _sim_conn(c, a, stats):

    def rd(n):
        b = b""
        while len(b) < n:
            d = c.recv(n - len(b))
            if not d:
                raise EOFError
            b += d
        return b

    def rep(s):
        c.sendall(b"@PMUX " + s.encode() + b"\n")
    try:
        while True:
            h = rd(10)
            assert h[:4] == MAGIC, "bad magic"
            t, fl = chr(h[4]), h[5]
            n = struct.unpack("<I", h[6:10])[0]
            pl = rd(n)
            if t == "P":
                rep("PONG v1 sd=1")
            elif t == "B":
                w, hh, fps = struct.unpack("<HHH", pl)
                rep("OK stream")
            elif t == "F":
                x, y, w, hh = struct.unpack("<HHHH", pl[:8])
                assert pl[8:10] == b"\xff\xd8" and pl[-2:] == b"\xff\xd9"
                stats["F"] += 1
                stats["bytes"] += len(pl) - 8
                if a.save_dir:
                    open(os.path.join(a.save_dir, "f%04d.jpg" % stats["F"]), "wb").write(pl[8:])
                rep("A")
            elif t == "E":
                rep("OK end")
                break
            elif t == "M":
                nl = pl[0]
                stats["M"] += 1
                if a.save_dir:
                    open(os.path.join(a.save_dir, "up_" + pl[1:1 + nl].decode()), "wb").write(pl[1 + nl:])
                rep("OK saved flags=%d" % fl)
    except EOFError:
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)

    def link(p):
        p.add_argument("--wifi", help="HOST[:8081]")
        p.add_argument("--usb", help="serial port, e.g. COM5 or /dev/ttyACM0")

    p = sp.add_parser("ping"); link(p); p.set_defaults(f=cmd_ping)
    p = sp.add_parser("list-ports"); p.set_defaults(f=cmd_ports)
    p = sp.add_parser("make"); p.add_argument("src"); p.add_argument("out")
    p.add_argument("--size", default="480x272"); p.add_argument("--fps", type=int, default=20)
    p.add_argument("--quality", type=int, default=5, help="ffmpeg mjpeg q:v 2(best)..15")
    p.add_argument("--max-sec", type=int, default=0); p.set_defaults(f=cmd_make)
    p = sp.add_parser("send"); p.add_argument("file"); link(p)
    p.add_argument("--sd", action="store_true", help="save to SD and play")
    p.add_argument("--sd-only", action="store_true"); p.set_defaults(f=cmd_send)
    p = sp.add_parser("play"); p.add_argument("src", help="file, URL, or 'screen'"); link(p)
    p.add_argument("--size", default="480x272"); p.add_argument("--fps", type=int, default=15)
    p.add_argument("--quality", type=int, default=6); p.add_argument("--loop", action="store_true")
    p.set_defaults(f=cmd_play)
    p = sp.add_parser("sim"); p.add_argument("--port", type=int, default=8081)
    p.add_argument("--save-dir"); p.set_defaults(f=cmd_sim)
    a = ap.parse_args()
    sys.exit(a.f(a) or 0)


if __name__ == "__main__":
    main()
