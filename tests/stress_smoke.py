"""Short real-server check before spending minutes on a full load run."""
import argparse
import asyncio
import json
import pathlib
from wire import Client, Server, success

ROOT = pathlib.Path(__file__).resolve().parents[1]
from stress import TEST_PASSWORD, run_load


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    args = parser.parse_args()
    server = Server(args.server)
    try:
        users = []
        for i in range(4):
            client = Client(server.port)
            username = "smoke_" + str(i)
            success(client.call("register", {"username": username, "display_name": username, "password": TEST_PASSWORD}))
            user = success(client.call("login", {"username": username, "password": TEST_PASSWORD}))["user"]
            users.append((client, user))
        for i in (0, 2):
            a, b = users[i], users[i + 1]
            request = success(a[0].call("friend_request", {"peer_id": b[1]["id"]}))
            success(b[0].call("friend_respond", {"request_id": request["request_id"], "accept": True}))
        for client, _ in users:
            client.close()
        results = []
        for mode in ("idle", "chat"):
            result = asyncio.run(run_load("127.0.0.1", server.port, 4, 1, mode, "smoke_", prepared=True))
            assert result["passed"], result
            if mode == "chat":
                assert result["accepted_messages"] > 0
                assert result["delivered_messages"] == result["accepted_messages"]
            results.append(result)
        output = ROOT / "tests/results/stress-smoke.json"
        output.parent.mkdir(exist_ok=True)
        output.write_text(json.dumps(results, indent=2), encoding="utf-8")
        print("PASS real authenticated idle/chat load and delivery accounting", flush=True)
    finally:
        server.close()


if __name__ == "__main__":
    main()
