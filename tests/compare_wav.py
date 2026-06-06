"""compare_wav.py — block-by-block amplitude envelope comparison of two WAV files.

Usage:
  python tests/compare_wav.py <native.wav> <reference.wav> [--block-ms 100]
"""

import sys, struct, math, argparse

def read_wav(path):
    with open(path, "rb") as f:
        data = f.read()
    # Parse RIFF header
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    pos = 12
    fmt = {}
    pcm = b""
    while pos < len(data):
        chunk = data[pos:pos+4].decode("ascii", errors="replace")
        size  = struct.unpack_from("<I", data, pos+4)[0]
        if chunk == "fmt ":
            fmt["audio_fmt"]   = struct.unpack_from("<H", data, pos+8)[0]
            fmt["channels"]    = struct.unpack_from("<H", data, pos+10)[0]
            fmt["rate"]        = struct.unpack_from("<I", data, pos+12)[0]
            fmt["bits"]        = struct.unpack_from("<H", data, pos+22)[0]
        elif chunk == "data":
            pcm = data[pos+8 : pos+8+size]
        pos += 8 + size
    return fmt, pcm

def to_floats(pcm, bits, channels):
    n = len(pcm) // (bits // 8) // channels
    samples = [[] for _ in range(channels)]
    step = bits // 8
    for i in range(n * channels):
        off = i * step
        if bits == 16:
            v = struct.unpack_from("<h", pcm, off)[0] / 32768.0
        elif bits == 8:
            v = (struct.unpack_from("B", pcm, off)[0] - 128) / 128.0
        else:
            raise ValueError(f"Unsupported bit depth {bits}")
        samples[i % channels].append(v)
    return samples   # list of channels, each a list of floats

def rms(block):
    if not block:
        return 0.0
    s = sum(x*x for x in block) / len(block)
    return math.sqrt(s)

def to_db(r):
    if r < 1e-10:
        return "-inf"
    return f"{20 * math.log10(r):+6.1f}"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("native")
    ap.add_argument("reference")
    ap.add_argument("--block-ms", type=int, default=200)
    args = ap.parse_args()

    print(f"Loading {args.native} ...")
    nfmt, npcm = read_wav(args.native)
    print(f"Loading {args.reference} ...")
    rfmt, rpcm = read_wav(args.reference)

    assert nfmt["rate"] == rfmt["rate"], "Sample rate mismatch"
    rate     = nfmt["rate"]
    block_sz = rate * args.block_ms // 1000

    nch = to_floats(npcm, nfmt["bits"], nfmt["channels"])
    rch = to_floats(rpcm, rfmt["bits"], rfmt["channels"])

    # Mix to mono for comparison
    n_mono = [(nch[0][i] + nch[1][i]) * 0.5 for i in range(min(len(nch[0]), len(nch[1])))]
    r_mono = [(rch[0][i] + rch[1][i]) * 0.5 for i in range(min(len(rch[0]), len(rch[1])))]

    n_blocks = len(n_mono) // block_sz
    r_blocks = len(r_mono) // block_sz
    blocks   = min(n_blocks, r_blocks)

    print(f"\nBlock size: {args.block_ms} ms ({block_sz} samples)")
    print(f"Blocks:     {blocks}  ({blocks * args.block_ms / 1000:.1f} s)")
    print(f"{'t(ms)':>6}  {'Native':>7}  {'Ref':>7}  {'Delta':>6}  Note")
    print("-" * 50)

    large_diffs = 0
    for b in range(blocks):
        s = b * block_sz
        e = s + block_sz
        nr = rms(n_mono[s:e])
        rr = rms(r_mono[s:e])
        nd = to_db(nr)
        rd = to_db(rr)
        if nr < 1e-10 and rr < 1e-10:
            note = "both silent"
        elif nr < 1e-10:
            note = "NATIVE SILENT"
        elif rr < 1e-10:
            note = "REF SILENT"
        else:
            delta_db = 20 * math.log10(nr / rr)
            if abs(delta_db) > 4:
                note = f"*** LARGE DIFF {delta_db:+.1f} dB ***"
                large_diffs += 1
            elif abs(delta_db) > 2:
                note = f"< diff {delta_db:+.1f} dB"
            else:
                note = f"ok ({delta_db:+.1f} dB)"
        print(f"{b*args.block_ms:6}  {nd:>7}  {rd:>7}  {note}")

    # Overall stats
    nr_all = rms(n_mono)
    rr_all = rms(r_mono)
    print("-" * 50)
    print(f"Overall:  Native {to_db(nr_all)} dB  |  Ref {to_db(rr_all)} dB")
    if nr_all > 1e-10 and rr_all > 1e-10:
        delta = 20 * math.log10(nr_all / rr_all)
        print(f"Level delta: {delta:+.2f} dB")
    print(f"Large-diff blocks (>4 dB): {large_diffs}/{blocks}")

    # Silence structure comparison
    n_sil = [rms(n_mono[b*block_sz:(b+1)*block_sz]) < 1e-3 for b in range(blocks)]
    r_sil = [rms(r_mono[b*block_sz:(b+1)*block_sz]) < 1e-3 for b in range(blocks)]
    mismatch = sum(1 for a, b in zip(n_sil, r_sil) if a != b)
    print(f"Silence-structure mismatches: {mismatch}/{blocks}")

if __name__ == "__main__":
    main()
