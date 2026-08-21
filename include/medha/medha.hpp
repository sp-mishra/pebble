#pragma once
// =============================================================================
// medha/medha.hpp — umbrella header (core only; adapters opt-in)
//
// C++23, header-only, no virtual, no macros.
//
// Includes: all medha core headers.
// Adapters: opt-in via:
//   #include "medha/adapters/smriti.hpp"
//   #include "medha/adapters/lithe.hpp"
//   #include "medha/adapters/pravaha.hpp"
//   #include "medha/adapters/tarka.hpp"
//
// Namespace alias:
//   namespace tx = medha;
// =============================================================================

#include "medha/access_log.hpp"
#include "medha/commit.hpp"
#include "medha/conflict.hpp"
#include "medha/context.hpp"
#include "medha/diagnostics.hpp"
#include "medha/edsl.hpp"
#include "medha/effects.hpp"
#include "medha/fwd.hpp"
#include "medha/identity.hpp"
#include "medha/isolation.hpp"
#include "medha/key.hpp"
#include "medha/options.hpp"
#include "medha/read_set.hpp"
#include "medha/resource_handle.hpp"
#include "medha/resource_traits.hpp"
#include "medha/retry.hpp"
#include "medha/transaction.hpp"
#include "medha/value.hpp"
#include "medha/version.hpp"
#include "medha/write_set.hpp"

// Convenience alias
namespace tx = medha;
