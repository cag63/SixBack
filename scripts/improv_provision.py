#!/usr/bin/env python3
"""Headless Improv-Serial Wi-Fi provisioning for SixBack (and any firmware that
uses the improv-wifi-busware library), with an optional reset to (re-)open the
boot-time Improv window.

Why --reset exists
------------------
The firmware only listens for Improv-Serial frames for a limited window after
boot (SixBack global rule: **30 s when Wi-Fi credentials are already stored,
120 s when none are**). That is by design — it lets anyone re-provision a
device (Web Flasher "CONNECT", or this script) without a factory reset, while
not keeping the serial radio busy forever. To re-provision a device that is
already connected, you must first reboot it so a fresh window opens — hence
--reset.

Reset methods (--reset)
-----------------------
  none     Send immediately; assumes the Improv window is already open
           (i.e. the device was just powered on / flashed).
  http     POST <reboot-url> over the network (e.g. http://<ip>/api/reboot),
           then wait for the device to re-enumerate. **Most reliable** for an
           already-running SixBack device. The HTTP request timing out is
           expected — the device calls ESP.restart() before flushing the
           response.
  rts      Pulse DTR/RTS on the serial line (RTS->EN). Works on boards with an
           external USB-UART bridge (classic ESP32 + CH340/CP2102, the S3 with
           CH343). **UNRELIABLE on native USB-Serial-JTAG (ESP32-C3/C6/C5):**
           RTS is not wired to EN there, so the pulse usually does NOT reset
           the chip, and repeated attempts can wedge the USB endpoint. Use
           'http' for those boards.
  esptool  Reset via esptool (--esptool-pkg <dir> --chip <esp32c5|...>). Uses
           the ROM reset path; works across board types but can occasionally
           wedge a native USB-JTAG endpoint (recover with a power-cycle).

Examples
--------
  # already-running device, reboot it over HTTP first (recommended):
  improv_provision.py --port /dev/serial/by-id/...-if00 \
      --ssid MyWifi --password secret \
      --reset http --reboot-url http://192.168.1.50/api/reboot

  # freshly powered/flashed device, window already open:
  improv_provision.py --port /dev/ttyACM7 --ssid MyWifi --password secret

Improv-Serial frame format (improv-wifi):
  "IMPROV" ver type len data[len] checksum '\n'
  checksum = sum(bytes[0 .. 8+len]) & 0xFF
  RPC command 0x01 = Send Wi-Fi Settings, data = [ssid_len, ssid, pw_len, pw]
  device->host: type 0x01 Current-State (0x03=Provisioning, 0x04=Provisioned),
                type 0x02 Error, type 0x04 RPC-Result (payload carries the URL).
"""
import argparse, fcntl, os, select, struct, sys, time, urllib.request

TIOCMSET = 0x5418
DTR = 0x002
RTS = 0x004


def resolve_port(p):
    return os.path.realpath(p) if os.path.islink(p) else p


def open_serial(dev, settle=0.0):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    fcntl.ioctl(fd, TIOCMSET, struct.pack('I', DTR | RTS))  # assert DTR (HWCDC TX) + RTS
    if settle:
        time.sleep(settle)
    return fd


def drain(fd):
    while True:
        r, _, _ = select.select([fd], [], [], 0.05)
        if not r:
            return
        try:
            os.read(fd, 4096)
        except OSError:
            return


def read_for(fd, secs, echo=True):
    end = time.time() + secs
    buf = b""
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if not r:
            continue
        try:
            d = os.read(fd, 4096)
        except OSError:
            break
        if d:
            buf += d
            if echo:
                sys.stdout.write(d.decode("utf-8", "replace"))
                sys.stdout.flush()
    return buf


def improv_wifi_frame(ssid, psk):
    data = bytes([len(ssid)]) + ssid + bytes([len(psk)]) + psk
    rpc = bytes([0x01, len(data)]) + data           # cmd 0x01 = Send Wi-Fi Settings
    pkt = b"IMPROV" + bytes([0x01, 0x03, len(rpc)]) + rpc   # ver 1, type 0x03 RPC
    return pkt + bytes([sum(pkt) & 0xFF, 0x0A])


def parse_frames(buf):
    """Return (provisioned: bool, url: str|None)."""
    provisioned, url = False, None
    i = 0
    while i < len(buf) - 9:
        if buf[i:i + 6] == b"IMPROV":
            typ, ln = buf[i + 7], buf[i + 8]
            payload = buf[i + 9:i + 9 + ln]
            if typ == 0x01 and ln >= 1 and payload[0] == 0x04:
                provisioned = True
            elif typ == 0x04:                        # RPC result: URL string(s)
                txt = payload.decode("utf-8", "ignore")
                j = txt.find("http")
                if j >= 0:
                    url = txt[j:].strip()
            i += 9 + ln
        else:
            i += 1
    return provisioned, url


def http_reboot(url, timeout):
    try:
        urllib.request.urlopen(url, data=b"", timeout=timeout)
    except Exception as e:
        # A timeout / reset connection is the normal case: the device restarts
        # before flushing the HTTP response.
        print(f"[reset] http POST {url} -> {type(e).__name__} (expected; device rebooting)")


def wait_for_port(dev_or_link, timeout):
    end = time.time() + timeout
    while time.time() < end:
        dev = resolve_port(dev_or_link)
        if os.path.exists(dev):
            try:
                fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                os.close(fd)
                return dev
            except OSError:
                pass
        time.sleep(0.3)
    return None


def esptool_reset(pkg, chip, port):
    import subprocess
    env = dict(os.environ, PYTHONPATH=pkg)
    subprocess.run([sys.executable, "-m", "esptool", "--chip", chip, "--port", port,
                    "--before", "default_reset", "--after", "hard_reset", "chip_id"],
                   env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=40)


def main():
    ap = argparse.ArgumentParser(description="Improv-Serial Wi-Fi provisioning with optional reset.")
    ap.add_argument("--port", required=True, help="serial device or /dev/serial/by-id symlink")
    ap.add_argument("--ssid", required=True)
    ap.add_argument("--password", default="")
    ap.add_argument("--reset", choices=["none", "http", "rts", "esptool"], default="none")
    ap.add_argument("--reboot-url", help="for --reset http, e.g. http://<ip>/api/reboot")
    ap.add_argument("--esptool-pkg", help="for --reset esptool: tool-esptoolpy dir with esp32c5 target")
    ap.add_argument("--chip", help="for --reset esptool, e.g. esp32c5")
    ap.add_argument("--settle", type=float, default=6.0, help="seconds to wait after (re)open before sending")
    ap.add_argument("--listen", type=float, default=45.0, help="seconds to listen for the result")
    args = ap.parse_args()

    dev = resolve_port(args.port)

    if args.reset == "http":
        if not args.reboot_url:
            sys.exit("--reset http needs --reboot-url")
        http_reboot(args.reboot_url, timeout=5)
        time.sleep(4.0)                              # let ESP.restart() + USB re-enumerate begin
        dev = wait_for_port(args.port, timeout=15) or dev
    elif args.reset == "esptool":
        if not (args.esptool_pkg and args.chip):
            sys.exit("--reset esptool needs --esptool-pkg and --chip")
        esptool_reset(args.esptool_pkg, args.chip, dev)
        time.sleep(2.0)
        dev = wait_for_port(args.port, timeout=15) or dev

    fd = open_serial(dev)
    try:
        if args.reset == "rts":                      # pulse RTS (EN) — UART-bridge boards only
            fcntl.ioctl(fd, TIOCMSET, struct.pack('I', RTS))
            time.sleep(0.25)
            fcntl.ioctl(fd, TIOCMSET, struct.pack('I', DTR))
        # settle: let the firmware boot far enough that Improv is listening
        read_for(fd, args.settle, echo=True)
        drain(fd)
        os.write(fd, improv_wifi_frame(args.ssid.encode(), args.password.encode()))
        print(f"\n>>> sent Improv Send-Wi-Fi-Settings (ssid={args.ssid})")
        buf = read_for(fd, args.listen, echo=True)
    finally:
        os.close(fd)

    provisioned, url = parse_frames(buf)
    print("\n--- result ---")
    print(f"provisioned: {provisioned}")
    if url:
        print(f"device url : {url}")
    sys.exit(0 if provisioned else 1)


if __name__ == "__main__":
    main()
