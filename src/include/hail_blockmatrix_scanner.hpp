#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {

struct HailBlockMatrixScanFunction {
	static void Register(ExtensionLoader &loader);
};

struct BlockMatrixMetadata {
	int64_t n_rows = 0;
	int64_t n_cols = 0;
	int32_t block_size = 0;
	std::vector<std::string> part_files; // relative to <path>/parts/
	// Flat block index per part_files entry (same position); Hail's `maybeFiltered` means not every
	// flat block index has a physical part file -- e.g. a symmetric matrix stored upper-triangle-only.
	std::vector<int32_t> block_indices;
};

// Reads <path>/metadata.json: blockSize, nRows, nCols, partFiles, and (if present) maybeFiltered.
// Shared by hail_scan_blockmatrix's bind phase and the LD query BM pair-extraction resolver.
BlockMatrixMetadata LoadBlockMatrixMetadata(FileSystem &fs, const std::string &path);

struct BlockMatrixBlockInfo {
	int64_t global_row_start;
	int64_t global_col_start;
	int64_t block_n_rows;
	int64_t block_n_cols;
};

// Maps a flat block index to its (global_row_start, global_col_start, block_n_rows, block_n_cols).
BlockMatrixBlockInfo ComputeBlockMatrixBlockInfo(int32_t block_idx, int32_t block_size, int64_t n_rows, int64_t n_cols);

// Decompresses a Hail LZ4-blocked stream (BlockingBufferSpec + LZ4FastBlockBufferSpec framing) into a
// flat byte vector. Shared by hail_scan_blockmatrix and the LD query BM pair-extraction resolver.
std::vector<uint8_t> DecompressHailLz4Stream(FileHandle &handle, const std::string &path_for_errors);

} // namespace duckdb
