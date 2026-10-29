#!/usr/bin/env python3
# BT SPP reader for the phase2-bt debug build → appends timestamped lines to tracker.log
# Usage: BT_MAC=AA:BB:CC:DD:EE:FF tools/bt_listen.py   (pair the board first)
import socket, time, datetime, os, sys

MAC = os.environ.get('BT_MAC') or sys.exit('set BT_MAC to the board\'s Bluetooth address')
CH = 1  # SPP channel
LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tracker.log')
IDLE_S = 90  # GPS wait is silent for up to 60 s; silence beyond = reset / out of range

def out(f, msg):
    line = f"{datetime.datetime.now():%H:%M:%S} [BT] {msg}"
    print(line, flush=True)
    f.write(line + '\n')

with open(LOG, 'a', buffering=1) as f:
    while True:
        s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM)
        try:
            s.settimeout(10)
            s.connect((MAC, CH))
            out(f, '=== BT connected ===')
            s.settimeout(IDLE_S)
            buf = b''
            while True:
                d = s.recv(1024)
                if not d:
                    raise ConnectionError('closed')
                buf += d
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    t = line.decode(errors='replace').rstrip('\r')
                    if t:
                        out(f, t)
        except (OSError, ConnectionError) as e:
            out(f, f'--- disconnected ({e.__class__.__name__}), retrying ---')
        finally:
            s.close()
        time.sleep(3)
