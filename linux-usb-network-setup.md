# SIM7600G-H as USB Network Adapter on Linux

Connect the board to the PC via the **USB-C port wired to the SIM7600 modem**
(not the ESP32 programming port, not the TTL-marked UART pins — those only
carry AT commands, never network traffic).

The modem enumerates as a SimTech USB composite device (`1e0e:9001`):

- `/dev/ttyUSB0` … `/dev/ttyUSB4` — DIAG / NMEA / **AT** / modem / audio
  (`option` driver). **`/dev/ttyUSB3` is the clean AT port** (115200 baud).
  `ttyUSB2` also answers AT but may show stale URCs from the ESP32's session.
- `/dev/cdc-wdm0` — QMI control channel (root-only: needs `sudo`).
- `wwp…` net interface (e.g. `wwp39s0f3u1u1i5`) — the data interface
  (`qmi_wwan` driver, 802.3 mode, `raw_ip = N`).

## Modem / network status (verified via AT on `/dev/ttyUSB3`)

| Check            | Result                                              |
|------------------|-----------------------------------------------------|
| Modem (`ATI`)    | SIMCOM_SIM7600G-H, rev SIM7600G_V2.0.2              |
| SIM (`AT+CPIN?`) | READY                                               |
| Signal (`AT+CSQ`)| 22 (~-69 dBm — good)                                |
| Operator         | simyo, LTE                                          |
| Registration     | registered, home network (`CREG: 0,1`, `CGREG: 0,1`)|
| System (`CPSI`)  | LTE Online, EUTRAN-BAND3                            |
| APN (`CGDCONT`)  | PDP ctx 1 = `orangeworld` (user `orange`, pass `orange`) |

## Setup — manual QMI (no extra packages needed)

Tools used (`qmicli`, `qmi-network`, `dhcpcd`) are preinstalled.
All commands need `sudo` (`/dev/cdc-wdm0` is root-only).

**1. Write the QMI config:**

```bash
sudo tee /etc/qmi-network.conf > /dev/null <<EOF
APN=orangeworld
APN_USER=orange
APN_PASS=orange
APN_AUTH=PAP
PROXY=no
EOF
```

**2. Start the data session:**

```bash
sudo qmi-network /dev/cdc-wdm0 start
```

**3. Bring the interface up and get an IP** (name may vary —
confirm with `ls /sys/class/net | grep wwp`):

```bash
sudo ip link set wwp39s0f3u1u1i5 up
sudo dhcpcd wwp39s0f3u1u1i5
```

Leave `/sys/class/net/<iface>/qmi/raw_ip` at `N` (802.3 mode).
Only if DHCP fails, check
`sudo qmicli -d /dev/cdc-wdm0 --wda-get-data-format` first.

**4. Verify:**

```bash
ip addr show wwp39s0f3u1u1i5
ping -I wwp39s0f3u1u1i5 8.8.8.8
curl --interface wwp39s0f3u1u1i5 ifconfig.me
```

The `-I` / `--interface` flags are required: the existing Ethernet
default route stays primary, so traffic only goes over LTE when
explicitly bound to the WWAN interface.

**To disconnect:**

```bash
sudo qmi-network /dev/cdc-wdm0 stop
```

## Setup — automatic via ModemManager (recommended long-term)

```bash
sudo pacman -S modemmanager
sudo systemctl enable --now ModemManager
```

NetworkManager then auto-detects the modem on plug-in; create the
connection once (`nmcli c add type gsm …`) and it autoconnects.

## Outcome (2026-10-12)

The QMI path above **did not work** on this firmware (V2.0.2): the data
session started (`qmi-network` CID ok, PDP active with IP) and DHCP got
no lease (expected — QMI modems don't serve DHCP), but static IP +
raw-ip framing also produced no RX: `ping -I wwp…` hung with TX only.

**Working alternative: RNDIS mode.** Switch the modem composition via
AT (AT port on the modem's USB-C, `/dev/ttyUSB3`):

```
AT+CUSBPIDSWITCH=9011,1,1   # 9011 = RNDIS (9001 = QMI), then AT+CRESET
```

The device re-enumerates as `1e0e:9011` and exposes a plain
Ethernet-style interface (e.g. `enp39s0f3u1u1`) that the modem serves
with DHCP/NAT (hands out 192.168.x.x, NATs to the LTE link). Bring it
up and let dhcpcd lease an address as with any USB NIC. Revert to QMI
composition with `AT+CUSBPIDSWITCH=9001,1,1` + reset.

