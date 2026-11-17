"""Networked gateway tests using real Opus packets and a simulated STM32 bridge."""
import asyncio,json,math,struct,unittest
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed
from audio import Encoder,Decoder,Resampler24k
from server import handle,bitmap
from dashboard import Dashboard

TOKEN="test-token-0123456789"
HELLO={"type":"hello","version":1,"transport":"websocket","features":{"mcp":True},
       "audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}}

class ProtocolTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.dashboard=Dashboard("echo",None)
        self.server=await serve(lambda ws:handle(ws,"echo",TOKEN,self.dashboard),"127.0.0.1",0,max_size=8192)
        port=self.server.sockets[0].getsockname()[1]
        self.url=f"ws://127.0.0.1:{port}/xiaozhi/v1/"
    async def asyncTearDown(self):
        self.server.close();await self.server.wait_closed()
    async def connected(self):
        return await connect(self.url,additional_headers={"Authorization":"Bearer "+TOKEN,"Device-Id":"00:11:22:33:44:55"})
    async def reply_mcp(self,ws,event):
        p=event["payload"];method=p.get("method")
        if "id" not in p:return
        if method=="initialize":result={"protocolVersion":"2024-11-05","capabilities":{"tools":{}},"serverInfo":{"name":"simulator","version":"1"}}
        elif method=="tools/list":result={"tools":[{"name":"self.weatherclock.get_environment","inputSchema":{"type":"object","properties":{}}}]}
        else:result={"content":[{"type":"text","text":"@WC1 STATE page=CHAT rev=7 valid=15 temp10=235 humidity10=500"}],"isError":False}
        await ws.send(json.dumps({"type":"mcp","payload":{"jsonrpc":"2.0","id":p["id"],"result":result}}))
    async def test_echo_opus_mcp_and_complete_bitmap(self):
        async with await self.connected() as ws:
            # Fragmented WebSocket text must still be accepted.
            hello=json.dumps(HELLO);await ws.send([hello[:30],hello[30:]])
            first=json.loads(await ws.recv());self.assertEqual(first["type"],"hello")
            sid=first["session_id"]
            await ws.send(json.dumps({"type":"listen","state":"start","mode":"manual","session_id":sid,"wc_revision":7}))
            pcm=b"".join(struct.pack("<h",int(6000*math.sin(2*math.pi*440*i/16000))) for i in range(960*4))
            enc=Encoder()
            for packet in list(enc.feed(pcm))+list(enc.finish()):await ws.send(packet)
            await ws.send(json.dumps({"type":"listen","state":"stop","session_id":sid}))
            decoded=bytearray();dec=Decoder();bits=bytearray();started=stopped=ended=False
            expected_sum=0
            async with asyncio.timeout(12):
                while not (ended and stopped):
                    raw=await ws.recv()
                    if isinstance(raw,bytes):decoded.extend(dec.feed(raw));continue
                    event=json.loads(raw);kind=event["type"]
                    if kind=="mcp":await self.reply_mcp(ws,event)
                    elif kind=="tts":
                        started|=event["state"]=="start";stopped|=event["state"]=="stop"
                    elif kind=="wc_lcd":
                        self.assertEqual(event["revision"],7)
                        if event["op"]=="start":expected_sum=event["sum"]
                        elif event["op"]=="data":
                            self.assertEqual(event["offset"],len(bits));bits.extend(bytes.fromhex(event["hex"]))
                        else:ended=True
                        await ws.send(json.dumps({"type":"wc_lcd_ack","ticket":event["ticket"],"ok":True,"session_id":sid}))
                    elif kind=="alert":self.fail(event)
            self.assertTrue(started);self.assertGreater(len(decoded),len(pcm)*0.7)
            self.assertEqual(len(bits),5616);self.assertEqual(sum(bits),expected_sum)
            async with asyncio.timeout(2):
                while not self.dashboard.history:await asyncio.sleep(.01)
            state=self.dashboard.snapshot()
            self.assertEqual(state["completed"],1)
            self.assertEqual(state["devices"][0]["phase"],"done")
            self.assertTrue(state["devices"][0]["tools_ready"])
            self.assertEqual(state["history"][0]["screen"],"confirmed")
            self.assertIn("回声测试成功",state["history"][0]["answer"])
        async with asyncio.timeout(2):
            while self.dashboard.devices:await asyncio.sleep(.01)
        self.assertEqual(len(self.dashboard.history),1)
    async def test_bad_token_rejected(self):
        async with connect(self.url,additional_headers={"Authorization":"Bearer wrong"}) as ws:
            with self.assertRaises(ConnectionClosed):await ws.recv()
            self.assertEqual(ws.close_code,1008)
    async def test_invalid_audio_contract_rejected(self):
        async with await self.connected() as ws:
            hello=dict(HELLO,audio_params=dict(HELLO["audio_params"],sample_rate=48000))
            await ws.send(json.dumps(hello))
            with self.assertRaises(ConnectionClosed):await ws.recv()
            self.assertEqual(ws.close_code,1008)
    async def test_audio_before_hello_is_not_a_turn(self):
        async with await self.connected() as ws:
            await ws.send(b"\xf8\xff\xfe")
            await ws.send(json.dumps(HELLO))
            self.assertEqual(json.loads(await ws.recv())["type"],"hello")
    async def test_stale_session_rejected(self):
        async with await self.connected() as ws:
            await ws.send(json.dumps(HELLO));await ws.recv()
            await ws.send(json.dumps({"type":"listen","state":"start","session_id":"stale"}))
            with self.assertRaises(ConnectionClosed):
                while True:await ws.recv()
            self.assertEqual(ws.close_code,1008)
    def test_resampler_partial_samples(self):
        converter=Resampler24k()
        output=converter.feed(bytes(997))+converter.feed(bytes(1883))+converter.finish()
        self.assertEqual(len(output),1920)
    def test_bitmap_size(self):
        self.assertEqual(len(bitmap("温度23.5度，湿度50%。")),5616)

if __name__=="__main__":unittest.main(verbosity=2)
