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
import ctypes
import ctypes.util
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
LZ4HC_FIXTURE_DIR = ROOT / "test" / "hailtable_fixture_lz4hc.ht"
LZ4FAST_FIXTURE_DIR = ROOT / "test" / "hailtable_fixture_lz4fast.ht"
BAD_CODEC_FIXTURE_DIR = ROOT / "test" / "hailtable_fixture_bad_codec.ht"
OPTIONAL_ARRAY_FIXTURE_DIR = ROOT / "test" / "hailtable_fixture_optional_array.ht"
ALT_ROWS_FIXTURE_DIR = ROOT / "test" / "hailtable_fixture_alt_rows.ht"
TYPE_MISMATCH_FIXTURE_DIR = ROOT / "test" / "hailtable_fixture_type_mismatch.ht"
LD_FIXTURE_DIR = ROOT / "test" / "hailtable_fixture_ld.ht"

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


def compress_lz4_payload(payload: bytes, codec: str) -> bytes:
    try:
        import lz4.block  # pip install lz4
        mode = "high_compression" if codec == "lz4hc" else "default"
        return lz4.block.compress(payload, mode=mode, store_size=False)
    except ImportError:
        pass

    lib_name = ctypes.util.find_library("lz4")
    if lib_name is None:
        raise RuntimeError(
            "Neither the 'lz4' python module nor system liblz4 is available "
            "-- install one to generate LZ4-codec fixtures.")

    lib = ctypes.CDLL(lib_name)
    lib.LZ4_compressBound.argtypes = [ctypes.c_int]
    lib.LZ4_compressBound.restype = ctypes.c_int
    max_len = lib.LZ4_compressBound(len(payload))
    out = ctypes.create_string_buffer(max_len)

    src = ctypes.create_string_buffer(payload)
    if codec == "lz4hc":
        lib.LZ4_compress_HC.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        lib.LZ4_compress_HC.restype = ctypes.c_int
        n = lib.LZ4_compress_HC(src, out, len(payload), max_len, 9)
    elif codec == "lz4fast":
        lib.LZ4_compress_default.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
        lib.LZ4_compress_default.restype = ctypes.c_int
        n = lib.LZ4_compress_default(src, out, len(payload), max_len)
    else:
        raise ValueError(codec)
    if n <= 0:
        raise RuntimeError("liblz4 failed to compress fixture payload")
    return out.raw[:n]


def build_part_file(rows, codec: str, encode_fn=encode_row) -> bytes:
    payload = encode_partition_body(rows, encode_fn)
    if codec == "zstd":
        compressed = compress_zstd_payload(payload)
    elif codec in ("lz4hc", "lz4fast"):
        compressed = compress_lz4_payload(payload, codec)
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

OPTIONAL_ARRAY_VTYPE = "Struct{idx:Int64,tags:Array[String]}"
OPTIONAL_ARRAY_ETYPE = "+EBaseStruct{idx:+EInt64,tags:+EArray[EBinary]}"
OPTIONAL_ARRAY_ROWS = [
    {"idx": 0, "tags": ["A", None, "B"]},
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


def encode_optional_array_row(row: dict) -> bytes:
    out = bytearray()
    out += leb128_u(row["idx"])
    tags = row["tags"]
    out += leb128_u(len(tags))
    missing_byte = 0
    for i, tag in enumerate(tags):
        if tag is None:
            missing_byte |= (1 << i)
    out += bytes([missing_byte])
    for tag in tags:
        if tag is not None:
            out += encode_string(tag)
    return bytes(out)


def write_optional_array_fixture(fixture_dir: Path, codec: str):
    parts_dir = fixture_dir / "rows" / "parts"
    parts_dir.mkdir(parents=True, exist_ok=True)

    part_bytes = build_part_file(OPTIONAL_ARRAY_ROWS, codec, encode_fn=encode_optional_array_row)
    (parts_dir / "part-00000").write_bytes(part_bytes)

    rows_metadata = {
        "_codecSpec": {
            "_eType": OPTIONAL_ARRAY_ETYPE,
            "_vType": OPTIONAL_ARRAY_VTYPE,
            "_bufferSpec": buffer_spec_chain(codec),
        },
        "_partFiles": ["part-00000"],
    }
    with gzip.open(fixture_dir / "rows" / "metadata.json.gz", "wb") as f:
        f.write(json.dumps(rows_metadata).encode("utf-8"))

    top_metadata = {
        "file_version": 1,
        "table_type": f"Table{{global:Struct{{}},key:[],row:{OPTIONAL_ARRAY_VTYPE}}}",
        "components": {
            "rows": {"name": "RVDComponentSpec", "rel_path": "rows"},
            "partition_counts": {"name": "PartitionCountsComponentSpec", "counts": [len(OPTIONAL_ARRAY_ROWS)]},
        },
    }
    with gzip.open(fixture_dir / "metadata.json.gz", "wb") as f:
        f.write(json.dumps(top_metadata).encode("utf-8"))


# ---------------------------------------------------------------------------
# Keyed, multi-partition fixture with _key/_jRangeBounds, for the LD query
# batch HT resolver (issue #18). Schema/key shape verified against a real
# s3://pan-ukb-us-east-1/ld_release/UKBB.EUR.ldadj.variant.b38.ht/rows/metadata.json.gz:
# _key = ["locus", "alleles"]; _jRangeBounds is one entry per partition, each
# {"start": {"locus": {...}, "alleles": [...]}, "end": {...}, "includeStart",
# "includeEnd"}. Both `locus` and `alleles` are OPTIONAL top-level fields on real tables (no
# leading '+' in the real _eType -- confirmed against the same real metadata: a failed-liftover
# row, for instance, can have a NULL locus), so this fixture includes one row with a NULL locus
# and one with a NULL allele list, to exercise both paths.
# ---------------------------------------------------------------------------

LD_VTYPE = "Struct{idx:Int64,locus:Locus(GRCh38),alleles:Array[String]}"
LD_ETYPE = "+EBaseStruct{idx:+EInt64,locus:EBaseStruct{contig:+EBinary,position:+EInt32},alleles:EArray[EBinary]}"

# Three partitions: two spanning distinct chr1 sub-ranges (to test position-based pruning within a
# contig) and one on chr2 (to test contig-based pruning). Row contents cover: an exact match, a
# flipped match, a multi-allelic row colliding with a request (must NOT be coerced into a match), a
# row with no matching request (not_found_in_ht), two same-position rows that both match one request
# in opposite orientations (ambiguous_in_ht), a NULL-alleles row, a NULL-locus row (both must be
# skipped during scanning, not crash), and an indel row (idx=9, an insertion: alleles are the real
# Hail/PanUKBB convention of a single shared anchor base plus the inserted sequence, e.g. ["A","ATG"]
# -- confirmed against real s3://pan-ukb-us-east-1/ld_release/UKBB.EUR.ldadj.variant.b38.ht data,
# which stores indels the same way) proving indel resolution isn't restricted to single-character
# alleles.
LD_PARTITIONS = [
    [
        {"idx": 0, "locus": ("chr1", 150), "alleles": None},
        {"idx": 1, "locus": ("chr1", 200), "alleles": ["A", "G"]},
        {"idx": 2, "locus": ("chr1", 300), "alleles": ["G", "A"]},
        {"idx": 3, "locus": ("chr1", 400), "alleles": ["A", "G", "T"]},
        {"idx": 8, "locus": None, "alleles": ["A", "G"]},
        {"idx": 9, "locus": ("chr1", 350), "alleles": ["A", "ATG"]},
    ],
    [
        {"idx": 4, "locus": ("chr1", 700), "alleles": ["C", "T"]},
        {"idx": 5, "locus": ("chr1", 800), "alleles": ["A", "C"]},
        {"idx": 6, "locus": ("chr1", 800), "alleles": ["C", "A"]},
    ],
    [
        {"idx": 7, "locus": ("chr2", 200), "alleles": ["A", "G"]},
    ],
]


def encode_ld_row(row: dict) -> bytes:
    # Row struct has 2 optional fields, in LD_ETYPE field declaration order (idx is required -> no
    # bit; locus is the 1st optional field -> bit 0; alleles is the 2nd -> bit 1), matching
    # DecodeValue's general missing-bit-prefix algorithm (bit position = order among optional fields
    # only, not overall field position).
    locus = row["locus"]
    alleles = row["alleles"]
    missing_byte = (0b01 if locus is None else 0) | (0b10 if alleles is None else 0)
    out = bytearray()
    out += bytes([missing_byte])
    out += leb128_u(row["idx"])              # idx: EInt64, required
    if locus is not None:
        contig, position = locus
        out += encode_string(contig)          # locus.contig: EBinary, required
        out += leb128_u(position)              # locus.position: EInt32, required
    if alleles is not None:
        out += leb128_u(len(alleles))          # alleles: EArray length
        # Elements are EBinary with no leading '+' in LD_ETYPE -> each element is itself optional,
        # so the wire format needs a ceil(len/8)-byte missing-bit prefix here too (all zero: none of
        # our synthetic alleles are ever null), matching DecodeValue's EKind::Array handling.
        out += bytes((len(alleles) + 7) // 8)
        for a in alleles:
            out += encode_string(a)
    return bytes(out)


def _ld_range_bound(contig_lo, pos_lo, contig_hi, pos_hi):
    return {
        "start": {"locus": {"contig": contig_lo, "position": pos_lo}, "alleles": []},
        "end": {"locus": {"contig": contig_hi, "position": pos_hi}, "alleles": []},
        "includeStart": True,
        "includeEnd": True,
    }


def write_ld_fixture(fixture_dir: Path, codec: str = "zstd"):
    parts_dir = fixture_dir / "rows" / "parts"
    parts_dir.mkdir(parents=True, exist_ok=True)

    part_files = []
    for i, rows in enumerate(LD_PARTITIONS):
        part_bytes = build_part_file(rows, codec, encode_fn=encode_ld_row)
        fname = f"part-{i:05d}"
        (parts_dir / fname).write_bytes(part_bytes)
        part_files.append(fname)

    j_range_bounds = [
        _ld_range_bound("chr1", 150, "chr1", 400),
        _ld_range_bound("chr1", 700, "chr1", 800),
        _ld_range_bound("chr2", 200, "chr2", 200),
    ]

    rows_metadata = {
        "_key": ["locus", "alleles"],
        "_codecSpec": {
            "_eType": LD_ETYPE,
            "_vType": LD_VTYPE,
            "_bufferSpec": buffer_spec_chain(codec),
        },
        "_partFiles": part_files,
        "_jRangeBounds": j_range_bounds,
    }
    with gzip.open(fixture_dir / "rows" / "metadata.json.gz", "wb") as f:
        f.write(json.dumps(rows_metadata).encode("utf-8"))

    top_metadata = {
        "file_version": 1,
        "table_type": f"Table{{global:Struct{{}},key:[locus,alleles],row:{LD_VTYPE}}}",
        "components": {
            "rows": {"name": "RVDComponentSpec", "rel_path": "rows"},
            "partition_counts": {
                "name": "PartitionCountsComponentSpec",
                "counts": [len(p) for p in LD_PARTITIONS],
            },
        },
    }
    with gzip.open(fixture_dir / "metadata.json.gz", "wb") as f:
        f.write(json.dumps(top_metadata).encode("utf-8"))


def write_fixture(fixture_dir: Path, codec: str, rows_rel_path: str = "rows"):
    rows_dir = fixture_dir / rows_rel_path
    parts_dir = rows_dir / "parts"
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
    with gzip.open(rows_dir / "metadata.json.gz", "wb") as f:
        f.write(json.dumps(rows_metadata).encode("utf-8"))

    # Top-level metadata.json.gz: components.rows.rel_path points at the
    # "rows" subdirectory; partition_counts gives per-partition row counts
    # for free (this plan's Task 3 doesn't use it, but real consumers may).
    top_metadata = {
        "file_version": 1,
        "table_type": f"Table{{global:Struct{{}},key:[],row:{VTYPE}}}",
        "components": {
            "rows": {"name": "RVDComponentSpec", "rel_path": rows_rel_path},
            "partition_counts": {"name": "PartitionCountsComponentSpec", "counts": [len(ROWS)]},
        },
    }
    with gzip.open(fixture_dir / "metadata.json.gz", "wb") as f:
        f.write(json.dumps(top_metadata).encode("utf-8"))


def write_bad_codec_fixture(fixture_dir: Path):
    rows_dir = fixture_dir / "rows"
    rows_dir.mkdir(parents=True, exist_ok=True)

    rows_metadata = {
        "_codecSpec": {
            "_eType": "+EBaseStruct{idx:+EInt64}",
            "_vType": "Struct{idx:Int64}",
            "_bufferSpec": {
                "name": "LEB128BufferSpec",
                "child": {
                    "name": "BlockingBufferSpec",
                    "blockSize": 32768,
                    "child": {
                        "name": "SomeUnknownSpec",
                        "blockSize": 32768,
                        "child": {"name": "StreamBlockBufferSpec"},
                    },
                },
            },
        },
        "_partFiles": [],
    }
    with gzip.open(rows_dir / "metadata.json.gz", "wb") as f:
        f.write(json.dumps(rows_metadata).encode("utf-8"))

    top_metadata = {
        "file_version": 1,
        "table_type": "Table{global:Struct{},key:[],row:Struct{idx:Int64}}",
        "components": {
            "rows": {"name": "RVDComponentSpec", "rel_path": "rows"},
            "partition_counts": {"name": "PartitionCountsComponentSpec", "counts": [0]},
        },
    }
    with gzip.open(fixture_dir / "metadata.json.gz", "wb") as f:
        f.write(json.dumps(top_metadata).encode("utf-8"))


def write_type_mismatch_fixture(fixture_dir: Path):
    rows_dir = fixture_dir / "rows"
    rows_dir.mkdir(parents=True, exist_ok=True)

    rows_metadata = {
        "_codecSpec": {
            "_eType": "+EBaseStruct{idx:+EInt64}",
            "_vType": "Struct{idx:Float64}",
            "_bufferSpec": buffer_spec_chain("zstd"),
        },
        "_partFiles": [],
    }
    with gzip.open(rows_dir / "metadata.json.gz", "wb") as f:
        f.write(json.dumps(rows_metadata).encode("utf-8"))

    top_metadata = {
        "file_version": 1,
        "table_type": "Table{global:Struct{},key:[],row:Struct{idx:Float64}}",
        "components": {
            "rows": {"name": "RVDComponentSpec", "rel_path": "rows"},
            "partition_counts": {"name": "PartitionCountsComponentSpec", "counts": [0]},
        },
    }
    with gzip.open(fixture_dir / "metadata.json.gz", "wb") as f:
        f.write(json.dumps(top_metadata).encode("utf-8"))


if __name__ == "__main__":
    write_fixture(ROOT / "test" / "hailtable_fixture.ht", "zstd")
    print("Wrote", ROOT / "test" / "hailtable_fixture.ht")
    write_optional_fixture(OPTIONAL_FIXTURE_DIR, "zstd")
    print("Wrote", OPTIONAL_FIXTURE_DIR)
    write_fixture(LZ4HC_FIXTURE_DIR, "lz4hc")
    print("Wrote", LZ4HC_FIXTURE_DIR)
    write_fixture(LZ4FAST_FIXTURE_DIR, "lz4fast")
    print("Wrote", LZ4FAST_FIXTURE_DIR)
    write_bad_codec_fixture(BAD_CODEC_FIXTURE_DIR)
    print("Wrote", BAD_CODEC_FIXTURE_DIR)
    write_optional_array_fixture(OPTIONAL_ARRAY_FIXTURE_DIR, "zstd")
    print("Wrote", OPTIONAL_ARRAY_FIXTURE_DIR)
    write_fixture(ALT_ROWS_FIXTURE_DIR, "zstd", rows_rel_path="rowdata")
    print("Wrote", ALT_ROWS_FIXTURE_DIR)
    write_type_mismatch_fixture(TYPE_MISMATCH_FIXTURE_DIR)
    print("Wrote", TYPE_MISMATCH_FIXTURE_DIR)
    write_ld_fixture(LD_FIXTURE_DIR)
    print("Wrote", LD_FIXTURE_DIR)
