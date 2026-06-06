"""Quick MOD header dump."""
import sys, struct

path = sys.argv[1] if len(sys.argv) > 1 else r"MODS\1987\mod.Sleepwalk"
with open(path, "rb") as f: d = f.read()

name        = d[0:20].rstrip(b"\x00").decode("latin1")
song_len    = d[950]
restart_pos = d[951]
magic       = d[1080:1084].decode("ascii", errors="replace")
num_orders  = song_len
orders      = list(d[952:952+song_len])
num_pats    = max(orders)+1

print(f"Song:        {name!r}")
print(f"Magic:       {magic}")
print(f"Song length: {song_len} orders")
print(f"Restart pos: {restart_pos} (0x{restart_pos:02X})")
print(f"Patterns:    {num_pats}")
print(f"Order table: {orders[:16]}{'...' if song_len>16 else ''}")

# Estimate duration at BPM=125, speed=6
# samplesPerTick = 44100*60/(125*24) = 882
# rowDuration = 6*882 = 5292 samples = 120ms
row_ms = 120
total_rows = num_orders * 64
print(f"\nEstimated song length (no loop): {total_rows * row_ms / 1000:.1f}s")
print(f"Restart order: {restart_pos}")
if restart_pos < song_len:
    pre_loop_rows = restart_pos * 64
    print(f"Intro length: {pre_loop_rows * row_ms / 1000:.1f}s")
    loop_rows = (song_len - restart_pos) * 64
    print(f"Loop length:  {loop_rows * row_ms / 1000:.1f}s")
