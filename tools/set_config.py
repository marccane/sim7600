#!/usr/bin/env python3
# Show or change a device's remote config: the retained devices/<id>/config
# document the firmware reads on every wake (and live while in emergency mode).
#
#   tools/set_config.py                                  show the current document
#   tools/set_config.py mode=emergency emergency_interval_s=30
#   tools/set_config.py --wait mode=normal               ...and wait for the device's ack
#
# Merges the given keys into the current document, bumps "v" and publishes it
# retained. The device answers on devices/<id>/ack when it applies (or rejects)
# a version; a sleeping device does so at its next wake.
# Config via env (or tools/secrets.env when run from the repo):
#   MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS, DEVICE_ID
import socket, struct, json, sys, os, time

HERE = os.path.dirname(os.path.abspath(__file__))
try:  # repo-local secrets; env vars win
    for l in open(os.path.join(HERE, 'secrets.env')):
        k, _, v = l.strip().partition('=')
        if k and not k.startswith('#'):
            os.environ.setdefault(k, v)
except FileNotFoundError:
    pass

HOST = os.environ.get('MQTT_HOST', 'localhost')
PORT = int(os.environ.get('MQTT_PORT', '1883'))
USER, PASS = os.environ.get('MQTT_USER'), os.environ.get('MQTT_PASS', '')
DEVICE = os.environ.get('DEVICE_ID', 'bike-01')
KEYS = {'report_interval_s', 'gps_timeout_s', 'mode', 'emergency_interval_s'}

def enc_str(s):
    b = s.encode(); return struct.pack('!H', len(b)) + b

def packet(ptype, body):
    n, rem = len(body), b''
    while True:
        byte, n = n % 128, n // 128
        rem += bytes([byte | (0x80 if n else 0)])
        if not n: break
    return bytes([ptype]) + rem + body

def read_packet(s):
    h = s.recv(1)
    if not h: raise ConnectionError('closed')
    mult, n = 1, 0
    while True:
        b = s.recv(1)[0]; n += (b & 0x7f) * mult; mult *= 128
        if not b & 0x80: break
    body = b''
    while len(body) < n:
        c = s.recv(n - len(body))
        if not c: raise ConnectionError('closed')
        body += c
    return h[0], body

def connect():
    s = socket.create_connection((HOST, PORT), timeout=15)
    flags, creds = 0x02, b''                                 # clean session
    if USER:
        flags |= 0xC0; creds = enc_str(USER) + enc_str(PASS)
    s.sendall(packet(0x10, enc_str('MQTT') + b'\x04' + bytes([flags])
                     + struct.pack('!H', 60) + enc_str(f'set-config-{os.getpid()}') + creds))
    _, ack = read_packet(s)
    if ack[1] != 0:
        raise ConnectionError(f'CONNACK refused, code {ack[1]}')
    return s

def subscribe(s, topic, pid):
    s.sendall(packet(0x82, struct.pack('!H', pid) + enc_str(topic) + b'\x00'))

def next_publish(s, timeout):
    """(topic, payload) of the next PUBLISH (QoS 0), or None on timeout."""
    end = time.time() + timeout
    while (left := end - time.time()) > 0:
        s.settimeout(left)
        try:
            t, body = read_packet(s)
        except socket.timeout:
            return None
        if t >> 4 == 3:
            tl = struct.unpack('!H', body[:2])[0]
            return body[2:2 + tl].decode(), body[2 + tl:].decode(errors='replace')
    return None

def main():
    args = sys.argv[1:]
    wait = '--wait' in args
    args = [a for a in args if a != '--wait']
    changes = {}
    for a in args:
        k, sep, v = a.partition('=')
        if not sep or k not in KEYS:
            sys.exit(f'usage: {sys.argv[0]} [--wait] [key=value ...]  keys: {", ".join(sorted(KEYS))}')
        changes[k] = int(v) if v.isdigit() else v

    cfg_topic, ack_topic = f'devices/{DEVICE}/config', f'devices/{DEVICE}/ack'
    s = connect()
    subscribe(s, cfg_topic, 1)
    current = {}
    while (m := next_publish(s, 3)) is not None:              # the retained document, if any
        if m[0] == cfg_topic and m[1]:
            current = json.loads(m[1]); break
    if not changes:
        print(json.dumps(current) if current else f'no config set for {DEVICE}')
        return

    doc = {**current, **changes, 'v': current.get('v', 0) + 1}
    if wait:
        subscribe(s, ack_topic, 2)
    s.sendall(packet(0x31, enc_str(cfg_topic) + json.dumps(doc).encode()))   # PUBLISH, retain
    print(f'published {cfg_topic}: {json.dumps(doc)}')
    if wait:
        print('waiting for the device to ack (it reads the config when it wakes)...', flush=True)
        end = time.time() + 3600
        while True:
            if time.time() > end:
                sys.exit('no ack within an hour')
            m = next_publish(s, 30)
            if m is None:
                s.sendall(b'\xc0\x00')                       # PINGREQ keeps the session alive
            elif m[0] == ack_topic and json.loads(m[1]).get('v') == doc['v']:
                print(f'ack: {m[1]}')
                break
    s.sendall(b'\xe0\x00')                                   # DISCONNECT

if __name__ == '__main__':
    main()
