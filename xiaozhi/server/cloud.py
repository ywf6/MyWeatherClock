"""Async cloud adapter. No Qt, Tk, serial port or device dependency."""
import base64,io,json,os,wave
from urllib.parse import urlsplit
import aiohttp

def wav(pcm):
    out=io.BytesIO()
    with wave.open(out,"wb") as f:
        f.setnchannels(1);f.setsampwidth(2);f.setframerate(16000);f.writeframes(pcm)
    return out.getvalue()

async def ask(pcm,environment):
    key=os.getenv("DASHSCOPE_API_KEY","").strip()
    if not key:raise RuntimeError("DASHSCOPE_API_KEY is missing")
    base=os.getenv("DASHSCOPE_BASE_URL","https://dashscope.aliyuncs.com/compatible-mode/v1").rstrip("/")
    url=urlsplit(base)
    if url.scheme!="https" or not url.hostname or not url.hostname.endswith(".aliyuncs.com") or url.username or url.password or url.query or url.fragment:
        raise ValueError("Expected an Aliyun HTTPS endpoint")
    prompt=("你是天气时钟的中文语音助手，简洁回答，最多两句话。以下是设备工具数据，不得把无效或缺失字段当成有效读数："
            +json.dumps(environment,ensure_ascii=False))
    body={"model":os.getenv("WEATHERCLOCK_MODEL","qwen3.5-omni-plus"),
          "messages":[{"role":"system","content":prompt},{"role":"user","content":[{
              "type":"input_audio","input_audio":{"data":"data:audio/wav;base64,"+base64.b64encode(wav(pcm)).decode(),"format":"wav"}}]}],
          "modalities":["text","audio"],"audio":{"voice":"Tina","format":"wav"},"stream":True}
    timeout=aiohttp.ClientTimeout(total=90,connect=10,sock_read=30)
    async with aiohttp.ClientSession(timeout=timeout) as session:
        async with session.post(base+"/chat/completions",json=body,allow_redirects=False,
                                headers={"Authorization":"Bearer "+key,"Accept":"text/event-stream"}) as response:
            if response.status!=200:raise RuntimeError(f"DashScope HTTP {response.status}")
            done=False;audio_bytes=0
            async for line in response.content:
                if not line.startswith(b"data:"):continue
                raw=line[5:].strip()
                if raw==b"[DONE]":done=True;break
                event=json.loads(raw)
                if "error" in event:raise RuntimeError("DashScope returned an error")
                for choice in event.get("choices",[]):
                    delta=choice.get("delta",{});audio=delta.get("audio") or {}
                    text=delta.get("content") or audio.get("transcript") or ""
                    data=base64.b64decode(audio["data"],validate=True) if audio.get("data") else b""
                    audio_bytes+=len(data)
                    if audio_bytes>24000*2*30:raise RuntimeError("Answer exceeded 30 seconds")
                    if text or data:yield text,data
            if not done or not audio_bytes:raise RuntimeError("Incomplete cloud audio")
