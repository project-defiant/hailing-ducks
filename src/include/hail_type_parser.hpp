#pragma once

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace duckdb {

// ---------------------------------------------------------------------------
// EType: Hail's *encoded* type description — drives wire-format decoding.
// A '+' prefix means "required" (non-optional). It is meaningful on the
// top-level EType and on nested struct fields/array elements; required
// fields/elements have no missing-bit slot in their containing value.
// ---------------------------------------------------------------------------

enum class EKind { Int32, Int64, Float32, Float64, Boolean, Binary, Array, BaseStruct };

struct ETypeNode {
	EKind kind;
	bool required = false;           // top-level marker or nested field/element requiredness
	std::string name;                // meaningful only as a struct field
	std::vector<ETypeNode> children; // Array: exactly 1 (the element type)
	                                 // BaseStruct: N, in field order
};

// ---------------------------------------------------------------------------
// VType: Hail's *virtual* (logical) type description — drives the DuckDB
// schema. Locus(GENOME) is a distinct primitive-like type; its GENOME string
// (e.g. "GRCh37") is informational only — hail_scan_table does not perform
// liftover, it surfaces (contig, position) exactly as stored.
// ---------------------------------------------------------------------------

enum class VKind { Int32, Int64, Float32, Float64, Boolean, String, Array, Struct, Locus };

struct VTypeNode {
	VKind kind;
	std::string name;                // meaningful only as a struct field
	std::string genome;              // meaningful only for Locus, e.g. "GRCh37"
	std::vector<VTypeNode> children; // Array: 1; Struct: N in field order
};

ETypeNode parse_etype(const std::string &s);
VTypeNode parse_vtype(const std::string &s);

// Round-trips a parsed node back to its canonical string form. Used by the
// hail_debug_etype/hail_debug_vtype test helpers, and useful for error
// messages elsewhere.
std::string etype_to_string(const ETypeNode &node);
std::string vtype_to_string(const VTypeNode &node);

// Maps a VTypeNode to the DuckDB LogicalType used for hail_scan_table's
// bind-phase return_types. Locus becomes STRUCT(contig VARCHAR, position INTEGER).
LogicalType VTypeToDuckDBType(const VTypeNode &vtype);

} // namespace duckdb
