"""Hold a room open on the netplay server so the in-game browser has something
to show and join. Not a player: it keeps the room alive and reports who joins,
but it cannot take part in a match.

    python scripts/netplay_dummy_room.py [name] [server]
"""
import json, os, socket, sys, time

# Not written down here: pass it as an argument or set MELEE_NETPLAY_SERVER.
SERVER = os.environ.get("MELEE_NETPLAY_SERVER", "")


def connect(server):
    host, _, path = server.partition("/")
    host, _, port = host.partition(":")
    port = int(port) if port else (80 if path else 7420)
    sock = socket.create_connection((host, port), 10)
    sock.settimeout(20)
    if path:
        sock.sendall(("GET /%s HTTP/1.1\r\nHost: %s\r\nUpgrade: melee-netplay\r\n"
                      "Connection: Upgrade\r\n\r\n" % (path, host)).encode())
        buffer = b""
        while b"\r\n\r\n" not in buffer:
            buffer += sock.recv(1024)
        head, rest = buffer.split(b"\r\n\r\n", 1)
        if b" 101" not in head.split(b"\r\n")[0]:
            raise SystemExit("server refused the connection: " + head.decode(errors="replace").splitlines()[0])
        return sock, rest
    return sock, b""


def main():
    name = sys.argv[1] if len(sys.argv) > 1 else "Test Room"
    server = sys.argv[2] if len(sys.argv) > 2 else SERVER
    if not server:
        raise SystemExit("pass the server address as the second argument, or set MELEE_NETPLAY_SERVER")
    sock, buffer = connect(server)
    print("connected to", server, flush=True)
    sock.sendall(b'{"op":"hello","name":"Dummy","version":2,"sync":"rollback-v2"}\n')
    sock.sendall(('{"op":"create","name":"%s","max":2}\n' % name).encode())
    sock.sendall(b'{"op":"ready","ready":true}\n')
    last_ping = time.time()
    while True:
        try:
            chunk = sock.recv(8192)
            if not chunk:
                break
            buffer += chunk
        except socket.timeout:
            chunk = b""
        while b"\n" in buffer:
            line, buffer = buffer.split(b"\n", 1)
            if not line.strip():
                continue
            try:
                message = json.loads(line)
            except ValueError:
                continue
            op = message.get("op")
            if op == "room" and message.get("room"):
                players = [p["name"] for p in message["room"]["players"]]
                print("room '%s' holding, players: %s" % (message["room"]["name"], ", ".join(players)), flush=True)
            elif op == "error":
                print("server:", message.get("message"), flush=True)
        if time.time() - last_ping > 20:
            last_ping = time.time()
            sock.sendall(b'{"op":"ping"}\n')
    print("connection closed", flush=True)


if __name__ == "__main__":
    main()
