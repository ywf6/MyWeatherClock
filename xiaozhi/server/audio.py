"""Raw Opus v1 packets: 16 kHz mono PCM16, 60 ms per frame."""
from fractions import Fraction
import struct
import av
RATE, FRAME_SAMPLES, FRAME_BYTES, MAX_PACKET = 16000, 960, 1920, 1275

class Encoder:
    def __init__(self):
        self.codec=av.CodecContext.create("libopus","w")
        self.codec.sample_rate=RATE
        self.codec.layout="mono"
        self.codec.format="s16"
        self.codec.bit_rate=24000
        self.codec.time_base=Fraction(1,RATE)
        self.codec.options={"application":"voip","frame_duration":"60"}
        self.codec.open()
        self.buffer=bytearray()
        self.pts=0
    def feed(self,pcm):
        self.buffer.extend(pcm)
        while len(self.buffer)>=FRAME_BYTES:
            raw=bytes(self.buffer[:FRAME_BYTES]);del self.buffer[:FRAME_BYTES]
            frame=av.AudioFrame(format="s16",layout="mono",samples=FRAME_SAMPLES)
            frame.sample_rate=RATE;frame.time_base=Fraction(1,RATE);frame.pts=self.pts
            self.pts+=FRAME_SAMPLES
            frame.planes[0].update(raw)
            yield from (bytes(p) for p in self.codec.encode(frame))
    def finish(self):
        if self.buffer: yield from self.feed(bytes(FRAME_BYTES-len(self.buffer)))
        yield from (bytes(p) for p in self.codec.encode(None))

class Decoder:
    def __init__(self):
        self.codec=av.CodecContext.create("opus","r")
        self.codec.extradata=b"OpusHead"+struct.pack("<BBHIhB",1,1,0,RATE,0,0)
        self.codec.open()
        self.resampler=av.AudioResampler(format="s16",layout="mono",rate=RATE)
    def feed(self,packet):
        if not 0<len(packet)<=MAX_PACKET: raise ValueError("Invalid Opus packet length")
        return b"".join(bytes(f.planes[0])[:f.samples*2] for decoded in self.codec.decode(av.Packet(packet))
                        for f in self.resampler.resample(decoded))

class Resampler24k:
    def __init__(self):
        self.resampler=av.AudioResampler(format="s16",layout="mono",rate=RATE)
        self.tail=b""
    def feed(self,pcm):
        pcm=self.tail+pcm;self.tail=pcm[len(pcm)&~1:];pcm=pcm[:len(pcm)&~1]
        if not pcm:return b""
        frame=av.AudioFrame(format="s16",layout="mono",samples=len(pcm)//2)
        frame.sample_rate=24000;frame.planes[0].update(pcm)
        return b"".join(bytes(f.planes[0])[:f.samples*2] for f in self.resampler.resample(frame))
    def finish(self):
        if self.tail:raise ValueError("Incomplete PCM16 sample")
        return b"".join(bytes(f.planes[0])[:f.samples*2] for f in self.resampler.resample(None))
