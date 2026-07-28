#!/usr/bin/env python3
"""Generates a synthetic Hail .bm BlockMatrix fixture with no Hail/JVM dependency.

Encodes the exact wire format documented in src/hail_blockmatrix_scanner.cpp:
  Repeating frames until EOF:
    [int32_LE outer_frame_size]        <- 4 + compressed_len (NOT including this field itself)
    [int32_LE decompressed_size]
    [outer_frame_size - 4 bytes: LZ4 data]
  Decompressed block payload:
    [int32_LE nRows][int32_LE nCols][uint8 isTranspose]
    [float64 * nRows*nCols: column-major if isTranspose==0, row-major if isTranspose==1]

Usage: python3 scripts/generate_blockmatrix_fixture.py
Requires: pip install lz4 (falls back to system liblz4 via ctypes if the module isn't importable).
"""
import ctypes
import ctypes.util
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LD_BM_FIXTURE_DIR = ROOT / "test" / "matrix_ld.bm"

# 6x6 matrix, blockSize=2 -> a 3x3 grid of 2x2 blocks (flat index = block_row*3 + block_col):
#   0=(0,0) 1=(0,1) 2=(0,2)
#   3=(1,0) 4=(1,1) 5=(1,2)
#   6=(2,0) 7=(2,1) 8=(2,2)
# Symmetric-matrix (LD) storage keeps only the LOWER triangle (block_row >= block_col): 0,3,4,7,8.
# Confirmed against real data: every stored block index in
# s3://pan-ukb-us-east-1/ld_release/UKBB.EUR.ldadj.bm/metadata.json's `maybeFiltered` (sampled across
# its full range, ~30 blocks) satisfies block_row >= block_col -- the resolver originally assumed the
# opposite (upper triangle) and was fixed after this fixture/smoke-testing caught it.
# Block 6=(2,0) is deliberately OMITTED here (not 1/2/5, which are legitimately upper-triangle and
# never queried by a correctly canonicalized lower-triangle resolver) to simulate a genuinely missing
# lower-triangle block for bm_missing_block testing.
N_ROWS = 6
N_COLS = 6
BLOCK_SIZE = 2

# block_idx -> 2x2 values[row][col] (local to the block). Values follow value(row, col) = row*10+col
# for every populated cell (global row/col), so expected test results are easy to hand-verify; one
# cell is overridden to NaN for the bm_missing_or_nan path.
BLOCK_VALUES = {
    0: [[0, 1], [10, 11]],  # (0,0): rows0-1,cols0-1 -- [1][0]=v(1,0)=10 used by pair (0,1)
    3: [[20, 21], [30, float("nan")]],  # (1,0): rows2-3,cols0-1 -- [1][0]=v(3,0)=30 used by (0,3);
    # [1][1] overridden to NaN (would be v(3,1)=31) -- used by (1,3)
    4: [[22, 23], [32, 33]],  # (1,1): rows2-3,cols2-3 -- [1][0]=v(3,2)=32 used by pair (2,3)
    7: [[42, 43], [52, 53]],  # (2,1): rows4-5,cols2-3 -- [0][1]=v(4,3)=43 used by pair (3,4)
    8: [[44, 45], [54, 55]],  # (2,2): rows4-5,cols4-5 -- unused by test pairs, present for completeness
}
STORED_BLOCK_INDICES = sorted(BLOCK_VALUES.keys())


def _lz4_compress(payload: bytes) -> bytes:
    try:
        import lz4.block

        return lz4.block.compress(payload, mode="default", store_size=False)
    except ImportError:
        pass
    lib_name = ctypes.util.find_library("lz4")
    if lib_name is None:
        raise RuntimeError(
            "Neither the 'lz4' python module nor system liblz4 is available -- "
            "install one to generate BlockMatrix fixtures."
        )
    lib = ctypes.CDLL(lib_name)
    lib.LZ4_compressBound.argtypes = [ctypes.c_int]
    lib.LZ4_compressBound.restype = ctypes.c_int
    max_len = lib.LZ4_compressBound(len(payload))
    out = ctypes.create_string_buffer(max_len)
    src = ctypes.create_string_buffer(payload)
    lib.LZ4_compress_default.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    lib.LZ4_compress_default.restype = ctypes.c_int
    n = lib.LZ4_compress_default(src, out, len(payload), max_len)
    if n <= 0:
        raise RuntimeError("liblz4 failed to compress fixture payload")
    return out.raw[:n]


def encode_block_frame(values, n_rows: int, n_cols: int, is_transpose: bool = False) -> bytes:
    payload = bytearray()
    payload += struct.pack("<i", n_rows)
    payload += struct.pack("<i", n_cols)
    payload += bytes([1 if is_transpose else 0])
    if is_transpose:
        for r in range(n_rows):
            for c in range(n_cols):
                payload += struct.pack("<d", values[r][c])
    else:
        for c in range(n_cols):
            for r in range(n_rows):
                payload += struct.pack("<d", values[r][c])
    compressed = _lz4_compress(bytes(payload))
    outer_frame_size = 4 + len(compressed)
    return struct.pack("<i", outer_frame_size) + struct.pack("<i", len(payload)) + compressed


def write_ld_blockmatrix_fixture(fixture_dir: Path):
    parts_dir = fixture_dir / "parts"
    parts_dir.mkdir(parents=True, exist_ok=True)

    part_files = []
    for block_idx in STORED_BLOCK_INDICES:
        values = BLOCK_VALUES[block_idx]
        frame = encode_block_frame(values, BLOCK_SIZE, BLOCK_SIZE, is_transpose=False)
        fname = f"part-{block_idx}"
        (parts_dir / fname).write_bytes(frame)
        part_files.append(fname)

    metadata = {
        "blockSize": BLOCK_SIZE,
        "nRows": N_ROWS,
        "nCols": N_COLS,
        "partFiles": part_files,
        "maybeFiltered": STORED_BLOCK_INDICES,
    }
    (fixture_dir / "metadata.json").write_text(json.dumps(metadata))


if __name__ == "__main__":
    write_ld_blockmatrix_fixture(LD_BM_FIXTURE_DIR)
    print("Wrote", LD_BM_FIXTURE_DIR)
