#pragma once

// ============================================================================
// sanchaya/sanchaya.hpp — Master Umbrella Header for Sanchaya Subsystem
// ============================================================================

#include "sanchaya/fwd.hpp"
#include "sanchaya/schema/descriptors.hpp"
#include "sanchaya/schema/model.hpp"
#include "sanchaya/schema/graph_validator.hpp"
#include "sanchaya/query/query.hpp"
#include "sanchaya/planner/cost_model.hpp"
#include "sanchaya/planner/logical_ir.hpp"
#include "sanchaya/planner/physical_ir.hpp"
#include "sanchaya/planner/egraph_optimizer.hpp"
#include "sanchaya/planner/planner.hpp"
#include "sanchaya/backend/petika_backend.hpp"
#include "sanchaya/backend/anukrama_backend.hpp"
#include "sanchaya/backend/sqlite_backend.hpp"
#include "sanchaya/backend/duckdb_backend.hpp"
#include "sanchaya/engine/memory_engine.hpp"
#include "sanchaya/engine/relational_engine.hpp"
#include "sanchaya/engine/petika_engine.hpp"
#include "sanchaya/sync/cdc_sync.hpp"
#include "sanchaya/integration/service_registry.hpp"
#include "sanchaya/integration/medha_adapter.hpp"
#include "sanchaya/session/session.hpp"
#include "sanchaya/workspace/workspace.hpp"
