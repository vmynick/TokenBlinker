#!/usr/bin/env python3
"""
Átlátszó proxy: Claude Code -> api.anthropic.com, közben UDP-n jelez a token-LED-nek.
Nem hív modellt, csak a már futó streamet figyeli.

pip install aiohttp
python3 token_proxy.py
Claude Code: ANTHROPIC_BASE_URL=http://127.0.0.1:8787
"""
import asyncio, json, os, socket
from aiohttp import web, ClientSession, ClientTimeout

UPSTREAM    = os.getenv("UPSTREAM", "https://api.anthropic.com")
LED_HOST    = os.getenv("LED_HOST", "tokenled.local")
LED_PORT    = int(os.getenv("LED_PORT", "4210"))
LISTEN_HOST = os.getenv("HOST", "127.0.0.1")
LISTEN_PORT = int(os.getenv("PORT", "8787"))
CHARS_PER_TOKEN = 4

HOP = {"host", "content-length", "transfer-encoding", "connection",
       "keep-alive", "content-encoding", "accept-encoding"}

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
led_addr = None
pending = {"t": 0, "k": 0, "j": 0}   # szöveg / thinking / tool input


def add(kind, chars=0, tokens=None):
    pending[kind] += tokens if tokens is not None else max(1, round(chars / CHARS_PER_TOKEN))


def on_sse(raw: bytes):
    try:
        ev = json.loads(raw)
    except ValueError:
        return
    if ev.get("type") != "content_block_delta":
        return
    d = ev.get("delta", {})
    t = d.get("type")
    if t == "text_delta":
        add("t", len(d.get("text", "")))
    elif t == "thinking_delta":
        add("k", len(d.get("thinking", "")))
    elif t == "input_json_delta":
        add("j", len(d.get("partial_json", "")))


async def resolver():
    global led_addr
    loop = asyncio.get_running_loop()
    while True:
        try:
            ip = await loop.run_in_executor(None, socket.gethostbyname, LED_HOST)
            led_addr = (ip, LED_PORT)
        except OSError:
            pass
        await asyncio.sleep(60)


async def flusher():
    while True:
        await asyncio.sleep(0.05)
        for k, n in pending.items():
            if n:
                pending[k] = 0
                if led_addr:
                    try:
                        sock.sendto(f"C {k} {n}".encode(), led_addr)
                    except OSError:
                        pass


async def handle(req: web.Request):
    headers = {k: v for k, v in req.headers.items() if k.lower() not in HOP}
    headers["accept-encoding"] = "identity"
    body = await req.read()
    session: ClientSession = req.app["session"]

    up = await session.request(req.method, UPSTREAM + req.rel_url.path_qs,
                               headers=headers, data=body, allow_redirects=False)
    try:
        resp = web.StreamResponse(
            status=up.status,
            headers={k: v for k, v in up.headers.items() if k.lower() not in HOP})
        await resp.prepare(req)

        ctype = up.headers.get("content-type", "")
        sse = "text/event-stream" in ctype
        buf = b""
        async for chunk in up.content.iter_any():
            await resp.write(chunk)
            if sse:
                buf += chunk
                *lines, buf = buf.split(b"\n")
                for line in lines:
                    if line.startswith(b"data:"):
                        on_sse(line[5:])
            elif "json" in ctype:
                buf += chunk

        if not sse and "json" in ctype and up.status == 200:
            try:
                n = json.loads(buf).get("usage", {}).get("output_tokens", 0)
                if n:
                    add("t", tokens=n)
            except ValueError:
                pass

        await resp.write_eof()
        return resp
    finally:
        up.release()


async def on_startup(app):
    app["session"] = ClientSession(timeout=ClientTimeout(total=None, sock_read=900))
    app["tasks"] = [asyncio.create_task(resolver()), asyncio.create_task(flusher())]


async def on_cleanup(app):
    for t in app["tasks"]:
        t.cancel()
    await app["session"].close()


app = web.Application(client_max_size=200 * 1024 * 1024)
app.router.add_route("*", "/{tail:.*}", handle)
app.on_startup.append(on_startup)
app.on_cleanup.append(on_cleanup)

if __name__ == "__main__":
    web.run_app(app, host=LISTEN_HOST, port=LISTEN_PORT, access_log=None)
