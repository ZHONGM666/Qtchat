"""Persistence, app-level acknowledgement loss, and authorization acceptance."""
import argparse
import json
import os
import pathlib
import sqlite3
import subprocess
import tempfile
import time
import uuid
from wire import Client, Server, frame, success

ROOT = pathlib.Path(__file__).resolve().parents[1]


def exercise(executable):
    clients = []
    with tempfile.TemporaryDirectory(prefix="qtchat-e2e-") as directory:
        database = pathlib.Path(directory) / "persist.db"
        server = Server(executable, database=database)
        try:
            def connect():
                client = Client(server.port)
                clients.append(client)
                return client
            a, b, c = connect(), connect(), connect()
            users = []
            for client, name in ((a, "alice"), (b, "bobby"), (c, "carol")):
                user = success(client.call("register", {"username": name, "display_name": name, "password": "Secret-123"}))["user"]
                success(client.call("login", {"username": name, "password": "Secret-123"}))
                users.append(user["id"])
            request = success(a.call("friend_request", {"peer_id": users[1]}))
            success(b.call("friend_respond", {"request_id": request["request_id"], "accept": True}))
            b.close()
            first_data = {"peer_id": users[1], "client_id": str(uuid.uuid4()), "text": "离线消息\n<em>按纯文本处理</em>"}
            first = success(a.call("message_send", first_data))["message"]
            b = connect()
            success(b.call("login", {"username": "bobby", "password": "Secret-123"}))
            assert success(b.call("history", {"peer_id": users[0]}))["messages"][0]["text"] == first_data["text"]
            uncertain = {"peer_id": users[1], "client_id": str(uuid.uuid4()), "text": "已保存但客户端未处理确认"}
            a.socket.sendall(frame(a.request("message_send", uncertain)))
            deadline = time.monotonic() + 5
            while True:
                with sqlite3.connect(database) as db:
                    committed = db.execute("SELECT count(*) FROM messages WHERE client_id=?", (uncertain["client_id"],)).fetchone()[0]
                if committed:
                    break
                assert time.monotonic() < deadline, "message was not committed"
                time.sleep(0.01)
            a.close()
            a = connect()
            success(a.call("login", {"username": "alice", "password": "Secret-123"}))
            resent = success(a.call("message_send", uncertain))
            assert resent["duplicate"] is True
            assert len(success(b.call("history", {"peer_id": users[0], "after_id": first["id"]}))["messages"]) == 1
            success(b.call("read", {"peer_id": users[0], "up_to_id": first["id"]}))
            assert success(b.call("conversations"))["conversations"][0]["unread"] == 1
            assert success(c.call("history", {"peer_id": users[1]}))["messages"] == []
            spoof = dict(uncertain, sender_id=users[0])
            assert c.call("message_send", spoof)["error"]["code"] == "INVALID_REQUEST"
            success(b.call("logout"))
            assert b.call("friend_list")["error"]["code"] == "UNAUTHENTICATED"
            server.close(force=True)
        finally:
            for client in clients:
                client.close()
            server.close()
        with sqlite3.connect(database) as db:
            assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
            assert db.execute("SELECT count(*) FROM messages").fetchone()[0] == 2
            rows = db.execute("SELECT salt,hash FROM users").fetchall()
            assert len({row[0] for row in rows}) == 3
            assert all("Secret-123" not in value for row in rows for value in row)
        restarted = Server(executable, database=database)
        try:
            client = Client(restarted.port)
            success(client.call("login", {"username": "bobby", "password": "Secret-123"}))
            assert len(success(client.call("friend_list"))["friends"]) == 1
            assert len(success(client.call("history", {"peer_id": users[0]}))["messages"]) == 2
            client.close()
        finally:
            restarted.close()
    print("PASS SIGKILL recovery, offline delivery, acknowledgement-loss deduplication, unread boundary and authorization", flush=True)


def clean_verification():
    env = dict(os.environ, QTCHAT_IN_CLEAN_BUILD="1")
    for name, client, sanitize in (("qtchat-final", True, False), ("qtchat-sanitize", False, True)):
        if sanitize:
            env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
            env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        build = ROOT / "build" / name
        commands = [
            ["cmake", "-S", str(ROOT), "-B", str(build), "-DBUILD_TESTING=ON", "-DCMAKE_BUILD_TYPE=Debug",
             "-DQTCHAT_BUILD_CLIENT=" + ("ON" if client else "OFF"), "-DQTCHAT_SANITIZE=" + ("ON" if sanitize else "OFF")],
            ["cmake", "--build", str(build), "--clean-first", "-j2"],
            ["ctest", "--test-dir", str(build), "--output-on-failure", "-E", "benchmark_suite"],
        ]
        for command in commands:
            print("VERIFY", name, command[:3], flush=True)
            subprocess.run(command, cwd=ROOT, env=env, check=True, timeout=300)
        print("PASS", name, flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    args = parser.parse_args()
    exercise(args.server)
    if (ROOT / "tests/results/clean-build.flag").exists() and not os.environ.get("QTCHAT_IN_CLEAN_BUILD"):
        clean_verification()


if __name__ == "__main__":
    main()
