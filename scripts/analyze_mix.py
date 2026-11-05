#!/usr/bin/env python3
"""
analyze_mix.py — build the acoustic similarity index (.r3bliss) r3player uses for
"Similar / For You" suggestions. Run this on a PC with the SD card mounted (card
reader, or the device's USB mass-storage mode); the device can't analyze audio itself.

Each track gets a 20-float feature vector (MFCC timbre + spectral shape + tempo + energy),
z-scored across the library so Euclidean distance on the device is meaningful. Output lines:

    Music/Artist/Album/01. Title.flac\t f0 f1 ... f19

Usage:
    pip install librosa numpy            # one-time (also needs ffmpeg for mp3/m4a)
    python3 scripts/analyze_mix.py /path/to/SD            # writes /path/to/SD/.r3bliss
    python3 scripts/analyze_mix.py /path/to/SD -j 8       # 8 workers

The device reads <SD>/.r3bliss at startup and prefers acoustic distance over tag affinity
for any track present in it. Re-run after adding music. (Paths are relative to <SD>, so the
file stays valid on the device where the card mounts at /usr/data/mnt/sd_0.)
"""
import os, sys, math, argparse
from concurrent.futures import ProcessPoolExecutor

DIM = 20
EXTS = (".mp3", ".mpga", ".flac", ".wav", ".m4a")

def features(path):
    import numpy as np, librosa
    y, sr = librosa.load(path, mono=True, sr=22050, duration=120)   # up to 2 min is plenty
    if y.size < sr:
        return None
    mfcc = librosa.feature.mfcc(y=y, sr=sr, n_mfcc=13).mean(axis=1)          # 13 timbre
    cen  = librosa.feature.spectral_centroid(y=y, sr=sr).mean()
    bw   = librosa.feature.spectral_bandwidth(y=y, sr=sr).mean()
    roll = librosa.feature.spectral_rolloff(y=y, sr=sr).mean()
    zcr  = librosa.feature.zero_crossing_rate(y).mean()
    rms  = librosa.feature.rms(y=y).mean()
    tempo = float(librosa.beat.beat_track(y=y, sr=sr)[0])
    v = list(mfcc) + [cen, bw, roll, zcr, rms, tempo]
    return [float(x) for x in v[:DIM]]

def one(args):
    root, rel = args
    try:
        f = features(os.path.join(root, rel))
        return (rel, f)
    except Exception as e:
        return (rel, None)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sd", help="SD card mount root (contains Music/)")
    ap.add_argument("-j", type=int, default=max(1, (os.cpu_count() or 2)))
    args = ap.parse_args()
    root = os.path.abspath(args.sd)
    music = os.path.join(root, "Music")
    if not os.path.isdir(music):
        sys.exit(f"no Music/ under {root}")
    rels = []
    for dp, _, fns in os.walk(music):
        for fn in fns:
            if fn.lower().endswith(EXTS):
                rels.append(os.path.relpath(os.path.join(dp, fn), root))
    print(f"{len(rels)} tracks; analyzing with {args.j} workers...")
    out = []
    done = 0
    with ProcessPoolExecutor(max_workers=args.j) as ex:
        for rel, f in ex.map(one, [(root, r) for r in rels], chunksize=8):
            done += 1
            if done % 200 == 0:
                print(f"  {done}/{len(rels)}")
            if f and len(f) == DIM and all(math.isfinite(x) for x in f):
                out.append((rel, f))
    if not out:
        sys.exit("no features extracted")
    # z-score each dimension across the library
    import statistics as st
    cols = list(zip(*[f for _, f in out]))
    mean = [st.fmean(c) for c in cols]
    sd = [ (st.pstdev(c) or 1.0) for c in cols ]
    dst = os.path.join(root, ".r3bliss")
    with open(dst, "w") as w:
        for rel, f in out:
            z = [ (f[i]-mean[i])/sd[i] for i in range(DIM) ]
            w.write(rel + "\t" + " ".join("%.4f" % x for x in z) + "\n")
    print(f"wrote {dst} ({len(out)} tracks). Put the card back; r3player uses it automatically.")

if __name__ == "__main__":
    main()
