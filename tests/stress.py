"""QtChat load generator. Use only isolated test servers, not a user database."""
import argparse
import asyncio
import json
import math
import pathlib
import struct
import time
import uuid

TEST_PASSWORD = "QtChat-Benchmark-123"


class RequestRejected(Exception):
    pass


def percentile(values, percent):
    if not values:
        return None
    ordered = sorted(values)
    return round(ordered[max(0, math.ceil(len(ordered) * percent / 100) - 1)], 3)


class Peer:
    def __init__(self):
        self.reader = self.writer = self.task = None
        self.pending = {}
        self.alive = False
        self.closing = False
        self.deliveries = 0
        self.user = None

    async def connect(self, host, port):
        self.reader, self.writer = await asyncio.wait_for(asyncio.open_connection(host, port), 10)
        self.alive = True
        self.task = asyncio.create_task(self.receive())

    async def receive(self):
        try:
            while True:
                size = struct.unpack("!I", await self.reader.readexactly(4))[0]
                if not 0 < size <= 65536:
                    raise ValueError("invalid response frame length")
                reply = json.loads(await self.reader.readexactly(size))
                if reply.get("event"):
                    if reply.get("type") == "message":
                        self.deliveries += 1
                    continue
                future = self.pending.get(reply.get("request_id"))
                if future is not None and not future.done():
                    future.set_result(reply)
        except asyncio.CancelledError:
            raise
        except Exception as error:
            self.alive = False
            for future in list(self.pending.values()):
                if not future.done():
                    future.set_exception(ConnectionError(str(error)))

    async def call(self, kind, data=None):
        if not self.alive:
            raise ConnectionError("peer is not connected")
        request_id = str(uuid.uuid4())
        future = asyncio.get_running_loop().create_future()
        self.pending[request_id] = future
        try:
            payload = json.dumps({"version": 1, "type": kind, "request_id": request_id,
                                  "data": data or {}}, ensure_ascii=False, separators=(",", ":")).encode()
            if not 0 < len(payload) <= 65536:
                raise ValueError("request exceeds protocol frame limit")
            self.writer.write(struct.pack("!I", len(payload)) + payload)
            await self.writer.drain()
            reply = await asyncio.wait_for(future, 120)
            if reply.get("ok") is not True:
                raise RequestRejected(reply.get("error", {}).get("code", "invalid response"))
            return reply["data"]
        finally:
            self.pending.pop(request_id, None)

    async def close(self):
        self.closing = True
        self.alive = False
        if self.task:
            self.task.cancel()
            await asyncio.gather(self.task, return_exceptions=True)
        if self.writer:
            self.writer.close()
            try:
                await self.writer.wait_closed()
            except OSError:
                pass


async def run_load(host, port, connections, seconds, mode, prefix, prepared=False, progress=None):
    if connections < 1 or seconds < 1 or mode not in {"idle", "chat"} or (mode == "chat" and connections < 2):
        raise ValueError("invalid load parameters")
    peers = [None] * connections
    heartbeats = []
    latencies = []
    errors = 0
    measuring = False
    setup_started = time.perf_counter()
    semaphore = asyncio.Semaphore(32)

    async def heartbeat(peer):
        nonlocal errors
        while peer.alive:
            await asyncio.sleep(15)
            try:
                started = time.perf_counter()
                await peer.call("ping")
                if measuring and mode == "idle":
                    latencies.append((time.perf_counter() - started) * 1000)
            except Exception:
                errors += 1
                return

    async def prepare(index):
        nonlocal errors
        async with semaphore:
            peer = Peer()
            try:
                await peer.connect(host, port)
                username = prefix + str(index)
                if not prepared:
                    await peer.call("register", {"username": username, "display_name": "Benchmark " + str(index), "password": TEST_PASSWORD})
                result = await peer.call("login", {"username": username, "password": TEST_PASSWORD})
                peer.user = result["user"]
                peers[index] = peer
                heartbeats.append(asyncio.create_task(heartbeat(peer)))
            except Exception:
                errors += 1
                await peer.close()

    async def monitor():
        while True:
            if progress:
                progress({"stage": "steady" if measuring else "authentication", "connected": sum(p is not None and p.alive for p in peers),
                          "samples": len(latencies), "elapsed_seconds": round(time.perf_counter() - setup_started, 1)})
            await asyncio.sleep(5)

    monitor_task = asyncio.create_task(monitor())
    started = None
    try:
        await asyncio.gather(*(prepare(i) for i in range(connections)))
        connected = sum(peer is not None for peer in peers)
        if mode == "chat" and not prepared:
            for i in range(0, connections, 2):
                a, b = peers[i], peers[i + 1] if i + 1 < connections else peers[0]
                if a is None or b is None:
                    continue
                try:
                    request = await a.call("friend_request", {"peer_id": b.user["id"]})
                    await b.call("friend_respond", {"request_id": request["request_id"], "accept": True})
                except Exception:
                    errors += 1
        setup_seconds = time.perf_counter() - setup_started
        measuring = True
        started = time.perf_counter()
        deadline = started + seconds

        async def chat(index, peer):
            nonlocal errors
            partner = peers[index ^ 1] if index ^ 1 < connections else peers[0]
            if partner is None:
                errors += 1
                return
            while time.perf_counter() < deadline and peer.alive:
                try:
                    before = time.perf_counter()
                    await peer.call("message_send", {"peer_id": partner.user["id"], "client_id": str(uuid.uuid4()), "text": "QtChat benchmark " + "x" * 110})
                    latencies.append((time.perf_counter() - before) * 1000)
                except Exception:
                    errors += 1
                    return

        if mode == "chat":
            await asyncio.gather(*(chat(i, peer) for i, peer in enumerate(peers) if peer is not None))
            await asyncio.sleep(0.3)
        else:
            await asyncio.sleep(seconds)
        duration = time.perf_counter() - started
        surviving = sum(peer is not None and peer.alive for peer in peers)
        delivered = sum(peer.deliveries for peer in peers if peer is not None)
        accepted = len(latencies) if mode == "chat" else 0
        result = {"mode": mode, "requested_connections": connections, "authenticated_connections": connected,
                  "surviving_connections": surviving, "setup_seconds": round(setup_seconds, 3),
                  "requested_seconds": seconds, "steady_seconds": round(duration, 3), "accepted_messages": accepted,
                  "delivered_messages": delivered, "errors": errors, "messages_per_second": round(accepted / duration, 2),
                  "latency_kind": "message_ack" if mode == "chat" else "ping_rtt", "latency_samples": len(latencies),
                  "latency_ms": {"p50": percentile(latencies, 50), "p95": percentile(latencies, 95), "p99": percentile(latencies, 99)}}
        result["passed"] = connected == connections and surviving == connections and errors == 0 and (mode != "chat" or delivered == accepted)
        return result
    finally:
        monitor_task.cancel()
        for task in heartbeats:
            task.cancel()
        await asyncio.gather(monitor_task, *heartbeats, return_exceptions=True)
        await asyncio.gather(*(peer.close() for peer in peers if peer is not None))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9527)
    parser.add_argument("--connections", type=int, default=100)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--mode", choices=("idle", "chat"), default="idle")
    parser.add_argument("--prepared", action="store_true")
    parser.add_argument("--prefix", default="bench_" + uuid.uuid4().hex[:6] + "_")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    if not 1 <= args.connections <= 10000 or not 1 <= args.seconds <= 600 or not 1 <= args.port <= 65535:
        parser.error("connections must be 1..10000, seconds 1..600, port 1..65535")
    result = asyncio.run(run_load(args.host, args.port, args.connections, args.seconds, args.mode, args.prefix, args.prepared))
    text = json.dumps(result, indent=2)
    print(text)
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
