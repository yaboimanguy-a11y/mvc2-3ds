# The online lobby (Cloudflare Worker)

The 3DS app's online VS finds rooms through this small server: the public
room list, the 6-digit room codes, and passing each joiner's address to the
room's host so both consoles reach each other through their routers. The
matches themselves go directly between the consoles; nothing about the game
goes through here.

It runs on Cloudflare's free Workers plan (100,000 requests a day: a room
being open for an hour costs about 720, a player browsing the list for a
minute about 6).

## Deploy it (about five minutes)

1. Make a free account at <https://dash.cloudflare.com/sign-up>.
2. Install Node.js (<https://nodejs.org>, the LTS version).
3. In a terminal, from this folder (`port/server/lobby`):

   ```
   npx wrangler login
   npx wrangler deploy
   ```

   `login` opens the browser to allow Wrangler on your account. `deploy`
   prints the address of the lobby, like
   `https://mvc2-lobby.yourname.workers.dev`.
4. Check it: open `https://mvc2-lobby.yourname.workers.dev/v1/ping` in a
   browser; it says `ok`.
5. Give the app that address: build with
   `-DMVC2_LOBBY_URL=https://mvc2-lobby.yourname.workers.dev`, or put the
   address alone in `sdmc:/3ds/mvc2/lobby_url.txt` on each console.

Updating it later is `npx wrangler deploy` again. The rooms live in memory
(a Durable Object); if Cloudflare restarts it, the hosts' next check-in
(every 5 seconds) puts their rooms back with the same codes.

## Test it without an account

```
node test.mjs
```

runs the Worker's code in Node against a list of checks (opening, listing,
joining by code, private rooms, other versions refused, full rooms, rooms
that stop checking in, the limits).

## The leaderboard

After every fight, both players' consoles send `/v1/report` with their
player id, its secret key (the first report registers it; only a hash is
kept), the match and the result. A fight counts when the two reports agree;
then both ratings move (Elo, 1000 to start, K 40 for a player's first 20
fights, then 24). The same two players count at most 20 times a day.
`/v1/leaderboard` and `/v1/player` read it; `/` shows it as a web page.

## What it stores

For each open room, while it is open (in memory only): its name (the
host's 3DS user name), mode, public or private, version, player count, the
host's internet and local addresses, and the addresses of players joining
it in the last 30 seconds. A room that stops checking in is forgotten after
45 seconds.

For the leaderboard (in the Durable Object's SQLite storage): each player's
id, a SHA-256 hash of their key, their 3DS user name, rating, wins, losses,
draws, current and best win streak, and when they last reported; reports
waiting for the other player's (up to 15 minutes); and per pair of players,
how many fights counted today.
