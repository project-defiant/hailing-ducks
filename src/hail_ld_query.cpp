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
static void HailLDStatusCodes(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    fprintf(stderr, "[hail_ld] HailLDStatusCodes called\n");
    // schema: status_domain, status_code, status_name, description
    const idx_t capacity = STANDARD_VECTOR_SIZE;
    idx_t out_row = 0;

    // Prepare static list
    struct Status { const char *domain; int code; const char *name; const char *desc; };
    Status statuses[] = {
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

    idx_t n = sizeof(statuses) / sizeof(statuses[0]);
    // Fill output vectors
    auto &domain_vec = output.data[0];
    auto &code_vec = output.data[1];
    auto &name_vec = output.data[2];
    auto &desc_vec = output.data[3];

    for (idx_t i = 0; i < n && i < capacity; ++i) {
        StringVector::AddString(domain_vec, statuses[i].domain);
        FlatVector::GetData<int32_t>(code_vec)[i] = statuses[i].code;
        StringVector::AddString(name_vec, statuses[i].name);
        StringVector::AddString(desc_vec, statuses[i].desc);
        out_row++;
    }
    output.SetCardinality(out_row);
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

static unique_ptr<LocalTableFunctionState> HailLDPreflightInitLocal(ExecutionContext &context,
                                                                   TableFunctionInitInput &input,
                                                                   GlobalTableFunctionState *) {
    return make_uniq<LocalTableFunctionState>();
}

static void HailLDPreflightScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    // For now, accept a single VARCHAR input captured at bind time in PreflightBindData
    auto &bind_data = data.bind_data->Cast<PreflightBindData>();
    std::string s = bind_data.request_arg;
    fprintf(stderr, "[hail_ld_preflight] called with arg='%s'\n", s.c_str());
    if (s.empty()) {
        output.SetCardinality(0);
        return;
    }
    // Expect format: locus_id|locus|var1,var2,var3
    auto parts = StringUtil::Split(s, "|");
    if (parts.size() != 3) {
        // Emit nothing for malformed for now
        output.SetCardinality(0);
        return;
    }
    std::string locus_id = parts[0];
    std::string locus = parts[1];
    std::string varlist = parts[2];
    auto vars = StringUtil::Split(varlist, ",");

    // Simple biallelic pattern: contig_pos_ref_alt
    for (size_t i = 0; i < vars.size(); ++i) {
        std::string v = vars[i];
        // naive validation: contains '_' twice
        size_t p1 = v.find('_');
        size_t p2 = v.find_last_of('_');
        if (p1 == std::string::npos || p2 == p1) {
            // unsupported_variant_id -> code 5
            auto &locus_vec = output.data[0];
            auto &req_vec = output.data[1];
            auto &dom_vec = output.data[2];
            auto &code_vec = output.data[3];
            StringVector::AddString(locus_vec, locus_id);
            StringVector::AddString(req_vec, v);
            StringVector::AddString(dom_vec, "variant");
            // set code at index 0
            FlatVector::GetData<int32_t>(code_vec)[0] = 5;
            output.SetCardinality(1);
            return;
        }
    }

    // If all OK, emit resolved_exact (code 0) for every unique var
    struct OutRow { std::string locus_id; std::string req; int32_t code; };
    std::unordered_set<std::string> seen;
    std::vector<OutRow> rows;
    for (auto &v : vars) {
        if (seen.count(v)) {
            continue; // dedupe
        }
        seen.insert(v);
        rows.push_back({locus_id, v, 0});
    }

    // Write rows into the output vectors
    auto &locus_vec = output.data[0];
    auto &req_vec = output.data[1];
    auto &dom_vec = output.data[2];
    auto &code_vec = output.data[3];

    for (idx_t i = 0; i < (idx_t)rows.size(); ++i) {
        StringVector::AddString(locus_vec, rows[i].locus_id);
        StringVector::AddString(req_vec, rows[i].req);
        StringVector::AddString(dom_vec, "variant");
        FlatVector::GetData<int32_t>(code_vec)[i] = rows[i].code;
    }
    output.SetCardinality((idx_t)rows.size());
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
        }
    );
    loader.RegisterFunction(status_func);

    // preflight function (simple string inline for TDD)
    TableFunction preflight_func("hail_ld_preflight", {LogicalType::VARCHAR}, HailLDPreflightScan,
                                HailLDPreflightBind, HailLDPreflightInitGlobal, HailLDPreflightInitLocal);
    loader.RegisterFunction(preflight_func);
}


} // namespace duckdb
