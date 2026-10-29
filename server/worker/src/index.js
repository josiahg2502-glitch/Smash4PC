// Smash 4 PC's netplay rooms, as a Cloudflare Worker: the public room list, room codes and the
// introductions that let two games connect directly. No game traffic passes through here: games find their own
// public UDP address with a STUN server, check in every couple of seconds while they are in a room or a match, and
// get back the addresses of the games they want to reach (and that want to reach them), then punch through their
// routers to each other.
//
// One request, POST /hello, a few text lines each way (ids are 24-bit hex, the same as the room codes):
//   v 1
//   id <id>
//   pub <ip> <port>                    this game's address as STUN saw it (missing until STUN answers)
//   lan <ip> <port>                    its address on its own network (for players behind the same router)
//   room <max> <players> <build>\t<name>   while it hosts a listed room
//   want <id>                          a game it is trying to reach (any number)
//   list 1                             it wants the room list
// The reply:
//   ok <ip>                            the address this request came from
//   peer <id> <pubip> <pubport> <lanip> <lanport>   per game to reach, or that wants to reach this one
//   room <id> <max> <players> <build>\t<name>

const TTL_MS = 20000;
const MAX_ROOMS = 50;
const MAX_LINE = 200;

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/") {
      return new Response("Smash 4 PC netplay rooms. See server/README.md in the source.\n");
    }
    if (url.pathname !== "/hello" || request.method !== "POST") {
      return new Response("not found\n", { status: 404 });
    }
    const lobby = env.LOBBY.get(env.LOBBY.idFromName("lobby"));
    return lobby.fetch(request);
  },
};

export class Lobby {
  constructor(state) {
    this.state = state;
    this.games = new Map(); // id -> { pub, lan, room, seen, wants: Set }
  }

  async fetch(request) {
    const body = (await request.text()).slice(0, 4096);
    const from = request.headers.get("CF-Connecting-IP") || "0.0.0.0";
    const now = Date.now();
    for (const [id, game] of this.games) {
      if (now - game.seen > TTL_MS) this.games.delete(id);
    }

    let id = null, pub = null, lan = null, room = null, list = false;
    const wants = new Set();
    for (const line of body.split("\n")) {
      if (line.length > MAX_LINE) continue;
      const tab = line.indexOf("\t");
      const head = (tab >= 0 ? line.slice(0, tab) : line).trim().split(/\s+/);
      switch (head[0]) {
        case "id": id = parseId(head[1]); break;
        case "pub": pub = parseEndpoint(head[1], head[2]); break;
        case "lan": lan = parseEndpoint(head[1], head[2]); break;
        case "want": { const w = parseId(head[1]); if (w !== null && wants.size < 8) wants.add(w); break; }
        case "list": list = head[1] === "1"; break;
        case "room":
          if (tab >= 0) {
            room = { max: clamp(head[1], 2, 8), players: clamp(head[2], 1, 8), build: (head[3] || "").slice(0, 60),
                     name: line.slice(tab + 1).trim().slice(0, 40) };
          }
          break;
      }
    }
    if (id === null) return new Response("bad request\n", { status: 400 });
    // Without STUN's answer yet, the address the request came from is the best guess at the public IP.
    if (pub === null && lan !== null) pub = { ip: from, port: lan.port };
    this.games.set(id, { pub, lan, room, seen: now, wants });

    const out = [`ok ${from}`];
    const peers = new Set();
    for (const w of wants) if (this.games.has(w)) peers.add(w);
    for (const [other, game] of this.games) if (other !== id && game.wants.has(id)) peers.add(other);
    for (const p of peers) {
      const g = this.games.get(p);
      if (!g || !g.pub) continue;
      const l = g.lan || g.pub;
      out.push(`peer ${hex(p)} ${g.pub.ip} ${g.pub.port} ${l.ip} ${l.port}`);
    }
    if (list) {
      let n = 0;
      for (const [other, game] of this.games) {
        if (!game.room || other === id || n++ >= MAX_ROOMS) continue;
        const r = game.room;
        out.push(`room ${hex(other)} ${r.max} ${r.players} ${r.build}\t${r.name}`);
      }
    }
    return new Response(out.join("\n") + "\n", { headers: { "content-type": "text/plain" } });
  }
}

function parseId(text) {
  const v = parseInt(text || "", 16);
  return Number.isInteger(v) && v > 0 && v <= 0xffffff ? v : null;
}

function parseEndpoint(ip, port) {
  const p = parseInt(port || "", 10);
  if (!/^\d{1,3}(\.\d{1,3}){3}$/.test(ip || "") || !(p > 0 && p < 65536)) return null;
  return { ip, port: p };
}

function clamp(text, lo, hi) {
  const v = parseInt(text || "", 10);
  return Number.isInteger(v) ? Math.min(hi, Math.max(lo, v)) : lo;
}

function hex(id) {
  return id.toString(16).padStart(6, "0");
}
