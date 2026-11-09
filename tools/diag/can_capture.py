#!/usr/bin/env python3
"""Record CAN frames with selected IDs as JSON lines until SIGTERM (evidence for ISO-TP multi-frame).

usage: can_capture.py <iface> <out.jsonl> <can_id> [<can_id>...]
"""
import json
import signal
import socket
import struct
import sys
import time

iface, out = sys.argv[1], sys.argv[2]
ids = [int(x, 0) for x in sys.argv[3:]]
stop = False


def on_term(*_):
    global stop
    stop = True


signal.signal(signal.SIGTERM, on_term)
signal.signal(signal.SIGINT, on_term)

s = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
s.setsockopt(socket.SOL_CAN_RAW, socket.CAN_RAW_FILTER, b"".join(struct.pack("=II", i, socket.CAN_SFF_MASK) for i in ids))
s.bind((iface,))
s.settimeout(0.2)
with open(out, "w", encoding="utf-8") as f:
    while not stop:
        try:
            frame = s.recv(16)
        except socket.timeout:
            continue
        except InterruptedError:
            continue
        can_id, dlc, data = struct.unpack("=IB3x8s", frame)
        f.write(json.dumps({"t": time.monotonic(), "id": can_id & socket.CAN_SFF_MASK, "data": data[:dlc].hex()}) + "\n")
        f.flush()
