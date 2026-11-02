# Running the reVC multiplayer relay

Every player connects to one small program, `revc-relay`. It hands out invite
codes (`VICE-XXXX`), matches joiners to hosts and forwards game traffic. Only
the relay needs a reachable address. Players never forward ports or share IPs.

The relay has no game logic and uses little CPU or bandwidth. Eight players
in one session is roughly 8 × 20 snapshots per second of under 100 bytes each,
so a few hundred KB/s at most. It listens on **UDP 7777** by default.

```
revc-relay [--port N] [--bind ADDR] [--max-sessions N]
```

It logs one line per create, join, leave and session end to stdout, and runs
until Ctrl+C or SIGTERM. On shutdown it tells everyone the session ended.

## Getting the binaries

Either download `mp-tools-<os>` from the latest "Multiplayer: build and test" workflow run in
GitHub Actions (or use the Linux download from the [latest release](https://github.com/musmannn/revc-multiplayer/releases/latest) with `./install.sh --cli --relay-only`), or build them yourself. You need CMake 3.14+ and a C++11
compiler, and no game files.

```bash
git clone https://github.com/musmannn/revc-multiplayer.git
cd revc-multiplayer
cmake -S mp -B build-mp -DCMAKE_BUILD_TYPE=Release
cmake --build build-mp --config Release --target revc-relay revc-server revc-fakeclient
```

The binaries end up in `build-mp/`, or in `build-mp/Release/` with Visual Studio.

## 1. Cloud VPS (the default relay)

Any small Linux VPS works. Two good options are the Oracle Cloud **Always
Free** tier (an Ampere A1 or AMD micro instance) and a ~$5/month VPS from any
provider. Pick a region close to the players, since every packet goes through
the relay.

1. Install the build tools and build as above. On Ubuntu/Debian:
   ```bash
   sudo apt update && sudo apt install -y build-essential cmake git
   ```
2. Install the binary and a dedicated user:
   ```bash
   sudo install -m 755 build-mp/revc-relay /usr/local/bin/revc-relay
   sudo useradd --system --no-create-home --shell /usr/sbin/nologin revc
   ```
3. Create `/etc/systemd/system/revc-relay.service`:
   ```ini
   [Unit]
   Description=reVC multiplayer relay
   After=network-online.target
   Wants=network-online.target

   [Service]
   User=revc
   ExecStart=/usr/local/bin/revc-relay --port 7777
   Restart=always
   RestartSec=2
   NoNewPrivileges=true
   ProtectSystem=strict
   ProtectHome=true
   PrivateTmp=true

   [Install]
   WantedBy=multi-user.target
   ```
   ```bash
   sudo systemctl daemon-reload
   sudo systemctl enable --now revc-relay
   journalctl -u revc-relay -f        # watch the log
   ```
4. **Open UDP 7777 in two places.** Both are needed.
   - **The cloud firewall.** On Oracle Cloud: *Networking → Virtual Cloud
     Networks → your VCN → Security Lists → Default → Add Ingress Rule*, with
     source `0.0.0.0/0`, IP protocol **UDP**, destination port **7777**. Other
     providers call this a "security group" or "firewall".
   - **The OS firewall.**
     - `ufw`: `sudo ufw allow 7777/udp`
     - `firewalld`: `sudo firewall-cmd --permanent --add-port=7777/udp && sudo firewall-cmd --reload`
     - Oracle's Ubuntu images ship with iptables rules that block everything
       except SSH, even with the security list open:
       ```bash
       sudo iptables -I INPUT -p udp --dport 7777 -j ACCEPT
       sudo netfilter-persistent save
       ```
5. **Check from another machine**, e.g. your own PC:
   ```bash
   revc-fakeclient --relay <vps-public-ip> --host --name Test
   ```
   It should print `[ready] code=VICE-XXXX id=0 host` within a second. If it
   prints `[failed] not found` after 5 seconds, the relay isn't reachable,
   which usually means one of the two firewalls. The relay's log shows a
   `create` line when it works.

## 2. Home PC (Linux, macOS or Windows)

Running the relay on your own PC works the same way. The difference is that
the router must forward the port, because friends connect from outside your
network.

1. Build or download `revc-relay` and run it:
   - Linux/macOS: `./revc-relay --port 7777`
   - Windows: `revc-relay.exe --port 7777` in a terminal
2. **Forward UDP 7777** on your router to this PC's LAN address (e.g.
   `192.168.1.20`). The setting is usually called *Port Forwarding* or
   *Virtual Server*. Give the PC a fixed LAN address (a DHCP reservation) so
   the rule keeps working.
3. **Allow it through the PC's firewall:**
   - Windows (admin terminal):
     ```bash
     netsh advfirewall firewall add rule name="reVC relay" dir=in action=allow protocol=UDP localport=7777
     ```
     Or accept the Windows Defender Firewall prompt the first time it runs.
   - macOS: accept the "allow incoming connections" prompt, or add
     `revc-relay` under *System Settings → Network → Firewall → Options*.
   - Linux: see the `ufw`/`firewalld` commands above.
4. **Find your public IP**, for example by searching "what is my IP" or
   running `curl https://api.ipify.org`. Friends use that address. Home IPs
   can change, so a free dynamic-DNS name (e.g. DuckDNS) gives you an address
   that keeps working.
5. Check from outside your network, e.g. from a phone hotspot:
   `revc-fakeclient --relay <public-ip> --host`. Inside your own network,
   use the LAN address or `127.0.0.1`. Many routers don't support connecting
   to their own public IP from inside ("NAT loopback").

The relay doesn't have to run on the same PC you play on, but it can.

## 3. Pointing the game at a relay

The game ships with the cloud relay's address built in as the default, so
friends only type the invite code. To use another relay, such as your home
PC, change the *Relay server* setting on the Multiplayer menu to `host` or
`host:port`. You don't need to edit any files. Everyone in a session must
use the same relay. (The menu arrives with Plan 2, and the default address
gets filled in once the VPS exists.)

## 4. A dedicated server (optional)

Normally the player who hosts is in charge of the session, and the session
ends when they quit. `revc-server` hosts a session instead: it stays up, has
no ped, and can start races and matches by itself. Players join with its code
like any other. The server takes one of the 8 places, so 7 people can play.

```
revc-server [--relay HOST[:PORT] | --with-relay PORT] [--name NAME] [--motd TEXT]
            [--hour H] [--fixed-time] [--weather N]
            [--routes DIR] [--arenas DIR] [--playlist LIST] [--playlist-gap SEC]
            [--min-players N] [--code-file PATH] [--quiet]
```

- `--with-relay PORT` runs the relay in the same process, so one program is
  enough on a VPS: `revc-server --with-relay 7777`. With `--relay` it uses a
  relay that's already running.
- It prints `CODE VICE-XXXX` (and writes it to `--code-file`) whenever it gets a
  code. If the relay drops it, it makes a new session after 5 seconds, with a
  new code.
- `--playlist "race:Ocean Drive Sprint,dm,koth:Washington Beach,stunt"` starts
  those events in turn, `--playlist-gap` seconds (default 60) after the last
  one ended, when at least `--min-players` (default 2) are in. Routes and
  arenas are looked up by name in `--routes` / `--arenas` (default
  `data/mp/races` and `data/mp/arenas`, so run it from the game folder or point
  them at a copy of the package's `data/mp`), or given as a `.route` path.
  Players can still start their own events from F2; their routes travel with
  the request, so the server doesn't need their files.

For systemd, use the relay unit above with
`ExecStart=/usr/local/bin/revc-server --with-relay 7777 --name "Vice Server" --code-file /var/lib/revc/code.txt`
(and give the `revc` user that folder). Read the current code with
`cat /var/lib/revc/code.txt` or `journalctl -u revc-relay`.

## 5. Legal note

reVC is a reverse-engineered build of a commercial game. Players must own the
original game files, and neither the relay nor reVC distributes them. Even
so, running a public, internet-facing service for a reverse-engineered game
carries some legal risk (e.g. a takedown request to you or your hosting
provider). Whoever runs a relay accepts that risk. The relay stores nothing.
It only forwards packets between people who share an invite code and logs
connection events.
