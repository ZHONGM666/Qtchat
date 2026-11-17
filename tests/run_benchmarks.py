"""Explicit, isolated 100/500/1000 authenticated-connection benchmarks."""
import argparse
import asyncio
import hashlib
import json
import os
import pathlib
import platform
import resource
import ssl
import sqlite3
import subprocess
import tempfile
import time
from wire import Server

ROOT = pathlib.Path(__file__).resolve().parents[1]
from stress import TEST_PASSWORD, run_load


def save(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True)
    parser.add_argument("--output", type=pathlib.Path, default=ROOT / "tests/results/benchmark.json")
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--release", action="store_true", help="build and measure a server-only Release target")
    args = parser.parse_args()
    if not args.execute and not (ROOT / "tests/results/run-benchmarks.flag").exists():
        print("SKIP: performance runs require --execute or tests/results/run-benchmarks.flag", flush=True)
        return 77
    if args.release:
        build = ROOT / "build/qtchat-release"
        subprocess.run(["cmake", "-S", str(ROOT), "-B", str(build), "-DQTCHAT_BUILD_CLIENT=OFF", "-DBUILD_TESTING=OFF", "-DCMAKE_BUILD_TYPE=Release"], check=True, timeout=120)
        subprocess.run(["cmake", "--build", str(build), "-j2"], check=True, timeout=180)
        args.server = str(build / "qtchat_server")
    salt = os.urandom(16)
    password_hash = hashlib.pbkdf2_hmac("sha256", TEST_PASSWORD.encode(), salt, 600000).hex()
    info = {"platform": platform.platform(), "os_release": platform.freedesktop_os_release(),
            "build_type": "Release" if args.release else "provided binary", "executable": str(pathlib.Path(args.server).resolve()),
            "compiler": subprocess.run(["g++", "--version"], capture_output=True, text=True, check=True).stdout.splitlines()[0],
            "sqlite": sqlite3.sqlite_version, "openssl": ssl.OPENSSL_VERSION,
            "cpu_count": os.cpu_count(), "nofile": resource.getrlimit(resource.RLIMIT_NOFILE),
            "memory": pathlib.Path("/proc/meminfo").read_text().splitlines()[0],
            "fixture": "isolated preseeded synthetic users share a disposable benchmark credential hash; normal 600000-iteration server authentication; setup excluded",
            "database_location": "Linux local temporary directory, not shared hgfs", "results": []}
    info["source_sha256"] = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                             for folder in ("common", "server") for path in sorted((ROOT / folder).glob("*")) if path.suffix in {".h", ".cpp"}}
    started = time.monotonic()
    save(args.output, info)
    with tempfile.TemporaryDirectory(prefix="qtchat-bench-") as directory:
        for count in (100, 500, 1000):
            for mode in ("idle", "chat"):
                key = str(count) + "-" + mode
                database = pathlib.Path(directory) / (key + ".db")
                initializer = Server(args.server, database=database)
                initializer.close()
                with sqlite3.connect(database) as db:
                    db.execute("PRAGMA foreign_keys=ON")
                    db.executemany("INSERT INTO users(username,display_name,salt,hash,iterations) VALUES(?,?,?,?,600000)",
                                   [("bench_" + str(i), "Bench " + str(i), salt.hex(), password_hash) for i in range(count)])
                    db.executemany("INSERT INTO friendships(user_low,user_high) VALUES(?,?)", [(i, i + 1) for i in range(1, count, 2)])
                server = Server(args.server, database=database)
                print("BENCHMARK", key, "starting", flush=True)
                try:
                    def progress(value):
                        save(ROOT / "tests/results/benchmark-progress.json", dict(case=key, **value))
                    result = asyncio.run(run_load("127.0.0.1", server.port, count, 60, mode, "bench_", prepared=True, progress=progress))
                finally:
                    server.close()
                info["results"].append(result)
                info["elapsed_seconds"] = round(time.monotonic() - started, 2)
                save(args.output, info)
                print("BENCHMARK", key, json.dumps(result), flush=True)
                if not result["passed"]:
                    print("Resource/error boundary reached; higher load levels were not attempted.", flush=True)
                    return 1
    save(ROOT / "tests/results/benchmark-progress.json", {"stage": "complete", "cases": 6})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
