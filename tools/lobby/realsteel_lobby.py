#!/usr/bin/env python3
"""Real Steel lobby and leaderboard server.

* Lists the online matches of everyone who points their game at it
  (Settings > Game > Lobby server), so players find each other without
  typing addresses, and introduces joiners to hosts so both send first
  (UDP hole punching through home routers).
* Keeps the game's leaderboards (the in-game LEADERBOARDS screen): results
  the games report after ranked matches are added up per player, total and
  weekly, and saved to a JSON file.

Needs only Python 3.8+ (standard library). Run it on any machine with a
public address:

    python3 realsteel_lobby.py                         UDP 28800
    python3 realsteel_lobby.py --port 28800 --http 8080 --data leaderboards.json

--http also serves the match list and the leaderboards as JSON
(http://server:8080/matches, /leaderboard/total, /leaderboard/weekly),
handy for a website or a Discord bot. See LOBBY_SERVER.md for the set-up
guide and the protocol.
"""
import argparse, datetime, json, logging, os, socket, struct, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAGIC = b'RSLB'
VERSION = 1
REGISTER, UNREGISTER, LIST, LISTING, INTRO, STATS, READ, ROWS, HELLO = 1, 2, 3, 4, 5, 6, 7, 8, 9
EXPIRY = 15.0             # seconds without a REGISTER before a match is dropped
MAX_PER_PACKET = 40
MAX_MATCHES = 2000
MAX_REQUESTS_PER_SECOND = 10  # per address, LIST + READ + STATS + HELLO
SAME_RESULT_WINDOW = 15 * 60  # both games of a ranked match report it; count it once

# The game's leaderboard schema (from its XLAST):
# view 1 = total, view 3 = weekly; column attribute id -> property id. Every
# column is summed. Rating (column 65534) orders the board.
VIEWS = {1: 'total', 3: 'weekly'}
COLUMNS = {1: 0x10000002,   # ranking points
           2: 0x10000003,   # matches
           3: 0x10000004,   # wins
           4: 0x10000005,   # losses
           5: 0x10000006,   # KOs
           65534: 0x20000001}  # rating
RATING = 0x20000001
LONG, LONGLONG = 1, 2


class Lobby:
    def __init__(self, data_path):
        self.matches = {}    # (ip, port) -> dict
        self.lock = threading.Lock()
        self.request_times = {}
        self.data_path = data_path
        # board -> xuid -> entry; names: xuid -> the name that player last used
        self.boards = {'total': {}, 'weekly': {}, 'week': self.week(), 'names': {}}
        self.recent = {}     # (xuid, view, values) -> (time, reporter): results already counted
        self.dirty = False
        if data_path and os.path.isfile(data_path):
            try:
                self.boards.update(json.load(open(data_path)))
                logging.info('leaderboards loaded from %s (%d players)', data_path, len(self.boards['total']))
            except (OSError, ValueError) as e:
                logging.warning('could not read %s: %s', data_path, e)

    @staticmethod
    def week():
        return datetime.datetime.utcnow().strftime('%G-W%V')    # ISO week: resets on Monday 00:00 UTC

    # ---- matches -------------------------------------------------------
    def register(self, addr, payload):
        if len(payload) < 16:
            return
        guest, flags, filled, open_slots, mods = struct.unpack_from('>IBBBQ', payload)
        name = payload[16:16 + payload[15]].decode('utf-8', 'replace')[:15]
        with self.lock:
            if addr not in self.matches and len(self.matches) >= MAX_MATCHES:
                return
            known = addr in self.matches
            self.matches[addr] = {'ip': addr[0], 'port': addr[1], 'guest': guest, 'password': bool(flags & 1),
                                  'filled': filled, 'open': open_slots, 'mods': mods, 'name': name,
                                  'since': self.matches.get(addr, {}).get('since', time.time()),
                                  'seen': time.time()}
        if not known:
            logging.info('match hosted by %r at %s:%d%s', name, addr[0], addr[1], ' (password)' if flags & 1 else '')

    def unregister(self, addr):
        with self.lock:
            if self.matches.pop(addr, None) is not None:
                logging.info('match at %s:%d closed', *addr)

    def listing(self, requester):
        now = time.time()
        with self.lock:
            for key in [k for k, m in self.matches.items() if now - m['seen'] > EXPIRY]:
                logging.info('match at %s:%d expired', *key)
                del self.matches[key]
            return [m for k, m in self.matches.items() if k != requester]

    def allow(self, addr):
        now = time.time()
        times = [t for t in self.request_times.get(addr[0], []) if now - t < 1.0]
        if len(times) >= MAX_REQUESTS_PER_SECOND:
            return False
        times.append(now)
        self.request_times[addr[0]] = times
        if len(self.request_times) > 10000:
            self.request_times.clear()
        return True

    # ---- leaderboards ---------------------------------------------------
    def board(self, view):
        with self.lock:
            if self.boards.get('week') != self.week():
                self.boards['weekly'] = {}
                self.boards['week'] = self.week()
                self.dirty = True
        return self.boards[VIEWS[view]]

    def hello(self, payload):
        xuid = struct.unpack_from('>Q', payload)[0]
        name = payload[9:9 + payload[8]].decode('utf-8', 'replace')[:15]
        if xuid and name:
            self.set_name('%016x' % xuid, name)

    def set_name(self, key, name):
        with self.lock:
            if self.boards['names'].get(key) == name:
                return
            self.boards['names'][key] = name
            for board in ('total', 'weekly'):
                if key in self.boards[board]:
                    self.boards[board][key]['name'] = name
            self.dirty = True

    def stats(self, payload):
        # reporter xuid, reporter name, player xuid, views. In a ranked match
        # BOTH games report BOTH players (the original Xbox Live arbitration),
        # usually a minute or so apart: a result already reported by the
        # other game is counted once.
        reporter = struct.unpack_from('>Q', payload)[0]
        at = 8
        reporter_name = payload[at + 1:at + 1 + payload[at]].decode('utf-8', 'replace')[:15]
        at += 1 + payload[at]
        if reporter and reporter_name:
            self.set_name('%016x' % reporter, reporter_name)
        xuid = struct.unpack_from('>Q', payload, at)[0]
        at += 8
        key = '%016x' % xuid
        views = payload[at]
        at += 1
        for _ in range(views):
            view, count = struct.unpack_from('>IB', payload, at)
            at += 5
            values = {}
            for _ in range(count):
                prop, kind, value = struct.unpack_from('>IBq', payload, at)
                at += 13
                values[prop] = value
            if view not in VIEWS:
                continue
            board = self.board(view)
            now = time.time()
            with self.lock:
                for k in [k for k, (t, _) in self.recent.items() if now - t > SAME_RESULT_WINDOW]:
                    del self.recent[k]
                same = (xuid, view, tuple(sorted(values.items())))
                seen = self.recent.get(same)
                if seen is not None and seen[1] != reporter:
                    del self.recent[same]      # the other game's copy: counted already
                    duplicate = True
                else:
                    self.recent[same] = (now, reporter)
                    duplicate = False
                    name = self.boards['names'].get(key, 'PLAYER')
                    entry = board.setdefault(key, {'name': name, 'values': {}})
                    entry['name'] = name
                    for prop, value in values.items():
                        k = '%08x' % prop
                        entry['values'][k] = entry['values'].get(k, 0) + value
                    self.dirty = True
            logging.info('%s for %r on the %s board (reported by %r): %s',
                         'same result again' if duplicate else 'results',
                         self.boards['names'].get(key, 'PLAYER'), VIEWS[view], reporter_name,
                         ', '.join('%08x=%d' % kv for kv in values.items()))

    def ranked(self, view):
        board = self.board(view)
        with self.lock:
            entries = [(int(x, 16), e) for x, e in board.items()]
        entries.sort(key=lambda xe: (-xe[1]['values'].get('%08x' % RATING, 0), xe[1]['name']))
        return entries

    def read(self, payload):
        request, view, by_player, first, rows, xuid, ncols = struct.unpack_from('>IIBIHQB', payload)
        columns = struct.unpack_from('>%dH' % ncols, payload, 24)
        if view not in VIEWS:
            return request, view, 0, []
        entries = self.ranked(view)
        if by_player:
            index = next((i for i, (x, _) in enumerate(entries) if x == xuid), None)
            start = 0 if index is None else max(0, index - rows // 2)
            if index is None:
                entries = []
        else:
            start = max(0, first - 1)
        page = []
        for i, (x, e) in enumerate(entries[start:start + rows], start + 1):
            cols = []
            for c in columns:
                prop = COLUMNS.get(c)
                value = e['values'].get('%08x' % prop, 0) if prop else 0
                cols.append((c, LONGLONG if prop and prop >> 28 == 2 else LONG, value))
            page.append((x, i, e['values'].get('%08x' % RATING, 0), e['name'], cols))
        return request, view, len(self.boards[VIEWS[view]]), page

    def save(self):
        if not self.data_path or not self.dirty:
            return
        with self.lock:
            text = json.dumps(self.boards)
            self.dirty = False
        tmp = self.data_path + '.tmp'
        open(tmp, 'w').write(text)
        os.replace(tmp, self.data_path)


def ip_to_int(ip):
    return struct.unpack('>I', socket.inet_aton(ip))[0]


def serve_udp(lobby, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('0.0.0.0', port))
    logging.info('lobby listening on UDP %d', port)
    while True:
        try:
            data, addr = sock.recvfrom(4096)
        except OSError:
            continue
        if len(data) < 6 or data[:4] != MAGIC or data[4] != VERSION:
            continue
        kind, payload = data[5], data[6:]
        try:
            if kind == REGISTER:
                lobby.register(addr, payload)
                sock.sendto(MAGIC + bytes((VERSION, REGISTER)), addr)          # acknowledgement
            elif kind == UNREGISTER:
                lobby.unregister(addr)
            elif kind == LIST and lobby.allow(addr):
                matches = lobby.listing(addr)
                logging.debug('list for %s:%d: %d match(es)', addr[0], addr[1], len(matches))
                for start in range(0, max(1, len(matches)), MAX_PER_PACKET):
                    part = matches[start:start + MAX_PER_PACKET]
                    out = bytearray(MAGIC + bytes((VERSION, LISTING, len(part))))
                    for m in part:
                        name = m['name'].encode('utf-8')[:15]
                        out += struct.pack('>IHBBBQH', ip_to_int(m['ip']), m['port'], int(m['password']), m['filled'],
                                           m['open'], m['mods'], min(65535, int(time.time() - m['since'])))
                        out += bytes((len(name),)) + name
                    sock.sendto(bytes(out), addr)
                # hole punching: every host sends the joiner a packet first
                intro = MAGIC + bytes((VERSION, INTRO)) + struct.pack('>IH', ip_to_int(addr[0]), addr[1])
                for m in matches:
                    sock.sendto(intro, (m['ip'], m['port']))
            elif kind == STATS and lobby.allow(addr):
                lobby.stats(payload)
            elif kind == HELLO and lobby.allow(addr):
                lobby.hello(payload)
            elif kind == READ and lobby.allow(addr):
                request, view, total, page = lobby.read(payload)
                out = bytearray(MAGIC + bytes((VERSION, ROWS)) + struct.pack('>IIIB', request, view, total, len(page)))
                for x, rank, rating, name, cols in page:
                    name = name.encode('utf-8')[:15]
                    out += struct.pack('>QIq', x, rank, rating) + bytes((len(name),)) + name + bytes((len(cols),))
                    for c, kind_, value in cols:
                        out += struct.pack('>HBq', c, kind_, value)
                sock.sendto(bytes(out), addr)
        except (struct.error, IndexError, KeyError) as e:
            logging.debug('bad packet from %s:%d: %s', addr[0], addr[1], e)


def serve_http(lobby, port):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            path = self.path.rstrip('/')
            if path in ('', '/matches'):
                body = {'matches': [{'name': m['name'], 'password': m['password'], 'players': m['filled'],
                                     'open_slots': m['open'], 'mods': '%016x' % m['mods'],
                                     'minutes': int((time.time() - m['since']) // 60)} for m in lobby.listing(None)]}
            elif path in ('/leaderboard/total', '/leaderboard/weekly'):
                view = 1 if path.endswith('total') else 3
                names = {0x10000002: 'points', 0x10000003: 'matches', 0x10000004: 'wins',
                         0x10000005: 'losses', 0x10000006: 'kos'}
                body = {'board': VIEWS[view], 'players': [
                    dict({'rank': i, 'name': e['name']},
                         **{n: e['values'].get('%08x' % p, 0) for p, n in names.items()})
                    for i, (_, e) in enumerate(lobby.ranked(view), 1)]}
            else:
                self.send_error(404)
                return
            data = json.dumps(body, indent=1).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *args):
            pass

    logging.info('JSON on http://0.0.0.0:%d/matches, /leaderboard/total, /leaderboard/weekly', port)
    ThreadingHTTPServer(('0.0.0.0', port), Handler).serve_forever()


def saver(lobby):
    while True:
        time.sleep(30)
        lobby.save()


def main():
    parser = argparse.ArgumentParser(description='Real Steel lobby and leaderboard server')
    parser.add_argument('--port', type=int, default=28800, help='UDP port the games talk to (default 28800)')
    parser.add_argument('--http', type=int, default=0, help='also serve matches / leaderboards as JSON on this TCP port')
    parser.add_argument('--data', default='leaderboards.json', help='file the leaderboards are kept in')
    parser.add_argument('--verbose', action='store_true', help='log every request')
    args = parser.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO, format='%(asctime)s %(message)s')
    lobby = Lobby(args.data)
    threading.Thread(target=saver, args=(lobby,), daemon=True).start()
    if args.http:
        threading.Thread(target=serve_http, args=(lobby, args.http), daemon=True).start()
    try:
        serve_udp(lobby, args.port)
    finally:
        lobby.save()


if __name__ == '__main__':
    main()
