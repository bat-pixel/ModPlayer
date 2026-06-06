"""Dump the pattern/row playing at a given timestamp for a MOD file."""
import sys, struct

def read_mod(path):
    with open(path, "rb") as f: d = f.read()
    song_len    = d[950]
    restart_pos = d[951]
    orders      = list(d[952:952+song_len])
    num_pats    = max(orders)+1

    # 31-sample MOD: each sample header is 30 bytes
    samp_hdrs = []
    for i in range(31):
        h = d[20 + i*30 : 20 + (i+1)*30]
        vol  = h[25]
        fine = h[24] & 0xF
        fine = fine - 16 if fine >= 8 else fine
        samp_hdrs.append({"vol": vol, "fine": fine})

    pat_data = d[1084:]
    pats = []
    for p in range(num_pats):
        rows = []
        off = p * 64 * 4 * 4
        for r in range(64):
            row = []
            for c in range(4):
                b0,b1,b2,b3 = pat_data[off:off+4]; off+=4
                samp  = (b0 & 0xF0) | (b2 >> 4)
                period= ((b0 & 0x0F) << 8) | b1
                eff   = b2 & 0xF
                param = b3
                row.append((samp, period, eff, param))
            rows.append(row)
        pats.append(rows)

    return {"song_len": song_len, "restart": restart_pos, "orders": orders, "pats": pats, "samp": samp_hdrs}

NOTE_NAMES = ["C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"]
PT_PERIODS = [
    856,808,762,720,678,640,604,570,538,508,480,453,
    428,404,381,360,339,320,302,285,269,254,240,226,
    214,202,190,180,170,160,151,143,135,127,120,113,
]
def period_to_note(p):
    if p == 0: return "---"
    best = min(range(len(PT_PERIODS)), key=lambda i: abs(PT_PERIODS[i]-p))
    oct_ = best//12 + 1
    return f"{NOTE_NAMES[best%12]}{oct_}"

def eff_str(e, p):
    if e == 0 and p == 0: return "   "
    return f"{e:X}{p:02X}"

mod = read_mod(sys.argv[1])
target_ms = float(sys.argv[2]) if len(sys.argv) > 2 else 23640.0
span_rows = int(sys.argv[3]) if len(sys.argv) > 3 else 8

# Compute row at target_ms (BPM=125, speed=6 default — may change with Fxx)
# Simple: constant 120ms/row
row_ms = 120.0
target_row = int(target_ms / row_ms)
start_row  = max(0, target_row - span_rows//2)
end_row    = start_row + span_rows

print(f"Target: {target_ms:.0f}ms ~= global row {target_row}")
print(f"Showing global rows {start_row}–{end_row}\n")

orders = mod["orders"]
pats   = mod["pats"]

print(f"{'t(ms)':>6}  {'ord':>3} pat row  {'CH1':^18} {'CH2':^18} {'CH3':^18} {'CH4':^18}")
print("-"*100)
for gr in range(start_row, end_row+1):
    ord_idx = gr // 64
    row_idx = gr % 64
    if ord_idx >= mod["song_len"]: break
    pat_idx = orders[ord_idx]
    row     = pats[pat_idx][row_idx]
    t_ms    = gr * row_ms
    arrow   = " <--" if abs(t_ms - target_ms) < row_ms else ""
    cells   = []
    for (smp, per, eff, prm) in row:
        note = period_to_note(per) if per else "---"
        sstr = f"s{smp:02d}" if smp else "   "
        estr = eff_str(eff, prm)
        cells.append(f"{note} {sstr} {estr}")
    print(f"{t_ms:6.0f}  {ord_idx:3} {pat_idx:3} {row_idx:3}  {'  '.join(cells)}{arrow}")
