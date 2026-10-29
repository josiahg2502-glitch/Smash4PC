# Smash 4 PC netplay rooms

`worker/` is a Cloudflare Worker (one Durable Object) that keeps the public room list and introduces games to each
other. No game traffic goes through it: a host checks in every couple of seconds while its game takes guests, a
guest asks for the room it chose, and both learn their public UDP address from a STUN server (stun.l.google.com).
They then send packets to each other's public and local addresses until one gets through (hole punching), and the
stream goes directly between them. It fits Cloudflare's free plan.

Players whose routers rewrite ports per destination (some mobile and carrier-grade connections) can't be reached
this way; they can still join over a LAN or a VPN (Tailscale, ZeroTier) with `smash.exe --join ADDRESS`.

```bash
cd worker
npm install
npx wrangler login     # once, in a browser
npx wrangler deploy    # prints https://smash4pc-online.<your-subdomain>.workers.dev
npx wrangler dev       # or run it locally at http://127.0.0.1:8787 (SMASH_ROOM_SERVER=http://127.0.0.1:8787)
```

The address goes in `kRoomServer` (runtime/src/host/rooms.h).
