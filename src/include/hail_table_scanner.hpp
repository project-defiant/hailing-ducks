#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "hail_codec.hpp"
#include "hail_type_parser.hpp"

namespace duckdb {

struct HailTableScanFunction {
	static void Register(ExtensionLoader &loader);
};

// One partitioner range bound, parsed from a real HailTable's `_jRangeBounds` entry -- verified
// against s3://pan-ukb-us-east-1/ld_release/UKBB.EUR.ldadj.variant.b38.ht/rows/metadata.json.gz.
// Partitions are contiguous and non-overlapping in the table's true sort order, but that order is
// NOT lexical on contig name (Hail uses reference-genome contig ordering), so pruning must treat
// contig comparisons as opaque equality checks against this table's own recorded bounds, never as a
// general ordering.
struct HailTablePartitionBound {
	std::string start_contig;
	int64_t start_position = 0;
	std::string end_contig;
	int64_t end_position = 0;
	bool include_start = false;
	bool include_end = false;
};

struct HailTableMetadata {
	ETypeNode etype;
	VTypeNode vtype;
	std::string buffer_spec_name;
	std::string rows_rel_path;
	std::string meta_path;               // rows/metadata.json.gz path, for error messages
	std::vector<std::string> part_files; // relative to <path>/<rows_rel_path>/parts/
	std::vector<std::string> key_fields; // from _key; empty if the table is unkeyed
	// Parsed from _jRangeBounds, in _partFiles order; empty if the table is unkeyed, has no
	// _jRangeBounds, or its key doesn't include a "locus" field (pruning unavailable).
	std::vector<HailTablePartitionBound> range_bounds;
};

// Reads <path>/metadata.json.gz and <path>/<rows_rel_path>/metadata.json.gz: resolves the codec,
// row EType/VType, partition file list, and (when present) the partitioner's per-partition key
// range bounds. Shared by hail_scan_table's bind phase and the LD query batch HT resolver.
HailTableMetadata LoadHailTableMetadata(FileSystem &fs, const std::string &path);

// Decodes one row from the currently-open partition's decoder into `output` at row index `row`.
// Returns false when the partition's continuation flag signals end-of-partition (0x00) -- the
// caller must then advance to the next partition. `output`'s columns must match `etype`'s top-level
// struct fields in order (as produced by VTypeToDuckDBType over the matching VTypeNode).
bool DecodeHailTableRow(BlockDecoder &decoder, const ETypeNode &etype, DataChunk &output, idx_t row);

} // namespace duckdb
