#include "hail_ld_query.hpp"

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/constants.hpp"

#include <nlohmann/json.hpp>
#include <cstdio>

#include <vector>
#include <string>
#include <unordered_set>
#include <regex>

namespace duckdb {

// Simple status codes table function: hail_ld_status_codes()
struct StatusLocalState : public LocalTableFunctionState {
    struct Row { std::string domain; int32_t code; std::string name; std::string desc; };
    std::vector<Row> rows;
    idx_t next = 0;
};

static unique_ptr<LocalTableFunctionState> HailLDStatusInitLocal(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *) {
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
        {"bm", 4, "bm_missing_or_nan", "BM value missing or NaN"}
    };
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
    struct OutRow { std::string locus_id; std::string req; int32_t code; };
    std::vector<OutRow> rows;
    idx_t next = 0;
};

static unique_ptr<LocalTableFunctionState> HailLDPreflightInitLocal(ExecutionContext &context,
                                                                   TableFunctionInitInput &input,
                                                                   GlobalTableFunctionState *) {
    auto state = make_uniq<PreflightLocalState>();
    // parse bind-time request_arg into rows, store in local state for chunked emission
    auto &bind = input.bind_data->Cast<PreflightBindData>();
    std::string s = bind.request_arg;
    // debug: dump raw bind string bytes
    {
        FILE *f = fopen("test/hail_ld_input.log", "a");
        if (f) {
            fprintf(f, "BIND_ARG='%s' len=%zu\n", s.c_str(), s.size());
            fclose(f);
        }
    }
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
    auto vars = StringUtil::Split(varlist, ",");

    // parse locus_range -> contig:start-end
    bool locus_ok = false;
    string locus_contig;
    int64_t locus_start = 0, locus_end = 0;
    if (!locus_range.empty()) {
        // expect e.g. chr1:100-200
        auto colon = locus_range.find(':');
        if (colon != string::npos) {
            locus_contig = locus_range.substr(0, colon);
            string rest = locus_range.substr(colon + 1);
            auto dash = rest.find('-');
            if (dash != string::npos) {
                string sstart = rest.substr(0, dash);
                string send = rest.substr(dash + 1);
                try {
                    locus_start = stoll(sstart);
                    locus_end = stoll(send);
                    locus_ok = (locus_start <= locus_end);
                } catch (...) { locus_ok = false; }
            }
        }
    }

    // temp parsed list preserves order
    struct Parsed { string token; string contig; string pos; string ref; string alt; bool ok; };
    vector<Parsed> parsed_list;
    std::unordered_set<std::string> seen;
    for (auto v : vars) {
        // normalize each token: trim and skip empty tokens (from trailing/leading/repeated commas)
        StringUtil::Trim(v);
        // Manual trim fallback: remove ASCII isspace from ends
        auto trim_manual = [&](string &s) {
            size_t start = 0;
            while (start < s.size() && isspace((unsigned char)s[start])) start++;
            size_t end = s.size();
            while (end > start && isspace((unsigned char)s[end - 1])) end--;
            if (start != 0 || end != s.size()) s = s.substr(start, end - start);
        };
        trim_manual(v);

        if (v.empty()) continue;
        // remove any internal whitespace characters (safety)
        string v2 = v;
        v2.erase(std::remove_if(v2.begin(), v2.end(), [](unsigned char c) { return isspace(c); }), v2.end());
        if (v2.empty()) continue;
        if (seen.count(v2)) continue;
        seen.insert(v2);

        // parse token into components
        Parsed pentry{v2, "", "", "", "", false};
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
            for (char c : pentry.pos) if (!isdigit((unsigned char)c)) okpos = false;
            auto is_base = [&](const string &a) {
                if (a.size() != 1) return false;
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
            state->rows.push_back({locus_id, p.token, 5});
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
                    if (posv < locus_start || posv > locus_end) outside = true;
                } catch (...) { outside = true; }
            }
        }
        if (outside) {
            state->rows.push_back({locus_id, p.token, 3});
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
        if (allele_pairs.size() > 2) {
            // ambiguous in HT-like sense: many allele pairs at same position
            for (auto idx : indices) {
                state->rows.push_back({locus_id, parsed_list[idx].token, 4});
            }
            continue;
        }
        if (allele_pairs.size() == 1) {
            // single allele pair -> first encountered marked resolved_exact
            // preserve input order: emit rows for indices in order, first 0, others 0 as duplicates handled earlier
            for (auto idx : indices) {
                state->rows.push_back({locus_id, parsed_list[idx].token, 0});
            }
            continue;
        }
        // allele_pairs.size() == 2
        // check if the two pairs are flips of each other
        auto it = allele_pairs.begin();
        string a1 = *it; ++it; string a2 = *it;
        auto split_pair = [&](const string &s) -> pair<string,string> {
            auto pos = s.find(':');
            return {s.substr(0,pos), s.substr(pos+1)};
        };
        auto pa1 = split_pair(a1);
        auto pa2 = split_pair(a2);
        bool is_flip = (pa1.first == pa2.second && pa1.second == pa2.first);
        if (is_flip) {
            // find first occurrence among indices; mark first as resolved_exact, others as resolved_flipped if they are flipped relative to first
            // determine which allele pair was seen first
            string first_pair = "";
            string first_token_pair = "";
            for (auto idx : indices) {
                auto &p = parsed_list[idx];
                string pair = p.ref + ":" + p.alt;
                if (first_pair.empty()) { first_pair = pair; first_token_pair = pair; break; }
            }
            // emit: for each in indices, if pair==first_pair -> 0 else -> 1
            for (auto idx : indices) {
                auto &p = parsed_list[idx];
                string pair = p.ref + ":" + p.alt;
                if (pair == first_pair) state->rows.push_back({locus_id, p.token, 0});
                else state->rows.push_back({locus_id, p.token, 1});
            }
        } else {
            // two distinct non-flip allele pairs -> mark first as 0, others as multiple_variants_at_position (6)
            // first encountered pair
            string first_pair = "";
            for (auto idx : indices) {
                auto &p = parsed_list[idx];
                string pair = p.ref + ":" + p.alt;
                if (first_pair.empty()) { first_pair = pair; state->rows.push_back({locus_id, p.token, 0}); }
                else { state->rows.push_back({locus_id, p.token, 6}); }
            }
        }
    }

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
        while (start < s.size() && isspace((unsigned char)s[start])) start++;
        size_t end = s.size();
        while (end > start && isspace((unsigned char)s[end - 1])) end--;
        return s.substr(start, end - start);
    };

    for (idx_t i = 0; i < to_emit; ++i) {
        auto &r = local.rows[local.next + i];
        string locus_out = normalize(r.locus_id);
        string req_out = normalize(r.req);
        // debug: write hex bytes of req_out to /tmp/hail_ld_emit.log
        {
            FILE *f = fopen("test/hail_ld_emit.log", "a");
            if (f) {
                fprintf(f, "EMIT ROW %zu: ", (size_t)(local.next + i));
                for (unsigned char c : req_out) {
                    fprintf(f, "%02x:", c);
                }
                fprintf(f, " -> '%s' len=%zu\n", req_out.c_str(), req_out.size());
                fclose(f);
            }
        }
        FlatVector::GetData<string_t>(locus_vec)[i] = StringVector::AddString(locus_vec, locus_out.c_str());
        FlatVector::GetData<string_t>(req_vec)[i] = StringVector::AddString(req_vec, req_out.c_str());
        FlatVector::GetData<string_t>(dom_vec)[i] = StringVector::AddString(dom_vec, "variant");
        FlatVector::GetData<int32_t>(code_vec)[i] = r.code;
    }
    local.next += to_emit;
    output.SetCardinality(to_emit);
}

// Debug table function: emit parsed parts for each token for inspection
struct DebugLocalState : public LocalTableFunctionState {
    struct Row { std::string original; std::string stripped; std::string contig; std::string pos; std::string ref; std::string alt; int32_t ok; };
    std::vector<Row> rows;
    idx_t next = 0;
};

static unique_ptr<LocalTableFunctionState> HailLDPreflightInitLocalDebug(ExecutionContext &context,
                                                                        TableFunctionInitInput &input,
                                                                        GlobalTableFunctionState *) {
    auto state = make_uniq<DebugLocalState>();
    std::string s;
    FILE *f = fopen("test/hail_ld_initlocal_debug.log", "a");
    if (input.bind_data) {
        auto &bind = input.bind_data->Cast<PreflightBindData>();
        s = bind.request_arg;
        if (f) fprintf(f, "INITLOCAL_DEBUG: bind_data present, request_arg='%s'\n", s.c_str());
    } else if (!input.inputs.empty()) {
        try { s = input.inputs[0].GetValue<string>(); if (f) fprintf(f, "INITLOCAL_DEBUG: input[0] provided '%s'\n", s.c_str()); } catch (...) { if (f) fprintf(f, "INITLOCAL_DEBUG: input[0] get failed\n"); }
    } else {
        if (f) fprintf(f, "INITLOCAL_DEBUG: no bind_data and no input args\n");
    }
    if (f) fclose(f);
    if (s.empty()) return std::move(state);
    auto parts = StringUtil::Split(s, "|");
    if (parts.size() != 3) return std::move(state);
    std::string varlist = parts[2];
    auto vars = StringUtil::Split(varlist, ',');
    std::unordered_set<std::string> seen;
    for (auto v : vars) {
        StringUtil::Trim(v);
        // manual ASCII trim
        size_t start = 0;
        while (start < v.size() && isspace((unsigned char)v[start])) start++;
        size_t end = v.size();
        while (end > start && isspace((unsigned char)v[end - 1])) end--;
        std::string trimmed = v.substr(start, end - start);
        if (trimmed.empty()) continue;
        // remove internal whitespace
        std::string stripped = trimmed;
        stripped.erase(std::remove_if(stripped.begin(), stripped.end(), [](unsigned char c) { return isspace(c); }), stripped.end());
        if (stripped.empty()) continue;
        if (seen.count(stripped)) continue;
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
            if (pos_str.empty()) ok = 0;
            for (char c : pos_str) if (!isdigit((unsigned char)c)) { ok = 0; break; }
            if (ref.size() != 1 || alt.size() != 1) ok = 0;
            auto is_base = [&](const std::string &a) { char C = a[0]; return a.size() == 1 && (C == 'A' || C == 'C' || C == 'G' || C == 'T'); };
            if (!is_base(ref) || !is_base(alt)) ok = 0;
        }
        state->rows.push_back({v, stripped, contig, pos_str, ref, alt, ok});
    }
    return std::move(state);
}

static void HailLDPreflightDebugScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    auto &local = data.local_state->Cast<DebugLocalState>();
    if (local.next >= local.rows.size()) { output.SetCardinality(0); return; }
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
    fprintf(stderr, "[hail_ld] RegisterHailLDQueryFunctions called\n");
    // status codes table function with explicit bind
    TableFunction status_func("hail_ld_status_codes", {}, HailLDStatusCodes, nullptr, nullptr);
    // Provide a bind that sets output schema
    // Implement bind as lambda capturing nothing
    status_func = TableFunction("hail_ld_status_codes", {}, HailLDStatusCodes, 
        [](ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types, vector<string> &names) -> unique_ptr<FunctionData> {
            names = {"status_domain", "status_code", "status_name", "description"};
            return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::VARCHAR};
            return nullptr;
        },
        nullptr, HailLDStatusInitLocal
    );
    loader.RegisterFunction(status_func);
    fprintf(stderr, "[hail_ld] registered status_func\n");

    // preflight function (simple string inline for TDD)
    TableFunction preflight_func("hail_ld_preflight", {LogicalType::VARCHAR}, HailLDPreflightScan,
                                HailLDPreflightBind, HailLDPreflightInitGlobal, HailLDPreflightInitLocal);
    loader.RegisterFunction(preflight_func);
    fprintf(stderr, "[hail_ld] registered hail_ld_preflight\n");

    // debug preflight parser inspection function
    TableFunction debug_func("hail_ld_preflight_debug", {LogicalType::VARCHAR}, HailLDPreflightDebugScan,
        [](ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types, vector<string> &names) -> unique_ptr<FunctionData> {
            names = {"original_token", "stripped_token", "contig", "pos", "ref", "alt", "parse_ok"};
            return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER};
            auto bind_data = make_uniq<PreflightBindData>();
            // debug logging
            FILE *f = fopen("test/hail_ld_bind_debug.log", "a");
            if (f) {
                fprintf(f, "DEBUG_BIND called: inputs_empty=%d, input_count=%zu\n", input.inputs.empty() ? 1 : 0, input.inputs.size());
                if (!input.inputs.empty()) {
                    try { auto v = input.inputs[0].GetValue<string>(); fprintf(f, "DEBUG_BIND input[0]='%s'\n", v.c_str()); } catch (...) { fprintf(f, "DEBUG_BIND input[0] get failed\n"); }
                }
                fclose(f);
            }
            if (!input.inputs.empty()) {
                bind_data->request_arg = input.inputs[0].GetValue<string>();
            }
            return std::move(bind_data);
        },

        nullptr, HailLDPreflightInitLocalDebug);
    loader.RegisterFunction(debug_func);
    fprintf(stderr, "[hail_ld] registered hail_ld_preflight_debug\n");

    // scalar helper: parse a single token into a tab-separated string for quick debug
    auto hail_ld_parse_scalar = ScalarFunction("hail_ld_parse_token", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
        [](DataChunk &args, ExpressionState &state, Vector &result) {
            auto &vec = args.data[0];
            UnaryExecutor::Execute<string_t, string_t>(vec, result, args.size(), [&](string_t inp) {
                string s = inp.GetString();
                // trim
                StringUtil::Trim(s);
                // remove internal whitespace
                string stripped = s;
                stripped.erase(std::remove_if(stripped.begin(), stripped.end(), [](unsigned char c) { return isspace(c); }), stripped.end());
                if (stripped.empty()) return StringVector::AddString(result, "");
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
                if (pos.empty()) ok = 0;
                for (char c : pos) if (!isdigit((unsigned char)c)) { ok = 0; break; }
                auto is_base = [&](const std::string &a) { char C = a[0]; return a.size() == 1 && (C == 'A' || C == 'C' || C == 'G' || C == 'T'); };
                if (!is_base(ref) || !is_base(alt)) ok = 0;
                std::string out = stripped + "|" + contig + "|" + pos + "|" + ref + "|" + alt + "|" + to_string(ok);
                return StringVector::AddString(result, out);
            });
        }
    );
    loader.RegisterFunction(hail_ld_parse_scalar);
    fprintf(stderr, "[hail_ld] registered hail_ld_parse_token scalar\n");
}


} // namespace duckdb
