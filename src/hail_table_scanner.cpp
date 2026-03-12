#include "hail_table_scanner.hpp"
#include "hail_codec.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/table_function.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// EType field kinds (encoded physical representation)
// ---------------------------------------------------------------------------

enum class EFieldKind {
	Int32,
	Int64,
	Float32,
	Float64,
	Boolean,
	Binary, // used for both TString and TBinary
	Nested, // EArray, EBaseStruct — see TODO below
};

// ---------------------------------------------------------------------------
// FieldInfo: one column as seen by the scanner
// ---------------------------------------------------------------------------

struct FieldInfo {
	std::string name;
	LogicalType duckdb_type; // DuckDB column type
	EFieldKind kind;         // encoded physical type
	bool required;           // EField required= flag (true → no missing bit)
	int missing_idx;         // index into missing-bits byte array; -1 if required
};

// ---------------------------------------------------------------------------
// EType parser
//
// Parses the Hail EType repr string produced by Python, e.g.:
//   EBaseStruct(fields=[EField(name='idx',typ=EInt64(required=True),required=True)])
//
// Only the top-level EBaseStruct is fully parsed. Nested types are identified
// by kind but their contents are skipped (they will be handled in Issue #4).
// ---------------------------------------------------------------------------

class ETypeParser {
public:
	explicit ETypeParser(const std::string &s) : s_(s), pos_(0) {
	}

	// Parse a top-level EBaseStruct and return one FieldInfo per field.
	std::vector<FieldInfo> parse_fields(const std::unordered_map<std::string, LogicalType> &vtype_map) {
		expect("EBaseStruct(fields=[");
		std::vector<FieldInfo> fields;
		int missing_counter = 0;
		while (!consume_if("])")) {
			expect("EField(name='");
			std::string name = consume_until('\'');
			expect("',typ=");
			EFieldKind kind = parse_etype_kind();
			expect(",required=");
			bool required = parse_bool();
			expect(")");
			consume_if(",");

			// Look up the DuckDB type from the _vType map; fall back to VARCHAR
			LogicalType dtype = LogicalType::VARCHAR;
			auto it = vtype_map.find(name);
			if (it != vtype_map.end()) {
				dtype = it->second;
			}

			FieldInfo fi;
			fi.name = name;
			fi.duckdb_type = std::move(dtype);
			fi.kind = kind;
			fi.required = required;
			fi.missing_idx = required ? -1 : missing_counter++;
			fields.push_back(std::move(fi));
		}
		return fields;
	}

private:
	std::string s_;
	size_t pos_;

	void expect(const std::string &tok) {
		if (s_.compare(pos_, tok.size(), tok) != 0) {
			throw BinderException("EType parse error: expected '" + tok + "' at position " + std::to_string(pos_) +
			                      " in: " + s_.substr(pos_, 40));
		}
		pos_ += tok.size();
	}

	bool consume_if(const std::string &tok) {
		if (s_.compare(pos_, tok.size(), tok) == 0) {
			pos_ += tok.size();
			return true;
		}
		return false;
	}

	std::string consume_until(char delim) {
		size_t start = pos_;
		while (pos_ < s_.size() && s_[pos_] != delim) {
			pos_++;
		}
		return s_.substr(start, pos_ - start);
	}

	bool parse_bool() {
		if (consume_if("True")) {
			return true;
		}
		if (consume_if("False")) {
			return false;
		}
		throw BinderException("EType parse error: expected True/False at position " + std::to_string(pos_) + " in: " +
		                      s_.substr(pos_, 20));
	}

	// Skip content between the opening bracket already consumed and its matching close.
	// Tracks '(' / ')' depth only; '[' and ']' are transparent.
	void skip_to_close_paren() {
		int depth = 1;
		while (depth > 0 && pos_ < s_.size()) {
			char c = s_[pos_++];
			if (c == '(') {
				depth++;
			} else if (c == ')') {
				depth--;
			}
		}
	}

	// Consume the type expression and return its EFieldKind.
	EFieldKind parse_etype_kind() {
		struct {
			const char *prefix;
			EFieldKind kind;
		} flat[] = {
		    {"EInt32(", EFieldKind::Int32},     {"EInt64(", EFieldKind::Int64},
		    {"EFloat32(", EFieldKind::Float32}, {"EFloat64(", EFieldKind::Float64},
		    {"EBoolean(", EFieldKind::Boolean}, {"EBinary(", EFieldKind::Binary},
		};
		for (auto &entry : flat) {
			if (consume_if(entry.prefix)) {
				skip_to_close_paren();
				return entry.kind;
			}
		}
		// Nested types — skip the entire expression
		if (consume_if("EArray(") || consume_if("EBaseStruct(")) {
			skip_to_close_paren();
			return EFieldKind::Nested;
		}
		throw BinderException("EType parse error: unknown type at position " + std::to_string(pos_) + " in: " +
		                      s_.substr(pos_, 40));
	}
};

// ---------------------------------------------------------------------------
// VType parser
//
// Parses the Hail VType parsable string, e.g.:
//   struct{locus:locus<GRCh38>,alleles:array<str>,idx:int64}
//
// Returns a map from field name → DuckDB LogicalType.
// Nested types (locus<…>, array<…>, struct{…}) map to VARCHAR (placeholder).
// ---------------------------------------------------------------------------

static std::unordered_map<std::string, LogicalType> parse_vtype(const std::string &vtype_str) {
	std::unordered_map<std::string, LogicalType> result;

	// Must start with "struct{"
	if (vtype_str.size() < 8 || vtype_str.compare(0, 7, "struct{") != 0) {
		throw BinderException("_vType must start with 'struct{', got: " + vtype_str.substr(0, 40));
	}

	// Helper: skip a type token (primitive or nested) and return the DuckDB type.
	auto skip_type_get_logical = [](const std::string &s, size_t &pos) -> LogicalType {
		// Primitive mappings
		struct {
			const char *name;
			LogicalType type;
		} primitives[] = {
		    {"int32", LogicalType::INTEGER},
		    {"int64", LogicalType::BIGINT},
		    {"float32", LogicalType::FLOAT},
		    {"float64", LogicalType::DOUBLE},
		    {"bool", LogicalType::BOOLEAN},
		    {"str", LogicalType::VARCHAR},
		    {"bytes", LogicalType::BLOB},
		};
		for (auto &p : primitives) {
			size_t len = strlen(p.name);
			if (s.compare(pos, len, p.name) == 0) {
				// Make sure it's not a prefix of a longer token
				char next = (pos + len < s.size()) ? s[pos + len] : '\0';
				if (next == ',' || next == '}' || next == '\0') {
					pos += len;
					return p.type;
				}
			}
		}

		// Nested: locus<...>, array<...>, set<...>, dict<...>
		if (s.compare(pos, 6, "locus<") == 0 || s.compare(pos, 6, "array<") == 0 ||
		    s.compare(pos, 4, "set<") == 0 || s.compare(pos, 5, "dict<") == 0) {
			// Skip to matching '>'
			while (pos < s.size() && s[pos] != '>') {
				pos++;
			}
			if (pos < s.size()) {
				pos++; // consume '>'
			}
			return LogicalType::VARCHAR; // placeholder for nested
		}

		// Nested struct{...}
		if (s.compare(pos, 7, "struct{") == 0) {
			pos += 7;
			int depth = 1;
			while (depth > 0 && pos < s.size()) {
				if (s[pos] == '{') {
					depth++;
				} else if (s[pos] == '}') {
					depth--;
				}
				pos++;
			}
			return LogicalType::VARCHAR; // placeholder for nested
		}

		throw BinderException("_vType: unknown type at position " + std::to_string(pos) + " in: " +
		                      s.substr(pos, 30));
	};

	size_t pos = 7; // skip "struct{"
	while (pos < vtype_str.size() && vtype_str[pos] != '}') {
		// Parse field name (up to ':')
		size_t name_start = pos;
		while (pos < vtype_str.size() && vtype_str[pos] != ':') {
			pos++;
		}
		if (pos >= vtype_str.size()) {
			throw BinderException("_vType: expected ':' after field name");
		}
		std::string fname = vtype_str.substr(name_start, pos - name_start);
		pos++; // skip ':'

		LogicalType lt = skip_type_get_logical(vtype_str, pos);
		result[fname] = std::move(lt);

		// Skip optional ','
		if (pos < vtype_str.size() && vtype_str[pos] == ',') {
			pos++;
		}
	}

	return result;
}

// ---------------------------------------------------------------------------
// Bind data
// ---------------------------------------------------------------------------

struct HailTableBindData : public TableFunctionData {
	std::string ht_path;
	std::vector<FieldInfo> fields;  // in EType decode order
	std::vector<std::string> part_files; // relative to <ht_path>/rows/parts/
	int n_missing_bytes;            // ceil(nOptional / 8)
};

// ---------------------------------------------------------------------------
// Global scan state
// ---------------------------------------------------------------------------

struct HailTableGlobalState : public GlobalTableFunctionState {
	std::atomic<idx_t> next_part {0};
	idx_t total_parts;

	explicit HailTableGlobalState(idx_t n) : total_parts(n) {
	}

	idx_t MaxThreads() const override {
		return total_parts;
	}
};

// ---------------------------------------------------------------------------
// Local scan state
// ---------------------------------------------------------------------------

struct HailTableLocalState : public LocalTableFunctionState {
	idx_t current_part = idx_t(-1);
	unique_ptr<FileHandle> part_handle;
	unique_ptr<ZstdBlockDecoder> decoder;

	bool Done() const {
		return decoder ? decoder->eof() : true;
	}
};

// ---------------------------------------------------------------------------
// Bind
// ---------------------------------------------------------------------------

static unique_ptr<FunctionData> HailTableBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<HailTableBindData>();
	bind_data->ht_path = input.inputs[0].GetValue<string>();

	// 1. Open rows/metadata.json.gz
	std::string meta_path = bind_data->ht_path + "/rows/metadata.json.gz";
	auto &fs = FileSystem::GetFileSystem(context);
	unique_ptr<FileHandle> meta_handle;
	try {
		meta_handle = fs.OpenFile(meta_path, FileFlags::FILE_FLAGS_READ);
	} catch (const std::exception &e) {
		throw BinderException("Cannot open HailTable metadata: " + meta_path + ": " + e.what());
	}

	// 2. Read and decompress gzip
	int64_t file_size = fs.GetFileSize(*meta_handle);
	std::vector<char> raw_gz(static_cast<size_t>(file_size));
	meta_handle->Read(raw_gz.data(), static_cast<idx_t>(file_size));
	std::string json_str;
	try {
		json_str = GZipFileSystem::UncompressGZIPString(raw_gz.data(), static_cast<idx_t>(file_size));
	} catch (const std::exception &e) {
		throw BinderException("Failed to decompress HailTable metadata at " + meta_path + ": " + e.what());
	}

	// 3. Parse JSON
	json meta;
	try {
		meta = json::parse(json_str);
	} catch (const std::exception &e) {
		throw BinderException("Failed to parse HailTable metadata at " + meta_path + ": " + e.what());
	}

	// 4. Extract fields from rg._RVDType
	const auto &rvd = meta.at("rg").at("_RVDType");
	std::string vtype_str = rvd.at("_vType").get<std::string>();
	std::string etype_str = rvd.at("_eType").get<std::string>();

	// 5. Parse _vType → name→LogicalType map
	std::unordered_map<std::string, LogicalType> vtype_map;
	try {
		vtype_map = parse_vtype(vtype_str);
	} catch (const std::exception &e) {
		throw BinderException("Failed to parse _vType '" + vtype_str + "': " + e.what());
	}

	// 6. Parse _eType → FieldInfo list
	try {
		ETypeParser eparser(etype_str);
		bind_data->fields = eparser.parse_fields(vtype_map);
	} catch (const BinderException &) {
		throw;
	} catch (const std::exception &e) {
		throw BinderException("Failed to parse _eType '" + etype_str + "': " + e.what());
	}

	// 7. Compute n_missing_bytes
	int n_optional = 0;
	for (auto &f : bind_data->fields) {
		if (!f.required) {
			n_optional++;
		}
	}
	bind_data->n_missing_bytes = (n_optional + 7) / 8;

	// 8. Enumerate part files
	const auto &pf_arr = meta.at("rg").at("_partFiles");
	for (auto &pf : pf_arr) {
		std::string filename = pf.get<std::string>();
		if (filename.find("..") != std::string::npos || filename.find('/') != std::string::npos ||
		    filename.find('\\') != std::string::npos) {
			throw BinderException("Invalid partition filename in metadata (path traversal detected): " + filename);
		}
		bind_data->part_files.push_back(std::move(filename));
	}

	// 9. Build DuckDB schema (in EType decode order)
	for (auto &fi : bind_data->fields) {
		names.push_back(fi.name);
		return_types.push_back(fi.duckdb_type);
	}

	return std::move(bind_data);
}

// ---------------------------------------------------------------------------
// Init global / local
// ---------------------------------------------------------------------------

static unique_ptr<GlobalTableFunctionState> HailTableInitGlobal(ClientContext &context,
                                                                TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<HailTableBindData>();
	return make_uniq<HailTableGlobalState>(bind_data.part_files.size());
}

static unique_ptr<LocalTableFunctionState> HailTableInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                              GlobalTableFunctionState *) {
	return make_uniq<HailTableLocalState>();
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

static void HailTableScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<HailTableBindData>();
	auto &global_state = data.global_state->Cast<HailTableGlobalState>();
	auto &local_state = data.local_state->Cast<HailTableLocalState>();

	const idx_t capacity = STANDARD_VECTOR_SIZE;
	idx_t out_row = 0;

	while (out_row < capacity) {
		// Load next partition if the current decoder is exhausted
		if (!local_state.decoder || local_state.decoder->eof()) {
			idx_t part_idx = global_state.next_part.fetch_add(1);
			if (part_idx >= global_state.total_parts) {
				break; // no more partitions
			}
			local_state.current_part = part_idx;
			std::string part_path =
			    bind_data.ht_path + "/rows/parts/" + bind_data.part_files[part_idx];
			auto &fs = FileSystem::GetFileSystem(context);
			local_state.part_handle = fs.OpenFile(part_path, FileFlags::FILE_FLAGS_READ);
			local_state.decoder = make_uniq<ZstdBlockDecoder>(*local_state.part_handle, part_path);
		}

		auto &dec = *local_state.decoder;
		if (!dec.has_more_data()) {
			break;
		}

		// --- Decode one row ---

		// Read missing-bits bytes
		uint8_t missing_bits[16] = {}; // max 128 optional fields
		if (bind_data.n_missing_bytes > 0) {
			dec.read_bytes(missing_bits, static_cast<size_t>(bind_data.n_missing_bytes));
		}

		const int n_fields = static_cast<int>(bind_data.fields.size());
		for (int fi = 0; fi < n_fields; fi++) {
			const FieldInfo &field = bind_data.fields[fi];
			auto &vec = output.data[fi];

			// Check if this field is null
			bool is_null = false;
			if (!field.required) {
				int bit_byte = field.missing_idx / 8;
				int bit_pos = field.missing_idx % 8;
				is_null = ((missing_bits[bit_byte] >> bit_pos) & 1) != 0;
			}

			if (is_null) {
				FlatVector::SetNull(vec, out_row, true);
				continue;
			}

			// Field is present — decode based on EFieldKind
			switch (field.kind) {
			case EFieldKind::Int32: {
				uint32_t uval = dec.read_leb128_u32();
				// Reinterpret as signed int32
				int32_t sval;
				std::memcpy(&sval, &uval, sizeof(sval));
				FlatVector::GetData<int32_t>(vec)[out_row] = sval;
				break;
			}
			case EFieldKind::Int64: {
				uint64_t uval = dec.read_leb128_u64();
				int64_t sval;
				std::memcpy(&sval, &uval, sizeof(sval));
				FlatVector::GetData<int64_t>(vec)[out_row] = sval;
				break;
			}
			case EFieldKind::Float32:
				FlatVector::GetData<float>(vec)[out_row] = dec.read_float();
				break;
			case EFieldKind::Float64:
				FlatVector::GetData<double>(vec)[out_row] = dec.read_double();
				break;
			case EFieldKind::Boolean:
				FlatVector::GetData<bool>(vec)[out_row] = (dec.read_byte() != 0);
				break;
			case EFieldKind::Binary: {
				uint32_t len = dec.read_leb128_u32();
				std::string str_val(len, '\0');
				if (len > 0) {
					dec.read_bytes(&str_val[0], len);
				}
				FlatVector::GetData<string_t>(vec)[out_row] = StringVector::AddString(vec, str_val);
				break;
			}
			case EFieldKind::Nested:
				// TODO: nested — full LIST/STRUCT decode addressed in Issue #4
				// For now: if a non-null nested field is encountered, return NULL
				// (a present nested field cannot be decoded without Issue #4).
				FlatVector::SetNull(vec, out_row, true);
				break;
			}
		}

		out_row++;
	}

	output.SetCardinality(out_row);
}

// ---------------------------------------------------------------------------
// Register
// ---------------------------------------------------------------------------

void HailTableScanFunction::Register(ExtensionLoader &loader) {
	TableFunction func("hail_scan_table", {LogicalType::VARCHAR}, HailTableScan, HailTableBind, HailTableInitGlobal,
	                   HailTableInitLocal);
	loader.RegisterFunction(func);
}

} // namespace duckdb
