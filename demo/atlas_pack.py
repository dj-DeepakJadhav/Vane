#!/usr/bin/env python3
"""
atlas_pack.py — turn GloVe word vectors into Vane's INT4 block format.

    python demo/atlas_pack.py glove.6B.50d.txt demo/atlas.bin --words 25000

Produces a single self-contained file holding the quantised matrix, the block
scales, a 2-D projection for drawing, and the vocabulary.

WHY NORMALISE FIRST
-------------------
Each vector is L2-normalised before quantisation, so the dot product Vane
computes IS cosine similarity. No post-processing, no division at query time —
the search is exactly one call to vn_dequant_gemv_int4.

THE FORMAT (little-endian throughout, matching every Arm target we ship to)
    magic     8   "VNATLAS2"
    n_words   u32
    dim       u32   padded up to a multiple of 32
    n_blocks  u32   dim / 32
    dim_raw   u32   the SOURCE dimension, before padding
    vocab_len u32   bytes of the vocabulary blob

dim_raw exists so a reader can state compression against the data the user
actually had, not against our own zero-padded copy. Comparing 5 MB of INT4
to a padded 32 MB fp32 matrix reports 84.4%; comparing it to the real 30 MB
source reports 83.3%. The second number is the honest one, and without
dim_raw in the file the server had no way to compute it.
    packed    n_words * dim/2      u8    two INT4 nibbles per byte
    scales    n_words * n_blocks   f32
    coords    n_words * 2          f32   2-D PCA, scaled into [-1, 1]
    vocab     vocab_len            u8    newline-separated UTF-8

Copyright 2026 The Vane Authors. Apache License 2.0.
"""
import argparse
import struct
import sys

try:
    import numpy as np
except ImportError:
    sys.exit("numpy is required:  pip install numpy")

BLOCK = 32


def load_glove(path, max_words):
    """Read GloVe text format: one word followed by its floats, per line."""
    words, vecs = [], []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            parts = line.rstrip().split(" ")
            if len(parts) < 3:
                continue
            words.append(parts[0])
            vecs.append([float(x) for x in parts[1:]])
            if len(words) >= max_words:
                break
    return words, np.asarray(vecs, dtype=np.float32)


def quantize_int4(mat):
    """Match src/kernels/kernel_scalar.cpp exactly.

    scale = max|x| / 7 per 32-element block; q = clamp(round(x/scale), -7, 7) + 8;
    byte j packs element 2j in the low nibble and 2j+1 in the high nibble.

    Any divergence here silently corrupts every search result, so this mirrors
    the C reference line for line rather than doing anything clever.
    """
    n, dim = mat.shape
    nblocks = dim // BLOCK
    blocks = mat.reshape(n, nblocks, BLOCK)

    scales = np.abs(blocks).max(axis=2) / 7.0
    inv = np.where(scales > 1e-20, 1.0 / np.maximum(scales, 1e-30), 0.0)

    q = np.rint(blocks * inv[:, :, None])
    q = np.clip(q, -7, 7).astype(np.int8) + 8            # -> [1, 15]

    q = q.reshape(n, dim)
    lo, hi = q[:, 0::2], q[:, 1::2]
    packed = ((lo & 0x0F) | ((hi & 0x0F) << 4)).astype(np.uint8)
    return packed, scales.astype(np.float32)


def project_2d(mat):
    """Top-2 principal components, scaled into [-1, 1] for the canvas."""
    centred = mat - mat.mean(axis=0, keepdims=True)
    # Economy SVD on 25k x 50 is instant; no need for a randomised solver.
    _, _, vt = np.linalg.svd(centred, full_matrices=False)
    coords = centred @ vt[:2].T
    span = np.abs(coords).max(axis=0)
    span[span == 0] = 1.0
    return (coords / span).astype(np.float32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("glove", help="GloVe .txt (e.g. glove.6B.50d.txt)")
    ap.add_argument("out", help="output .bin")
    ap.add_argument("--words", type=int, default=25000,
                    help="vocabulary size; GloVe is ordered by frequency")
    args = ap.parse_args()

    print(f"reading {args.glove} ...")
    words, mat = load_glove(args.glove, args.words)
    n, dim_raw = mat.shape
    print(f"  {n} words x {dim_raw} dims")

    # Pad the dimension up to a multiple of 32. Zeros contribute nothing to a
    # dot product, so padding cannot change a ranking.
    dim = ((dim_raw + BLOCK - 1) // BLOCK) * BLOCK
    if dim != dim_raw:
        mat = np.pad(mat, ((0, 0), (0, dim - dim_raw)))
        print(f"  padded {dim_raw} -> {dim} dims (a multiple of {BLOCK})")

    coords = project_2d(mat[:, :dim_raw])

    norms = np.linalg.norm(mat, axis=1, keepdims=True)
    norms[norms == 0] = 1.0
    mat = mat / norms
    print("  L2-normalised, so the INT4 dot product is cosine similarity")

    packed, scales = quantize_int4(mat)

    # Report compression against the ORIGINAL data, not against our own padded
    # copy. Padding 50 dims up to 64 inflates both sides of a padded-vs-padded
    # comparison and flatters the result by several points; the number that
    # matters to a user is how our file compares to the vectors they started
    # with. Both are printed so the difference is visible rather than hidden.
    src_fp32_bytes = n * dim_raw * 4          # the real, unpadded source
    padded_fp32_bytes = n * dim * 4           # our padded working copy
    int4_bytes = packed.nbytes + scales.nbytes
    print(f"  int4 {int4_bytes/1e6:.2f} MB  (packed {packed.nbytes/1e6:.2f} "
          f"+ scales {scales.nbytes/1e6:.2f}, both counted)")
    print(f"    vs source fp32 {src_fp32_bytes/1e6:.2f} MB "
          f"({n}x{dim_raw})  -> {100*(1-int4_bytes/src_fp32_bytes):.1f}% smaller")
    if dim != dim_raw:
        print(f"    vs padded fp32 {padded_fp32_bytes/1e6:.2f} MB "
              f"({n}x{dim})  -> {100*(1-int4_bytes/padded_fp32_bytes):.1f}% smaller "
              f"[padding inflates this one; prefer the line above]")

    # Round-trip check against the quantiser's own output.
    deq = ((packed & 0x0F).astype(np.int16) - 8)
    deq_hi = ((packed >> 4).astype(np.int16) - 8)
    rec = np.empty((n, dim), dtype=np.float32)
    rec[:, 0::2] = deq * np.repeat(scales, BLOCK // 2, axis=1)
    rec[:, 1::2] = deq_hi * np.repeat(scales, BLOCK // 2, axis=1)
    err = np.abs(rec - mat).max()
    print(f"  max reconstruction error {err:.3e}")

    vocab = ("\n".join(words) + "\n").encode("utf-8")

    with open(args.out, "wb") as fh:
        fh.write(b"VNATLAS2")
        fh.write(struct.pack("<IIIII", n, dim, dim // BLOCK, dim_raw, len(vocab)))
        fh.write(packed.tobytes())
        fh.write(scales.tobytes())
        fh.write(coords.tobytes())
        fh.write(vocab)

    import os
    print(f"wrote {args.out}  ({os.path.getsize(args.out)/1e6:.2f} MB)")


if __name__ == "__main__":
    main()
