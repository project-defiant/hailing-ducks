#include "hail_table_scanner.hpp"

#include "hail_codec.hpp"
#include "hail_type_parser.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/function/table_function.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace duckdb {

using json = nlohmann::json;

static void ReadExact(FileHandle &handle, void *buf, int64_t n, const std::string &path) {
	int64_t total = 0;
	while (total < n) {
		int64_t got = handle.Read(static_cast<uint8_t *>(buf) + total, static_cast<idx_t>(n - total));
		if (got == 0) {
			throw IOException("Truncated read in HailTable metadata: " + path);
		}
		total += got;
	}
}

static json ReadGzipJson(FileSystem &fs, const std::string &path) {
	unique_ptr<FileHandle> handle;
	try {
		handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	} catch (const std::exception &e) {
		throw BinderException("Cannot open Hail table metadata: " + path + ": " + e.what());
	}

	int64_t file_size = fs.GetFileSize(*handle);
	std::vector<uint8_t> gz_raw(static_cast<size_t>(file_size));
	if (file_size > 0) {
		ReadExact(*handle, gz_raw.data(), file_size, path);
	}

	std::string json_str = GZipFileSystem::UncompressGZIPString(
	    reinterpret_cast<const char *>(gz_raw.data()), gz_raw.size());
	try {
		return json::parse(json_str);
	} catch (const std::exception &e) {
		throw BinderException("Failed to parse HailTable metadata at " + path + ": " + e.what());
	}
}

static bool IsSafeRelativePath(const std::string &path) {
	if (path.empty() || path[0] == '/' || path.find('\\') != std::string::npos) {
		return false;
	}
	size_t start = 0;
	while (start <= path.size()) {
		size_t end = path.find('/', start);
		std::string part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (part.empty() || part == "." || part == "..") {
			return false;
		}
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}
	return true;
}

static std::string ResolveCompressionCodecName(const json &buffer_spec) {
	static const std::unordered_set<std::string> compression_codecs = {
	    "ZstdBlockBufferSpec", "LZ4HCBlockBufferSpec", "LZ4FastBlockBufferSpec"};
	static const std::unordered_set<std::string> pass_through = {
	    "LEB128BufferSpec", "BlockingBufferSpec", "StreamBlockBufferSpec"};

	const json *node = &buffer_spec;
	while (true) {
		std::string name = node->at("name").get<std::string>();
		if (compression_codecs.count(name)) {
			return name;
		}
		if (!pass_through.count(name) || !node->contains("child")) {
			throw InvalidInputException("Unknown Hail codec: " + name);
		}
		node = &node->at("child");
	}
}

static void ValidateTypeCompatibility(const VTypeNode &vtype, const ETypeNode &etype, const std::string &path) {
	auto incompatible = [&]() {
		throw BinderException("Incompatible HailTable type metadata at " + path + ": VType " + vtype_to_string(vtype) +
		                      " is not compatible with EType " + etype_to_string(etype));
	};

	switch (vtype.kind) {
	case VKind::Int32:
		if (etype.kind != EKind::Int32) {
			incompatible();
		}
		return;
	case VKind::Int64:
		if (etype.kind != EKind::Int64) {
			incompatible();
		}
		return;
	case VKind::Float32:
		if (etype.kind != EKind::Float32) {
			incompatible();
		}
		return;
	case VKind::Float64:
		if (etype.kind != EKind::Float64) {
			incompatible();
		}
		return;
	case VKind::Boolean:
		if (etype.kind != EKind::Boolean) {
			incompatible();
		}
		return;
	case VKind::String:
		if (etype.kind != EKind::Binary) {
			incompatible();
		}
		return;
	case VKind::Array:
		if (etype.kind != EKind::Array || vtype.children.size() != 1 || etype.children.size() != 1) {
			incompatible();
		}
		ValidateTypeCompatibility(vtype.children[0], etype.children[0], path);
		return;
	case VKind::Struct:
		if (etype.kind != EKind::BaseStruct || vtype.children.size() != etype.children.size()) {
			incompatible();
		}
		for (idx_t i = 0; i < vtype.children.size(); i++) {
			if (vtype.children[i].name != etype.children[i].name) {
				incompatible();
			}
			ValidateTypeCompatibility(vtype.children[i], etype.children[i], path);
		}
		return;
	case VKind::Locus:
		if (etype.kind != EKind::BaseStruct || etype.children.size() != 2 ||
		    etype.children[0].name != "contig" || etype.children[0].kind != EKind::Binary ||
		    etype.children[1].name != "position" || etype.children[1].kind != EKind::Int32) {
			incompatible();
		}
		return;
	}
	incompatible();
}

// ---------------------------------------------------------------------------
// SkipValue: consumes exactly the bytes a value of the given EType occupies,
// without writing anything out. Needed so that fields not yet fully decoded
// (nested Array/BaseStruct, until Task 5 lands) don't desync the byte cursor
// for whatever comes after them in the row.
// ---------------------------------------------------------------------------

static void SkipValue(const ETypeNode &etype, BlockDecoder &decoder) {
	switch (etype.kind) {
	case EKind::Int32:
		decoder.read_leb128_u32();
		return;
	case EKind::Int64:
		decoder.read_leb128_u64();
		return;
	case EKind::Float32:
		decoder.read_float();
		return;
	case EKind::Float64:
		decoder.read_double();
		return;
	case EKind::Boolean:
		decoder.read_byte();
		return;
	case EKind::Binary: {
		uint32_t len = decoder.read_leb128_u32();
		decoder.skip_bytes(len);
		return;
	}
	case EKind::Array: {
		uint32_t len = decoder.read_leb128_u32();
		const ETypeNode &elem = etype.children[0];
		if (!elem.required) {
			// Same missing-bit protocol as BaseStruct below, but per-element: a
			// NULL element occupies zero bytes on the wire, so the bits must be
			// read and consulted -- calling SkipValue unconditionally for every
			// element (including NULL ones) would over-read into the next
			// element/field/row.
			size_t n_missing_bytes = (static_cast<size_t>(len) + 7) / 8;
			std::vector<uint8_t> missing_bits(n_missing_bytes);
			if (n_missing_bytes > 0) {
				decoder.read_bytes(missing_bits.data(), n_missing_bytes);
			}
			for (uint32_t i = 0; i < len; i++) {
				bool is_null = (missing_bits[i / 8] >> (i % 8)) & 1;
				if (!is_null) {
					SkipValue(elem, decoder);
				}
			}
		} else {
			for (uint32_t i = 0; i < len; i++) {
				SkipValue(elem, decoder);
			}
		}
		return;
	}
	case EKind::BaseStruct: {
		int n_optional = 0;
		for (auto &f : etype.children) {
			if (!f.required) {
				n_optional++;
			}
		}
		size_t n_missing_bytes = (static_cast<size_t>(n_optional) + 7) / 8;
		std::vector<uint8_t> missing_bits(n_missing_bytes);
		if (n_missing_bytes > 0) {
			decoder.read_bytes(missing_bits.data(), n_missing_bytes);
		}
		int optional_idx = 0;
		for (auto &f : etype.children) {
			bool is_null = false;
			if (!f.required) {
				is_null = (missing_bits[optional_idx / 8] >> (optional_idx % 8)) & 1;
				optional_idx++;
			}
			if (!is_null) {
				SkipValue(f, decoder);
			}
		}
		return;
	}
	}
	throw std::runtime_error("Unhandled EKind in SkipValue");
}

// ---------------------------------------------------------------------------
// DecodeValue: recursively writes the value of an EType into a DuckDB Vector
// at the given row. Optional/null handling is owned by the caller for the
// current value; nested arrays/structs read their own missing-bit prefixes.
// ---------------------------------------------------------------------------

static void DecodeValue(const ETypeNode &etype, BlockDecoder &decoder, Vector &out, idx_t row) {
	switch (etype.kind) {
	case EKind::Int32:
		FlatVector::GetData<int32_t>(out)[row] = static_cast<int32_t>(decoder.read_leb128_u32());
		return;
	case EKind::Int64:
		FlatVector::GetData<int64_t>(out)[row] = static_cast<int64_t>(decoder.read_leb128_u64());
		return;
	case EKind::Float32:
		FlatVector::GetData<float>(out)[row] = decoder.read_float();
		return;
	case EKind::Float64:
		FlatVector::GetData<double>(out)[row] = decoder.read_double();
		return;
	case EKind::Boolean:
		FlatVector::GetData<bool>(out)[row] = decoder.read_byte() != 0;
		return;
	case EKind::Binary: {
		uint32_t len = decoder.read_leb128_u32();
		std::vector<uint8_t> buf(len);
		decoder.read_bytes(buf.data(), len);
		FlatVector::GetData<string_t>(out)[row] =
		    StringVector::AddString(out, reinterpret_cast<const char *>(buf.data()), len);
		return;
	}
	case EKind::Array: {
		uint32_t len = decoder.read_leb128_u32();
		const ETypeNode &elem = etype.children[0];

		std::vector<uint8_t> missing_bits;
		if (!elem.required) {
			missing_bits.resize((static_cast<size_t>(len) + 7) / 8);
			if (!missing_bits.empty()) {
				decoder.read_bytes(missing_bits.data(), missing_bits.size());
			}
		}

		idx_t offset = ListVector::GetListSize(out);
		ListVector::Reserve(out, offset + len);
		Vector &child = ListVector::GetEntry(out);

		for (uint32_t i = 0; i < len; i++) {
			bool is_null = !elem.required && ((missing_bits[i / 8] >> (i % 8)) & 1);
			if (is_null) {
				FlatVector::SetNull(child, offset + i, true);
			} else {
				DecodeValue(elem, decoder, child, offset + i);
			}
		}

		ListVector::SetListSize(out, offset + len);
		ListVector::GetData(out)[row] = list_entry_t {offset, len};
		return;
	}
	case EKind::BaseStruct: {
		int n_optional = 0;
		for (auto &f : etype.children) {
			if (!f.required) {
				n_optional++;
			}
		}
		std::vector<uint8_t> missing_bits((static_cast<size_t>(n_optional) + 7) / 8);
		if (!missing_bits.empty()) {
			decoder.read_bytes(missing_bits.data(), missing_bits.size());
		}

		auto &entries = StructVector::GetEntries(out);
		int optional_idx = 0;
		for (idx_t i = 0; i < etype.children.size(); i++) {
			const ETypeNode &field = etype.children[i];
			bool is_null = false;
			if (!field.required) {
				is_null = (missing_bits[optional_idx / 8] >> (optional_idx % 8)) & 1;
				optional_idx++;
			}
			if (is_null) {
				FlatVector::SetNull(*entries[i], row, true);
			} else {
				DecodeValue(field, decoder, *entries[i], row);
			}
		}
		return;
	}
	}
	throw std::runtime_error("Unhandled EKind in DecodeValue");
}

// ---------------------------------------------------------------------------
// Bind data
// ---------------------------------------------------------------------------

struct HailTableBindData : public TableFunctionData {
	std::string path;
	std::string rows_rel_path;
	ETypeNode etype; // top-level EBaseStruct, one child per output column, in order
	std::string buffer_spec_name;
	std::vector<std::string> part_files; // relative to <path>/<rows_rel_path>/parts/
};

// ---------------------------------------------------------------------------
// Global / local scan state — same partition-parallelism pattern as
// hail_blockmatrix_scanner.cpp's HailBlockMatrixGlobalState/LocalState.
// ---------------------------------------------------------------------------

struct HailTableGlobalState : public GlobalTableFunctionState {
	std::atomic<idx_t> next_part {0};
	idx_t total_parts;

	explicit HailTableGlobalState(idx_t total) : total_parts(total) {
	}

	idx_t MaxThreads() const override {
		return total_parts;
	}
};

struct HailTableLocalState : public LocalTableFunctionState {
	std::unique_ptr<FileHandle> handle;
	std::unique_ptr<BlockDecoder> decoder;
	idx_t current_part = idx_t(-1);
	// Set when the per-row continuation flag reads 0 (see HailTableScan).
	// This, not decoder->eof(), is the authoritative "no more rows" signal --
	// verified against real Hail output, whose row stream is terminated by
	// an explicit 0x00 byte, not just by the codec block stream running out.
	bool partition_done = false;

	bool HasPartition() const {
		return decoder != nullptr;
	}

	bool Done() const {
		return decoder != nullptr && partition_done;
	}
};

// ---------------------------------------------------------------------------
// Bind
// ---------------------------------------------------------------------------

static unique_ptr<FunctionData> HailTableBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<HailTableBindData>();
	bind_data->path = input.inputs[0].GetValue<string>();

	auto &fs = FileSystem::GetFileSystem(context);

	std::string table_meta_path = bind_data->path + "/metadata.json.gz";
	json table_meta = ReadGzipJson(fs, table_meta_path);
	bind_data->rows_rel_path = table_meta.at("components").at("rows").at("rel_path").get<std::string>();
	if (!IsSafeRelativePath(bind_data->rows_rel_path)) {
		throw BinderException("Invalid HailTable rows component path in metadata: " + bind_data->rows_rel_path);
	}

	std::string meta_path = bind_data->path + "/" + bind_data->rows_rel_path + "/metadata.json.gz";
	json meta = ReadGzipJson(fs, meta_path);

	// NOTE: schema lives at the ROOT of rows/metadata.json.gz under "_codecSpec" --
	// there is no "rg"/"_RVDType" wrapper (that shape does not exist in real Hail
	// output; verified against s3://pan-ukb-us-east-1/ld_release/UKBB.EUR.ldadj.variant.b38.ht/).
	auto &codec_spec = meta.at("_codecSpec");
	std::string etype_str = codec_spec.at("_eType").get<std::string>();
	std::string vtype_str = codec_spec.at("_vType").get<std::string>();
	bind_data->buffer_spec_name = ResolveCompressionCodecName(codec_spec.at("_bufferSpec"));

	VTypeNode vtype;
	try {
		bind_data->etype = parse_etype(etype_str);
		vtype = parse_vtype(vtype_str);
	} catch (const std::exception &e) {
		throw BinderException("Failed to parse HailTable type strings at " + meta_path + ": " + e.what());
	}

	if (vtype.kind != VKind::Struct || bind_data->etype.kind != EKind::BaseStruct) {
		throw BinderException("HailTable row type must be a Struct at " + meta_path);
	}
	ValidateTypeCompatibility(vtype, bind_data->etype, meta_path);

	for (auto &field : vtype.children) {
		names.push_back(field.name);
		return_types.push_back(VTypeToDuckDBType(field));
	}

	for (auto &pf : meta.at("_partFiles")) {
		std::string filename = pf.get<std::string>();
		if (filename.find("..") != std::string::npos || filename.find('/') != std::string::npos ||
		    filename.find('\\') != std::string::npos) {
			throw BinderException("Invalid partition filename in metadata (path traversal detected): " + filename);
		}
		bind_data->part_files.push_back(std::move(filename));
	}

	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> HailTableInitGlobal(ClientContext &context,
                                                                TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<HailTableBindData>();
	return make_uniq<HailTableGlobalState>(bind_data.part_files.size());
}

static unique_ptr<LocalTableFunctionState> HailTableInitLocal(ExecutionContext &context,
                                                               TableFunctionInitInput &input,
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

	idx_t out_row = 0;
	const idx_t capacity = STANDARD_VECTOR_SIZE;

	// The row struct (bind_data.etype) is structurally just a BaseStruct like any
	// nested one -- if it has optional top-level fields, Hail's wire format
	// requires a ceil(n_optional/8)-byte missing-bit prefix to be read and
	// consulted before decoding the fields, exactly as SkipValue's BaseStruct
	// case already does for nested structs. Compute this once (bind_data.etype
	// doesn't change across rows) and reuse the scratch buffer per row.
	int n_optional_fields = 0;
	for (auto &f : bind_data.etype.children) {
		if (!f.required) {
			n_optional_fields++;
		}
	}
	size_t n_missing_bytes = (static_cast<size_t>(n_optional_fields) + 7) / 8;
	std::vector<uint8_t> missing_bits(n_missing_bytes);

	while (out_row < capacity) {
		if (!local_state.HasPartition() || local_state.Done()) {
			idx_t part_idx = global_state.next_part.fetch_add(1);
			if (part_idx >= global_state.total_parts) {
				break;
			}
			local_state.current_part = part_idx;
			std::string part_path = bind_data.path + "/" + bind_data.rows_rel_path + "/parts/" +
			                        bind_data.part_files[part_idx];
			auto &fs = FileSystem::GetFileSystem(context);
			local_state.handle = fs.OpenFile(part_path, FileFlags::FILE_FLAGS_READ);
			local_state.decoder = make_decoder(bind_data.buffer_spec_name, *local_state.handle, part_path);
			local_state.partition_done = false;
		}

		while (out_row < capacity && !local_state.partition_done) {
			// Every row is prefixed by a 1-byte continuation flag: nonzero means
			// "a row follows," 0x00 means "end of partition." This is a real Hail
			// wire-protocol detail (verified by manually decoding a real Pan-UKBB
			// partition) that is NOT part of the row struct's own missing-bit
			// encoding -- do not fold it into DecodeValue/SkipValue.
			uint8_t continue_flag = local_state.decoder->read_byte();
			if (continue_flag == 0) {
				local_state.partition_done = true;
				break;
			}
			if (n_missing_bytes > 0) {
				local_state.decoder->read_bytes(missing_bits.data(), n_missing_bytes);
			}
			int optional_idx = 0;
			for (idx_t col = 0; col < output.ColumnCount(); col++) {
				const auto &field = bind_data.etype.children[col];
				bool is_null = false;
				if (!field.required) {
					is_null = (missing_bits[optional_idx / 8] >> (optional_idx % 8)) & 1;
					optional_idx++;
				}
				if (is_null) {
					FlatVector::SetNull(output.data[col], out_row, true);
				} else {
					DecodeValue(field, *local_state.decoder, output.data[col], out_row);
				}
			}
			out_row++;
		}
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
