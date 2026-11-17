"""Checks load-test error accounting against real loopback TCP peers."""
import asyncio
import importlib.util
import json
import pathlib
import struct
import unittest

ROOT=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("stress",ROOT/"tests/stress.py")
stress=importlib.util.module_from_spec(spec)
spec.loader.exec_module(stress)


class StressTest(unittest.IsolatedAsyncioTestCase):
    def test_percentiles_use_observed_latencies(self):
        self.assertEqual(stress.percentile([10,20,30,40,50,60,70,80,90,100],50),50)
        self.assertEqual(stress.percentile([10,20,30,40,50,60,70,80,90,100],95),100)
        self.assertIsNone(stress.percentile([],99))

    async def test_server_rejection_never_becomes_success(self):
        async def reject(reader,writer):
            try:
                size=struct.unpack("!I",await reader.readexactly(4))[0]
                request=json.loads(await reader.readexactly(size))
                body=json.dumps({"version":1,"type":request["type"],"request_id":request["request_id"],"ok":False,"error":{"code":"BUSY","message":"busy"}}).encode()
                writer.write(struct.pack("!I",len(body))+body);await writer.drain()
            finally:writer.close();await writer.wait_closed()
        server=await asyncio.start_server(reject,"127.0.0.1",0)
        peer=stress.Peer()
        try:
            await peer.connect("127.0.0.1",server.sockets[0].getsockname()[1])
            with self.assertRaises(stress.RequestRejected):await peer.call("ping")
        finally:await peer.close();server.close();await server.wait_closed()

    async def test_connection_loss_and_bad_frame_cannot_count_as_response(self):
        async def invalid(reader,writer):
            await reader.read(2048);writer.write(struct.pack("!I",65537));await writer.drain();writer.close();await writer.wait_closed()
        server=await asyncio.start_server(invalid,"127.0.0.1",0)
        peer=stress.Peer()
        try:
            await peer.connect("127.0.0.1",server.sockets[0].getsockname()[1])
            with self.assertRaises(ConnectionError):await peer.call("ping")
        finally:await peer.close();server.close();await server.wait_closed()


if __name__=="__main__":unittest.main()
