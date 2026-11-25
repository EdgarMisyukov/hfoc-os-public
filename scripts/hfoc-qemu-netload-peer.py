#!/usr/bin/env python3
"""ITEM-467: a QEMU socket-netdev peer that gives the guest an address and then
floods its receive path with frames.

The combined input proof needs the production receive path busy while the
operator types and moves the pointer, because the owner's hardware run showed
the keyboard losing characters under a stream of ARP requests.  QEMU's
``-netdev socket,listen=...`` accepts a plain TCP peer speaking the documented
framing (four-byte big-endian length, then the Ethernet frame).

The peer first acts as a minimal DHCP server - the socket backend has none of
its own, and without an address the production network stays offline and never
classifies a frame - then floods broadcast ARP requests for the leased address
at a fixed pace until the guest exits or the duration runs out.

The traffic is deliberately modest - a few hundred frames per second - so the
proof measures the machine's input latency under a real stream instead of
overwhelming a TCG guest for its own sake.
"""

import argparse
import select
import socket
import struct
import sys
import time

GUEST_MAC = bytes((0x52, 0x54, 0x00, 0x12, 0x34, 0x56))
PEER_MAC = bytes((0x02, 0x00, 0x00, 0x00, 0x00, 0x02))
BROADCAST = b"\xff" * 6
GUEST_IP = bytes((10, 0, 2, 15))
PEER_IP = bytes((10, 0, 2, 2))
DHCP_LEASE_SECONDS = 3600


def checksum(data: bytes) -> int:
    if len(data) % 2:
        data += b"\x00"
    total = 0
    for index in range(0, len(data), 2):
        total += (data[index] << 8) | data[index + 1]
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def build_dhcp_reply(message_type: int, xid: int) -> bytes:
    payload = bytearray(240)
    payload[0] = 2
    payload[1] = 1
    payload[2] = 6
    struct.pack_into("!I", payload, 4, xid)
    payload[10] = 0x80
    payload[11] = 0x00
    payload[16:20] = GUEST_IP
    payload[28:34] = GUEST_MAC
    struct.pack_into("!I", payload, 236, 0x63825363)
    payload += bytes((53, 1, message_type))
    payload += bytes((1, 4)) + bytes((255, 255, 255, 0))
    payload += bytes((3, 4)) + PEER_IP
    payload += bytes((6, 4)) + PEER_IP
    payload += bytes((54, 4)) + PEER_IP
    payload += bytes((51, 4)) + struct.pack("!I", DHCP_LEASE_SECONDS)
    payload += bytes((255,))
    if len(payload) < 300:
        payload += b"\x00" * (300 - len(payload))

    udp = bytearray()
    udp += struct.pack("!HHHH", 67, 68, 8 + len(payload), 0)
    udp += payload
    pseudo = PEER_IP + b"\xff\xff\xff\xff" + b"\x00\x11" + \
        struct.pack("!H", 8 + len(payload))
    udp[6:8] = struct.pack("!H", checksum(pseudo + bytes(udp)))

    ip = bytearray()
    ip += b"\x45\x00"
    ip += struct.pack("!H", 20 + len(udp))
    ip += b"\x22\x33"
    ip += b"\x00\x00"
    ip += b"\x40\x11"
    ip += b"\x00\x00"
    ip += PEER_IP
    ip += b"\xff\xff\xff\xff"
    ip[10:12] = struct.pack("!H", checksum(bytes(ip)))

    frame = bytearray()
    frame += BROADCAST + PEER_MAC + b"\x08\x00"
    frame += ip
    frame += udp
    if len(frame) < 60:
        frame += b"\x00" * (60 - len(frame))
    return bytes(frame)


def dhcp_request(frame: bytes):
    """Return (message type, xid) of a DHCP request from the guest, or None."""
    if len(frame) < 42 + 240 or frame[12:14] != b"\x08\x00":
        return None
    if frame[23] != 17 or frame[30:34] != b"\xff\xff\xff\xff":
        return None
    if frame[34:36] != struct.pack("!H", 68) or \
            frame[36:38] != struct.pack("!H", 67):
        return None
    payload = frame[42:]
    if payload[236:240] != b"\x63\x82\x53\x63":
        return None
    if payload[28:34] != GUEST_MAC:
        return None
    index = 240
    while index < len(payload):
        option = payload[index]
        if option == 255:
            break
        if option == 0:
            index += 1
            continue
        if index + 1 >= len(payload):
            break
        length = payload[index + 1]
        if option == 53 and length == 1 and index + 2 < len(payload):
            return payload[index + 2], struct.unpack_from("!I", payload, 4)[0]
        index += 2 + length
    return None


def build_arp_request() -> bytes:
    ethernet = BROADCAST + PEER_MAC + struct.pack(">H", 0x0806)
    arp = struct.pack(">HHBBH", 1, 0x0806, 6, 4, 1)
    arp += PEER_MAC + PEER_IP + b"\x00" * 6 + GUEST_IP
    frame = ethernet + arp
    if len(frame) < 60:
        frame += b"\x00" * (60 - len(frame))
    return frame


def drain(sock: socket.socket):
    """Read one length-prefixed frame if one is ready, else None."""
    header = b""
    while len(header) < 4:
        ready = select.select([sock], [], [], 0)[0]
        if not ready:
            return None
        chunk = sock.recv(4 - len(header))
        if not chunk:
            return None
        header += chunk
    length = struct.unpack("!I", header)[0]
    data = b""
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--duration", type=float, default=180.0)
    parser.add_argument("--interval-ms", type=float, default=4.0)
    parser.add_argument("--max-frames", type=int, default=20000)
    parser.add_argument("--connect-timeout", type=float, default=60.0)
    parser.add_argument("--dhcp-timeout", type=float, default=90.0)
    args = parser.parse_args()

    connect_deadline = time.monotonic() + args.connect_timeout
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        while True:
            try:
                sock.connect(("127.0.0.1", args.port))
                break
            except OSError:
                if time.monotonic() >= connect_deadline:
                    sys.stderr.write(
                        "netload peer: QEMU socket never accepted a "
                        "connection\n")
                    return 1
                time.sleep(0.1)

        leased = False
        frame = build_arp_request()
        prefix = struct.pack("!I", len(frame))
        start = time.monotonic()
        last_send = start - 1.0
        sent = 0
        interval = max(args.interval_ms, 0.0) / 1000.0
        dhcp_deadline = start + args.dhcp_timeout
        while time.monotonic() - start < args.duration:
            # Serve the guest's DHCP as it arrives, and keep the receive path
            # busy with ARP requests at the same time, so the flood overlaps the
            # typed stream whether or not the lease has completed yet.
            try:
                incoming = drain(sock)
            except OSError:
                incoming = None
                break
            if incoming is not None and not leased and \
                    time.monotonic() < dhcp_deadline:
                request = dhcp_request(incoming)
                if request is not None:
                    message_type, xid = request
                    if message_type == 1:
                        reply = build_dhcp_reply(2, xid)
                    elif message_type == 3:
                        reply = build_dhcp_reply(5, xid)
                    else:
                        reply = None
                    if reply is not None:
                        sock.sendall(struct.pack("!I", len(reply)) + reply)
                        if message_type == 3:
                            leased = True
                            sys.stderr.write("netload peer: DHCP leased\n")
            now = time.monotonic()
            if sent < args.max_frames and now - last_send >= interval:
                try:
                    sock.sendall(prefix + frame)
                except OSError:
                    break
                sent += 1
                last_send = now
        sys.stderr.write("netload peer: sent %d frames\n" % sent)
        return 0
    finally:
        sock.close()


if __name__ == "__main__":
    raise SystemExit(main())
