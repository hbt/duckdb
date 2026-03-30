//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/lock_config.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

//! DEFAULT: acquire lock normally.
//! TRY: attempt lock; proceed without lock on conflict (read-only only).
enum class LockConfig : uint8_t { DEFAULT = 0, TRY = 1 };

} // namespace duckdb
