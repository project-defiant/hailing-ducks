#!/usr/bin/env python3
"""Generates a synthetic Hail .ht fixture with no Hail/JVM dependency.

Encodes the exact wire format documented in src/hail_codec.cpp:
  StreamBlockBufferSpec outer frame: [int32_LE stream_block_len][bytes]
  ZstdBlockBufferSpec inner frame:   [int32_LE decomp_size][zstd payload]
  Row payload: LEB128 (unsigned) ints/longs, raw IEEE754 floats/doubles,
  missing-bit-prefixed structs/arrays per src/hail_type_parser.hpp's grammar.

Usage: python3 scripts/generate_hailtable_fixture.py
Requires: pip install zstandard (falls back to the system `zstd` CLI via
subprocess if the `zstandard` module isn't importable -- both produce
standard zstd frames, so either is fine for ZstdBlockDecoder).
"""
import gzip
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

try:
    import zstandard as zstd
except ImportError:
    zstd = None

ROOT = Path(__file__).resolve().parent.parent
FIXTURE_DIR = ROOT / "test" / "hailtable_fixture.ht"
OPTIONAL_FIXTURE_DIR = ROOT / "test" / "hailtable_fixture_optional.ht"

VTYPE = ("Struct{idx:Int64,locus:Locus(GRCh38),alleles:Array[String],"
         "pop_freq:Struct{AC:Int32,AF:Float64,AN:Int32,homozygote_count:Int32}}")
ETYPE = ("+EBaseStruct{idx:+EInt64,locus:+EBaseStruct{contig:+EBinary,position:+EInt32},"
         "alleles:+EArray[+EBinary],pop_freq:+EBaseStruct{AC:+EInt32,AF:+EFloat64,"
         "AN:+EInt32,homozygote_count:+EInt32}}")

ROWS = [
    {"idx": 0, "locus": ("1", 10000), "alleles": ["A", "T"],
     "pop_freq": (5, 0.001, 5000, 0)},
    {"idx": 1, "locus": ("1", 10050), "alleles": ["G", "C", "A"],
     "pop_freq": (12, 0.0024, 5000, 1)},
    {"idx": 2, "locus": ("2", 20000), "alleles": ["T", "G"],
     "pop_freq": (0, 0.0, 4998, 0)},
]


def leb128_u(value: int) -> bytes:
    out = bytearray()
    while True:
        b = value & 0x7F
        value >>= 7
        if value:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def encode_string(s: str) -> bytes:
    raw = s.encode("utf-8")
    return leb128_u(len(raw)) + raw


def encode_row(row: dict) -> bytes:
    # Top-level EBaseStruct has 0 optional fields -> 0 missing-bit bytes for
    # the struct itself. (The leading per-row continuation flag below is a
    # separate, outer protocol -- not part of the struct's own encoding.)
    out = bytearray()
    out += leb128_u(row["idx"])                      # idx: EInt64
    contig, position = row["locus"]
    out += encode_string(contig)                      # locus.contig: EBinary
    out += leb128_u(position)                          # locus.position: EInt32
    alleles = row["alleles"]
    out += leb128_u(len(alleles))                      # alleles: EArray length
    # elements are required -> 0 missing-bit bytes for the array
    for a in alleles:
        out += encode_string(a)
    ac, af, an, hom = row["pop_freq"]
    out += leb128_u(ac)
    out += struct.pack("<d", af)
    out += leb128_u(an)
    out += leb128_u(hom)
    return bytes(out)


def encode_partition_body(rows, encode_fn=encode_row) -> bytes:
    # Verified against a real partition of s3://pan-ukb-us-east-1/ld_release/
    # UKBB.EUR.ldadj.variant.b38.ht/rows/parts/part-00000-...: every row is
    # prefixed by a 1-byte "continue" flag (nonzero = a row follows), and the
    # partition body ends with a single 0x00 terminator byte. This is NOT
    # documented anywhere in issues #10-#13 -- it was found by manually
    # decompressing and byte-decoding real Hail output.
    out = bytearray()
    for row in rows:
        out += b"\x01"       # continue flag: another row follows
        out += encode_fn(row)
    out += b"\x00"           # terminator: end of partition
    return bytes(out)


def compress_zstd_payload(payload: bytes) -> bytes:
    if zstd is not None:
        return zstd.ZstdCompressor().compress(payload)
    zstd_cli = shutil.which("zstd")
    if zstd_cli is None:
        raise RuntimeError(
            "Neither the 'zstandard' python module nor a 'zstd' CLI binary "
            "is available -- install one to generate zstd-codec fixtures.")
    result = subprocess.run([zstd_cli, "-q", "-c", "-"], input=payload,
                             stdout=subprocess.PIPE, check=True)
    return result.stdout


def build_part_file(rows, codec: str, encode_fn=encode_row) -> bytes:
    payload = encode_partition_body(rows, encode_fn)
    if codec == "zstd":
        compressed = compress_zstd_payload(payload)
    elif codec in ("lz4hc", "lz4fast"):
        import lz4.block  # pip install lz4
        mode = "high_compression" if codec == "lz4hc" else "default"
        compressed = lz4.block.compress(payload, mode=mode, store_size=False)
    else:
        raise ValueError(codec)
    inner = struct.pack("<i", len(payload)) + compressed  # decomp_size-prefixed inner frame
    outer = struct.pack("<i", len(inner)) + inner           # StreamBlockBufferSpec outer frame
    return outer


# Buffer-spec name as it appears at the *compression* layer of the nested
# chain (see BUFFER_SPEC_CHAIN below) -- verified against real metadata,
# which nests as LEB128BufferSpec -> BlockingBufferSpec -> <this name> ->
# StreamBlockBufferSpec, NOT a flat {name, blockSize} as issue #10 assumed.
CODEC_TO_BUFFER_SPEC_NAME = {
    "zstd": "ZstdBlockBufferSpec",
    "lz4hc": "LZ4HCBlockBufferSpec",
    "lz4fast": "LZ4FastBlockBufferSpec",
}


def buffer_spec_chain(codec: str) -> dict:
    return {
        "name": "LEB128BufferSpec",
        "child": {
            "name": "BlockingBufferSpec",
            "blockSize": 32768,
            "child": {
                "name": CODEC_TO_BUFFER_SPEC_NAME[codec],
                "blockSize": 32768,
                "child": {"name": "StreamBlockBufferSpec"},
            },
        },
    }


# ---------------------------------------------------------------------------
# Second, minimal fixture: a top-level (row-struct) OPTIONAL field, to
# regression-test that the row struct's own missing-bit prefix is read
# before decoding its fields (see task-3 fix report, Important #2). The main
# fixture above never exercises this because every field in its ETYPE is
# marked '+' (required), so the row struct's missing-bit prefix is always
# 0 bytes there -- invisible to that fixture regardless of whether the
# scanner reads it.
# ---------------------------------------------------------------------------

OPTIONAL_VTYPE = "Struct{idx:Int64,qual:Float64}"
# 'qual' has no leading '+' -> optional at the top level -> the row struct
# has exactly 1 optional field -> a 1-byte missing-bit prefix per row.
OPTIONAL_ETYPE = "+EBaseStruct{idx:+EInt64,qual:EFloat64}"

OPTIONAL_ROWS = [
    {"idx": 0, "qual": 1.5},
    {"idx": 1, "qual": None},
    {"idx": 2, "qual": 3.25},
]


def encode_optional_row(row: dict) -> bytes:
    is_null = row["qual"] is None
    # Row struct's own missing-bit prefix MUST be read before any field is
    # decoded -- this is exactly the byte that Important #2 was about the
    # scanner never reading.
    missing_byte = 0b1 if is_null else 0b0
    out = bytearray()
    out += bytes([missing_byte])
    out += leb128_u(row["idx"])          # idx: EInt64, required -> no bit consumed
    if not is_null:
        out += struct.pack("<d", row["qual"])  # qual: EFloat64, occupies 0 bytes when NULL
    return bytes(out)


def write_optional_fixture(fixture_dir: Path, codec: str):
    parts_dir = fixture_dir / "rows" / "parts"
    parts_dir.mkdir(parents=True, exist_ok=True)

    part_bytes = build_part_file(OPTIONAL_ROWS, codec, encode_fn=encode_optional_row)
    (parts_dir / "part-00000").write_bytes(part_bytes)

    rows_metadata = {
        "_codecSpec": {
            "_eType": OPTIONAL_ETYPE,
            "_vType": OPTIONAL_VTYPE,
            "_bufferSpec": buffer_spec_chain(codec),
        },
        "_partFiles": ["part-00000"],
    }
    with gzip.open(fixture_dir / "rows" / "metadata.json.gz", "wb") as f:
        f.write(json.dumps(rows_metadata).encode("utf-8"))

    top_metadata = {
        "file_version": 1,
        "table_type": f"Table{{global:Struct{{}},key:[],row:{OPTIONAL_VTYPE}}}",
        "components": {
            "rows": {"name": "RVDComponentSpec", "rel_path": "rows"},
            "partition_counts": {"name": "PartitionCountsComponentSpec", "counts": [len(OPTIONAL_ROWS)]},
        },
    }
    with gzip.open(fixture_dir / "metadata.json.gz", "wb") as f:
        f.write(json.dumps(top_metadata).encode("utf-8"))


def write_fixture(fixture_dir: Path, codec: str):
    parts_dir = fixture_dir / "rows" / "parts"
    parts_dir.mkdir(parents=True, exist_ok=True)

    part_bytes = build_part_file(ROWS, codec)
    (parts_dir / "part-00000").write_bytes(part_bytes)

    # rows/metadata.json.gz: root-level _codecSpec/_partFiles, NOT nested
    # under an "rg"/"_RVDType" wrapper (that shape, from issue #10, does not
    # exist in real Hail output -- verified against the real file's root keys:
    # ['name', '_key', '_codecSpec', '_indexSpec', '_partFiles', '_jRangeBounds', '_attrs']).
    rows_metadata = {
        "_codecSpec": {
            "_eType": ETYPE,
            "_vType": VTYPE,
            "_bufferSpec": buffer_spec_chain(codec),
        },
        "_partFiles": ["part-00000"],
    }
    with gzip.open(fixture_dir / "rows" / "metadata.json.gz", "wb") as f:
        f.write(json.dumps(rows_metadata).encode("utf-8"))

    # Top-level metadata.json.gz: components.rows.rel_path points at the
    # "rows" subdirectory; partition_counts gives per-partition row counts
    # for free (this plan's Task 3 doesn't use it, but real consumers may).
    top_metadata = {
        "file_version": 1,
        "table_type": f"Table{{global:Struct{{}},key:[],row:{VTYPE}}}",
        "components": {
            "rows": {"name": "RVDComponentSpec", "rel_path": "rows"},
            "partition_counts": {"name": "PartitionCountsComponentSpec", "counts": [len(ROWS)]},
        },
    }
    with gzip.open(fixture_dir / "metadata.json.gz", "wb") as f:
        f.write(json.dumps(top_metadata).encode("utf-8"))


if __name__ == "__main__":
    write_fixture(ROOT / "test" / "hailtable_fixture.ht", "zstd")
    print("Wrote", ROOT / "test" / "hailtable_fixture.ht")
    write_optional_fixture(OPTIONAL_FIXTURE_DIR, "zstd")
    print("Wrote", OPTIONAL_FIXTURE_DIR)
