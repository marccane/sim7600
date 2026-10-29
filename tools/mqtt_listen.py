#!/usr/bin/env python3
# Minimal MQTT 3.1.1 subscriber (no paho) → appends to tracker.log
import socket, struct, time, datetime, os

HOST = os.environ.get('MQTT_HOST', 'broker.emqx.io')
PORT = int(os.environ.get('MQTT_PORT', '1883'))
TOPIC = os.environ.get('MQTT_TOPIC', 'devices/bike-01/#')
LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tracker.log')

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

def out(f, msg):
    line = f"{datetime.datetime.now():%H:%M:%S} [MQTT] {msg}"
    print(line, flush=True); f.write(line + '\n')

with open(LOG, 'a', buffering=1) as f:
    while True:
        try:
            s = socket.create_connection((HOST, PORT), timeout=15)
            cid = f'pc-listen-{os.getpid()}'
            s.sendall(packet(0x10, enc_str('MQTT') + b'\x04\x02' + struct.pack('!H', 60) + enc_str(cid)))
            read_packet(s)
            s.sendall(packet(0x82, struct.pack('!H', 1) + enc_str(TOPIC) + b'\x00'))
            read_packet(s)
            out(f, f'subscribed to {TOPIC} @ {HOST}')
            s.settimeout(30)
            while True:
                try:
                    t, body = read_packet(s)
                except socket.timeout:
                    s.sendall(b'\xc0\x00'); continue          # PINGREQ
                if t >> 4 == 3:                              # PUBLISH (QoS 0)
                    tl = struct.unpack('!H', body[:2])[0]
                    topic = body[2:2 + tl].decode(errors='replace')
                    out(f, f"{topic.rsplit('/', 1)[-1]}: " + body[2 + tl:].decode(errors='replace'))
        except (OSError, ConnectionError, IndexError) as e:
            out(f, f'--- disconnected ({e}), retrying ---')
            time.sleep(5)
