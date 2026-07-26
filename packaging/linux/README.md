# Debian 13 deployment

The release directory is immutable application data. A production host uses
three separate locations:

- `/opt/laiue-server/current` — executable, laiue libraries and MsQuic;
- `/etc/laiue-server` — environment and TLS credentials;
- `/var/lib/laiue-server` — saves, enabled mods and other mutable state.

Install Debian runtime dependencies first:

```sh
apt-get update
apt-get install -y libssl3t64 libnuma1 libxdp1 libnl-route-3-200
```

Create a locked service account and directories:

```sh
adduser --system --group --home /var/lib/laiue-server \
  --no-create-home laiue
install -d -o root -g root -m 0755 /opt/laiue-server
install -d -o root -g laiue -m 0750 \
  /etc/laiue-server /etc/laiue-server/certificates
install -d -o laiue -g laiue -m 0750 /var/lib/laiue-server
```

Copy the extracted release to a versioned directory, then atomically update
`/opt/laiue-server/current`. Copy `server.env.example` to
`/etc/laiue-server/server.env` and adjust the values. Do not place private
keys below `/opt`; the key must be a regular non-symlink file owned by
`laiue`, with mode `0400`, inside the root-owned non-writable credentials
directory. For example:

```sh
chown root:root /etc/laiue-server/certificates/server-cert.pem
chmod 0644 /etc/laiue-server/certificates/server-cert.pem
chown laiue:laiue /etc/laiue-server/certificates/server-key.pem
chmod 0400 /etc/laiue-server/certificates/server-key.pem
```

Install `laiue-server.service` as
`/etc/systemd/system/laiue-server.service`, then:

```sh
systemctl daemon-reload
systemctl enable --now laiue-server.service
systemctl status laiue-server.service
ss -lunp | grep ':27180'
```

The public firewall/NAT rule is UDP, not TCP. For UFW:

```sh
ufw allow 27180/udp
```

Generate the certificate only after the host's real IPv4, IPv6 and DNS names
are known, and include every client endpoint in its SAN. See
`docs/secure_server.md` before exposing the listener.
