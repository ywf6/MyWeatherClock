"""Self-hosted Xiaozhi WebSocket v1 gateway: echo or DashScope audio."""
import argparse,asyncio,contextlib,hmac,json,logging,os,secrets,uuid
from pathlib import Path
from time import monotonic
from dashboard import Dashboard, serve_dashboard
from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed
from audio import Encoder,Decoder,Resampler24k
import cloud
LOG=logging.getLogger("wc-xiaozhi")
ROOT=Path(__file__).resolve().parents[1]
MAX_RECORD_BYTES=16000*2*15

def bitmap(text):
    from PIL import Image,ImageDraw,ImageFont
    font=ImageFont.truetype(os.getenv("WC_FONT_PATH","C:/Windows/Fonts/msyh.ttc"),18)
    image=Image.new("1",(216,208),0);draw=ImageDraw.Draw(image)
    line="";y=4
    for ch in ("语音回答\n"+text)[:400]:
        if ch=="\n" or draw.textlength(line+ch,font=font)>208:
            draw.text((4,y),line,font=font,fill=1,anchor="lt");y+=25
            line="" if ch=="\n" else ch
            if y>182:break
        else:line+=ch
    if y<=182 and line:draw.text((4,y),line,font=font,fill=1,anchor="lt")
    return image.tobytes()

class Session:
    def __init__(self,ws,backend):
        self.ws,self.backend=ws,backend;self.id=uuid.uuid4().hex
        self.ready=self.recording=False;self.decoder=None;self.pcm=bytearray()
        self.turn=None;self.pending={};self.sequence=0;self.revision=0;self.tools=[]
        self.mcp_ready=asyncio.Event();self.generation=0;self.warmup=None
        self.dashboard=None

    def status(self, **fields):
        if self.dashboard is not None:
            self.dashboard.update(self.id, **fields)
    async def send(self,kind,**fields):
        await self.ws.send(json.dumps({"type":kind,"session_id":self.id,**fields},ensure_ascii=False))
    async def rpc(self,method,params=None):
        self.sequence+=1;ident=self.sequence;future=asyncio.get_running_loop().create_future()
        self.pending[("mcp",ident)]=future
        try:
            await self.send("mcp",payload={"jsonrpc":"2.0","id":ident,"method":method,"params":params or {}})
            result=await asyncio.wait_for(future,5)
            if "error" in result:raise RuntimeError("Device MCP error")
            return result["result"]
        finally:self.pending.pop(("mcp",ident),None)
    async def tool(self,name):
        result=await self.rpc("tools/call",{"name":name,"arguments":{}})
        if result.get("isError"):raise RuntimeError("STM32 tool call failed")
        content=result.get("content",[])
        return content[0].get("text","") if content else ""
    async def warm_tools(self):
        try:
            await self.rpc("initialize",{"protocolVersion":"2024-11-05","capabilities":{},
                                        "clientInfo":{"name":"weatherclock","version":"0.1"}})
            await self.send("mcp",payload={"jsonrpc":"2.0","method":"notifications/initialized"})
            self.tools=(await self.rpc("tools/list")).get("tools",[])
            LOG.info("MCP ready: %s",[t.get("name") for t in self.tools])
            self.status(tools_ready=True)
        except (TimeoutError,RuntimeError,ConnectionClosed):LOG.warning("Device tools unavailable")
        finally:self.mcp_ready.set()
    async def cancel(self):
        self.generation+=1;self.recording=False;self.pcm.clear()
        if self.turn:
            self.turn.cancel()
            with contextlib.suppress(asyncio.CancelledError):await self.turn
            self.turn=None
    async def lcd(self,text,revision):
        if not revision:return
        bits=bitmap(text);image_id=secrets.randbelow(2**30)
        commands=[{"op":"start","sum":sum(bits)}]
        commands += [{"op":"data","offset":i,"hex":bits[i:i+96].hex()} for i in range(0,len(bits),96)]
        commands += [{"op":"end"}]
        for index,command in enumerate(commands):
            ticket=f"{image_id}:{index}";future=asyncio.get_running_loop().create_future()
            self.pending[("lcd",ticket)]=future
            try:
                await self.send("wc_lcd",ticket=ticket,id=image_id,revision=revision,**command)
                try:
                    ack=await asyncio.wait_for(future,4)
                except TimeoutError:
                    LOG.warning("LCD acknowledgment timed out at %s, block %s",command["op"],index)
                    return False
                if not ack.get("ok"):
                    LOG.warning("LCD command rejected at %s, block %s",command["op"],index)
                    return False
            finally:self.pending.pop(("lcd",ticket),None)
        return True
    async def respond(self,pcm,revision,generation):
        started=monotonic()
        self.status(phase="processing")
        try:
            LOG.info("Processing %.2fs of audio, backend=%s",len(pcm)/32000,self.backend)
            environment={}
            await asyncio.wait_for(self.mcp_ready.wait(),6)
            if any(t.get("name")=="self.weatherclock.get_environment" for t in self.tools):
                try:environment=await self.tool("self.weatherclock.get_environment")
                except (RuntimeError,TimeoutError):LOG.warning("Snapshot unavailable")
            encoder=Encoder();next_frame=asyncio.get_running_loop().time()
            first_packet=True
            async def send_packet(packet):
                nonlocal next_frame, first_packet
                if self.generation!=generation:raise asyncio.CancelledError
                if first_packet:
                    self.status(phase="playing")
                    first_packet=False
                await asyncio.sleep(max(0,next_frame-asyncio.get_running_loop().time()))
                await self.ws.send(packet)
                next_frame=max(next_frame,asyncio.get_running_loop().time())+0.060
            await self.send("tts",state="start",wc_revision=revision)
            if self.backend=="echo":
                answer="回声测试成功：这是刚才录入的声音。"
                await self.send("stt",text="本地回声验证，不调用云端")
                await self.send("tts",state="sentence_start",text=answer)
                for packet in encoder.feed(pcm):await send_packet(packet)
            else:
                converter=Resampler24k();text=[]
                async for delta,audio in cloud.ask(pcm,environment):
                    if delta:text.append(delta)
                    if audio:
                        for packet in encoder.feed(converter.feed(audio)):await send_packet(packet)
                for packet in encoder.feed(converter.finish()):await send_packet(packet)
                answer="".join(text).strip() or "已收到语音回答。"
                await self.send("tts",state="sentence_start",text=answer)
            for packet in encoder.finish():await send_packet(packet)
            await self.send("tts",state="stop")
            self.status(phase="displaying" if revision else "idle")
            screen_ok=await self.lcd(answer,revision)
            if self.dashboard is not None:
                screen="confirmed" if screen_ok else ("unconfirmed" if revision else "skipped")
                self.dashboard.complete(self.id,answer,len(pcm)/32000,monotonic()-started,screen)
            LOG.info("Turn completed: input %.2fs, backend=%s, screen=%s",len(pcm)/32000,self.backend,
                     "confirmed" if screen_ok else "unconfirmed")
        except asyncio.CancelledError:raise
        except Exception as exc:
            self.status(phase="error")
            LOG.warning("Turn failed: %s",type(exc).__name__)
            with contextlib.suppress(ConnectionClosed):
                await self.send("alert",title="Conversation failed",message=str(exc)[:160],level="error")
                await self.send("tts",state="stop")
    async def receive(self,message):
        if isinstance(message,bytes):
            if not self.ready or not self.recording:return
            self.pcm.extend(self.decoder.feed(message))
            if len(self.pcm)>MAX_RECORD_BYTES:raise ValueError("Recording exceeded 15 seconds")
            return
        event=json.loads(message)
        if not isinstance(event,dict):raise ValueError("Expected JSON object")
        kind=event.get("type")
        if kind=="hello":
            params=event.get("audio_params",{})
            if self.ready or event.get("version")!=1 or event.get("transport")!="websocket":raise ValueError("Expected protocol v1")
            if any(params.get(k)!=v for k,v in {"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}.items()):
                raise ValueError("Expected Opus/16k/mono/60ms")
            self.ready=True
            if self.dashboard is not None:
                self.dashboard.connect(self.id,self.ws.request.headers.get("Device-Id","ESP32-C3"))
            await self.send("hello",transport="websocket",audio_params=params)
            self.warmup=asyncio.create_task(self.warm_tools());return
        if not self.ready:raise ValueError("hello required")
        if event.get("session_id") not in (None,self.id):raise ValueError("Stale session")
        if kind=="mcp":
            payload=event.get("payload",{});future=self.pending.get(("mcp",payload.get("id")))
            if future and not future.done():future.set_result(payload)
        elif kind=="wc_lcd_ack":
            future=self.pending.get(("lcd",event.get("ticket")))
            if future and not future.done():future.set_result(event)
        elif kind=="abort":
            await self.cancel();self.status(phase="idle");await self.send("tts",state="stop")
        elif kind=="listen":
            if event.get("state")=="start":
                await self.cancel();self.decoder=Decoder();self.recording=True
                self.revision=int(event.get("wc_revision",0))
                self.status(phase="recording")
            elif event.get("state")=="stop" and self.recording:
                self.recording=False;pcm=bytes(self.pcm);self.pcm.clear()
                if len(pcm)<3200:
                    self.status(phase="too_short")
                    await self.send("alert",title="Too short",message="请再说一次",level="warning")
                else:self.turn=asyncio.create_task(self.respond(pcm,self.revision,self.generation))

async def handle(ws,backend,token,dashboard=None):
    if token and not hmac.compare_digest(ws.request.headers.get("Authorization",""),"Bearer "+token):
        await ws.close(1008,"Unauthorized");return
    if ws.request.path!="/xiaozhi/v1/":await ws.close(1008,"Invalid path");return
    session=Session(ws,backend)
    session.dashboard=dashboard
    LOG.info("Device connected: %s",ws.request.headers.get("Device-Id","unknown"))
    try:
        async for message in ws:await session.receive(message)
    except (ValueError,json.JSONDecodeError):await ws.close(1008,"Invalid protocol")
    except ConnectionClosed:pass
    finally:
        if dashboard is not None:
            dashboard.disconnect(session.id)
        await session.cancel()
        if session.warmup:
            session.warmup.cancel()
            with contextlib.suppress(asyncio.CancelledError):await session.warmup
        LOG.info("Device disconnected")

async def main(args):
    config=json.loads((ROOT/"config.local.json").read_text()) if (ROOT/"config.local.json").exists() else {}
    token=os.getenv("WEATHERCLOCK_XIAOZHI_TOKEN",config.get("device_token",""))
    if args.host not in ("127.0.0.1","::1") and not token:raise SystemExit("LAN requires device_token in config.local.json")
    if args.backend=="dashscope" and not os.getenv("DASHSCOPE_API_KEY"):raise SystemExit("Set DASHSCOPE_API_KEY")
    dashboard=Dashboard(args.backend,os.getenv("WEATHERCLOCK_MODEL","qwen3.5-omni-plus"))
    async with serve(lambda ws:handle(ws,args.backend,token,dashboard),args.host,args.port,max_size=8192,max_queue=16,ping_interval=20,ping_timeout=20):
        async with serve_dashboard(dashboard,args.dashboard_port):
            LOG.info("Ready: ws://%s:%d/xiaozhi/v1/ backend=%s",args.host,args.port,args.backend)
            LOG.info("Dashboard: http://127.0.0.1:%d/",args.dashboard_port)
            await asyncio.Future()

if __name__=="__main__":
    parser=argparse.ArgumentParser()
    parser.add_argument("--host",default="127.0.0.1");parser.add_argument("--port",type=int,default=8000)
    parser.add_argument("--dashboard-port",type=int,default=8001)
    parser.add_argument("--backend",choices=["echo","dashscope"],default="echo")
    logging.basicConfig(level=logging.INFO,format="%(asctime)s %(levelname)s %(message)s")
    asyncio.run(main(parser.parse_args()))
