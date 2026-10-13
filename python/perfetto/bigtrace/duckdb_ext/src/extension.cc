// Entry point of the perfetto DuckDB extension: registers the native kernels
// of each operator family, then runs the SQL macro layer (sql/*.sql, embedded
// at build time as generated/sql_macros.h) that exposes the operators.
#include "common.h"  // no-include-violation-check

#include <string>

#include "generated/sql_macros.h"

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_connection conn,
                            duckdb_extension_info info,
                            struct duckdb_extension_access* access) {
  bool ok = RegisterIntervalFunctions(conn) && RegisterTreeFunctions(conn) &&
            RegisterGraphFunctions(conn) &&
            RegisterProtoIngestFunctions(conn) && RegisterSqlGenFunctions(conn);
  if (!ok) {
    access->set_error(info, "perfetto: failed to register functions");
    return false;
  }
  for (const char* sql : kSqlMacros) {
    duckdb_result res;
    if (duckdb_query(conn, sql, &res) == DuckDBError) {
      std::string s(sql);
      const std::string from = "CREATE OR REPLACE MACRO ";
      size_t pos = s.find(from);
      duckdb_result tmp_res;
      if (pos != std::string::npos &&
          duckdb_query(
              conn,
              s.replace(pos, from.size(), "CREATE OR REPLACE TEMP MACRO ")
                  .c_str(),
              &tmp_res) == DuckDBSuccess) {
        duckdb_destroy_result(&res);
        duckdb_destroy_result(&tmp_res);
        continue;
      }
      if (pos != std::string::npos) {
        duckdb_destroy_result(&tmp_res);
      }
      std::string err =
          std::string("perfetto: macro failed: ") + duckdb_result_error(&res);
      duckdb_destroy_result(&res);
      access->set_error(info, err.c_str());
      return false;
    }
    duckdb_destroy_result(&res);
  }
  return true;
}
