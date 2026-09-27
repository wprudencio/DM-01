#!/usr/bin/env python3
"""Capture screenshots from an ESP32 framebuffer over USB serial.

The sketch must call screenshotHandle(fb, WIDTH, HEIGHT) in loop()
(see Screenshot.h). This script sends 'S', reads the RGB565 frame and
writes a PNG. No Pillow required.

Examples:
    python3 screenshot.py                      # auto port, 3x scale
    python3 screenshot.py -p /dev/cu.usbmodem14501 -o neko.png
    python3 screenshot.py -n 8 -i 0.6          # animation frames
    python3 screenshot.py -w -i 1 -o shot.png  # continuous, no reset between shots
"""
import argparse
import glob
import os
import struct
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install -r requirements.txt")


class CaptureError(Exception):
    pass


def auto_port():
    cands = sorted(glob.glob('/dev/cu.usbmodem*') +
                   glob.glob('/dev/cu.wchusbserial*') +
                   glob.glob('/dev/ttyUSB*') +
                   glob.glob('/dev/ttyACM*'))
    if not cands:
        raise CaptureError('no serial port found — pass one with -p')
    return cands[0]


def open_serial(port, baud):
    ser = serial.Serial()
    ser.port = port if os.path.exists(port) else auto_port()
    ser.baudrate = baud
    ser.timeout = 0.2
    try:
        ser.dtr = ser.rts = False
    except (OSError, AttributeError):
        pass
    ser.open()
    return ser


def read_exact(ser, n, deadline):
    buf = bytearray()
    while len(buf) < n:
        if time.time() > deadline:
            raise CaptureError(f'timeout: got {len(buf)}/{n} bytes')
        try:
            chunk = ser.read(n - len(buf))
        except serial.SerialException as e:
            raise CaptureError(str(e))
        if chunk:
            buf += chunk
    return bytes(buf)


def capture(ser, timeout):
    deadline = time.time() + timeout
    try:
        ser.reset_input_buffer()
    except serial.SerialException as e:
        raise CaptureError(str(e))
    magic = b''
    next_ping = 0.0
    while magic != b'SHOT':
        now = time.time()
        if now > deadline:
            raise CaptureError('no SHOT response — is the screenshot hook flashed?')
        if now >= next_ping:          # keep asking; a reset may swallow the first 'S'
            try:
                ser.write(b'S')
                ser.flush()
            except serial.SerialException as e:
                raise CaptureError(str(e))
            next_ping = now + 0.5
        try:
            b = ser.read(1)
        except serial.SerialException as e:
            raise CaptureError(str(e))
        if b:
            magic = (magic + b)[-4:]
    w, h = struct.unpack('>HH', read_exact(ser, 4, deadline))
    px = read_exact(ser, w * h * 2, deadline)
    return w, h, px


def grab(ser, port, baud, settle, timeout, retries=5):
    """Capture one frame, reconnecting only when the USB link drops."""
    for attempt in range(1, retries + 2):
        try:
            if ser is None:
                ser = open_serial(port, baud)
                time.sleep(settle)    # let the board finish booting
            w, h, px = capture(ser, timeout)
            return ser, w, h, px
        except (CaptureError, serial.SerialException, OSError) as e:
            if ser is not None:
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
            if attempt > retries:
                raise
            print(f'usb drop, reconnecting ({attempt}/{retries}): {e}', file=sys.stderr)
            time.sleep(1.0)
    raise CaptureError('unreachable')


def to_rgb(w, h, px):
    out = bytearray(w * h * 3)
    for i in range(w * h):
        v = px[2 * i] | (px[2 * i + 1] << 8)          # little-endian RGB565
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        out[3 * i]     = (r << 3) | (r >> 2)
        out[3 * i + 1] = (g << 2) | (g >> 4)
        out[3 * i + 2] = (b << 3) | (b >> 2)
    return bytes(out)


def scale_rgb(w, h, rgb, s):
    if s <= 1:
        return w, h, rgb
    out = bytearray()
    for y in range(h):
        row = bytearray()
        for x in range(w):
            row += rgb[(y * w + x) * 3:(y * w + x) * 3 + 3] * s
        for _ in range(s):
            out += row
    return w * s, h * s, bytes(out)


def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))

    png = (b'\x89PNG\r\n\x1a\n' +
           chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(raw, 9)) +
           chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(png)


def save(args, i, w, h, px):
    rw, rh, rgb = scale_rgb(w, h, to_rgb(w, h, px), args.scale)
    out = args.out if (args.count == 1 and not args.watch) else numbered(args.out, i)
    write_png(out, rw, rh, rgb)
    print(f'{out}  {w}x{h} -> {rw}x{rh}')
    return out


def numbered(path, i):
    root, ext = os.path.splitext(path)
    return f'{root}_{i:03d}{ext}'


def main():
    ap = argparse.ArgumentParser(description='Capture ESP32 framebuffer screenshots.')
    ap.add_argument('-p', '--port', help='serial port (default: auto-detect)')
    ap.add_argument('-o', '--out', default='screenshot.png', help='output PNG')
    ap.add_argument('-n', '--count', type=int, default=1, help='number of frames')
    ap.add_argument('-i', '--interval', type=float, default=0.6, help='seconds between frames')
    ap.add_argument('-w', '--watch', action='store_true',
                    help='keep capturing until Ctrl-C (port stays open, no reset between shots)')
    ap.add_argument('-s', '--scale', type=int, default=3, help='integer upscale (default 3)')
    ap.add_argument('-b', '--baud', type=int, default=115200)
    ap.add_argument('-t', '--timeout', type=float, default=10.0, help='per-frame read timeout')
    ap.add_argument('--settle', type=float, default=1.5, help='wait after opening the port')
    args = ap.parse_args()

    port = args.port or auto_port()
    ser = None
    try:
        if args.watch:
            print('watching — every %.1fs, Ctrl-C to stop' % args.interval)
            i = 0
            try:
                while True:
                    ser, w, h, px = grab(ser, port, args.baud, args.settle, args.timeout)
                    save(args, i, w, h, px)
                    i += 1
                    time.sleep(args.interval)
            except KeyboardInterrupt:
                pass
        else:
            for i in range(args.count):
                ser, w, h, px = grab(ser, port, args.baud, args.settle, args.timeout)
                save(args, i, w, h, px)
                if i + 1 < args.count:
                    time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    finally:
        if ser is not None:
            ser.close()


if __name__ == '__main__':
    main()
