# Serving the FileHost over TFTP

For whoever runs `files.mega65.org`. This is the server half; the format the
machine reads is fixed by `docs/FILEHOST.md` §2 in the ether65 repository, which
is not public at the time of writing — the parts of it a server has to honour
are stated here rather than pointed at.

What the catalogue is, in one paragraph: a 128-byte header — `M65FHCAT`, a
version byte, the record size and count as 16-bit little-endian, a 32-bit
generation time — followed by fixed-width records of that size. Each record is
a 40-byte space-padded title, a 16-byte author, a 48-byte NUL-padded path, a
kind byte (0 = prg, 1 = d81), a 32-bit size, and from version 2 a category byte
and a 16-bit year. Text is printable ASCII, records are sorted by title, and
anything that is not a `.prg` or `.d81` is left out.

Written for what that host actually runs, which its own headers give as
`Apache/2.4.67 (Debian)` with `PHP/8.1.33`. Nothing here needs the web server
changed: the catalogue is rendered from the same JSON the site already serves,
and the files are the ones already on disk.

## Why TFTP at all

The MEGA65 has 384KB of memory frozen under it and no room for a TCP stack. TFTP
is a request and a stream of 512-byte blocks over UDP, and the whole client is
about a kilobyte of 6502. That is the only reason: it is not chosen for speed
and not for safety, but because it fits.

Two consequences shape everything below. TFTP has no authentication, so
**anything under the served root is public** — and it is a UDP protocol that
answers a small packet with a large one, so an open server is an amplifier
somebody else can point at a third party.

## 1. The daemon

```sh
apt install tftpd-hpa
```

`/etc/default/tftpd-hpa`:

```sh
TFTP_USERNAME="tftp"
TFTP_DIRECTORY="/srv/tftp"
TFTP_ADDRESS=":69"
TFTP_OPTIONS="--secure --blocksize 512 --timeout 5 --retransmit 1000000"
```

What each of those is for:

- `--secure` chroots to `TFTP_DIRECTORY` before serving, so a request for
  `../../etc/passwd` cannot leave the tree. Not optional.
- **No `--create`.** The daemon must not accept writes; the machine only ever
  reads. This is the default, and stating it in the options is how it stays that
  way through a later edit.
- `--blocksize 512` pins the block size. The client does not negotiate `blksize`
  and expects 512 bytes, which is also one SD sector — that equality is what
  lets an image be written to the card a block at a time with no buffering.
- `--timeout 5` and `--retransmit 1000000` are the server's patience: one second
  between resends, five before it abandons a transfer. Leave them long enough
  that a machine writing a sector to its SD card is not given up on mid-image.

Then:

```sh
systemctl enable --now tftpd-hpa
```

## 2. The directory, and why it is not the web root

Pointing `TFTP_DIRECTORY` at `/var/www/files.mega65.org` works and serves the
whole of it — every file the site has, catalogued or not, with no authentication
in front. Serve a tree that holds only what the catalogue names, filled with
**hardlinks**: no copying, no second disk, and paths identical to the HTTP ones,
which is what lets the catalogue carry one path for both.

The renderer in `FILEHOST.md` §3 builds that tree and writes `catalog` beside
it. Two things about it worth keeping if you rewrite it:

- It writes `catalog.tmp` and renames. A transfer that starts while the file is
  being written must not see half of it, and rename is atomic where a write is
  not.
- It skips anything that is not `.prg` or `.d81`, so `.cor` core files are never
  exposed. A browser that can flash a core is a browser that can brick a board.

## 3. Keeping it current

A systemd timer rather than cron, so the run is logged where everything else is:

`/etc/systemd/system/filehost-catalog.service`

```ini
[Unit]
Description=Render the FileHost catalogue and hardlink tree for TFTP
After=network-online.target

[Service]
Type=oneshot
User=www-data
ExecStart=/usr/bin/php /usr/local/lib/filehost-catalog.php
```

`/etc/systemd/system/filehost-catalog.timer`

```ini
[Unit]
Description=Render the FileHost catalogue every fifteen minutes

[Timer]
OnBootSec=2min
OnUnitActiveSec=15min
Persistent=true

[Install]
WantedBy=timers.target
```

```sh
systemctl enable --now filehost-catalog.timer
systemctl list-timers filehost-catalog.timer
```

Fifteen minutes is a choice about staleness, not about load: a catalogue that
has not caught up names a file the machine then fails to fetch, which costs a
failed transfer and not a wrong one.

## 4. Rate limiting, and what it is for

**Amplification, not disclosure.** The files are public and meant to be; the
risk is that a 30-byte request produces a stream of 512-byte replies, and a
forged source address points that stream at somebody else. Bound what one
address can start:

`/etc/nftables.conf`:

```
table inet filter {
    set tftp_flood {
        type ipv4_addr
        flags dynamic, timeout
        timeout 1m
    }

    chain input {
        type filter hook input priority filter; policy drop;

        ct state established,related accept
        iif lo accept

        # A machine starts a handful of transfers a session, never dozens.
        udp dport 69 add @tftp_flood { ip saddr limit rate 6/minute } accept
        udp dport 69 drop
    }
}
```

```sh
nft -c -f /etc/nftables.conf && systemctl reload nftables
```

Only the request port needs this. A transfer moves to an ephemeral port after
the first packet, and `ct state established,related` already covers it — which
is also why the rule counts *transfers started* rather than packets.

If the host serves a LAN only, bind to it instead and the question does not
arise: `TFTP_ADDRESS="192.168.1.1:69"`.

## 5. Checking it works

From another machine, not the server — `--secure` and the firewall are both
things that only look right from outside:

```sh
# The catalogue: 128-byte header, "M65FHCAT", then the records.
tftp files.mega65.org -c get catalog
head -c 8 catalog                  # M65FHCAT
stat -c %s catalog                 # 128 + 128 * records

# A file the catalogue names, and the same file over HTTP, byte for byte.
tftp files.mega65.org -c get files/b/bordercolorbars_Zu8DOI.prg
curl -s -o web.prg https://files.mega65.org/files/b/bordercolorbars_Zu8DOI.prg
cmp bordercolorbars_Zu8DOI.prg web.prg
```

The `cmp` is the check that matters. Equal lengths prove nothing: the failure
this whole path is built around is a file of the right length with the wrong
bytes in the tail.

To read the header without a MEGA65:

```sh
python3 - <<'PY'
import struct
raw = open("catalog", "rb").read()
magic, version, stride, count, generated = struct.unpack_from("<8sBHHI", raw, 0)
print(magic, "version", version, "stride", stride, "records", count)
PY
```

`version` should be 2. Version 1 is still readable by a current client, but it
carries no category and no year.

## 6. When a MEGA65 cannot fetch

The tool says which of these it is; work down the list.

| What the machine says | Where to look |
|---|---|
| `NO ADDRESS: NOTHING ANSWERED ON THE NETWORK` | Not your server. The machine got no DHCP lease — its own cable, or the LAN's DHCP. |
| `NO TFTP SERVER: PUT ONE IN TFTP-IP.TXT ON THE CARD` | Not your server either. The machine has no address to ask. |
| `THE TFTP SERVER DID NOT ANSWER` | The machine has an address and got no ARP reply. Wrong address, or a firewall dropping before nftables logs it. |
| `THE SERVER REFUSED IT, CODE 1` | The daemon answered and has no such file: the hardlink tree is stale, or the renderer skipped that record. Re-run the timer's service by hand. |
| `THE SERVER REFUSED IT, CODE 2` | Permissions under `/srv/tftp`, or `--secure` and a path that leaves the tree. |
| `THE TRANSFER STOPPED PART WAY` | A transfer began and died. The interesting one — see below. |
| `THAT IS NOT A CATALOGUE` | The `catalog` file is not what the client expects: a half-written file, or a version this client does not know. Check the header with the snippet above. |

A transfer that stops part way is the one worth investigating, because the
client cannot tell a slow server from a lost packet. On the server:

```sh
journalctl -u tftpd-hpa --since -10min
nft list set inet filter tftp_flood     # is the client rate-limited?
```

A client that vanishes mid-transfer and a server that gave up look identical in
the log. If it recurs for one machine and not others, suspect the path rather
than the daemon: TFTP has no congestion control, and a link that reorders or
drops under load will end transfers that a TCP download would survive.

## What this does not do

- **No authentication and no encryption.** Everything under `/srv/tftp` is
  world-readable to anyone who can reach port 69. That is the design; keep the
  tree to what is meant to be public.
- **No integrity check.** The catalogue carries a length and no digest, so a
  file that arrives wrong is only found by comparing it against the server. If
  that becomes worth fixing, it is a format change and belongs in
  `FILEHOST.md`, not here.
