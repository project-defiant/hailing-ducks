#include "hail_codec.hpp"

#include "duckdb/common/exception.hpp"

// DuckDB bundles its own Zstd; use it to avoid an extra system dependency.
#include "zstd.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace duckdb {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ZstdBlockDecoder::ZstdBlockDecoder(FileHandle &handle, const std::string &path) : handle_(handle), path_(path) {
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

bool ZstdBlockDecoder::eof() const {
	return eof_ && (block_pos_ >= block_buf_.size());
}

bool ZstdBlockDecoder::has_more_data() {
	if (block_pos_ < block_buf_.size()) {
		return true;
	}
	if (eof_) {
		return false;
	}
	return fill_block();
}

uint8_t ZstdBlockDecoder::read_byte() {
	while (block_pos_ >= block_buf_.size()) {
		if (!fill_block()) {
			throw std::runtime_error("Unexpected EOF reading byte in: " + path_);
		}
	}
	return block_buf_[block_pos_++];
}

uint32_t ZstdBlockDecoder::read_leb128_u32() {
	uint32_t result = 0;
	int shift = 0;
	uint8_t b;
	do {
		b = read_byte();
		result |= static_cast<uint32_t>(b & 0x7Fu) << shift;
		shift += 7;
	} while (b & 0x80u);
	return result;
}

uint64_t ZstdBlockDecoder::read_leb128_u64() {
	uint64_t result = 0;
	int shift = 0;
	uint8_t b;
	do {
		b = read_byte();
		result |= static_cast<uint64_t>(b & 0x7Fu) << shift;
		shift += 7;
	} while (b & 0x80u);
	return result;
}

float ZstdBlockDecoder::read_float() {
	float v;
	read_bytes(&v, sizeof(v));
	return v;
}

double ZstdBlockDecoder::read_double() {
	double v;
	read_bytes(&v, sizeof(v));
	return v;
}

void ZstdBlockDecoder::read_bytes(void *buf, size_t n) {
	size_t done = 0;
	while (done < n) {
		while (block_pos_ >= block_buf_.size()) {
			if (!fill_block()) {
				throw std::runtime_error("Unexpected EOF reading bytes in: " + path_);
			}
		}
		size_t available = block_buf_.size() - block_pos_;
		size_t to_copy = std::min(available, n - done);
		std::memcpy(static_cast<uint8_t *>(buf) + done, block_buf_.data() + block_pos_, to_copy);
		block_pos_ += to_copy;
		done += to_copy;
	}
}

void ZstdBlockDecoder::skip_bytes(size_t n) {
	size_t skipped = 0;
	while (skipped < n) {
		while (block_pos_ >= block_buf_.size()) {
			if (!fill_block()) {
				throw std::runtime_error("Unexpected EOF skipping bytes in: " + path_);
			}
		}
		size_t available = block_buf_.size() - block_pos_;
		size_t to_skip = std::min(available, n - skipped);
		block_pos_ += to_skip;
		skipped += to_skip;
	}
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool ZstdBlockDecoder::read_exact(void *buf, int64_t n) {
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
	// Outer frame header: [int32_LE stream_block_len]
	uint8_t hdr[4];
	if (!read_exact(hdr, 4)) {
		eof_ = true;
		return false;
	}
	int32_t stream_block_len = static_cast<int32_t>(hdr[0]) | (static_cast<int32_t>(hdr[1]) << 8) |
	                           (static_cast<int32_t>(hdr[2]) << 16) | (static_cast<int32_t>(hdr[3]) << 24);
	if (stream_block_len < 4) {
		throw std::runtime_error("Invalid stream block length (" + std::to_string(stream_block_len) + ") in: " + path_);
	}

	// Inner content: [int32_LE decomp_size][zstd_payload]
	std::vector<uint8_t> inner(static_cast<size_t>(stream_block_len));
	if (!read_exact(inner.data(), stream_block_len)) {
		throw std::runtime_error("Truncated stream block in: " + path_);
	}

	int32_t decomp_size = static_cast<int32_t>(inner[0]) | (static_cast<int32_t>(inner[1]) << 8) |
	                      (static_cast<int32_t>(inner[2]) << 16) | (static_cast<int32_t>(inner[3]) << 24);
	if (decomp_size < 0) {
		throw std::runtime_error("Invalid decompressed size in: " + path_);
	}

	block_buf_.resize(static_cast<size_t>(decomp_size));
	size_t ret =
	    duckdb_zstd::ZSTD_decompress(block_buf_.data(), static_cast<size_t>(decomp_size), inner.data() + 4,
	                                 static_cast<size_t>(stream_block_len - 4));
	if (duckdb_zstd::ZSTD_isError(ret)) {
		throw std::runtime_error(std::string("Zstd decompression failed in: ") + path_ + ": " +
		                         duckdb_zstd::ZSTD_getErrorName(ret));
	}

	block_pos_ = 0;
	eof_ = false;
	return true;
}

} // namespace duckdb
