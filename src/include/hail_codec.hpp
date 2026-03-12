#pragma once

#include "duckdb/common/file_system.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {

// ---------------------------------------------------------------------------
// ZstdBlockDecoder
//
// Reads a Hail part-file using the StreamBlockBufferSpec + ZstdBlockBufferSpec
// codec stack:
//
//   File:
//     Repeating frames until EOF:
//       [int32_LE stream_block_len]   <- byte length of the inner content
//       [int32_LE decomp_size]        <- first 4 bytes of the inner content
//       [stream_block_len-4 bytes]    <- Zstd-compressed payload
//
// After decompression, individual values are accessed via:
//   - read_leb128_u32 / read_leb128_u64  (unsigned LEB128 — also used for signed ints)
//   - read_float / read_double            (raw IEEE-754, little-endian)
//   - read_byte / read_bytes / skip_bytes (raw bytes)
// ---------------------------------------------------------------------------

class ZstdBlockDecoder {
public:
	ZstdBlockDecoder(FileHandle &handle, const std::string &path);

	// Returns true if all frames have been consumed and the internal buffer is empty.
	bool eof() const;

	// Returns true if there is more data to read (advance past an exhausted block
	// if necessary). Sets eof_ if no further frames exist.
	bool has_more_data();

	uint8_t read_byte();
	uint32_t read_leb128_u32();
	uint64_t read_leb128_u64();
	float read_float();
	double read_double();
	void read_bytes(void *buf, size_t n);
	void skip_bytes(size_t n);

private:
	FileHandle &handle_;
	std::string path_;
	std::vector<uint8_t> block_buf_;
	size_t block_pos_ = 0;
	bool eof_ = false;

	// Read exactly n bytes from handle_; returns false only on a clean EOF when
	// total == 0 (i.e. no bytes have been read yet).
	bool read_exact(void *buf, int64_t n);

	// Read the next stream frame, decompress it, and reset block_pos_.
	// Returns false (and sets eof_) if there are no more frames.
	bool fill_block();
};

} // namespace duckdb
