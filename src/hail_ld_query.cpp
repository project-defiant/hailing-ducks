#include "hail_ld_query.hpp"
#include "hail_table_scanner.hpp"
#include "hail_blockmatrix_scanner.hpp"
#include "hail_codec.hpp"

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/file_system.hpp"

#include <nlohmann/json.hpp>
#include <cstdio>

#include <vector>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <regex>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace duckdb {

// Simple status codes table function: hail_ld_status_codes()
struct StatusLocalState : public LocalTableFunctionState {
	struct Row {
		std::string domain;
		int32_t code;
		std::string name;
		std::string desc;
	};
	std::vector<Row> rows;
	idx_t next = 0;
};

static unique_ptr<LocalTableFunctionState>
HailLDStatusInitLocal(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *) {
	auto state = make_uniq<StatusLocalState>();
	// Prepare static list
	state->rows = {
	    {"variant", 0, "resolved_exact", "Variant resolved by exact match in HT"},
	    {"variant", 1, "resolved_flipped", "Variant resolved by flipped alleles in HT"},
	    {"variant", 2, "not_found_in_ht", "Variant not found in HT"},
	    {"variant", 3, "outside_locus", "Variant alleles outside supplied locus interval"},
	    {"variant", 4, "ambiguous_in_ht", "Variant ambiguous in HT"},
	    {"variant", 5, "unsupported_variant_id", "Variant ID not in supported biallelic format"},
	    {"variant", 6, "multiple_variants_at_position", "Distinct requested variant IDs share same contig/position"},
	    {"bm", 0, "bm_resolved", "BM value resolved"},
	    {"bm", 1, "bm_missing_in_ht", "BM lookup skipped due to missing HT row"},
	    {"bm", 2, "bm_index_out_of_bounds", "Requested BM index out of bounds"},
	    {"bm", 3, "bm_missing_block", "BM block file missing"},
	    {"bm", 4, "bm_missing_or_nan", "BM value missing or NaN"}};
	return std::move(state);
}

static void HailLDStatusCodes(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &local = data.local_state->Cast<StatusLocalState>();
	if (local.next >= local.rows.size()) {
		output.SetCardinality(0);
		return;
	}
	// use STANDARD_VECTOR_SIZE for table function emission capacity
	const idx_t capacity = STANDARD_VECTOR_SIZE;
	idx_t to_emit = std::min<idx_t>(capacity, (idx_t)(local.rows.size() - local.next));

	auto &domain_vec = output.data[0];
	auto &code_vec = output.data[1];
	auto &name_vec = output.data[2];
	auto &desc_vec = output.data[3];

	for (idx_t i = 0; i < to_emit; ++i) {
		auto &r = local.rows[local.next + i];
		FlatVector::GetData<string_t>(domain_vec)[i] = StringVector::AddString(domain_vec, r.domain.c_str());
		FlatVector::GetData<int32_t>(code_vec)[i] = r.code;
		FlatVector::GetData<string_t>(name_vec)[i] = StringVector::AddString(name_vec, r.name.c_str());
		FlatVector::GetData<string_t>(desc_vec)[i] = StringVector::AddString(desc_vec, r.desc.c_str());
	}
	local.next += to_emit;
	output.SetCardinality(to_emit);
}

// Hail LD request preflight: validate request rows and emit variant status events
// For TDD: implement basic parsing and status emission without HT/BM resolution.

struct PreflightOutRow {
	std::string locus_id;
	std::string req;
	int32_t code;
};

// A variant that passed all preflight structural checks (well-formed, in-locus, sole allele pair at
// its position) and is a candidate for real HailTable resolution (see ClassifyLocusVariants below).
struct LocusCandidate {
	std::string token;
	std::string contig;
	int64_t pos;
	std::string ref;
	std::string alt;
};

// The parsed (contig, start, end) of a locus_range string, as consumed by outside-locus filtering.
struct LocusInterval {
	std::string contig;
	int64_t start = 0;
	int64_t end = 0;
};

struct PreflightBindData : public TableFunctionData {
	std::string request_arg;
};

static unique_ptr<FunctionData> HailLDPreflightBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	// Output schema (variant status event): locus_id, requested_variant_id, status_domain, status_code
	names = {"locus_id", "requested_variant_id", "status_domain", "status_code"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER};
	auto bind_data = make_uniq<PreflightBindData>();
	if (!input.inputs.empty()) {
		bind_data->request_arg = input.inputs[0].GetValue<string>();
	}
	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> HailLDPreflightInitGlobal(ClientContext &context,
                                                                      TableFunctionInitInput &input) {
	return make_uniq<GlobalTableFunctionState>();
}

struct PreflightLocalState : public LocalTableFunctionState {
	// buffered output rows for this local scan
	std::vector<PreflightOutRow> rows;
	idx_t next = 0;
};

// Parses a "<contig>:<start>-<end>" locus range string. A malformed range is a structural error,
// not something to silently ignore (silently skipping outside-locus filtering would let out-of-range
// variants through undetected). Shared by ClassifyLocusVariants and the HT partition-pruning path.
static LocusInterval ParseLocusRangeOrThrow(const std::string &locus_range, const std::string &locus_id,
                                            const std::string &caller_name) {
	LocusInterval interval;
	bool malformed = true;
	if (!locus_range.empty()) {
		// expect e.g. chr1:100-200
		auto colon = locus_range.find(':');
		if (colon != string::npos) {
			interval.contig = locus_range.substr(0, colon);
			string rest = locus_range.substr(colon + 1);
			auto dash = rest.find('-');
			if (dash != string::npos) {
				string sstart = rest.substr(0, dash);
				string send = rest.substr(dash + 1);
				try {
					interval.start = stoll(sstart);
					interval.end = stoll(send);
					malformed = !(interval.start <= interval.end);
				} catch (...) {
					malformed = true;
				}
			}
		}
	}
	if (malformed) {
		throw BinderException("%s: malformed locus range '%s' for locus_id '%s' (expected "
		                      "'<contig>:<start>-<end>' with start <= end)",
		                      caller_name, locus_range, locus_id);
	}
	return interval;
}

// Shared per-locus classification: parses raw variant tokens against a locus range and appends
// variant-status events (dedupe, outside_locus, ambiguous/flipped allele grouping) to `rows`.
// Variants that pass every structural check are also appended to `candidates` (still without a
// terminal status row) for callers that go on to do real HailTable resolution; `interval` receives
// the parsed locus range. Used by the single-locus inline function, the multi-locus request-file
// function, and (via candidates/interval) the HT resolver.
static void ClassifyLocusVariants(const std::string &locus_id, const std::string &locus_range,
                                  const std::vector<std::string> &raw_tokens, std::vector<PreflightOutRow> &rows,
                                  std::vector<LocusCandidate> &candidates, LocusInterval &interval) {
	interval = ParseLocusRangeOrThrow(locus_range, locus_id, "hail_ld_preflight");
	const string &locus_contig = interval.contig;
	int64_t locus_start = interval.start, locus_end = interval.end;
	const bool locus_ok = true;

	// temp parsed list preserves order
	struct Parsed {
		string token;
		string contig;
		string pos;
		string ref;
		string alt;
		bool ok;
	};
	vector<Parsed> parsed_list;
	std::unordered_set<std::string> seen;
	for (auto v : raw_tokens) {
		// normalize each token: trim and skip empty tokens (from trailing/leading/repeated commas)
		StringUtil::Trim(v);
		// Manual trim fallback: remove ASCII isspace from ends
		auto trim_manual = [&](string &s) {
			size_t start = 0;
			while (start < s.size() && isspace((unsigned char)s[start]))
				start++;
			size_t end = s.size();
			while (end > start && isspace((unsigned char)s[end - 1]))
				end--;
			if (start != 0 || end != s.size())
				s = s.substr(start, end - start);
		};
		trim_manual(v);

		if (v.empty())
			continue;
		// remove any internal whitespace characters (safety)
		string v2 = v;
		v2.erase(std::remove_if(v2.begin(), v2.end(), [](unsigned char c) { return isspace(c); }), v2.end());
		if (v2.empty())
			continue;
		if (seen.count(v2))
			continue;
		seen.insert(v2);

		// parse token into components
		Parsed pentry {v2, "", "", "", "", false};
		auto parts_ = StringUtil::Split(v2, '_');
		if (parts_.size() >= 4) {
			pentry.alt = parts_.back();
			pentry.ref = parts_[parts_.size() - 2];
			pentry.pos = parts_[parts_.size() - 3];
			// contig = join(parts_[0 .. n-4])
			pentry.contig = parts_[0];
			// append all head parts up to the last three (pos, ref, alt)
			if (parts_.size() >= 4) {
				for (size_t ii = 1; ii <= parts_.size() - 4; ++ii) {
					pentry.contig += "_" + parts_[ii];
				}
			}
			// validate
			bool okpos = !pentry.pos.empty();
			for (char c : pentry.pos)
				if (!isdigit((unsigned char)c))
					okpos = false;
			auto is_base = [&](const string &a) {
				if (a.size() != 1)
					return false;
				char C = a[0];
				return C == 'A' || C == 'C' || C == 'G' || C == 'T';
			};
			if (okpos && is_base(pentry.ref) && is_base(pentry.alt)) {
				pentry.ok = true;
			}
		}
		parsed_list.push_back(std::move(pentry));
	}

	// post-process parsed_list to detect flips, multiples, and ambiguous groups
	// build groups by contig:pos
	std::unordered_map<std::string, vector<idx_t>> groups;
	for (idx_t i = 0; i < parsed_list.size(); ++i) {
		auto &p = parsed_list[i];
		if (!p.ok) {
			// emit unsupported immediately
			rows.push_back({locus_id, p.token, 5});
			continue;
		}
		// outside-locus detection: if locus range parsed, ensure contig matches and pos within [start,end]
		bool outside = false;
		if (locus_ok) {
			if (p.contig != locus_contig) {
				outside = true;
			} else {
				try {
					int64_t posv = stoll(p.pos);
					if (posv < locus_start || posv > locus_end)
						outside = true;
				} catch (...) {
					outside = true;
				}
			}
		}
		if (outside) {
			rows.push_back({locus_id, p.token, 3});
			continue;
		}
		string key = p.contig + ":" + p.pos;
		groups[key].push_back(i);
	}

	for (auto &kv : groups) {
		auto &indices = kv.second;
		// collect distinct allele pairs (ref,alt)
		std::unordered_set<string> allele_pairs;
		for (auto idx : indices) {
			auto &p = parsed_list[idx];
			allele_pairs.insert(p.ref + ":" + p.alt);
		}
		if (allele_pairs.size() > 1) {
			// request-level conflict (2+ distinct allele pairs at the same contig/position, whether or
			// not they're ref/alt flips of each other): exclude ALL of them from LD planning before any
			// HT work. Real exact/flipped resolution is the HT resolver's job (against actual HT rows),
			// not a preflight-time comparison between sibling requested IDs. Code 4 (ambiguous_in_ht) is
			// reserved for ambiguity discovered later, during HT resolution.
			for (auto idx : indices) {
				rows.push_back({locus_id, parsed_list[idx].token, 6});
			}
			continue;
		}
		// single allele pair -> candidate for real HT resolution; preflight only marks it as
		// structurally clean (placeholder 0), not yet confirmed against HT.
		for (auto idx : indices) {
			auto &p = parsed_list[idx];
			rows.push_back({locus_id, p.token, 0});
			candidates.push_back({p.token, p.contig, stoll(p.pos), p.ref, p.alt});
		}
	}
}

// ---------------------------------------------------------------------------
// HT partition pruning
// ---------------------------------------------------------------------------

// Returns the contiguous [lo, hi) band of partition indices whose range could contain rows of
// `contig`, using only the table's own recorded partition boundaries -- never a hardcoded reference
// contig order. Hail's real contig ordering is NOT lexical (confirmed against a real PanUKBB HT
// whose last partition's bound spans chr7_KI270803v1_alt -> chr19_KI270938v1_alt), so contig name
// comparisons here are opaque equality checks only. Because partitions are contiguous and
// non-overlapping in the table's true sort order, every partition touching a given contig forms one
// contiguous index range -- found by collecting every partition whose start OR end bound names that
// contig and taking the min/max of that set. Returns {0, 0} if the contig never appears.
static std::pair<idx_t, idx_t> FindPartitionRangeForContig(const std::vector<HailTablePartitionBound> &bounds,
                                                           const std::string &contig) {
	idx_t lo = bounds.size();
	idx_t hi = 0;
	for (idx_t i = 0; i < bounds.size(); ++i) {
		if (bounds[i].start_contig == contig || bounds[i].end_contig == contig) {
			lo = std::min(lo, i);
			hi = std::max(hi, i + 1);
		}
	}
	if (lo >= hi) {
		return {0, 0};
	}
	return {lo, hi};
}

// Whether a single partition's bound could contain a row in [qstart, qend] on `contig`. Only prunes
// by position when the partition's start and end share `contig` (a pure single-contig partition);
// a partition that straddles a contig boundary is always kept, since deciding which sub-range of it
// belongs to `contig` would require a full reference contig-order table -- safe (never wrongly
// pruned), just not maximally pruned.
static bool PartitionCouldContainPosition(const HailTablePartitionBound &b, const std::string &contig, int64_t qstart,
                                          int64_t qend) {
	if (b.start_contig != contig || b.end_contig != contig) {
		return true;
	}
	int64_t effective_start = b.include_start ? b.start_position : b.start_position + 1;
	int64_t effective_end = b.include_end ? b.end_position : b.end_position - 1;
	return effective_start <= qend && effective_end >= qstart;
}

// Selects the partition indices that could contain rows for the given locus interval. Callers must
// check `bounds.empty()` themselves first: an empty `bounds` means no partitioner info is available
// at all (unkeyed table, or no _jRangeBounds), which is "must scan every partition," not "select
// zero partitions" -- this function only makes sense once real bounds exist.
static std::vector<idx_t> SelectPartitionsForLocus(const std::vector<HailTablePartitionBound> &bounds,
                                                   const LocusInterval &interval) {
	std::vector<idx_t> selected;
	auto range = FindPartitionRangeForContig(bounds, interval.contig);
	for (idx_t i = range.first; i < range.second; ++i) {
		if (PartitionCouldContainPosition(bounds[i], interval.contig, interval.start, interval.end)) {
			selected.push_back(i);
		}
	}
	return selected;
}

// hail_ld_ht_partitions_for_locus(ht_path, locus_range) -> partition_idx: a small, directly
// queryable introspection function proving partition-pruning behavior through observable output
// (per the PRD's "verify external behavior, not internal structures" testing rule), mirroring the
// existing hail_ld_preflight_debug precedent.
struct HTPartitionsBindData : public TableFunctionData {
	std::vector<idx_t> partitions;
};

static unique_ptr<FunctionData> HailLDHTPartitionsBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<string> &names) {
	names = {"partition_idx"};
	return_types = {LogicalType::INTEGER};
	auto bind_data = make_uniq<HTPartitionsBindData>();
	if (input.inputs.size() < 2) {
		return std::move(bind_data);
	}
	std::string ht_path = input.inputs[0].GetValue<string>();
	std::string locus_range = input.inputs[1].GetValue<string>();

	auto &fs = FileSystem::GetFileSystem(context);
	auto metadata = LoadHailTableMetadata(fs, ht_path);
	auto interval = ParseLocusRangeOrThrow(locus_range, "n/a", "hail_ld_ht_partitions_for_locus");

	if (metadata.range_bounds.empty()) {
		for (idx_t i = 0; i < metadata.part_files.size(); ++i) {
			bind_data->partitions.push_back(i);
		}
	} else {
		bind_data->partitions = SelectPartitionsForLocus(metadata.range_bounds, interval);
	}
	return std::move(bind_data);
}

struct HTPartitionsLocalState : public LocalTableFunctionState {
	idx_t next = 0;
};

static unique_ptr<LocalTableFunctionState>
HailLDHTPartitionsInitLocal(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *) {
	return make_uniq<HTPartitionsLocalState>();
}

static void HailLDHTPartitionsScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<HTPartitionsBindData>();
	auto &local = data.local_state->Cast<HTPartitionsLocalState>();
	if (local.next >= bind_data.partitions.size()) {
		output.SetCardinality(0);
		return;
	}
	const idx_t capacity = STANDARD_VECTOR_SIZE;
	idx_t to_emit = std::min<idx_t>(capacity, bind_data.partitions.size() - local.next);
	auto &out_vec = output.data[0];
	for (idx_t i = 0; i < to_emit; ++i) {
		FlatVector::GetData<int32_t>(out_vec)[i] = static_cast<int32_t>(bind_data.partitions[local.next + i]);
	}
	local.next += to_emit;
	output.SetCardinality(to_emit);
}

static unique_ptr<LocalTableFunctionState>
HailLDPreflightInitLocal(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *) {
	auto state = make_uniq<PreflightLocalState>();
	// parse bind-time request_arg into rows, store in local state for chunked emission
	auto &bind = input.bind_data->Cast<PreflightBindData>();
	std::string s = bind.request_arg;
	if (s.empty()) {
		return std::move(state);
	}
	auto parts = StringUtil::Split(s, "|");
	if (parts.size() != 3) {
		return std::move(state);
	}
	std::string locus_id = parts[0];
	StringUtil::Trim(locus_id);
	std::string locus_range = parts[1];
	StringUtil::Trim(locus_range);
	std::string varlist = parts[2];
	auto raw_tokens = StringUtil::Split(varlist, ",");

	std::vector<LocusCandidate> unused_candidates;
	LocusInterval unused_interval;
	ClassifyLocusVariants(locus_id, locus_range, raw_tokens, state->rows, unused_candidates, unused_interval);
	return std::move(state);
}

static void HailLDPreflightScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &local = data.local_state->Cast<PreflightLocalState>();
	if (local.next >= local.rows.size()) {
		// no data left
		output.SetCardinality(0);
		return;
	}
	// use STANDARD_VECTOR_SIZE for table function emission capacity
	const idx_t capacity = STANDARD_VECTOR_SIZE;
	idx_t to_emit = std::min<idx_t>(capacity, (idx_t)(local.rows.size() - local.next));

	auto &locus_vec = output.data[0];
	auto &req_vec = output.data[1];
	auto &dom_vec = output.data[2];
	auto &code_vec = output.data[3];

	auto normalize = [&](string s) {
		// trim ASCII whitespace
		size_t start = 0;
		while (start < s.size() && isspace((unsigned char)s[start]))
			start++;
		size_t end = s.size();
		while (end > start && isspace((unsigned char)s[end - 1]))
			end--;
		return s.substr(start, end - start);
	};

	for (idx_t i = 0; i < to_emit; ++i) {
		auto &r = local.rows[local.next + i];
		string locus_out = normalize(r.locus_id);
		string req_out = normalize(r.req);
		FlatVector::GetData<string_t>(locus_vec)[i] = StringVector::AddString(locus_vec, locus_out.c_str());
		FlatVector::GetData<string_t>(req_vec)[i] = StringVector::AddString(req_vec, req_out.c_str());
		FlatVector::GetData<string_t>(dom_vec)[i] = StringVector::AddString(dom_vec, "variant");
		FlatVector::GetData<int32_t>(code_vec)[i] = r.code;
	}
	local.next += to_emit;
	output.SetCardinality(to_emit);
}

// One parsed row of a request-file Parquet: (locus_id VARCHAR, locus VARCHAR, variant_ids LIST<VARCHAR>).
struct LocusRequest {
	std::string locus_id;
	std::string locus_range;
	std::vector<std::string> raw_tokens;
};

// Reads a request-file Parquet with columns (locus_id VARCHAR, locus VARCHAR, variant_ids
// LIST<VARCHAR>), one row per fine-mapping locus. Shared by hail_ld_preflight_requests and
// hail_ld_resolve_ht so the file-reading/NULL-checking logic isn't duplicated between them.
static std::vector<LocusRequest> ReadLocusRequests(ClientContext &context, const std::string &requests_path,
                                                   const std::string &caller_name) {
	std::string escaped_path = StringUtil::Replace(requests_path, "'", "''");

	Connection con(*context.db);
	auto result = con.Query("SELECT locus_id, locus, variant_ids FROM read_parquet('" + escaped_path + "')");
	if (result->HasError()) {
		throw BinderException("%s: failed to read request file '%s': %s", caller_name, requests_path,
		                      result->GetError());
	}
	std::vector<LocusRequest> requests;
	while (auto chunk = result->Fetch()) {
		for (idx_t row = 0; row < chunk->size(); ++row) {
			auto locus_id_value = chunk->GetValue(0, row);
			auto locus_range_value = chunk->GetValue(1, row);
			auto variant_ids_value = chunk->GetValue(2, row);
			if (locus_id_value.IsNull() || locus_range_value.IsNull() || variant_ids_value.IsNull()) {
				throw BinderException(
				    "%s: request file '%s' row %d has a NULL locus_id/locus/variant_ids column; all three "
				    "columns are required",
				    caller_name, requests_path, (int)row);
			}
			LocusRequest req;
			req.locus_id = locus_id_value.GetValue<string>();
			req.locus_range = locus_range_value.GetValue<string>();
			for (auto &item : ListValue::GetChildren(variant_ids_value)) {
				if (item.IsNull()) {
					throw BinderException(
					    "%s: request file '%s' locus_id '%s' has a NULL entry in variant_ids; all requested "
					    "variant IDs must be non-NULL",
					    caller_name, requests_path, req.locus_id);
				}
				req.raw_tokens.push_back(item.GetValue<string>());
			}
			requests.push_back(std::move(req));
		}
	}
	return requests;
}

// Multi-locus request file: reads a Parquet file with columns
// (locus_id VARCHAR, locus VARCHAR, variant_ids LIST<VARCHAR>), one row per fine-mapping locus,
// and classifies each locus's variant list the same way hail_ld_preflight does for a single locus.
struct PreflightRequestsBindData : public TableFunctionData {
	std::vector<PreflightOutRow> rows;
};

static unique_ptr<FunctionData> HailLDPreflightRequestsBind(ClientContext &context, TableFunctionBindInput &input,
                                                            vector<LogicalType> &return_types, vector<string> &names) {
	names = {"locus_id", "requested_variant_id", "status_domain", "status_code"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER};
	auto bind_data = make_uniq<PreflightRequestsBindData>();
	if (input.inputs.empty()) {
		return std::move(bind_data);
	}
	std::string requests_path = input.inputs[0].GetValue<string>();
	auto requests = ReadLocusRequests(context, requests_path, "hail_ld_preflight_requests");
	for (auto &req : requests) {
		std::vector<LocusCandidate> unused_candidates;
		LocusInterval unused_interval;
		ClassifyLocusVariants(req.locus_id, req.locus_range, req.raw_tokens, bind_data->rows, unused_candidates,
		                      unused_interval);
	}
	return std::move(bind_data);
}

static unique_ptr<LocalTableFunctionState>
HailLDPreflightRequestsInitLocal(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *) {
	auto state = make_uniq<PreflightLocalState>();
	auto &bind = input.bind_data->Cast<PreflightRequestsBindData>();
	state->rows = bind.rows;
	return std::move(state);
}

// ---------------------------------------------------------------------------
// hail_ld_resolve_ht(ht_path, requests_path): batch HailTable resolver
// ---------------------------------------------------------------------------

// One resolved (or terminal-status) variant event: a superset of PreflightOutRow with the extra
// columns real HT resolution can fill in. Nullable columns are tracked with explicit flags rather
// than sentinel values, since idx/allele_order have no value that can't legitimately occur.
struct HTResolveOutRow {
	std::string locus_id;
	std::string requested_variant_id;
	std::string matched_variant_id;
	bool has_matched_variant_id = false;
	int64_t idx = 0;
	bool has_idx = false;
	int32_t allele_order = 0;
	bool has_allele_order = false;
	int32_t status_code = 0;
};

struct HTResolveBindData : public TableFunctionData {
	std::vector<HTResolveOutRow> rows;
};

// One HT-side match found for a candidate: its row idx, the HT's own allele pair (used to
// reconstruct matched_variant_id, which can differ from requested_variant_id on a flip), and the
// allele_order sign (1 = exact, -1 = flipped).
struct HTMatch {
	int64_t idx;
	std::string allele0, allele1;
	int32_t allele_order;
};

static unique_ptr<FunctionData> HailLDResolveHTBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	names = {"locus_id",     "requested_variant_id", "matched_variant_id", "idx",
	         "allele_order", "status_domain",        "status_code"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT,
	                LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::INTEGER};
	auto bind_data = make_uniq<HTResolveBindData>();
	if (input.inputs.size() < 2) {
		return std::move(bind_data);
	}
	std::string ht_path = input.inputs[0].GetValue<string>();
	std::string requests_path = input.inputs[1].GetValue<string>();

	auto &fs = FileSystem::GetFileSystem(context);
	auto metadata = LoadHailTableMetadata(fs, ht_path);

	// Real HT schemas carry many more fields than these three (rsid, AF, liftover metadata, ...);
	// DecodeHailTableRow decodes the FULL row (skipping past whatever else is there) so the byte
	// cursor stays correctly synced -- this resolver only reads back the columns it needs, by name.
	int locus_col = -1, alleles_col = -1, idx_col = -1;
	for (idx_t i = 0; i < metadata.vtype.children.size(); ++i) {
		auto &f = metadata.vtype.children[i];
		if (f.name == "locus" && f.kind == VKind::Locus) {
			locus_col = static_cast<int>(i);
		} else if (f.name == "alleles" && f.kind == VKind::Array && !f.children.empty() &&
		           f.children[0].kind == VKind::String) {
			alleles_col = static_cast<int>(i);
		} else if (f.name == "idx" && f.kind == VKind::Int64) {
			idx_col = static_cast<int>(i);
		}
	}
	if (locus_col < 0 || alleles_col < 0 || idx_col < 0) {
		throw BinderException(
		    "hail_ld_resolve_ht: HailTable at '%s' is missing a required 'locus'/'alleles'/'idx' field "
		    "(found locus=%s, alleles=%s, idx=%s) -- this is a structural precondition for HT resolution, "
		    "not biological missingness",
		    ht_path, locus_col >= 0 ? "yes" : "no", alleles_col >= 0 ? "yes" : "no", idx_col >= 0 ? "yes" : "no");
	}

	vector<LogicalType> row_types;
	for (auto &f : metadata.vtype.children) {
		row_types.push_back(VTypeToDuckDBType(f));
	}
	DataChunk row_chunk;
	row_chunk.Initialize(context, row_types, 1);

	auto requests = ReadLocusRequests(context, requests_path, "hail_ld_resolve_ht");
	for (auto &req : requests) {
		std::vector<PreflightOutRow> preflight_rows;
		std::vector<LocusCandidate> candidates;
		LocusInterval interval;
		ClassifyLocusVariants(req.locus_id, req.locus_range, req.raw_tokens, preflight_rows, candidates, interval);

		// Terminal preflight statuses (outside_locus/unsupported/multiple_variants_at_position) pass
		// straight through, never touching HT; the placeholder "0" rows are dropped here since their
		// corresponding candidate gets its own authoritative status below.
		for (auto &r : preflight_rows) {
			if (r.code == 0) {
				continue;
			}
			HTResolveOutRow out;
			out.locus_id = r.locus_id;
			out.requested_variant_id = r.req;
			out.status_code = r.code;
			bind_data->rows.push_back(std::move(out));
		}
		if (candidates.empty()) {
			continue;
		}

		std::vector<idx_t> selected_partitions;
		if (metadata.range_bounds.empty()) {
			// No partitioner info available (unkeyed table / no _jRangeBounds) -- scan everything.
			for (idx_t i = 0; i < metadata.part_files.size(); ++i) {
				selected_partitions.push_back(i);
			}
		} else {
			selected_partitions = SelectPartitionsForLocus(metadata.range_bounds, interval);
		}

		std::unordered_map<std::string, std::vector<idx_t>> candidates_by_pos;
		for (idx_t i = 0; i < candidates.size(); ++i) {
			auto &c = candidates[i];
			candidates_by_pos[c.contig + ":" + std::to_string(c.pos)].push_back(i);
		}

		std::vector<std::vector<HTMatch>> matches(candidates.size());
		for (idx_t part_idx : selected_partitions) {
			std::string part_path = ht_path + "/" + metadata.rows_rel_path + "/parts/" + metadata.part_files[part_idx];
			auto handle = fs.OpenFile(part_path, FileFlags::FILE_FLAGS_READ);
			auto decoder = make_decoder(metadata.buffer_spec_name, *handle, part_path);
			while (true) {
				row_chunk.Reset();
				if (!DecodeHailTableRow(*decoder, metadata.etype, row_chunk, 0)) {
					break;
				}
				auto alleles_value = row_chunk.GetValue(alleles_col, 0);
				if (alleles_value.IsNull()) {
					continue;
				}
				auto &allele_children = ListValue::GetChildren(alleles_value);
				if (allele_children.size() != 2) {
					// Multi-allelic (or mono-allelic) HT rows are never eligible for biallelic matching.
					continue;
				}
				if (allele_children[0].IsNull() || allele_children[1].IsNull()) {
					continue;
				}

				auto locus_value = row_chunk.GetValue(locus_col, 0);
				auto &locus_children = StructValue::GetChildren(locus_value);
				std::string row_contig = locus_children[0].GetValue<string>();
				int64_t row_pos = locus_children[1].GetValue<int32_t>();

				auto pos_it = candidates_by_pos.find(row_contig + ":" + std::to_string(row_pos));
				if (pos_it == candidates_by_pos.end()) {
					continue;
				}

				std::string a0 = allele_children[0].GetValue<string>();
				std::string a1 = allele_children[1].GetValue<string>();
				int64_t row_idx = row_chunk.GetValue(idx_col, 0).GetValue<int64_t>();

				for (idx_t cand_idx : pos_it->second) {
					auto &c = candidates[cand_idx];
					if (a0 == c.ref && a1 == c.alt) {
						matches[cand_idx].push_back({row_idx, a0, a1, 1});
					} else if (a0 == c.alt && a1 == c.ref) {
						matches[cand_idx].push_back({row_idx, a0, a1, -1});
					}
				}
			}
		}

		for (idx_t i = 0; i < candidates.size(); ++i) {
			auto &c = candidates[i];
			auto &m = matches[i];
			HTResolveOutRow out;
			out.locus_id = req.locus_id;
			out.requested_variant_id = c.token;
			if (m.empty()) {
				out.status_code = 2; // not_found_in_ht
			} else if (m.size() > 1) {
				out.status_code = 4; // ambiguous_in_ht: HT itself has >1 row matching this candidate
			} else {
				out.has_matched_variant_id = true;
				out.matched_variant_id =
				    c.contig + "_" + std::to_string(c.pos) + "_" + m[0].allele0 + "_" + m[0].allele1;
				out.has_idx = true;
				out.idx = m[0].idx;
				out.has_allele_order = true;
				out.allele_order = m[0].allele_order;
				out.status_code = m[0].allele_order == 1 ? 0 : 1; // resolved_exact / resolved_flipped
			}
			bind_data->rows.push_back(std::move(out));
		}
	}

	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> HailLDResolveHTInitGlobal(ClientContext &context,
                                                                      TableFunctionInitInput &input) {
	return make_uniq<GlobalTableFunctionState>();
}

struct HTResolveLocalState : public LocalTableFunctionState {
	std::vector<HTResolveOutRow> rows;
	idx_t next = 0;
};

static unique_ptr<LocalTableFunctionState>
HailLDResolveHTInitLocal(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *) {
	auto state = make_uniq<HTResolveLocalState>();
	auto &bind = input.bind_data->Cast<HTResolveBindData>();
	state->rows = bind.rows;
	return std::move(state);
}

static void HailLDResolveHTScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &local = data.local_state->Cast<HTResolveLocalState>();
	if (local.next >= local.rows.size()) {
		output.SetCardinality(0);
		return;
	}
	const idx_t capacity = STANDARD_VECTOR_SIZE;
	idx_t to_emit = std::min<idx_t>(capacity, local.rows.size() - local.next);

	auto &locus_vec = output.data[0];
	auto &req_vec = output.data[1];
	auto &matched_vec = output.data[2];
	auto &idx_vec = output.data[3];
	auto &allele_order_vec = output.data[4];
	auto &dom_vec = output.data[5];
	auto &code_vec = output.data[6];

	for (idx_t i = 0; i < to_emit; ++i) {
		auto &r = local.rows[local.next + i];
		FlatVector::GetData<string_t>(locus_vec)[i] = StringVector::AddString(locus_vec, r.locus_id);
		FlatVector::GetData<string_t>(req_vec)[i] = StringVector::AddString(req_vec, r.requested_variant_id);
		if (r.has_matched_variant_id) {
			FlatVector::GetData<string_t>(matched_vec)[i] = StringVector::AddString(matched_vec, r.matched_variant_id);
		} else {
			FlatVector::SetNull(matched_vec, i, true);
		}
		if (r.has_idx) {
			FlatVector::GetData<int64_t>(idx_vec)[i] = r.idx;
		} else {
			FlatVector::SetNull(idx_vec, i, true);
		}
		if (r.has_allele_order) {
			FlatVector::GetData<int32_t>(allele_order_vec)[i] = r.allele_order;
		} else {
			FlatVector::SetNull(allele_order_vec, i, true);
		}
		FlatVector::GetData<string_t>(dom_vec)[i] = StringVector::AddString(dom_vec, "variant");
		FlatVector::GetData<int32_t>(code_vec)[i] = r.status_code;
	}
	local.next += to_emit;
	output.SetCardinality(to_emit);
}

// ---------------------------------------------------------------------------
// hail_ld_bm_pairs(bm_path, resolved): BlockMatrix pair extraction for one locus
// ---------------------------------------------------------------------------

struct BMResolvedVariant {
	int64_t idx;
	int32_t allele_order;
};

struct BMPairOutRow {
	int64_t idx_i;
	int64_t idx_j;
	double r = 0.0;
	bool has_r = false;
	int32_t status_code;
};

struct HailLDBMPairsBindData : public TableFunctionData {
	std::vector<BMPairOutRow> rows;
};

static unique_ptr<FunctionData> HailLDBMPairsBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	names = {"idx_i", "idx_j", "r", "bm_status_domain", "bm_status_code"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::DOUBLE, LogicalType::VARCHAR,
	                LogicalType::INTEGER};
	auto bind_data = make_uniq<HailLDBMPairsBindData>();
	if (input.inputs.size() < 2) {
		return std::move(bind_data);
	}
	std::string bm_path = input.inputs[0].GetValue<string>();

	std::vector<BMResolvedVariant> resolved;
	for (auto &item : ListValue::GetChildren(input.inputs[1])) {
		if (item.IsNull()) {
			continue;
		}
		auto &fields = StructValue::GetChildren(item);
		resolved.push_back({fields[0].GetValue<int64_t>(), fields[1].GetValue<int32_t>()});
	}
	// Canonical pair generation (idx_i < idx_j) falls out naturally from generating pairs over a
	// sorted-by-idx list, rather than sorting each pair individually.
	std::sort(resolved.begin(), resolved.end(),
	          [](const BMResolvedVariant &a, const BMResolvedVariant &b) { return a.idx < b.idx; });
	if (resolved.size() < 2) {
		return std::move(bind_data); // fewer than 2 resolved variants -> no pairs to form
	}

	auto &fs = FileSystem::GetFileSystem(context);
	auto metadata = LoadBlockMatrixMetadata(fs, bm_path);
	int64_t n_block_cols = (metadata.n_cols + metadata.block_size - 1) / metadata.block_size;

	std::unordered_map<int32_t, idx_t> block_idx_to_part_pos;
	for (idx_t i = 0; i < metadata.block_indices.size(); ++i) {
		block_idx_to_part_pos[metadata.block_indices[i]] = i;
	}

	// Decoded-block cache, keyed by flat block index: a block file is opened and decompressed lazily
	// on first need, then reused for every later pair whose cell falls in the same block -- this is
	// what makes each touched block get read once per this locus's whole pair set. "Confirmed
	// missing" is memoized too, so a repeatedly-requested absent block isn't re-looked-up either.
	struct DecodedBlock {
		std::vector<double> data;
		bool is_transpose;
		int64_t block_n_rows, block_n_cols;
	};
	std::unordered_map<int32_t, unique_ptr<DecodedBlock>> block_cache;
	std::unordered_set<int32_t> confirmed_missing;

	// Returns the raw (uncorrected) cell value at (row, col); sets out_missing_block when the block
	// containing this cell has no physical part file (Hail's `maybeFiltered` sparse storage).
	auto get_cell = [&](int64_t row, int64_t col, bool &out_missing_block) -> double {
		int64_t block_row = row / metadata.block_size;
		int64_t block_col = col / metadata.block_size;
		int32_t block_idx = static_cast<int32_t>(block_row * n_block_cols + block_col);

		if (confirmed_missing.count(block_idx)) {
			out_missing_block = true;
			return 0.0;
		}
		auto cache_it = block_cache.find(block_idx);
		if (cache_it == block_cache.end()) {
			auto pos_it = block_idx_to_part_pos.find(block_idx);
			if (pos_it == block_idx_to_part_pos.end()) {
				confirmed_missing.insert(block_idx);
				out_missing_block = true;
				return 0.0;
			}
			std::string part_path = bm_path + "/parts/" + metadata.part_files[pos_it->second];
			auto handle = fs.OpenFile(part_path, FileFlags::FILE_FLAGS_READ);
			std::vector<uint8_t> raw = DecompressHailLz4Stream(*handle, part_path);
			if (raw.size() < 9) {
				throw IOException("hail_ld_bm_pairs: BlockMatrix block file too small: " + part_path);
			}
			size_t cursor = 0;
			auto read32 = [&]() -> int32_t {
				int32_t v = static_cast<int32_t>(raw[cursor]) | (static_cast<int32_t>(raw[cursor + 1]) << 8) |
				            (static_cast<int32_t>(raw[cursor + 2]) << 16) |
				            (static_cast<int32_t>(raw[cursor + 3]) << 24);
				cursor += 4;
				return v;
			};
			int32_t stored_rows = read32();
			int32_t stored_cols = read32();
			bool is_transpose = (raw[cursor++] != 0);
			auto bi = ComputeBlockMatrixBlockInfo(block_idx, metadata.block_size, metadata.n_rows, metadata.n_cols);
			if (stored_rows != bi.block_n_rows || stored_cols != bi.block_n_cols) {
				throw IOException("hail_ld_bm_pairs: BlockMatrix block dimension mismatch in " + part_path);
			}
			int64_t n_elements = static_cast<int64_t>(stored_rows) * stored_cols;
			size_t data_bytes = static_cast<size_t>(n_elements) * sizeof(double);
			if (raw.size() - cursor < data_bytes) {
				throw IOException("hail_ld_bm_pairs: BlockMatrix block file data truncated: " + part_path);
			}
			auto block = make_uniq<DecodedBlock>();
			block->data.resize(n_elements);
			std::memcpy(block->data.data(), raw.data() + cursor, data_bytes);
			block->is_transpose = is_transpose;
			block->block_n_rows = bi.block_n_rows;
			block->block_n_cols = bi.block_n_cols;
			cache_it = block_cache.emplace(block_idx, std::move(block)).first;
		}
		auto &block = *cache_it->second;
		int64_t local_row = row - block_row * metadata.block_size;
		int64_t local_col = col - block_col * metadata.block_size;
		int64_t elem_idx = block.is_transpose ? (local_row * block.block_n_cols + local_col)
		                                      : (local_col * block.block_n_rows + local_row);
		return block.data[elem_idx];
	};

	for (idx_t a = 0; a < resolved.size(); ++a) {
		for (idx_t b = a + 1; b < resolved.size(); ++b) {
			auto &vi = resolved[a];
			auto &vj = resolved[b];
			if (vi.idx == vj.idx) {
				continue; // never emit a diagonal row, even defensively against duplicate input idx
			}
			BMPairOutRow out;
			out.idx_i = vi.idx;
			out.idx_j = vj.idx;
			if (vi.idx < 0 || vi.idx >= metadata.n_rows || vj.idx < 0 || vj.idx >= metadata.n_cols) {
				out.status_code = 2; // bm_index_out_of_bounds
				bind_data->rows.push_back(out);
				continue;
			}
			bool missing_block = false;
			double raw_value = get_cell(out.idx_i, out.idx_j, missing_block);
			if (missing_block) {
				out.status_code = 3; // bm_missing_block
			} else if (std::isnan(raw_value)) {
				out.status_code = 4; // bm_missing_or_nan
			} else {
				out.has_r = true;
				out.r = raw_value * vi.allele_order * vj.allele_order;
				out.status_code = 0; // bm_resolved
			}
			bind_data->rows.push_back(out);
		}
	}

	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> HailLDBMPairsInitGlobal(ClientContext &context,
                                                                    TableFunctionInitInput &input) {
	return make_uniq<GlobalTableFunctionState>();
}

struct BMPairsLocalState : public LocalTableFunctionState {
	std::vector<BMPairOutRow> rows;
	idx_t next = 0;
};

static unique_ptr<LocalTableFunctionState>
HailLDBMPairsInitLocal(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *) {
	auto state = make_uniq<BMPairsLocalState>();
	auto &bind = input.bind_data->Cast<HailLDBMPairsBindData>();
	state->rows = bind.rows;
	return std::move(state);
}

static void HailLDBMPairsScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &local = data.local_state->Cast<BMPairsLocalState>();
	if (local.next >= local.rows.size()) {
		output.SetCardinality(0);
		return;
	}
	const idx_t capacity = STANDARD_VECTOR_SIZE;
	idx_t to_emit = std::min<idx_t>(capacity, local.rows.size() - local.next);

	auto &idx_i_vec = output.data[0];
	auto &idx_j_vec = output.data[1];
	auto &r_vec = output.data[2];
	auto &dom_vec = output.data[3];
	auto &code_vec = output.data[4];

	for (idx_t i = 0; i < to_emit; ++i) {
		auto &row = local.rows[local.next + i];
		FlatVector::GetData<int64_t>(idx_i_vec)[i] = row.idx_i;
		FlatVector::GetData<int64_t>(idx_j_vec)[i] = row.idx_j;
		if (row.has_r) {
			FlatVector::GetData<double>(r_vec)[i] = row.r;
		} else {
			FlatVector::SetNull(r_vec, i, true);
		}
		FlatVector::GetData<string_t>(dom_vec)[i] = StringVector::AddString(dom_vec, "bm");
		FlatVector::GetData<int32_t>(code_vec)[i] = row.status_code;
	}
	local.next += to_emit;
	output.SetCardinality(to_emit);
}

// Debug table function: emit parsed parts for each token for inspection
struct DebugLocalState : public LocalTableFunctionState {
	struct Row {
		std::string original;
		std::string stripped;
		std::string contig;
		std::string pos;
		std::string ref;
		std::string alt;
		int32_t ok;
	};
	std::vector<Row> rows;
	idx_t next = 0;
};

static unique_ptr<LocalTableFunctionState>
HailLDPreflightInitLocalDebug(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *) {
	auto state = make_uniq<DebugLocalState>();
	std::string s;
	if (input.bind_data) {
		auto &bind = input.bind_data->Cast<PreflightBindData>();
		s = bind.request_arg;
	}
	if (s.empty())
		return std::move(state);
	auto parts = StringUtil::Split(s, "|");
	if (parts.size() != 3)
		return std::move(state);
	std::string varlist = parts[2];
	auto vars = StringUtil::Split(varlist, ',');
	std::unordered_set<std::string> seen;
	for (auto v : vars) {
		std::string original_raw = v;
		StringUtil::Trim(v);
		// manual ASCII trim
		size_t start = 0;
		while (start < v.size() && isspace((unsigned char)v[start]))
			start++;
		size_t end = v.size();
		while (end > start && isspace((unsigned char)v[end - 1]))
			end--;
		std::string trimmed = v.substr(start, end - start);
		if (trimmed.empty())
			continue;
		// remove internal whitespace
		std::string stripped = trimmed;
		stripped.erase(std::remove_if(stripped.begin(), stripped.end(), [](unsigned char c) { return isspace(c); }),
		               stripped.end());
		if (stripped.empty())
			continue;
		if (seen.count(stripped))
			continue;
		seen.insert(stripped);
		// parse into parts (contig may contain underscores)
		int32_t ok = 1;
		std::string contig, pos_str, ref, alt;
		auto parts_ = StringUtil::Split(stripped, '_');
		if (parts_.size() < 4) {
			ok = 0;
		} else {
			alt = parts_.back();
			ref = parts_[parts_.size() - 2];
			pos_str = parts_[parts_.size() - 3];
			// contig is joining remaining head parts
			contig = parts_[0];
			if (parts_.size() >= 4) {
				for (size_t i = 1; i <= parts_.size() - 4; ++i) {
					contig += "_" + parts_[i];
				}
			}
			// validate
			if (pos_str.empty())
				ok = 0;
			for (char c : pos_str)
				if (!isdigit((unsigned char)c)) {
					ok = 0;
					break;
				}
			if (ref.size() != 1 || alt.size() != 1)
				ok = 0;
			auto is_base = [&](const std::string &a) {
				char C = a[0];
				return a.size() == 1 && (C == 'A' || C == 'C' || C == 'G' || C == 'T');
			};
			if (!is_base(ref) || !is_base(alt))
				ok = 0;
		}
		state->rows.push_back({original_raw, stripped, contig, pos_str, ref, alt, ok});
	}
	return std::move(state);
}

static void HailLDPreflightDebugScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &local = data.local_state->Cast<DebugLocalState>();
	if (local.next >= local.rows.size()) {
		output.SetCardinality(0);
		return;
	}
	// use STANDARD_VECTOR_SIZE for debug emission as well
	const idx_t capacity = STANDARD_VECTOR_SIZE;
	idx_t to_emit = std::min<idx_t>(capacity, (idx_t)(local.rows.size() - local.next));

	// guard: ensure output has expected 7 columns
	if (output.ColumnCount() < 7) {
		// set empty and bail out
		output.SetCardinality(0);
		return;
	}

	auto &orig_vec = output.data[0];
	auto &stripped_vec = output.data[1];
	auto &contig_vec = output.data[2];
	auto &pos_vec = output.data[3];
	auto &ref_vec = output.data[4];
	auto &alt_vec = output.data[5];
	auto &ok_vec = output.data[6];

	try {
		for (idx_t i = 0; i < to_emit; ++i) {
			auto &r = local.rows[local.next + i];
			auto s0 = StringVector::AddString(orig_vec, r.original.c_str());
			auto s1 = StringVector::AddString(stripped_vec, r.stripped.c_str());
			auto s2 = StringVector::AddString(contig_vec, r.contig.c_str());
			auto s3 = StringVector::AddString(pos_vec, r.pos.c_str());
			auto s4 = StringVector::AddString(ref_vec, r.ref.c_str());
			auto s5 = StringVector::AddString(alt_vec, r.alt.c_str());
			FlatVector::GetData<string_t>(orig_vec)[i] = s0;
			FlatVector::GetData<string_t>(stripped_vec)[i] = s1;
			FlatVector::GetData<string_t>(contig_vec)[i] = s2;
			FlatVector::GetData<string_t>(pos_vec)[i] = s3;
			FlatVector::GetData<string_t>(ref_vec)[i] = s4;
			FlatVector::GetData<string_t>(alt_vec)[i] = s5;
			FlatVector::GetData<int32_t>(ok_vec)[i] = r.ok;
		}
	} catch (...) {
		// on unexpected error, fail gracefully with no rows
		output.SetCardinality(0);
		return;
	}

	local.next += to_emit;
	output.SetCardinality(to_emit);
}

void RegisterHailLDQueryFunctions(ExtensionLoader &loader) {
	// status codes table function with explicit bind
	TableFunction status_func("hail_ld_status_codes", {}, HailLDStatusCodes, nullptr, nullptr);
	// Provide a bind that sets output schema
	// Implement bind as lambda capturing nothing
	status_func = TableFunction(
	    "hail_ld_status_codes", {}, HailLDStatusCodes,
	    [](ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
	       vector<string> &names) -> unique_ptr<FunctionData> {
		    names = {"status_domain", "status_code", "status_name", "description"};
		    return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::VARCHAR};
		    return nullptr;
	    },
	    nullptr, HailLDStatusInitLocal);
	loader.RegisterFunction(status_func);

	// preflight function (simple string inline for TDD)
	TableFunction preflight_func("hail_ld_preflight", {LogicalType::VARCHAR}, HailLDPreflightScan, HailLDPreflightBind,
	                             HailLDPreflightInitGlobal, HailLDPreflightInitLocal);
	loader.RegisterFunction(preflight_func);

	// multi-locus request file: Parquet with (locus_id, locus, variant_ids LIST<VARCHAR>) rows
	TableFunction requests_func("hail_ld_preflight_requests", {LogicalType::VARCHAR}, HailLDPreflightScan,
	                            HailLDPreflightRequestsBind, HailLDPreflightInitGlobal,
	                            HailLDPreflightRequestsInitLocal);
	loader.RegisterFunction(requests_func);

	// partition-pruning introspection: which HT partitions a locus range would select
	TableFunction ht_partitions_func("hail_ld_ht_partitions_for_locus", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                 HailLDHTPartitionsScan, HailLDHTPartitionsBind, HailLDPreflightInitGlobal,
	                                 HailLDHTPartitionsInitLocal);
	loader.RegisterFunction(ht_partitions_func);

	// batch HailTable resolver: per-locus direct/flipped matching with partition pruning
	TableFunction resolve_ht_func("hail_ld_resolve_ht", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                              HailLDResolveHTScan, HailLDResolveHTBind, HailLDResolveHTInitGlobal,
	                              HailLDResolveHTInitLocal);
	loader.RegisterFunction(resolve_ht_func);

	// BlockMatrix pair extraction: strict canonical pairs among one locus's resolved variants
	child_list_t<LogicalType> resolved_struct_fields = {{"idx", LogicalType::BIGINT},
	                                                    {"allele_order", LogicalType::INTEGER}};
	TableFunction bm_pairs_func("hail_ld_bm_pairs",
	                            {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::STRUCT(resolved_struct_fields))},
	                            HailLDBMPairsScan, HailLDBMPairsBind, HailLDBMPairsInitGlobal, HailLDBMPairsInitLocal);
	loader.RegisterFunction(bm_pairs_func);

	// debug preflight parser inspection function
	TableFunction debug_func(
	    "hail_ld_preflight_debug", {LogicalType::VARCHAR}, HailLDPreflightDebugScan,
	    [](ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
	       vector<string> &names) -> unique_ptr<FunctionData> {
		    names = {"original_token", "stripped_token", "contig", "pos", "ref", "alt", "parse_ok"};
		    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
		                    LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER};
		    auto bind_data = make_uniq<PreflightBindData>();
		    if (!input.inputs.empty()) {
			    bind_data->request_arg = input.inputs[0].GetValue<string>();
		    }
		    return std::move(bind_data);
	    },

	    HailLDPreflightInitGlobal, HailLDPreflightInitLocalDebug);
	loader.RegisterFunction(debug_func);

	// scalar helper: parse a single token into a tab-separated string for quick debug
	auto hail_ld_parse_scalar = ScalarFunction(
	    "hail_ld_parse_token", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	    [](DataChunk &args, ExpressionState &state, Vector &result) {
		    auto &vec = args.data[0];
		    UnaryExecutor::Execute<string_t, string_t>(vec, result, args.size(), [&](string_t inp) {
			    string s = inp.GetString();
			    // trim
			    StringUtil::Trim(s);
			    // remove internal whitespace
			    string stripped = s;
			    stripped.erase(
			        std::remove_if(stripped.begin(), stripped.end(), [](unsigned char c) { return isspace(c); }),
			        stripped.end());
			    if (stripped.empty())
				    return StringVector::AddString(result, "");
			    auto parts = StringUtil::Split(stripped, '_');
			    if (parts.size() < 4) {
				    return StringVector::AddString(result, stripped + "|||||0");
			    }
			    string alt = parts.back();
			    string ref = parts[parts.size() - 2];
			    string pos = parts[parts.size() - 3];
			    string contig = parts[0];
			    if (parts.size() >= 4) {
				    for (size_t i = 1; i <= parts.size() - 4; ++i) {
					    contig += "_" + parts[i];
				    }
			    }
			    int ok = 1;
			    if (pos.empty())
				    ok = 0;
			    for (char c : pos)
				    if (!isdigit((unsigned char)c)) {
					    ok = 0;
					    break;
				    }
			    auto is_base = [&](const std::string &a) {
				    char C = a[0];
				    return a.size() == 1 && (C == 'A' || C == 'C' || C == 'G' || C == 'T');
			    };
			    if (!is_base(ref) || !is_base(alt))
				    ok = 0;
			    std::string out = stripped + "|" + contig + "|" + pos + "|" + ref + "|" + alt + "|" + to_string(ok);
			    return StringVector::AddString(result, out);
		    });
	    });
	loader.RegisterFunction(hail_ld_parse_scalar);
}

} // namespace duckdb
