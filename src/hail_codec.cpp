#include "hail_codec.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"

// Use zstd bundled with DuckDB (duckdb_zstd namespace)
#include "zstd.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace duckdb {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

bool ZstdBlockDecoder::read_exact_raw(void *buf, int64_t n) {
	int64_t total = 0;
	while (total < n) {
		int64_t got = handle_.Read(static_cast<uint8_t *>(buf) + total, static_cast<idx_t>(n - total));
		if (got == 0) {
			if (total == 0) {
				return false; // clean EOF between frames
			}
			throw std::runtime_error("Truncated read in: " + path_);
		}
		total += got;
	}
	return true;
}

bool ZstdBlockDecoder::fill_block() {
	// StreamBlockBufferSpec outer frame:
	//   [int32_LE: stream_block_len] [stream_block_len bytes: raw_block]
	uint8_t hdr[4];
	if (!read_exact_raw(hdr, 4)) {
		eof_ = true;
		block_buf_.clear();
		block_pos_ = 0;
		return false; // clean EOF between frames
	}

	int32_t stream_block_len = static_cast<int32_t>(hdr[0]) | (static_cast<int32_t>(hdr[1]) << 8) |
	                           (static_cast<int32_t>(hdr[2]) << 16) | (static_cast<int32_t>(hdr[3]) << 24);
	if (stream_block_len < 4) {
		throw std::runtime_error("Invalid stream block size in: " + path_);
	}

	std::vector<uint8_t> raw(static_cast<size_t>(stream_block_len));
	if (!read_exact_raw(raw.data(), stream_block_len)) {
		throw std::runtime_error("Truncated stream block in: " + path_);
	}

	// ZstdBlockBufferSpec inner frame:
	//   raw_block[0..3] = int32_LE decomp_size
	//   raw_block[4..]  = zstd compressed payload
	int32_t decomp_size = static_cast<int32_t>(raw[0]) | (static_cast<int32_t>(raw[1]) << 8) |
	                      (static_cast<int32_t>(raw[2]) << 16) | (static_cast<int32_t>(raw[3]) << 24);
	if (decomp_size < 0) {
		throw std::runtime_error("Invalid negative decomp_size in: " + path_);
	}

	block_buf_.resize(static_cast<size_t>(decomp_size));
	size_t ret = duckdb_zstd::ZSTD_decompress(block_buf_.data(), static_cast<size_t>(decomp_size), raw.data() + 4,
	                             static_cast<size_t>(stream_block_len - 4));
	if (duckdb_zstd::ZSTD_isError(ret)) {
		throw std::runtime_error(std::string("Zstd decompression error in: ") + path_ + ": " +
		                         duckdb_zstd::ZSTD_getErrorName(ret));
	}
	if (ret != static_cast<size_t>(decomp_size)) {
		throw std::runtime_error("Zstd decompressed size mismatch in: " + path_);
	}

	block_pos_ = 0;
	return true;
}

// ---------------------------------------------------------------------------
// Constructor: eagerly fill the first block so eof() is accurate from the start.
// ---------------------------------------------------------------------------

ZstdBlockDecoder::ZstdBlockDecoder(FileHandle &handle, const std::string &path) : handle_(handle), path_(path) {
	fill_block(); // sets eof_=true for an empty file; otherwise populates block_buf_
}

// ---------------------------------------------------------------------------
// read_byte: after consuming the current block, immediately probe the next
// frame so that eof() reflects reality without requiring a separate call.
// ---------------------------------------------------------------------------

uint8_t ZstdBlockDecoder::read_byte() {
	if (eof_) {
		throw std::runtime_error("Read past end of stream in: " + path_);
	}
	// Refill if the current block is exhausted (handles decomp_size == 0 frames).
	while (block_pos_ >= block_buf_.size()) {
		if (!fill_block()) {
			throw std::runtime_error("Read past end of stream in: " + path_);
		}
	}
	uint8_t b = block_buf_[block_pos_++];
	// Eagerly probe next frame when the current block is fully consumed so
	// eof() returns true after the caller reads the last byte.
	if (block_pos_ >= block_buf_.size() && !eof_) {
		fill_block(); // populates next block_buf_ or sets eof_=true
	}
	return b;
}

// ---------------------------------------------------------------------------
// LEB128 readers (unsigned, not zigzag — Hail convention)
// ---------------------------------------------------------------------------

uint32_t ZstdBlockDecoder::read_leb128_u32() {
	uint32_t result = 0;
	int shift = 0;
	uint8_t b;
	int count = 0;
	do {
		if (count >= 5) {
			throw std::runtime_error("LEB128 u32 overflow (>5 bytes) in: " + path_);
		}
		b = read_byte();
		result |= (static_cast<uint32_t>(b & 0x7F) << shift);
		shift += 7;
		count++;
	} while (b & 0x80);
	return result;
}

uint64_t ZstdBlockDecoder::read_leb128_u64() {
	uint64_t result = 0;
	int shift = 0;
	uint8_t b;
	int count = 0;
	do {
		if (count >= 10) {
			throw std::runtime_error("LEB128 u64 overflow (>10 bytes) in: " + path_);
		}
		b = read_byte();
		result |= (static_cast<uint64_t>(b & 0x7F) << shift);
		shift += 7;
		count++;
	} while (b & 0x80);
	return result;
}

// ---------------------------------------------------------------------------
// Fixed-width readers
// ---------------------------------------------------------------------------

float ZstdBlockDecoder::read_float() {
	uint8_t buf[4];
	read_bytes(buf, 4);
	float v;
	std::memcpy(&v, buf, 4);
	return v;
}

double ZstdBlockDecoder::read_double() {
	uint8_t buf[8];
	read_bytes(buf, 8);
	double v;
	std::memcpy(&v, buf, 8);
	return v;
}

// ---------------------------------------------------------------------------
// read_bytes / skip_bytes: optimised bulk copy across block boundaries
// ---------------------------------------------------------------------------

void ZstdBlockDecoder::read_bytes(void *buf, size_t n) {
	size_t total = 0;
	while (total < n) {
		if (eof_) {
			throw std::runtime_error("Read past end of stream in: " + path_);
		}
		while (block_pos_ >= block_buf_.size()) {
			if (!fill_block()) {
				throw std::runtime_error("Read past end of stream in: " + path_);
			}
		}
		size_t avail = block_buf_.size() - block_pos_;
		size_t to_copy = std::min(avail, n - total);
		std::memcpy(static_cast<uint8_t *>(buf) + total, block_buf_.data() + block_pos_, to_copy);
		block_pos_ += to_copy;
		total += to_copy;
		// Eagerly probe next frame when block is exhausted
		if (block_pos_ >= block_buf_.size() && !eof_) {
			fill_block();
		}
	}
}

void ZstdBlockDecoder::skip_bytes(size_t n) {
	while (n > 0) {
		if (eof_) {
			throw std::runtime_error("Skip past end of stream in: " + path_);
		}
		while (block_pos_ >= block_buf_.size()) {
			if (!fill_block()) {
				throw std::runtime_error("Skip past end of stream in: " + path_);
			}
		}
		size_t avail = block_buf_.size() - block_pos_;
		size_t to_skip = std::min(avail, n);
		block_pos_ += to_skip;
		n -= to_skip;
		// Eagerly probe next frame when block is exhausted
		if (block_pos_ >= block_buf_.size() && !eof_) {
			fill_block();
		}
	}
}

// ===========================================================================
// SQL table functions for testing
// ===========================================================================

// ---------------------------------------------------------------------------
// hail_zstd_info(path VARCHAR) → (frame_idx INTEGER, decomp_size INTEGER)
//
// Reads raw outer stream frames without decompression and returns the
// decomp_size stored in the first 4 bytes of each inner frame.  Useful for
// asserting "First frame: decomp_size = 30145".
// ---------------------------------------------------------------------------

struct HailZstdInfoBindData : public TableFunctionData {
	std::string path;
};

struct HailZstdInfoLocalState : public LocalTableFunctionState {
	std::unique_ptr<FileHandle> handle;
	int32_t frame_idx = 0;
	bool done = false;

	// read_exact helper mirroring the pattern in hail_blockmatrix_scanner.cpp
	bool read_exact(void *buf, int64_t n) {
		int64_t total = 0;
		while (total < n) {
			int64_t got = handle->Read(static_cast<uint8_t *>(buf) + total, static_cast<idx_t>(n - total));
			if (got == 0) {
				if (total == 0) {
					return false;
				}
				throw std::runtime_error("Truncated read");
			}
			total += got;
		}
		return true;
	}
};

static unique_ptr<FunctionData> HailZstdInfoBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<HailZstdInfoBindData>();
	bind_data->path = input.inputs[0].GetValue<string>();
	names = {"frame_idx", "decomp_size"};
	return_types = {LogicalType::INTEGER, LogicalType::INTEGER};
	return std::move(bind_data);
}

static unique_ptr<LocalTableFunctionState> HailZstdInfoInitLocal(ExecutionContext &context,
                                                                  TableFunctionInitInput &input,
                                                                  GlobalTableFunctionState *) {
	auto &bind_data = input.bind_data->Cast<HailZstdInfoBindData>();
	auto local = make_uniq<HailZstdInfoLocalState>();
	auto &fs = FileSystem::GetFileSystem(context.client);
	local->handle = fs.OpenFile(bind_data.path, FileFlags::FILE_FLAGS_READ);
	return std::move(local);
}

static void HailZstdInfoScan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &local = data.local_state->Cast<HailZstdInfoLocalState>();
	if (local.done) {
		output.SetCardinality(0);
		return;
	}

	idx_t out_row = 0;
	const idx_t capacity = STANDARD_VECTOR_SIZE;

	while (out_row < capacity) {
		// Read outer frame length
		uint8_t hdr[4];
		if (!local.read_exact(hdr, 4)) {
			local.done = true;
			break;
		}
		int32_t stream_block_len = static_cast<int32_t>(hdr[0]) | (static_cast<int32_t>(hdr[1]) << 8) |
		                           (static_cast<int32_t>(hdr[2]) << 16) | (static_cast<int32_t>(hdr[3]) << 24);
		if (stream_block_len < 4) {
			throw std::runtime_error("Invalid stream block size");
		}

		// Read just the first 4 bytes of the inner frame (decomp_size)
		uint8_t ds_buf[4];
		if (!local.read_exact(ds_buf, 4)) {
			throw std::runtime_error("Truncated inner frame");
		}
		int32_t decomp_size = static_cast<int32_t>(ds_buf[0]) | (static_cast<int32_t>(ds_buf[1]) << 8) |
		                      (static_cast<int32_t>(ds_buf[2]) << 16) | (static_cast<int32_t>(ds_buf[3]) << 24);

		// Skip the zstd payload (stream_block_len - 4 bytes)
		int32_t skip_len = stream_block_len - 4;
		std::vector<uint8_t> skip_buf(static_cast<size_t>(skip_len));
		if (!local.read_exact(skip_buf.data(), skip_len)) {
			throw std::runtime_error("Truncated zstd payload");
		}

		FlatVector::GetData<int32_t>(output.data[0])[out_row] = local.frame_idx++;
		FlatVector::GetData<int32_t>(output.data[1])[out_row] = decomp_size;
		out_row++;
	}

	output.SetCardinality(out_row);
}

// ---------------------------------------------------------------------------
// hail_leb128_u32(path VARCHAR) → (value UINTEGER)
//
// Opens the path with ZstdBlockDecoder, reads the first LEB128 u32 as a
// count N, then reads N more LEB128 u32 values and returns them as rows.
// ---------------------------------------------------------------------------

struct HailLeb128U32BindData : public TableFunctionData {
	std::string path;
};

struct HailLeb128U32LocalState : public LocalTableFunctionState {
	std::unique_ptr<FileHandle> handle;
	std::unique_ptr<ZstdBlockDecoder> decoder;
	uint32_t count = 0;
	uint32_t idx = 0;
};

static unique_ptr<FunctionData> HailLeb128U32Bind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<HailLeb128U32BindData>();
	bind_data->path = input.inputs[0].GetValue<string>();
	names = {"value"};
	return_types = {LogicalType::UINTEGER};
	return std::move(bind_data);
}

static unique_ptr<LocalTableFunctionState> HailLeb128U32InitLocal(ExecutionContext &context,
                                                                   TableFunctionInitInput &input,
                                                                   GlobalTableFunctionState *) {
	auto &bind_data = input.bind_data->Cast<HailLeb128U32BindData>();
	auto local = make_uniq<HailLeb128U32LocalState>();
	auto &fs = FileSystem::GetFileSystem(context.client);
	local->handle = fs.OpenFile(bind_data.path, FileFlags::FILE_FLAGS_READ);
	local->decoder = make_uniq<ZstdBlockDecoder>(*local->handle, bind_data.path);
	local->count = local->decoder->read_leb128_u32(); // first value is count
	local->idx = 0;
	return std::move(local);
}

static void HailLeb128U32Scan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &local = data.local_state->Cast<HailLeb128U32LocalState>();

	idx_t out_row = 0;
	const idx_t capacity = STANDARD_VECTOR_SIZE;

	while (out_row < capacity && local.idx < local.count) {
		uint32_t val = local.decoder->read_leb128_u32();
		FlatVector::GetData<uint32_t>(output.data[0])[out_row] = val;
		out_row++;
		local.idx++;
	}

	output.SetCardinality(out_row);
}

// ---------------------------------------------------------------------------
// hail_leb128_u64(path VARCHAR) → (value UBIGINT)
//
// Same as hail_leb128_u32 but reads u64 values.  Count is also encoded as
// a LEB128 u64.
// ---------------------------------------------------------------------------

struct HailLeb128U64BindData : public TableFunctionData {
	std::string path;
};

struct HailLeb128U64LocalState : public LocalTableFunctionState {
	std::unique_ptr<FileHandle> handle;
	std::unique_ptr<ZstdBlockDecoder> decoder;
	uint64_t count = 0;
	uint64_t idx = 0;
};

static unique_ptr<FunctionData> HailLeb128U64Bind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<HailLeb128U64BindData>();
	bind_data->path = input.inputs[0].GetValue<string>();
	names = {"value"};
	return_types = {LogicalType::UBIGINT};
	return std::move(bind_data);
}

static unique_ptr<LocalTableFunctionState> HailLeb128U64InitLocal(ExecutionContext &context,
                                                                   TableFunctionInitInput &input,
                                                                   GlobalTableFunctionState *) {
	auto &bind_data = input.bind_data->Cast<HailLeb128U64BindData>();
	auto local = make_uniq<HailLeb128U64LocalState>();
	auto &fs = FileSystem::GetFileSystem(context.client);
	local->handle = fs.OpenFile(bind_data.path, FileFlags::FILE_FLAGS_READ);
	local->decoder = make_uniq<ZstdBlockDecoder>(*local->handle, bind_data.path);
	local->count = local->decoder->read_leb128_u64(); // first value is count
	local->idx = 0;
	return std::move(local);
}

static void HailLeb128U64Scan(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &local = data.local_state->Cast<HailLeb128U64LocalState>();

	idx_t out_row = 0;
	const idx_t capacity = STANDARD_VECTOR_SIZE;

	while (out_row < capacity && local.idx < local.count) {
		uint64_t val = local.decoder->read_leb128_u64();
		FlatVector::GetData<uint64_t>(output.data[0])[out_row] = val;
		out_row++;
		local.idx++;
	}

	output.SetCardinality(out_row);
}

// ---------------------------------------------------------------------------
// Register all codec test table functions
// ---------------------------------------------------------------------------

void HailCodecScanFunction::Register(ExtensionLoader &loader) {
	// hail_zstd_info(path)
	{
		TableFunction func("hail_zstd_info", {LogicalType::VARCHAR}, HailZstdInfoScan, HailZstdInfoBind,
		                   /* init_global */ nullptr, HailZstdInfoInitLocal);
		loader.RegisterFunction(func);
	}
	// hail_leb128_u32(path)
	{
		TableFunction func("hail_leb128_u32", {LogicalType::VARCHAR}, HailLeb128U32Scan, HailLeb128U32Bind,
		                   /* init_global */ nullptr, HailLeb128U32InitLocal);
		loader.RegisterFunction(func);
	}
	// hail_leb128_u64(path)
	{
		TableFunction func("hail_leb128_u64", {LogicalType::VARCHAR}, HailLeb128U64Scan, HailLeb128U64Bind,
		                   /* init_global */ nullptr, HailLeb128U64InitLocal);
		loader.RegisterFunction(func);
	}
}

} // namespace duckdb
