"""Zoom into a WAV comparison around a specific timestamp."""
import sys, struct, math

def read_wav(path):
    with open(path, "rb") as f: data = f.read()
    pos = 12; fmt = {}; pcm = b""
    while pos < len(data):
        chunk = data[pos:pos+4].decode("ascii", errors="replace")
        size  = struct.unpack_from("<I", data, pos+4)[0]
        if chunk == "fmt ":
            fmt["channels"] = struct.unpack_from("<H", data, pos+10)[0]
            fmt["rate"]     = struct.unpack_from("<I", data, pos+12)[0]
            fmt["bits"]     = struct.unpack_from("<H", data, pos+22)[0]
        elif chunk == "data": pcm = data[pos+8:pos+8+size]
        pos += 8 + size
    return fmt, pcm

def pcm_to_mono(pcm, bits, channels):
    step = bits // 8
    n = len(pcm) // step // channels
    result = []
    for i in range(n):
        s = 0.0
        for c in range(channels):
            raw = pcm[(i*channels+c)*step : (i*channels+c+1)*step]
            v = struct.unpack("<h", raw)[0] / 32768.0 if bits == 16 else 0.0
            s += v
        result.append(s / channels)
    return result

def rms(block):
    if not block: return 0.0
    return math.sqrt(sum(x*x for x in block) / len(block))

def to_db(r):
    return f"{20*math.log10(r):+.1f}" if r > 1e-10 else "-inf"

nfmt, npcm = read_wav(sys.argv[1])
rfmt, rpcm = read_wav(sys.argv[2])
rate = nfmt["rate"]

# Extract 2 second window centred on the anomaly
centre_ms = int(sys.argv[3]) if len(sys.argv) > 3 else 24600
window_ms = 2000
start_s = max(0, (centre_ms - window_ms//2) / 1000)
end_s   = start_s + window_ms / 1000

n_mono = pcm_to_mono(npcm, nfmt["bits"], nfmt["channels"])
r_mono = pcm_to_mono(rpcm, rfmt["bits"], rfmt["channels"])

si = int(start_s * rate)
ei = int(end_s   * rate)
block = 10 * rate // 1000   # 10ms blocks

print(f"Zoom: {start_s*1000:.0f}–{end_s*1000:.0f} ms, 10ms blocks")
print(f"{'t(ms)':>7}  {'Native':>7}  {'Ref':>7}  Note")
print("-"*45)
for i in range((ei-si)//block):
    s = si + i*block; e = s+block; ms = int(start_s*1000 + i*10)
    nr = rms(n_mono[s:e]); rr = rms(r_mono[s:e])
    d = (20*math.log10(nr/rr) if nr>1e-10 and rr>1e-10 else 0)
    flag = " <<< ANOMALY" if abs(d)>3 else (" <" if abs(d)>1.5 else "")
    print(f"{ms:7}  {to_db(nr):>7}  {to_db(rr):>7}  {d:+.1f} dB{flag}")
