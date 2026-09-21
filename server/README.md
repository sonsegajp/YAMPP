# Melee PC netplay server

A Python standard-library service with local protocol/repository modules. It hosts the room list and relays the match
traffic between players, so nobody needs to forward ports at home.

## Run it on a VPS

```
sudo mkdir -p /opt/melee-netplay
sudo cp melee_netplay_server.py mod_repository.py upstream_builds.py upstream_builds.json profile_protocol.py /opt/melee-netplay/
sudo cp melee-netplay.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now melee-netplay
sudo systemctl status melee-netplay
```

Open TCP port 7420 in the VPS firewall (for example `ufw allow 7420/tcp`).
Change the port with `--port` in the service file if needed. A reverse proxy
can also forward an HTTP `Upgrade: melee-netplay` connection to this port;
match inputs use the same TCP connection as the lobby.

The prepared Windows build connects through the configured public Online hostname. Private deployment addresses and credentials are kept outside the client UI and source tree.

## Updating the existing service

Copy the service modules, upstream-build JSON and their tests to a private upload directory. Validate
them and back up the installed modules and JSON before replacing them. Run these from the upload directory:

```sh
python3 -m unittest discover -s . -p "test_*.py" -v
python3 -m py_compile melee_netplay_server.py mod_repository.py upstream_builds.py profile_protocol.py
backup="/opt/melee-netplay/melee_netplay_server.py.$(date -u +%Y%m%dT%H%M%SZ).bak"
sudo cp -p /opt/melee-netplay/melee_netplay_server.py "$backup"
sudo cp -p /opt/melee-netplay/mod_repository.py "${backup}.repository"
sudo install -m 644 melee_netplay_server.py mod_repository.py upstream_builds.py upstream_builds.json profile_protocol.py /opt/melee-netplay/
sudo systemctl restart melee-netplay
sudo systemctl is-active melee-netplay
sudo journalctl -u melee-netplay -n 30 --no-pager
```

The existing service and nginx route can stay in place. Restarting disconnects
current sessions. Verify the public HTTP-upgrade route with a version-2 hello;
its welcome must include `"version":2` and `"sync":"rollback-v2"`.
If the service fails, restore the named backup and restart it.

### Opening the datagram relay

The server relays input over UDP as well as over the lobby stream, because a
stream delivers in order and one lost segment holds every input behind it
until the retransmission lands. `--udp-port` defaults to `--port`; `0` turns
the channel off and keeps everything on the stream.

nginx cannot proxy this, so the UDP port has to reach the service directly.
Where the lobby is published through nginx on 443, run the datagram relay on
its own port and open it in the host firewall as well as the provider's:

```
sudo ufw allow 7420/udp
# IONOS and similar providers also need the rule added in the cloud firewall,
# which drops anything not explicitly allowed regardless of the host firewall.
```

If the relay cannot bind, the server logs a warning and keeps serving on the
stream alone. If it binds but players cannot reach it, their clients probe for
a second, give up and use the stream; nothing is lost but the probes. Use
`--advertise-udp-port` when clients must send to a different port from the one
the service binds, for example behind a port forward. Confirm it is working by
watching for `client N reachable by datagram` in the service log, or by the
in-match readout on a client showing `direct` rather than `relayed`.

## Protocol summary

A connection must begin with `{"op":"hello","name":"Player","version":2,"sync":"rollback-v2"}`.
The welcome and session-start replies repeat `version` and `sync`. Older
clients and unknown synchronisation engines are rejected so they cannot enter a
rollback room; `rollback-v2` adds the sender's frame advantage to each input
packet, which is what lets the two clients keep their clocks together. Update
the server alongside the game; the client also checks the server handshake
before enabling the lobby.

- TCP, one JSON object per line: `hello`, `list`, `create`, `join`, `leave`,
  `ready`, `rules`, `start`, `ping`, `resume`. The server answers with
  `welcome`, `rooms`, `room`, `start`, `held`, `resumed`, `ended`, `error`,
  `pong`.
- Input relay: `{ "op": "r", "d": "<base64>" }` on the lobby connection.
  The decoded packet starts with little-endian `<IIHH>`: magic `0x4D4C4E50`,
  session ID, sender ID, recipient ID (zero broadcasts to the other players).
  The server verifies session membership and sender identity before forwarding.
  Decoded packets are limited to 1400 bytes and incoming JSON lines to 8192 bytes.
- Datagram relay: the same packet, on the UDP port, with the client's 32-character
  `udp_token` from its `welcome` in front of it. The token decides who the
  sender is; the source address is only recorded as the place to answer, so a
  renumbered NAT mapping follows the player and a forged source address cannot
  redirect anyone's inputs. Each client is capped at 1000 datagrams a second.
  Delivery is per recipient: whoever has proved a working datagram path gets a
  datagram and everyone else gets the stream, so one player on a UDP-hostile
  network does not push the other back onto TCP.
- `delay: 0` in the room rules means automatic. Clients report their measured
  round trip to this server with `{"op":"ping","rtt":N}`; inputs travel
  client -> server -> client, so half of each client's round trip is the trip
  the delay must cover. The server resolves one number at `start` and sends it
  to everyone in the same message, as the top-level `delay` field, while
  `rules.delay` stays 0 so the lobby keeps showing Auto. It must be one
  number: a match seeds the frames before the delay as known-empty input on
  every port, so two clients disagreeing about that boundary would contradict
  each other on the first frame someone held a direction.
- Losing a connection during a live match **holds** that player's place for
  `--hold-seconds` (40 by default) instead of ending the session. The other
  players get `held`; the returning player reconnects, sends
  `{"op":"resume","session":N}` and is seated back on the same port, and
  everyone gets `resumed` with the new player list so their relays recognise
  the new client ID. A held room is not advertised in the lobby while nobody
  is connected to it. When the window closes the session ends normally.
- A valid session BYE packet ends the match without removing the room, allowing
  a rematch. Leaving on purpose is not held. Departing hosts transfer ownership
  to the next player. A rule change clears all Ready flags; duplicate Start
  cannot replace a live match.

Only the host's rules are used for a match. Both players must run the same
game build and original game assets. Current clients enforce the
[compat-v1 fingerprint gate](../docs/netplay-compatibility.md) before joining. Arbitrary gameplay-changing Workshop mods are blocked; the verified selective Akaneia runtime has a separate required-content and compatibility flow.

## Local protocol regression checks

```
python -m unittest discover -s server -p "test_*.py" -v
```

These checks cover room lifecycle, rematches, malformed input, relay identity,
ID wraparound, actual localhost TCP and HTTP-upgrade connections, and
(`test_netplay_resilience.py`) the datagram relay, the reconnect window and
the automatic delay. They do not establish gameplay synchronisation; run the
game-pair check described in [docs/netplay.md](../docs/netplay.md) separately.

## Optional costume repository

The [repository contract](../docs/mod-repository-contract.md) documents the HTTPS
catalog, authenticated publishing, immutable package downloads, and room consent
flow. Configure `--mods-dir`, `--mod-api`, and the private
`MELEE_MOD_UPLOAD_TOKEN` environment variable. Expose `/melee/api/` only through
HTTPS, redirect HTTP API requests, and give the service write access only to its
repository data directory. The provided DynamicUser service needs a
`StateDirectory=melee-netplay` override and a directory under
`/var/lib/melee-netplay` when enabling uploads. Never put publisher tokens in
source control, command-line arguments, API responses, or logs.

The official preview service keeps `profile-v1` sharing disabled, so Profile pictures stay local. On September 20 the current catalog integrity/cache update passed 59 production-variant server tests and public TLS room create/join/rejoin/ready/start checks. No active rooms were interrupted.
