# Running a Real Steel lobby server

A lobby server is what lets Real Steel players find each other over the
internet. Anyone can run one. Players type its address into the game, and
from then on the game's own MULTIPLAYER menu just works:

* **FIND MATCHES** lists every match hosted by players using the same server.
* **HOST MATCH** puts your match on that list.
* **LEADERBOARDS** shows the server's rankings, built from the results of
  ranked matches played through it.
* Joining usually works without anyone touching their router: the server
  introduces the two games to each other so both send first (UDP hole
  punching).

The server never carries the fight itself. Matches are peer to peer, so a tiny
machine can serve a lot of players. It is one Python file with no
dependencies: `tools/lobby/realsteel_lobby.py`, in this repository and in every
release download.

---

## 1. What you need

* A machine that is on whenever people want to play, with a **public IP
  address** (or a router you can forward a port on). A $5/month cloud VPS, a
  home server or a Raspberry Pi all work.
* **Python 3.8 or newer.** Nothing else; no `pip install`.
* **UDP port 28800** reachable from the internet (you can pick another).
* Optional: a TCP port for the JSON web feed (section 6).

Traffic is small: a hosted match sends one ~40 byte packet every 3 seconds,
and a search is a handful of packets. Memory use stays in the tens of MB.

## 2. Quick start

Copy `realsteel_lobby.py` to the machine and run it:

```bash
python3 realsteel_lobby.py
```

You should see:

```
2026-10-09 17:00:00,000 lobby listening on UDP 28800
```

That's it. Open UDP 28800 in the firewall (section 3) and tell players your
address (section 5).

All options:

| Option | Default | What it does |
| --- | --- | --- |
| `--port N` | 28800 | UDP port the games talk to |
| `--data FILE` | leaderboards.json | where the leaderboards are kept |
| `--http N` | off | also serve matches and leaderboards as JSON on TCP port N |
| `--verbose` | off | log every request (noisy; for debugging) |

On Windows use `python` (or `py`) instead of `python3`.

## 3. Opening the port

The server must receive UDP packets on its port from anywhere.

**Linux with ufw**

```bash
sudo ufw allow 28800/udp
```

**Linux with firewalld**

```bash
sudo firewall-cmd --permanent --add-port=28800/udp && sudo firewall-cmd --reload
```

**Cloud providers** (AWS, Google Cloud, Azure, Oracle, Hetzner, DigitalOcean,
...) also have their own firewall, usually called a *security group* or
*firewall rule*. Add an inbound rule: protocol UDP, port 28800, source
`0.0.0.0/0`.

**Windows**: the first run pops up a Windows Firewall prompt; allow it for
the network type you are on. Or, in an administrator PowerShell:

```powershell
New-NetFirewallRule -DisplayName "Real Steel lobby" -Direction Inbound -Protocol UDP -LocalPort 28800 -Action Allow
```

**At home**: forward UDP 28800 on your router to the machine running the
server, and give players your public IP (search "what is my IP"). A dynamic
DNS name (DuckDNS, No-IP, ...) saves you from handing out a new address every
time your ISP changes it.

## 4. Keeping it running

### Linux: systemd service (recommended)

```bash
sudo useradd --system --home /opt/realsteel-lobby --create-home realsteel
sudo cp realsteel_lobby.py /opt/realsteel-lobby/
sudo tee /etc/systemd/system/realsteel-lobby.service > /dev/null <<'EOF'
[Unit]
Description=Real Steel lobby and leaderboard server
After=network-online.target
Wants=network-online.target

[Service]
User=realsteel
WorkingDirectory=/opt/realsteel-lobby
ExecStart=/usr/bin/python3 /opt/realsteel-lobby/realsteel_lobby.py --port 28800 --data /opt/realsteel-lobby/leaderboards.json
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable --now realsteel-lobby
```

Check on it with `systemctl status realsteel-lobby` and read the log with
`journalctl -u realsteel-lobby -f`.

### Docker

```dockerfile
FROM python:3-slim
COPY realsteel_lobby.py /app/realsteel_lobby.py
WORKDIR /data
EXPOSE 28800/udp
CMD ["python3", "/app/realsteel_lobby.py", "--port", "28800", "--data", "/data/leaderboards.json"]
```

```bash
docker build -t realsteel-lobby .
docker run -d --name realsteel-lobby --restart unless-stopped -p 28800:28800/udp -v realsteel-lobby:/data realsteel-lobby
```

Do not forget the `/udp` in `-p`; Docker forwards TCP only by default.

### Windows

Put this in `run_lobby.bat` next to the script and double-click it, or add it
to Task Scheduler with the trigger "At startup":

```bat
@echo off
cd /d "%~dp0"
:again
python realsteel_lobby.py --port 28800 --data leaderboards.json
timeout /t 5 > nul
goto again
```

## 5. Telling players how to connect

Give players your server's address: a host name or an IP address, plus
`:port` if you changed it from 28800. For example `lobby.example.com`,
`203.0.113.7`, or `203.0.113.7:29000`.

In the game they enter it in either place:

* **The MULTIPLAYER menu**: a panel next to the menu shows *LOBBY SERVER*.
  Select the field and type the address.
* **The settings page** (F1 on a keyboard, or click both sticks, L3 + R3):
  *Game > Lobby server*.

The *Lobby status* line underneath reports what happened:

| Status | Meaning |
| --- | --- |
| not set | No lobby server entered. |
| waiting for the server | Sent a hello, no reply yet. |
| connected | The server answered. |
| connected, N matches listed | Answered a search with N matches. |
| no answer from the server | It stopped answering: wrong address, port closed, or server down. |
| cannot find *name* | The host name does not resolve (typo, or DNS not set up). |

Players can also start the game with
`--realsteel_lobby_server=lobby.example.com`; the setting is saved either way.

Everyone who wants to play together must use **the same server**. A server
only lists matches hosted through it, and keeps its own leaderboards.

### When joining fails

Hole punching works on most home routers, but not all (some mobile networks
and "strict NAT" routers block it). If a player can see a match but cannot
join it, the **host** forwards their game's UDP port on their router. The port
is shown on the MULTIPLAYER panel as *YOUR PORT* (normally UDP 28700). Nothing
changes on the server.

## 6. The JSON web feed (optional)

With `--http 8080` the server also answers plain HTTP on TCP 8080 (open that
port too). Handy for a website, a Discord bot or just checking on things:

| URL | Returns |
| --- | --- |
| `/matches` | Open matches: name, password protected, players, open slots, mod set, minutes open |
| `/leaderboard/total` | All-time board: rank, name, points, matches, wins, losses, KOs |
| `/leaderboard/weekly` | This week's board, same fields |

Example:

```bash
curl http://lobby.example.com:8080/leaderboard/total
```

```json
{
 "board": "total",
 "players": [
  {"rank": 1, "name": "ZEUSFAN", "points": 119, "matches": 6, "wins": 4, "losses": 2, "kos": 3}
 ]
}
```

The feed is read only and sends `Access-Control-Allow-Origin: *`, so a web
page anywhere can fetch it. Match addresses are never included. Put it behind
a reverse proxy (nginx, Caddy) if you want HTTPS.

## 7. Leaderboards

* Players appear after their first **ranked** match through the server
  (*MULTIPLAYER > RANKED MATCH*). Player matches do not count.
* The game reports each player's points, matches, wins, losses and KOs at the
  end of a ranked match; the server adds them up. As on the original Xbox
  Live, **both** games report **both** players, each when its player leaves
  the result screen. The server counts a result once: the same result from
  the other game within 15 minutes is logged as `same result again` and
  ignored.
* There are two boards, as in the original game: **total** (all time) and
  **weekly**, which empties every Monday at 00:00 UTC.
* Boards are saved to the `--data` file every 30 seconds and when the server
  stops. Back that file up if you care about it.
* A player is identified by an ID the game makes on first launch and keeps in
  `realsteel_settings.cfg`. Their name is whatever they last played under.
  A player who deletes that file starts again as a new entry.

### Editing or resetting the boards

Stop the server, edit the data file, start it again. The file is plain JSON:

```json
{
 "total":  {"0009a1b2c3d4e5f6": {"name": "ZEUSFAN", "values": {"10000002": 119, "10000003": 6, "10000004": 4, "10000005": 2, "10000006": 3, "20000001": 119}}},
 "weekly": {"...": "same layout"},
 "week": "2026-W41",
 "names": {"0009a1b2c3d4e5f6": "ZEUSFAN"}
}
```

* To remove a player, delete their entry from `total` and `weekly`.
* To reset everything, delete the file.
* The value keys are the game's statistics: `10000002` ranking points,
  `10000003` matches, `10000004` wins, `10000005` losses, `10000006` KOs,
  `20000001` the rating the board is sorted by.

### Trust

The server believes what the games tell it. There are no accounts and no
passwords; a determined player could send made-up results. For a group of
friends or a community server this is usually fine. If it turns into a
problem, delete the offending entries (above) or run a private server and
share the address only with people you trust.

## 8. Security and limits

* The server only speaks its own small protocol (section 9) and ignores
  anything else. It never runs code from packets or touches files other than
  its data file.
* Each address may make 10 searches, leaderboard reads, result reports or
  hellos per second; more are dropped.
* A listed match disappears 15 seconds after its host stops sending updates,
  so crashed games do not linger.
* At most 2000 matches are listed at once.
* It runs fine as an unprivileged user (the systemd example above does that).
* The server learns each player's public IP address and port, which it must
  to introduce players. It shows them only to players searching for matches,
  never in the JSON feed.

## 9. Protocol reference

Everything is UDP. Every packet starts with the 4 bytes `RSLB`, a version
byte (1) and a type byte. All numbers are big-endian. A *name* is one length
byte followed by that many UTF-8 bytes (at most 15).

| Type | Direction | Body |
| --- | --- | --- |
| 1 REGISTER | game > server, every 3 s while hosting | guest address u32, flags u8 (bit 0: password), players u8, open slots u8, mod set u64, host name |
| 1 REGISTER | server > game | (empty) acknowledgement |
| 2 UNREGISTER | game > server | (empty) the match closed |
| 3 LIST | game > server | guest address u32 |
| 4 LISTING | server > game | count u8, then per match: IP u32, port u16, flags u8, players u8, open slots u8, mod set u64, age in seconds u16, host name |
| 5 INTRO | server > host | joiner IP u32, joiner port u16 (the host sends the joiner a packet so its router lets the joiner in) |
| 6 STATS | game > server | reporter ID u64, reporter name, player ID u64, view count u8, per view: view ID u32, value count u8, per value: property ID u32, type u8, value i64 |
| 7 READ | game > server | request u32, view ID u32, around player u8, first rank u32, rows u16, player ID u64, column count u8, column IDs u16 each |
| 8 ROWS | server > game | request u32, view ID u32, total players u32, row count u8, per row: player ID u64, rank u32, rating i64, name, column count u8, per column: column ID u16, type u8, value i64 |
| 9 HELLO | game > server, every 30 s | player ID u64, name (so boards show each player's own name) |

A LISTING carries up to 40 matches; longer lists come in several packets. The
game then talks to each listed host directly on its game port with its own
packets, which the server never sees.

Leaderboard IDs come from the game's own Xbox Live configuration:

| View | ID |
| --- | --- |
| Total | 1 |
| Weekly | 3 |

| Column ID | Property ID | Statistic |
| --- | --- | --- |
| 1 | 0x10000002 | Ranking points |
| 2 | 0x10000003 | Matches |
| 3 | 0x10000004 | Wins |
| 4 | 0x10000005 | Losses |
| 5 | 0x10000006 | KOs |
| 65534 | 0x20000001 | Rating (the board's sort key) |

## 10. Troubleshooting

**"no answer from the server" in the game.** Run the server with
`--verbose` and have a player open FIND MATCHES: each search logs a
`list for ...` line. If nothing appears, the packets are not arriving: the
firewall or the cloud security group is still closed, the router is not
forwarding the port, or the player typed the wrong address.

**A match shows up but joining fails.** That is the host's router refusing
the joiner. The host forwards their game port (see *When joining fails*).

**Matches show up for some players and not others.** They are on different
servers, or one has *Local network* only. Everyone needs the same lobby
server address.

**The weekly board is empty.** It resets every Monday at 00:00 UTC.

**Port already in use.** Another copy is still running (`systemctl status`,
`docker ps`, Task Manager), or pick another port with `--port` and give
players `address:port`.
