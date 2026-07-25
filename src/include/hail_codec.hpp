#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {

class BlockDecoder {
public:
	virtual ~BlockDecoder() = default;

	virtual bool eof() const = 0;
	virtual uint8_t read_byte() = 0;
	virtual uint32_t read_leb128_u32() = 0;
	virtual uint64_t read_leb128_u64() = 0;
	virtual float read_float() = 0;
	virtual double read_double() = 0;
	virtual void read_bytes(void *buf, size_t n) = 0;
	virtual void skip_bytes(size_t n) = 0;
};

// ---------------------------------------------------------------------------
// ZstdBlockDecoder
//
// Decodes a Hail codec stack (innermost first):
//   StreamBlockBufferSpec : [int32_LE stream_block_len][stream_block_len bytes]
//     ZstdBlockBufferSpec : [int32_LE decomp_size][zstd payload]
//       BlockingBufferSpec: 64 KB logical window
//         LEB128BufferSpec: int/long = unsigned LEB128; float/double = raw IEEE 754
//
// Uses DuckDB's FileHandle so any VFS backend (local, S3, GCS, HTTP, …) is
// transparently supported.  The fill_block() method loops over partial reads
// (common for HTTP/cloud backends) just like DecompressHailLz4Stream does.
// ---------------------------------------------------------------------------
class ZstdBlockDecoder : public BlockDecoder {
public:
	ZstdBlockDecoder(FileHandle &handle, const std::string &path);

	// Returns true when all frames have been consumed and no bytes remain.
	// Maintained eagerly: after each byte is read, if the current block is
	// exhausted the next frame header is probed immediately, so eof() is
	// accurate without needing an extra read call.
	bool eof() const override {
		return eof_;
	}

	uint8_t read_byte() override;
	uint32_t read_leb128_u32() override; // unsigned LEB128, 1–5 bytes
	uint64_t read_leb128_u64() override; // unsigned LEB128, 1–9 bytes
	float read_float() override;         // 4 bytes raw IEEE 754
	double read_double() override;       // 8 bytes raw IEEE 754
	void read_bytes(void *buf, size_t n) override;
	void skip_bytes(size_t n) override;

private:
	FileHandle &handle_;
	std::string path_;
	std::vector<uint8_t> block_buf_; // current decompressed block
	size_t block_pos_ = 0;
	bool eof_ = false;

	// Reads exactly n bytes from the raw file handle, looping over partial reads.
	// Returns false only on a clean EOF when total == 0 (between frames).
	bool read_exact_raw(void *buf, int64_t n);

	// Reads the next outer stream frame, decompresses it into block_buf_, and
	// resets block_pos_.  Sets eof_ = true and returns false on clean EOF
	// between frames.
	bool fill_block();
};

class Lz4BlockDecoder : public BlockDecoder {
public:
	Lz4BlockDecoder(FileHandle &handle, const std::string &path);

	bool eof() const override {
		return eof_;
	}

	uint8_t read_byte() override;
	uint32_t read_leb128_u32() override;
	uint64_t read_leb128_u64() override;
	float read_float() override;
	double read_double() override;
	void read_bytes(void *buf, size_t n) override;
	void skip_bytes(size_t n) override;

private:
	FileHandle &handle_;
	std::string path_;
	std::vector<uint8_t> block_buf_;
	size_t block_pos_ = 0;
	bool eof_ = false;

	bool read_exact_raw(void *buf, int64_t n);
	bool fill_block();
};

std::unique_ptr<BlockDecoder> make_decoder(const std::string &codec_name, FileHandle &handle, const std::string &path);

// ---------------------------------------------------------------------------
// SQL table functions (testing helpers)
//
// hail_zstd_info(path)   → (frame_idx INTEGER, decomp_size INTEGER)
//   One row per outer stream frame; lets tests assert decomp_size values.
//
// hail_leb128_u32(path)  → (value UINTEGER)
//   Reads a count-prefixed sequence of unsigned LEB128 u32 values from the
//   stream (first LEB128 value = N, then N values follow).
//
// hail_leb128_u64(path)  → (value UBIGINT)
//   Same as above but reads u64 values.
// ---------------------------------------------------------------------------
struct HailCodecScanFunction {
	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
