import argparse
import socket
import struct
import time
import pathlib
import select
import sqlite3
from wire import Client,Server,frame,success


def main():
    parser=argparse.ArgumentParser();parser.add_argument("--server",required=True);args=parser.parse_args()
    server=Server(args.server,["--heartbeat-timeout","2"])
    try:
        client=Client(server.port)
        first=client.request("ping")
        wire=frame(first);client.socket.sendall(wire[:2]);client.socket.settimeout(0.05)
        try:client.socket.recv(1);raise AssertionError("half prefix produced response")
        except socket.timeout:pass
        client.socket.settimeout(5);client.socket.sendall(wire[2:]);success(client.receive())
        one,two=client.request("ping"),client.request("ping")
        client.socket.sendall(frame(one)+frame(two))
        assert [client.receive()["request_id"],client.receive()["request_id"]]==[one["request_id"],two["request_id"]]
        for bad in (struct.pack("!I",65537),struct.pack("!I",0),struct.pack("!I",1)+b"{"):
            broken=Client(server.port);broken.socket.sendall(bad)
            try:broken.receive();raise AssertionError("invalid wire input accepted")
            except (EOFError,ConnectionResetError):pass
            broken.close();success(client.call("ping"))
        reg=client.request("register",{"username":"alice","display_name":"Alice","password":"Secret-123"})
        login=client.request("login",{"username":"alice","password":"Secret-123"})
        friends=client.request("friend_list")
        client.socket.sendall(frame(reg)+frame(login)+frame(friends))
        for request in (reg,login,friends):
            response=client.receive();assert response["request_id"]==request["request_id"];success(response)
        replacement=Client(server.port);success(replacement.call("login",{"username":"alice","password":"Secret-123"}))
        assert client.receive()["type"]=="session_replaced"
        try:client.receive();raise AssertionError("replaced session stayed open")
        except (EOFError,ConnectionResetError):pass
        client.close()
        half=Client(server.port);half.socket.sendall(frame(half.request("ping")));half.socket.shutdown(socket.SHUT_WR)
        success(half.receive());half.close()
        idle=Client(server.port)
        deadline=time.monotonic()+3.2
        while time.monotonic()<deadline:
            success(replacement.call("ping"));time.sleep(0.35)
        try:idle.receive();raise AssertionError("idle connection did not expire")
        except (EOFError,ConnectionResetError):pass
        idle.close();success(replacement.call("friend_list"));replacement.close()
        print("PASS framing, ordered requests, replacement, half close, heartbeat expiry and survival")
    finally:server.close()
    bounded=Server(args.server,["--max-pending","1","--max-inflight","2","--max-output","4096"])
    try:
        flood=Client(bounded.port)
        flood.socket.sendall(b"".join(frame(flood.request("register",{"username":"user"+str(i),"display_name":"Name","password":"Secret-123"})) for i in range(20)))
        other=Client(bounded.port);start=time.monotonic();success(other.call("ping"));assert time.monotonic()-start<1.5
        replies=[]
        for _ in range(20):
            try:replies.append(flood.receive())
            except (EOFError,ConnectionResetError):break
        assert any(reply.get("error",{}).get("code")=="BUSY" for reply in replies),"capacity overload was not rejected"
        flood.close();other.close();print("PASS bounded overload keeps other connections responsive")
    finally:bounded.close()
    pending=Server(args.server)
    try:
        database=pathlib.Path(pending.directory.name)/"test.db"
        with sqlite3.connect(database) as lock:
            lock.execute("BEGIN IMMEDIATE")
            old=Client(pending.port)
            old.socket.sendall(frame(old.request("register",{"username":"unfinished","display_name":"Old","password":"Secret-123"})))
            time.sleep(0.3)
            old.close()
            fresh=Client(pending.port)
            success(fresh.call("ping"))
            lock.rollback()
        deadline=time.monotonic()+5
        while True:
            with sqlite3.connect(database) as db:
                done=db.execute("SELECT count(*) FROM users WHERE username='unfinished'").fetchone()[0]
            if done:break
            assert time.monotonic()<deadline,"old connection task did not finish"
            time.sleep(0.02)
        assert not select.select([fresh.socket],[],[],0.3)[0],"old completion reached a new connection"
        success(fresh.call("ping"));fresh.close()
        print("PASS unfinished old task cannot deliver to replacement connection")
    finally:pending.close()
    slow=Server(args.server,["--max-pending","512","--max-output","65540"])
    try:
        sender,receiver=Client(slow.port),Client(slow.port)
        a=success(sender.call("register",{"username":"reader","display_name":"Reader","password":"Secret-123"}))["user"]["id"]
        b=success(receiver.call("register",{"username":"writer","display_name":"Writer","password":"Secret-123"}))["user"]["id"]
        success(sender.call("login",{"username":"reader","password":"Secret-123"}))
        success(receiver.call("login",{"username":"writer","password":"Secret-123"}))
        request=success(sender.call("friend_request",{"peer_id":b}))
        success(receiver.call("friend_respond",{"request_id":request["request_id"],"accept":True}))
        import uuid
        for _ in range(12):
            success(sender.call("message_send",{"peer_id":b,"client_id":str(uuid.uuid4()),"text":"x"*8192}))
        def socket_count():
            return sum(path.readlink().as_posix().startswith("socket:[") for path in pathlib.Path(f"/proc/{slow.process.pid}/fd").iterdir())
        before=socket_count()
        sender.socket.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4096)
        sender.socket.sendall(b"".join(frame(sender.request("history",{"peer_id":b})) for _ in range(150)))
        time.sleep(1)
        start=time.monotonic();success(receiver.call("ping"));assert time.monotonic()-start<1.5
        # TCP may retain already-sent kernel bytes long after server close.
        # Observe the actual server descriptor while this peer never drains.
        deadline=time.monotonic()+5
        while socket_count() >= before and time.monotonic()<deadline:time.sleep(0.02)
        remaining=socket_count()
        print("slow-reader server socket descriptors:",before,"->",remaining)
        assert remaining==before-1,"slow reader was not disconnected at output cap"
        sender.close();success(receiver.call("ping"));receiver.close()
        print("PASS slow reader output is bounded and another connection stays responsive")
    finally:slow.close()


if __name__=="__main__":main()
