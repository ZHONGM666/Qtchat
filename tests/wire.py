"""Independent Python protocol client for real TCP integration checks."""
import json
import pathlib
import select
import socket
import struct
import subprocess
import tempfile
import time


def frame(value):
    payload=json.dumps(value,ensure_ascii=False,separators=(",", ":")).encode()
    return struct.pack("!I",len(payload))+payload


class Client:
    def __init__(self,port):
        self.socket=socket.create_connection(("127.0.0.1",port),timeout=5)
        self.socket.settimeout(5)
        self.counter=0
        self.events=[]

    def receive(self):
        def exact(count):
            data=b""
            while len(data)<count:
                part=self.socket.recv(count-len(data))
                if not part:raise EOFError("connection closed")
                data+=part
            return data
        size=struct.unpack("!I",exact(4))[0]
        assert 0<size<=65536,"oversize response frame"
        return json.loads(exact(size))

    def request(self,kind,data=None):
        self.counter+=1
        return {"version":1,"type":kind,"request_id":str(self.counter),"data":data or {}}

    def call(self,kind,data=None):
        request=self.request(kind,data)
        self.socket.sendall(frame(request))
        while True:
            reply=self.receive()
            if reply.get("event"):
                self.events.append(reply)
            elif reply.get("request_id")==request["request_id"]:
                return reply

    def close(self):
        self.socket.close()


class Server:
    def __init__(self,executable,extra=(),database=None):
        self.directory=tempfile.TemporaryDirectory(prefix="qtchat-integration-")
        self.log_path=pathlib.Path(self.directory.name)/"server.log"
        self.log=self.log_path.open("w")
        self.process=subprocess.Popen([str(pathlib.Path(executable).resolve()),"--port","0","--db",str(database or pathlib.Path(self.directory.name)/"test.db"),*extra],stdout=subprocess.PIPE,stderr=self.log,text=True)
        assert select.select([self.process.stdout],[],[],10)[0],"server startup timed out"
        line=self.process.stdout.readline().strip()
        assert line.startswith("LISTENING "),"server did not start: "+line
        self.port=int(line.split()[1])

    def close(self, force=False):
        if getattr(self, "closed", False):
            return
        self.closed = True
        timed_out = False
        if self.process.poll() is None:
            if force: self.process.kill()
            else: self.process.terminate()
            try:self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                timed_out = True
                self.process.kill();self.process.wait()
        self.log.close()
        diagnostic = self.log_path.read_text(encoding="utf-8", errors="replace")
        failed = timed_out or (not force and self.process.returncode != 0)
        if failed:
            failures = pathlib.Path(__file__).resolve().parents[1] / "tests/results/failures"
            failures.mkdir(parents=True, exist_ok=True)
            saved = failures / ("shutdown-" + str(time.time_ns()) + ".log")
            saved.write_text(diagnostic, encoding="utf-8")
        self.directory.cleanup()
        assert not failed, f"server shutdown failed: rc={self.process.returncode}, timeout={timed_out}; log={saved}\n{diagnostic}"


def success(reply):
    assert reply.get("ok"),reply
    return reply["data"]
