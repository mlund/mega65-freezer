<!-- Deploy a read-only TFTP tree that FILEHOST can fetch. -->

# Serving FILEHOST over TFTP

FILEHOST reads `catalog` and the `.prg` and `.d81` files it names. TFTP has no
authentication: serve a dedicated, public, read-only tree, never a web root.

## Tree

```text
/srv/tftp/
├── catalog
└── files/
    └── b/
        └── bordercolorbars_Zu8DOI.prg
```

Every path in `catalog` is relative to `/srv/tftp` and must name a readable
file below it. Generate into a temporary tree, then rename `catalog` into place
only after its files and hardlinks exist; a client must never fetch half a
catalogue.

`catalog` is a 128-byte header followed by fixed-width records. Use version 2
and a 128-byte record size: header bytes 0--7 are `M65FHCAT`, byte 8 is `2`,
bytes 9--10 are the little-endian record size, and bytes 11--12 the record
count. Each record has a 40-byte space-padded title, 16-byte space-padded
author, 48-byte NUL-padded path, kind (`0` `.prg`, `1` `.d81`), 32-bit
little-endian size, category, and little-endian year. Text is printable ASCII.

## Server

On Debian:

```sh
apt install tftpd-hpa
```

`/etc/default/tftpd-hpa`:

```sh
TFTP_USERNAME="tftp"
TFTP_DIRECTORY="/srv/tftp"
TFTP_ADDRESS=":69"
TFTP_OPTIONS="--secure --retransmit 1000000"
```

`--secure` confines requests to `/srv/tftp`; do not add `--create`. FILEHOST
uses `octet` mode and asks for `tsize` and 1024-byte blocks; a server may grant
the options or send 512-byte block 1 directly. `--retransmit 1000000` means one
second before the first resend.

```sh
systemctl enable --now tftpd-hpa
```

## Firewall

TFTP begins on UDP/69, then the server chooses a transfer port. An nftables
policy therefore needs the TFTP conntrack helper; accepting `related` alone is
not enough. Merge this into the existing `inet filter` table:

```nft
ct helper tftp-69 {
    type "tftp" protocol udp
    l3proto inet
}

chain input {
    type filter hook input priority filter; policy drop;
    ct state established,related accept
    iif lo accept
    udp dport 69 ct helper set "tftp-69" meter tftp_rate {
        ip saddr limit rate 6/minute burst 3 packets
    } accept
}
```

The rate limit limits requests, not transfer packets. Narrow the UDP/69 rule
to the intended network if the service is not public.

## Check

Run from another host:

```sh
tftp files.example.org -c get catalog
head -c 8 catalog                     # M65FHCAT
tftp files.example.org -c get files/b/bordercolorbars_Zu8DOI.prg
curl -fsS -o web.prg https://files.example.org/files/b/bordercolorbars_Zu8DOI.prg
cmp bordercolorbars_Zu8DOI.prg web.prg
```

`cmp` matters: equal lengths do not prove that a file's contents match.

## Failures

| FILEHOST says | Check |
|---|---|
| `NO ADDRESS` | DHCP and the machine's network, not TFTP. |
| `NO TFTP SERVER` | The configured server address. |
| `DID NOT ANSWER` | ARP, UDP/69 and the firewall helper. |
| `REFUSED, CODE 1` | Missing path or stale catalogue. |
| `REFUSED, CODE 2` | File permissions or a path outside `/srv/tftp`. |
| `TRANSFER STOPPED PART WAY` | Packet loss and server logs: `journalctl -u tftpd-hpa --since -10min`. |
