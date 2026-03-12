#include "hail_blockmatrix_scanner.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/function.hpp"

#include <nlohmann/json.hpp>
#include <lz4.h>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <atomic>
#include <cstring>
#include <cstdint>

namespace duckdb {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Hail LZ4-block stream reader
//
// On-disk format (BlockingBufferSpec + LZ4FastBlockBufferSpec):
//   Repeating frames until EOF:
//     [int32_LE: outer_frame_size]          <- 4 + compressed_len
//     [int32_LE: decompressed_size]         <- first 4 bytes of the frame
//     [outer_frame_size - 4 bytes: LZ4 data]
//
// Decompressed content for a block matrix partition:
//     [int32_LE: nRows]
//     [int32_LE: nCols]
//     [uint8:    isTranspose]
//     [float64 * nRows * nCols: data (column-major if !isTranspose, row-major if isTranspose)]
// ---------------------------------------------------------------------------

static int32_t read_le_int32(std::istream &in) {
	uint8_t buf[4];
	in.read(reinterpret_cast<char *>(buf), 4);
	if (!in) {
		throw std::runtime_error("Unexpected EOF reading int32");
	}
	return static_cast<int32_t>(buf[0]) | (static_cast<int32_t>(buf[1]) << 8) |
	       (static_cast<int32_t>(buf[2]) << 16) | (static_cast<int32_t>(buf[3]) << 24);
}

// Decompress a Hail LZ4-blocked stream into a flat byte vector.
static std::vector<uint8_t> DecompressHailLz4Stream(const std::string &path) {
	std::ifstream f(path, std::ios::binary);
	if (!f.is_open()) {
		throw std::runtime_error("Cannot open block matrix part file: " + path);
	}

	std::vector<uint8_t> result;
	result.reserve(64 * 1024);

	while (true) {
		// Peek: try to read outer_frame_size; break cleanly on EOF
		uint8_t probe[4];
		f.read(reinterpret_cast<char *>(probe), 4);
		if (f.eof() || f.gcount() == 0) {
			break;
		}
		if (f.gcount() != 4) {
			throw std::runtime_error("Truncated frame header in: " + path);
		}

		int32_t outer_frame_size = static_cast<int32_t>(probe[0]) | (static_cast<int32_t>(probe[1]) << 8) |
		                           (static_cast<int32_t>(probe[2]) << 16) | (static_cast<int32_t>(probe[3]) << 24);
		if (outer_frame_size < 4) {
			throw std::runtime_error("Invalid LZ4 frame size in: " + path);
		}

		int32_t compressed_len = outer_frame_size - 4;
		int32_t decompressed_len = read_le_int32(f);
		if (decompressed_len < 0) {
			throw std::runtime_error("Invalid negative decompressed size in: " + path);
		}

		std::vector<uint8_t> comp_buf(compressed_len);
		f.read(reinterpret_cast<char *>(comp_buf.data()), compressed_len);
		if (f.gcount() != compressed_len) {
			throw std::runtime_error("Truncated LZ4 compressed data in: " + path);
		}

		size_t prev_size = result.size();
		result.resize(prev_size + decompressed_len);

		int ret = LZ4_decompress_safe(reinterpret_cast<const char *>(comp_buf.data()),
		                              reinterpret_cast<char *>(result.data() + prev_size), compressed_len,
		                              decompressed_len);
		if (ret != decompressed_len) {
			throw std::runtime_error("LZ4 decompression failed in: " + path);
		}
	}

	return result;
}

// ---------------------------------------------------------------------------
// GridPartitioner: maps a flat partition index to (blockRow, blockCol)
// and computes the global row/col offsets and block dimensions.
// ---------------------------------------------------------------------------

struct BlockInfo {
	int64_t global_row_start;
	int64_t global_col_start;
	int64_t block_n_rows;
	int64_t block_n_cols;
};

static BlockInfo ComputeBlockInfo(int32_t block_idx, int32_t block_size, int64_t n_rows, int64_t n_cols) {
	int64_t n_block_cols = (n_cols + block_size - 1) / block_size;
	int64_t block_row = block_idx / n_block_cols;
	int64_t block_col = block_idx % n_block_cols;
	int64_t row_start = block_row * block_size;
	int64_t col_start = block_col * block_size;
	int64_t block_n_rows = std::min(static_cast<int64_t>(block_size), n_rows - row_start);
	int64_t block_n_cols = std::min(static_cast<int64_t>(block_size), n_cols - col_start);
	return {row_start, col_start, block_n_rows, block_n_cols};
}

// ---------------------------------------------------------------------------
// Bind data
// ---------------------------------------------------------------------------

struct HailBlockMatrixBindData : public TableFunctionData {
	std::string path;
	int64_t n_rows;
	int64_t n_cols;
	int32_t block_size;
	std::vector<std::string> part_files;   // path relative to <root>/parts/
	std::vector<int32_t> block_indices;    // which block index each part_file corresponds to
};

// ---------------------------------------------------------------------------
// Global scan state
// ---------------------------------------------------------------------------

struct HailBlockMatrixGlobalState : public GlobalTableFunctionState {
	std::atomic<idx_t> next_part {0};
	idx_t total_parts;

	explicit HailBlockMatrixGlobalState(idx_t total) : total_parts(total) {
	}

	idx_t MaxThreads() const override {
		return total_parts;
	}
};

// ---------------------------------------------------------------------------
// Local scan state — holds the decompressed block currently being emitted
// ---------------------------------------------------------------------------

struct HailBlockMatrixLocalState : public LocalTableFunctionState {
	// Currently loaded block
	std::vector<double> data; // column-major (or row-major if is_transpose)
	bool is_transpose = false;
	int64_t block_n_rows = 0;
	int64_t block_n_cols = 0;
	int64_t global_row_start = 0;
	int64_t global_col_start = 0;

	// Position within the current block: how many elements emitted so far
	int64_t element_offset = 0;
	int64_t total_elements = 0;

	// Which partition this local state is currently processing
	idx_t current_part = idx_t(-1);

	bool Done() const {
		return element_offset >= total_elements;
	}
};

// ---------------------------------------------------------------------------
// Bind
// ---------------------------------------------------------------------------

static unique_ptr<FunctionData> HailBlockMatrixBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<HailBlockMatrixBindData>();
	bind_data->path = input.inputs[0].GetValue<string>();

	// Read metadata.json
	std::string meta_path = bind_data->path + "/metadata.json";
	std::ifstream mf(meta_path);
	if (!mf.is_open()) {
		throw BinderException("Cannot open Hail BlockMatrix metadata: " + meta_path);
	}
	json meta;
	try {
		mf >> meta;
	} catch (const std::exception &e) {
		throw BinderException("Failed to parse BlockMatrix metadata at " + meta_path + ": " + e.what());
	}

	bind_data->block_size = meta.at("blockSize").get<int32_t>();
	if (bind_data->block_size <= 0) {
		throw BinderException("Invalid blockSize in metadata: must be positive");
	}
	bind_data->n_rows = meta.at("nRows").get<int64_t>();
	bind_data->n_cols = meta.at("nCols").get<int64_t>();

	for (auto &pf : meta.at("partFiles")) {
		std::string filename = pf.get<std::string>();
		if (filename.find("..") != std::string::npos || filename.find('/') != std::string::npos ||
		    filename.find('\\') != std::string::npos) {
			throw BinderException("Invalid partition filename in metadata (path traversal detected): " + filename);
		}
		bind_data->part_files.push_back(std::move(filename));
	}

	auto maybe_filtered = meta.find("maybeFiltered");
	if (maybe_filtered != meta.end() && !maybe_filtered->is_null()) {
		for (auto &idx : *maybe_filtered) {
			bind_data->block_indices.push_back(idx.get<int32_t>());
		}
	} else {
		// all partitions in order
		for (int32_t i = 0; i < static_cast<int32_t>(bind_data->part_files.size()); ++i) {
			bind_data->block_indices.push_back(i);
		}
	}

	// Output schema: (row_idx BIGINT, col_idx BIGINT, value DOUBLE)
	names = {"row_idx", "col_idx", "value"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE};

	return std::move(bind_data);
}

// ---------------------------------------------------------------------------
// Init global
// ---------------------------------------------------------------------------

static unique_ptr<GlobalTableFunctionState> HailBlockMatrixInitGlobal(ClientContext &context,
                                                                       TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<HailBlockMatrixBindData>();
	return make_uniq<HailBlockMatrixGlobalState>(bind_data.part_files.size());
}

// ---------------------------------------------------------------------------
// Init local
// ---------------------------------------------------------------------------

static unique_ptr<LocalTableFunctionState> HailBlockMatrixInitLocal(ExecutionContext &context,
                                                                     TableFunctionInitInput &input,
                                                                     GlobalTableFunctionState *global_state) {
	return make_uniq<HailBlockMatrixLocalState>();
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

static void HailBlockMatrixScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<HailBlockMatrixBindData>();
	auto &global_state = data.global_state->Cast<HailBlockMatrixGlobalState>();
	auto &local_state = data.local_state->Cast<HailBlockMatrixLocalState>();

	// Outer loop: fill output chunk, potentially spanning multiple partitions
	idx_t out_row = 0;
	const idx_t capacity = STANDARD_VECTOR_SIZE;

	while (out_row < capacity) {
		// If the current local block is exhausted, load the next partition
		if (local_state.Done()) {
			idx_t part_idx = global_state.next_part.fetch_add(1);
			if (part_idx >= global_state.total_parts) {
				break; // no more partitions
			}
			local_state.current_part = part_idx;

			std::string part_path =
			    bind_data.path + "/parts/" + bind_data.part_files[part_idx];
			int32_t block_idx = bind_data.block_indices[part_idx];

			BlockInfo bi = ComputeBlockInfo(block_idx, bind_data.block_size, bind_data.n_rows, bind_data.n_cols);
			local_state.global_row_start = bi.global_row_start;
			local_state.global_col_start = bi.global_col_start;
			local_state.block_n_rows = bi.block_n_rows;
			local_state.block_n_cols = bi.block_n_cols;
			local_state.element_offset = 0;

			// Decompress and parse the block
			std::vector<uint8_t> raw = DecompressHailLz4Stream(part_path);

			// Parse header: int32 nRows, int32 nCols, bool isTranspose
			if (raw.size() < 9) {
				throw std::runtime_error("Block file too small: " + part_path);
			}
			size_t cursor = 0;
			auto read32 = [&]() -> int32_t {
				int32_t v = static_cast<int32_t>(raw[cursor]) | (static_cast<int32_t>(raw[cursor + 1]) << 8) |
				            (static_cast<int32_t>(raw[cursor + 2]) << 16) | (static_cast<int32_t>(raw[cursor + 3]) << 24);
				cursor += 4;
				return v;
			};
			int32_t stored_rows = read32();
			int32_t stored_cols = read32();
			local_state.is_transpose = (raw[cursor++] != 0);

			if (stored_rows <= 0 || stored_cols <= 0) {
				throw std::runtime_error("Invalid block dimensions in: " + part_path);
			}
			if (stored_rows != bi.block_n_rows || stored_cols != bi.block_n_cols) {
				throw std::runtime_error("Block dimension mismatch in " + part_path + ": expected " +
				                         std::to_string(bi.block_n_rows) + "x" + std::to_string(bi.block_n_cols) +
				                         " but got " + std::to_string(stored_rows) + "x" + std::to_string(stored_cols));
			}

			int64_t n_elements = static_cast<int64_t>(stored_rows) * stored_cols;
			size_t data_bytes = static_cast<size_t>(n_elements) * sizeof(double);
			if (raw.size() - cursor < data_bytes) {
				throw std::runtime_error("Block file data truncated: " + part_path);
			}

			local_state.data.resize(n_elements);
			std::memcpy(local_state.data.data(), raw.data() + cursor, data_bytes);
			local_state.total_elements = n_elements;
			local_state.element_offset = 0;
		}

		// Emit elements from the current block into the output chunk
		idx_t to_emit = std::min(capacity - out_row,
		                         static_cast<idx_t>(local_state.total_elements - local_state.element_offset));

		auto &row_vec = output.data[0];
		auto &col_vec = output.data[1];
		auto &val_vec = output.data[2];

		int64_t block_rows = local_state.block_n_rows;
		int64_t block_cols = local_state.block_n_cols;

		for (idx_t i = 0; i < to_emit; i++) {
			int64_t elem_idx = local_state.element_offset + static_cast<int64_t>(i);
			int64_t local_row, local_col;
			if (local_state.is_transpose) {
				// row-major: element order is [col0row0, col1row0, ...]
				local_row = elem_idx / block_cols;
				local_col = elem_idx % block_cols;
			} else {
				// column-major: element order is [row0col0, row1col0, ...]
				local_row = elem_idx % block_rows;
				local_col = elem_idx / block_rows;
			}
			FlatVector::GetData<int64_t>(row_vec)[out_row + i] = local_state.global_row_start + local_row;
			FlatVector::GetData<int64_t>(col_vec)[out_row + i] = local_state.global_col_start + local_col;
			FlatVector::GetData<double>(val_vec)[out_row + i] = local_state.data[elem_idx];
		}

		local_state.element_offset += to_emit;
		out_row += to_emit;
	}

	output.SetCardinality(out_row);
}

// ---------------------------------------------------------------------------
// Register
// ---------------------------------------------------------------------------

void HailBlockMatrixScanFunction::Register(ExtensionLoader &loader) {
	TableFunction func("hail_scan_blockmatrix", {LogicalType::VARCHAR}, HailBlockMatrixScan, HailBlockMatrixBind,
	                   HailBlockMatrixInitGlobal, HailBlockMatrixInitLocal);
	loader.RegisterFunction(func);
}

} // namespace duckdb
