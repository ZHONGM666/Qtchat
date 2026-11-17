"""Local shared-folder CMake/CTest helper. No socket or shell interface.

Start manually in the Linux guest: python3 tests/linux_verify.py
Stop at any time with Ctrl+C. Requests and results stay in tests/results.
"""
import datetime
import json
import os
import pathlib
import platform
import re
import shlex
import shutil
import subprocess
import sys
import time


def command_plan(request, root):
    if not isinstance(request, dict) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", str(request.get("id", ""))):
        raise ValueError("request id must be 1..64 letters, digits, underscores or hyphens")
    if set(request) - {"id", "action", "client", "test_regex", "sanitize"}:
        raise ValueError("unsupported request fields")
    action = request.get("action")
    if action not in {"env", "build", "test", "build_test", "stop"}:
        raise ValueError("only env/build/test/build_test/stop are supported")
    for flag in ("client", "sanitize"):
        if flag in request and type(request[flag]) is not bool:
            raise ValueError(flag + " must be a boolean")
    if not isinstance(request.get("test_regex", ""), str) or len(request.get("test_regex", "")) > 128:
        raise ValueError("test_regex must be a string of at most 128 characters")
    build = str(root / "build" / ("qtchat-sanitize" if request.get("sanitize") else "qtchat-cmake"))
    commands = []
    if action in {"build", "build_test"}:
        configure = ["cmake", "-S", str(root), "-B", build, "-DBUILD_TESTING=ON",
                     "-DCMAKE_BUILD_TYPE=Debug", "-DQTCHAT_BUILD_CLIENT=" + ("ON" if request.get("client") else "OFF")]
        if request.get("sanitize"):
            configure.append("-DQTCHAT_SANITIZE=ON")
        commands.extend([configure, ["cmake", "--build", build, "-j2"]])
    if action in {"test", "build_test"}:
        test = ["ctest", "--test-dir", build]
        if request.get("test_regex"):
            test += ["-R", request["test_regex"]]
        commands.append(test + ["--output-on-failure"])
    return commands


def run_commands(commands, log_path, root):
    with log_path.open("w", encoding="utf-8") as log:
        for command in commands:
            log.write("$ " + shlex.join(command) + "\n")
            log.flush()
            try:
                result = subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT,
                                        timeout=900, check=False)
            except FileNotFoundError as error:
                log.write(str(error) + "\n")
                return 127
            except subprocess.TimeoutExpired:
                log.write("Command exceeded the 900 second limit\n")
                return 124
            log.write("exit_code=" + str(result.returncode) + "\n")
            log.flush()
            if result.returncode:
                return result.returncode
    return 0


def write_json(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    temporary.replace(path)


def environment(root):
    info = {"project_root": str(root), "platform": platform.platform(), "python": sys.version,
            "cpu_count": os.cpu_count(), "utc": datetime.datetime.now(datetime.timezone.utc).isoformat()}
    for tool in ("cmake", "g++", "python3", "pkg-config"):
        executable = shutil.which(tool)
        info[tool] = {"path": executable}
        if executable:
            result = subprocess.run([executable, "--version"], capture_output=True, text=True, timeout=10)
            info[tool]["version"] = (result.stdout or result.stderr).splitlines()[:1]
    info["headers"] = {header: pathlib.Path("/usr/include", header).is_file()
                       for header in ("sqlite3.h", "openssl/evp.h", "boost/circular_buffer.hpp")}
    qt_prefixes = [path.parent.parent.parent.parent for path in
                   pathlib.Path.home().glob("Qt/*/gcc_64/lib/cmake/Qt6/Qt6Config.cmake")]
    info["qt_prefixes"] = [str(path) for path in qt_prefixes]
    if len(qt_prefixes) == 1:
        os.environ["CMAKE_PREFIX_PATH"] = str(qt_prefixes[0])
    if shutil.which("pkg-config"):
        for package in ("sqlite3", "openssl", "Qt6Widgets"):
            result = subprocess.run(["pkg-config", "--modversion", package], capture_output=True,
                                    text=True, timeout=10)
            info[package] = {"version": result.stdout.strip(), "exit_code": result.returncode}
    return info


def main():
    if sys.platform != "linux":
        raise SystemExit("Run this helper in the Linux virtual machine, not Windows.")
    root = pathlib.Path(__file__).resolve().parents[1]
    output = root / "tests/results" / "linux-helper"
    output.mkdir(parents=True, exist_ok=True)
    write_json(output / "environment.json", environment(root))
    write_json(output / "status.json", {"state": "ready", "pid": os.getpid()})
    print("QtChat build helper ready:", root, flush=True)
    print("CMake/CTest requests only. Stop with Ctrl+C.", flush=True)
    previous = None
    try:
        while True:
            request_file = output / "request.json"
            if request_file.is_file():
                text = request_file.read_text(encoding="utf-8-sig")
                if text != previous:
                    try:
                        request = json.loads(text)
                        commands = command_plan(request, root)
                        result_file = output / (request["id"] + ".json")
                        if not result_file.exists():
                            write_json(output / "status.json", {"state": "running", "id": request["id"]})
                            started = time.monotonic()
                            if request["action"] == "env":
                                write_json(output / "environment.json", environment(root))
                            code = run_commands(commands, output / (request["id"] + ".log"), root)
                            result = {"id": request["id"], "exit_code": code,
                                      "seconds": round(time.monotonic() - started, 3)}
                            write_json(result_file, result)
                            write_json(output / "status.json", {"state": "ready", "last_result": result})
                            print(result, flush=True)
                            if request["action"] == "stop":
                                break
                        previous = text
                    except (ValueError, OSError) as error:
                        write_json(output / "status.json", {"state": "request_error", "error": str(error)})
                        previous = text
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("QtChat build helper stopped.", flush=True)
    finally:
        write_json(output / "status.json", {"state": "stopped"})


if __name__ == "__main__":
    main()
