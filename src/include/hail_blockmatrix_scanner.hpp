#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

struct HailBlockMatrixScanFunction {
	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
