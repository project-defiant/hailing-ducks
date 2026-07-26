#include "hail_ld_query.hpp"

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/common/string_util.hpp"

#include <nlohmann/json.hpp>

#include <vector>
#include <string>
#include <unordered_set>

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
    const idx_t capacity = STANDARD_VECTOR_SIZE;
    idx_t to_emit = std::min<idx_t>(capacity, (idx_t)(local.rows.size() - local.next));

    auto &domain_vec = output.data[0];
    auto &code_vec = output.data[1];
    auto &name_vec = output.data[2];
    auto &desc_vec = output.data[3];

    for (idx_t i = 0; i < to_emit; ++i) {
        auto &r = local.rows[local.next + i];
        StringVector::AddString(domain_vec, r.domain);
        FlatVector::GetData<int32_t>(code_vec)[i] = r.code;
        StringVector::AddString(name_vec, r.name);
        StringVector::AddString(desc_vec, r.desc);
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
    if (s.empty()) {
        return std::move(state);
    }
    auto parts = StringUtil::Split(s, "|");
    if (parts.size() != 3) {
        return std::move(state);
    }
    std::string locus_id = parts[0];
    std::string varlist = parts[2];
    auto vars = StringUtil::Split(varlist, ",");

    std::unordered_set<std::string> seen;
    for (auto &v : vars) {
        if (seen.count(v)) continue;
        seen.insert(v);
        // validate pattern quickly: must contain at least two '_'
        size_t p1 = v.find('_');
        size_t p2 = v.find_last_of('_');
        if (p1 == std::string::npos || p2 == p1) {
            state->rows.push_back({locus_id, v, 5}); // unsupported_variant_id
        } else {
            state->rows.push_back({locus_id, v, 0}); // resolved_exact for TDD
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
    const idx_t capacity = STANDARD_VECTOR_SIZE;
    idx_t to_emit = std::min<idx_t>(capacity, (idx_t)(local.rows.size() - local.next));

    auto &locus_vec = output.data[0];
    auto &req_vec = output.data[1];
    auto &dom_vec = output.data[2];
    auto &code_vec = output.data[3];

    for (idx_t i = 0; i < to_emit; ++i) {
        auto &r = local.rows[local.next + i];
        StringVector::AddString(locus_vec, r.locus_id);
        StringVector::AddString(req_vec, r.req);
        StringVector::AddString(dom_vec, "variant");
        FlatVector::GetData<int32_t>(code_vec)[i] = r.code;
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

    // preflight function (simple string inline for TDD)
    TableFunction preflight_func("hail_ld_preflight", {LogicalType::VARCHAR}, HailLDPreflightScan,
                                HailLDPreflightBind, HailLDPreflightInitGlobal, HailLDPreflightInitLocal);
    loader.RegisterFunction(preflight_func);
}


} // namespace duckdb
