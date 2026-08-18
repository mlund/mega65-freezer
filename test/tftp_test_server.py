#!/usr/bin/env python3
"""A TFTP server that misbehaves on purpose, for the failure paths hardware hides.

The machine's fetch has failure handling no honest network will exercise: a
transfer that stops dead, one that pauses and comes back, a server that repeats
a block, one slow enough that a person can reach RUN/STOP mid-transfer.  Waiting
for a real network to do those things is not testing, it is hoping -- the stall
that produced this script took a dozen 800KB fetches to show itself twice.

It also behaves correctly on purpose where that is the point: --tsize and
--blksize grant the RFC 2347 options a real server would, so one server can
serve both halves of a comparison and only the thing under test differs.

Nothing here is a dependency: plain sockets, no tftpy, no pip, so it runs
wherever python does.  It is also not a ctest test, deliberately -- it needs a
MEGA65 pointed at it, which a test suite cannot arrange.  megatalk-tftpd is what
serves the real catalogue; this stands in for it when the point is to break
something.

    # a normal server, to check the rig works at all
    python3 test/tftp_test_server.py --root /tmp/tftproot

    # the same server either side of a block-size comparison: --tsize on both
    # runs so both negotiate, and --blksize on one so the block size is the
    # only thing that differs
    python3 test/tftp_test_server.py --root /tmp/tftproot --tsize
    python3 test/tftp_test_server.py --root /tmp/tftproot --tsize --blksize 1024

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
        self.tries = args.tries
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


def _options(request, at):
    """The name/value pairs an RRQ carries after its mode, RFC 2347.

    Names are matched case-insensitively because the RFC says so and a client
    is free to send `blkSize`.  A pair that runs off the end stops the walk
    rather than raising: a malformed request is one to ignore, not one to fall
    over."""
    asked = {}
    while True:
        name, at = _string(request, at)
        if name is None:
            return asked
        value, at = _string(request, at)
        if value is None:
            return asked
        asked[name.lower()] = value


def _grant(asked, size, args):
    """What of `asked` this server will honour.

    Nothing, unless a flag says otherwise.  RFC 2347 expresses "no options" by
    sending the first data block instead of an acknowledgement, so declining is
    a path worth keeping reachable -- and it is what this server did before it
    could grant anything, which every existing invocation still expects.

    Granting is therefore opt-in and orthogonal, which is also what a clean
    block-size comparison needs: `--tsize` on both runs makes both negotiate,
    and `--blksize` on one leaves the block size as the only difference."""
    granted = {}
    if args.tsize and "tsize" in asked:
        granted["tsize"] = str(size)
    if args.blksize and "blksize" in asked:
        try:
            wanted = int(asked["blksize"])
        except ValueError:
            return granted
        # RFC 2348: a server may answer with any size up to the one asked for.
        granted["blksize"] = str(min(wanted, args.blksize))
    return granted


def _await_ack(sock, peer, packet, want, tries_allowed, what):
    """Waits for the acknowledgement of `want`, resending `packet` on silence.

    The deadline is a wall clock, not a socket timeout, and only the
    acknowledgement being waited for renews it.  A stale one -- of the block
    before, say, which a client repeats on its own clock -- does not spend a
    try, because it is not the client falling silent; but it must not buy the
    client another second either.  With settimeout() alone it did: every
    datagram restarted the second, so a client repeating itself faster than that
    was never timed out and the server would neither resend nor give up.  A
    stall could hide behind its own chatter, which is the failure this server
    exists to observe.

    Both waits go through here -- the option handshake and every block -- so the
    reasoning above governs both rather than whichever one it was written above.
    """
    tries = 0
    deadline = time.monotonic() + 1.0
    while tries < tries_allowed:
        left = deadline - time.monotonic()
        if left <= 0:
            tries += 1
            print(f"    silence {tries}s at {what}", flush=True)
            sock.sendto(packet, peer)
            deadline = time.monotonic() + 1.0
            continue
        sock.settimeout(left)
        try:
            reply, who = sock.recvfrom(1024)
        except socket.timeout:
            continue
        if who != peer or len(reply) < 4:
            continue
        opcode, number = struct.unpack_from(">HH", reply)
        if opcode == ERROR:
            print(f"    client refused {what}", flush=True)
            return False
        if opcode == ACK and number == want:
            return True
    print(f"    no acknowledgement of {what}; giving up", flush=True)
    return False


def _agree(sock, peer, granted, tries_allowed):
    """Sends the option acknowledgement and waits for its block zero.

    Granting an option costs a round trip RFC 1350 has no notion of: the client
    answers the OACK with an acknowledgement of block 0, and only then does data
    begin."""
    packet = struct.pack(">H", OACK)
    for name, value in granted.items():
        packet += name.encode() + b"\0" + value.encode() + b"\0"
    sock.sendto(packet, peer)
    print(f"    granted {granted}", flush=True)
    return _await_ack(sock, peer, packet, 0, tries_allowed, "the options")


def serve_one(sock, peer, path, root, faults, asked, args):
    """One read request, block by block, lock-step as RFC 1350 has it."""
    whole = os.path.join(root, path)
    if not os.path.isfile(whole) or not os.path.abspath(whole).startswith(os.path.abspath(root)):
        sock.sendto(struct.pack(">HH", ERROR, ERR_NOT_FOUND) + b"not found\0", peer)
        print(f"  refused {path}", flush=True)
        return

    data = open(whole, "rb").read()
    granted = _grant(asked, len(data), args)
    block_bytes = int(granted.get("blksize", BLOCK))
    if granted and not _agree(sock, peer, granted, faults.tries):
        return
    print(f"  sending {path}, {len(data)} bytes, {block_bytes}-byte blocks", flush=True)
    began = time.monotonic()
    block = 1
    at = 0
    sent = 0
    while True:
        chunk = data[at : at + block_bytes]
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

        if not _await_ack(sock, peer, packet, block & 0xFFFF, faults.tries, f"block {block}"):
            return

        if len(chunk) < block_bytes:
            spent = time.monotonic() - began
            rate = len(data) / spent / 1024 if spent else 0
            print(f"    done, {sent} datagrams for {block} blocks"
                  f" in {spent:.1f}s ({rate:.0f} KB/s)", flush=True)
            return
        at += block_bytes
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
    parser.add_argument("--tries", type=int, default=5,
                        help="one-second silences to spend before abandoning a block.  Raise it "
                             "to tell a client that has stalled from one that has died")
    parser.add_argument("--tsize", action="store_true",
                        help="answer the tsize option with the file's length.  Off by default, "
                             "so a server that declines every option stays reachable")
    parser.add_argument("--blksize", type=int, default=0, metavar="N",
                        help="largest block size to grant, RFC 2348.  0 declines the option, "
                             "which is what a server that never heard of it looks like -- so "
                             "one server can serve both halves of a block-size comparison")
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
        asked = _options(request, at)
        print(f"{peer[0]}:{peer[1]} asks for {path!r} ({mode})", flush=True)

        # A port of its own for the transfer, as RFC 1350 §4 requires, so the
        # listener stays free -- and so a client that answers the wrong port is
        # caught here rather than on the machine.
        transfer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        transfer.bind((args.bind, 0))
        try:
            serve_one(transfer, peer, path, args.root, Faults(args), asked, args)
        finally:
            transfer.close()


if __name__ == "__main__":
    sys.exit(main())
