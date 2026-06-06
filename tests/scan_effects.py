"""Scan all loadable MOD files and aggregate effect usage, highlighting anything
the native mixer does not implement (E0x Filter, EFx Funk Repeat).

Run from repo root:
    python tests/scan_effects.py [MODS_dir]
"""

import subprocess, sys, os, re, collections, pathlib

MODS_DIR = sys.argv[1] if len(sys.argv) > 1 else "MODS"
TEST_EXE = r"tests\bin\test_audio.exe"

# All effects the native mixer implements (upper-case to match eff.upper())
IMPLEMENTED = {
    "0XX","1XX","2XX","3XX","4XX","5XX","6XX","7XX","8XX",
    "9XX","AXX","BXX","CXX","DXX","FXX",
    "EXX",                          # parent bucket — sub-codes listed separately
    "E1X","E2X","E3X","E4X","E5X","E6X","E7X",
    "E9X","EAX","EBX","ECX","EDX","EEX",
    # NOT implemented: E0X (LED filter), E8X (ext panning), EFX (funk repeat)
}

# Aggregate across all files
total_counts  = collections.Counter()
unimpl_counts = collections.Counter()   # counts of {effect: files using it}
files_with_unimpl = []

mod_paths = []
for root, dirs, files in os.walk(MODS_DIR):
    for f in files:
        lo = f.lower()
        if lo.endswith(".mod") or lo.startswith("mod."):
            mod_paths.append(os.path.join(root, f))

mod_paths.sort()
print(f"Scanning {len(mod_paths)} MOD files ...\n")

for path in mod_paths:
    try:
        result = subprocess.run(
            [TEST_EXE, path],
            capture_output=True, timeout=10
        )
        out = result.stdout.decode("utf-8", errors="replace")
    except Exception as e:
        continue

    # Parse effect usage lines like: "    66  Cxx Set Volume"
    effects_found = {}
    in_usage = False
    for line in out.splitlines():
        if "Effect usage:" in line:
            in_usage = True
            continue
        if in_usage:
            m = re.match(r"\s+(\d+)\s+([0-9A-Fa-f]xx|E[0-9A-Fa-f]x)\s", line)
            if m:
                cnt   = int(m.group(1))
                eff   = m.group(2).upper()
                effects_found[eff] = cnt
                total_counts[eff] += cnt
            elif line.strip() == "":
                in_usage = False

    # Which unimplemented effects does this file use?
    unimpl = {k for k in effects_found if k not in IMPLEMENTED}
    if unimpl:
        files_with_unimpl.append((path, {k: effects_found[k] for k in unimpl}))
        for k in unimpl:
            unimpl_counts[k] += 1

# ── Report ────────────────────────────────────────────────────────────────────

print("=" * 60)
print("EFFECT USAGE ACROSS ALL MODS (native mixer view)")
print("=" * 60)

EFFECT_NAMES = {
    "0XX":"Arpeggio","1XX":"Porta Up","2XX":"Porta Down",
    "3XX":"Porta-to-Note","4XX":"Vibrato","5XX":"Porta+VolSlide",
    "6XX":"Vib+VolSlide","7XX":"Tremolo","8XX":"Set Pan",
    "9XX":"Sample Offset","AXX":"Vol Slide","BXX":"Jump Order",
    "CXX":"Set Volume","DXX":"Pat Break","EXX":"Extended","FXX":"Speed/BPM",
    "E0X":"Filter","E1X":"Fine Porta Up","E2X":"Fine Porta Dn",
    "E3X":"Glissando","E4X":"Vib Wave","E5X":"Set Finetune",
    "E6X":"Pat Loop","E7X":"Trem Wave","E8X":"Set Pan(E)",
    "E9X":"Retrigger","EAX":"Fine Vol Up","EBX":"Fine Vol Dn",
    "ECX":"Note Cut","EDX":"Note Delay","EEX":"Pat Delay","EFX":"Funk Repeat",
}

for eff, cnt in sorted(total_counts.items(), key=lambda x: -x[1]):
    name = EFFECT_NAMES.get(eff.upper().replace("x","X"), eff)
    impl = "" if eff.upper().replace("x","X") in {k.upper() for k in IMPLEMENTED} else "  *** NOT IMPLEMENTED ***"
    print(f"  {cnt:7,}  {eff:<5}  {name}{impl}")

print()
print("=" * 60)
print("UNIMPLEMENTED EFFECTS BY FILE")
print("=" * 60)
if not files_with_unimpl:
    print("  All effects in all scanned MODs are implemented!")
else:
    for path, effs in files_with_unimpl[:60]:  # cap output
        print(f"  {os.path.basename(path)}")
        for eff, cnt in effs.items():
            print(f"      {eff}: {cnt} uses")

if len(files_with_unimpl) > 60:
    print(f"  ... and {len(files_with_unimpl)-60} more files")

print()
print(f"Files using unimplemented effects: {len(files_with_unimpl)} / {len(mod_paths)}")
print()
print("Summary of unimplemented effects:")
for eff, nfiles in sorted(unimpl_counts.items(), key=lambda x: -x[1]):
    print(f"  {eff}: used in {nfiles} files")
