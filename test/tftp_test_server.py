#!/usr/bin/env python3
"""A TFTP server that misbehaves on purpose, for the failure paths hardware hides.

The machine's fetch has failure handling no honest network will exercise: a
transfer that stops dead, one that pauses and comes back, a server that repeats
a block, one slow enough that a person can reach RUN/STOP mid-transfer.  Waiting
for a real network to do those things is not testing, it is hoping -- the stall
that produced this script took a dozen 800KB fetches to show itself twice.

Nothing here is a dependency: plain sockets, no tftpy, no pip, so it runs
wherever python does.  It is also not a ctest test, deliberately -- it needs a
MEGA65 pointed at it, which a test suite cannot arrange.  megatalk-tftpd is what
serves the real catalogue; this stands in for it when the point is to break
something.

    # a normal server, to check the rig works at all
    python3 test/tftp_test_server.py --root /tmp/tftproot

    # stop dead after 200 blocks: the machine must give up, not hang
    python3 test/tftp_test_server.py --root /tmp/tftproot --stall-after 200

    # pause eight seconds mid-transfer and resume: it must survive
    python3 test/tftp_test_server.py --root /tmp/tftproot --pause-after 200 --pause 8

    # send every 50th block twice: Sorcerer's Apprentice regression
    python3 test/tftp_test_server.py --root /tmp/tftproot --repeat-every 50

    # a block every 40ms, slow enough to press RUN/STOP during it
    python3 test/tftp_test_server.py --root /tmp/tftproot --slow 40

Point the machine at it with FILEHOST's T key -- 192.168.1.2:6970, say -- so
nothing on the card has to change.  Serve a tree laid out as the catalogue names
it, `files/b/NAME.D81` and a `catalog` beside it; the simplest way to get one is
to copy megatalk-tftpd's cache directory.
"""

import argparse
import os
import socket
import struct
import sys
import time

# RFC 1350 §5.  Only the three a read needs are ever built here.
RRQ, DATA, ACK, ERROR, OACK = 1, 3, 4, 5, 6
BLOCK = 512
# RFC 1350 §4: "file not found" and "access violation" are the two a reader meets.
ERR_NOT_FOUND, ERR_ACCESS = 1, 2


def _string(data, at):
    """A NUL-terminated field, and where the next one starts, or None where the
    request ran out before its terminator -- a malformed request must not take
    the server down with it."""
    end = data.find(b"\0", at)
    if end < 0:
        return None, at
    return data[at:end].decode("latin-1"), end + 1


class Faults:
    """What this server does wrong, and when."""

    def __init__(self, args):
        self.stall_after = args.stall_after
        self.pause_after = args.pause_after
        self.pause = args.pause
        self.repeat_every = args.repeat_every
        self.drop_every = args.drop_every
        self.slow = args.slow / 1000.0
        self.paused_yet = False

    def before(self, block):
        """Called with the block about to be sent.  False means send nothing
        ever again -- the transfer stops dead and the client is on its own."""
        if self.stall_after and block > self.stall_after:
            return False
        if self.pause_after and block == self.pause_after and not self.paused_yet:
            self.paused_yet = True
            print(f"    pausing {self.pause}s at block {block}", flush=True)
            time.sleep(self.pause)
        if self.slow:
            time.sleep(self.slow)
        return True

    def drop(self, block):
        return self.drop_every and block % self.drop_every == 0

    def repeat(self, block):
        return self.repeat_every and block % self.repeat_every == 0


def serve_one(sock, peer, path, root, faults):
    """One read request, block by block, lock-step as RFC 1350 has it."""
    whole = os.path.join(root, path)
    if not os.path.isfile(whole) or not os.path.abspath(whole).startswith(os.path.abspath(root)):
        sock.sendto(struct.pack(">HH", ERROR, ERR_NOT_FOUND) + b"not found\0", peer)
        print(f"  refused {path}", flush=True)
        return

    data = open(whole, "rb").read()
    print(f"  sending {path}, {len(data)} bytes", flush=True)
    block = 1
    at = 0
    sent = 0
    while True:
        chunk = data[at : at + BLOCK]
        if not faults.before(block):
            print(f"    stalling for good at block {block}", flush=True)
            return
        packet = struct.pack(">HH", DATA, block & 0xFFFF) + chunk
        if not faults.drop(block):
            sock.sendto(packet, peer)
            sent += 1
        else:
            print(f"    dropped block {block}", flush=True)
        if faults.repeat(block):
            sock.sendto(packet, peer)
            sent += 1

        # Wait for the acknowledgement, resending on a timeout as a server must.
        acked = False
        for _ in range(5):
            try:
                sock.settimeout(1.0)
                reply, who = sock.recvfrom(1024)
            except socket.timeout:
                sock.sendto(packet, peer)
                continue
            if who != peer or len(reply) < 4:
                continue
            opcode, number = struct.unpack_from(">HH", reply)
            if opcode == ERROR:
                print("    client refused the transfer", flush=True)
                return
            if opcode == ACK and number == (block & 0xFFFF):
                acked = True
                break
        if not acked:
            print(f"    no acknowledgement of block {block}; giving up", flush=True)
            return

        if len(chunk) < BLOCK:
            print(f"    done, {sent} datagrams for {block} blocks", flush=True)
            return
        at += BLOCK
        block += 1


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--root", required=True, help="the tree to serve")
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=6970)
    parser.add_argument("--stall-after", type=int, default=0,
                        help="send this many blocks and then nothing, for ever")
    parser.add_argument("--pause-after", type=int, default=0, help="pause once, at this block")
    parser.add_argument("--pause", type=float, default=8.0, help="seconds to pause for")
    parser.add_argument("--repeat-every", type=int, default=0, help="send every Nth block twice")
    parser.add_argument("--drop-every", type=int, default=0, help="do not send every Nth block")
    parser.add_argument("--slow", type=float, default=0.0, help="milliseconds between blocks")
    args = parser.parse_args()

    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listener.bind((args.bind, args.port))
    print(f"serving {args.root} on {args.bind}:{args.port}", flush=True)
    while True:
        request, peer = listener.recvfrom(1024)
        if len(request) < 2 or struct.unpack_from(">H", request)[0] != RRQ:
            continue
        path, at = _string(request, 2)
        mode, at = _string(request, at)
        if path is None or mode is None:
            print(f"{peer[0]}:{peer[1]} sent a malformed request", flush=True)
            continue
        print(f"{peer[0]}:{peer[1]} asks for {path!r} ({mode})", flush=True)

        # A port of its own for the transfer, as RFC 1350 §4 requires, so the
        # listener stays free -- and so a client that answers the wrong port is
        # caught here rather than on the machine.
        transfer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        transfer.bind((args.bind, 0))
        try:
            serve_one(transfer, peer, path, args.root, Faults(args))
        finally:
            transfer.close()


if __name__ == "__main__":
    sys.exit(main())
