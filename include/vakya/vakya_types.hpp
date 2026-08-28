#pragma once

// vakya/vakya_types.hpp — Aggregator for the full Vākya type-system stack.
//
// Opt-in convenience umbrella. Pull this when you need the complete type-checking,
// inference, constraint-solving, guarded-rewriting, validation, and diagnostics stack.
//
// Does NOT change vakya.hpp, lithe_core.hpp, or any existing Vākya surface.
// New headers are opt-in; this file just pulls them all at once.

// V2 type-system stack (unchanged)
#include "vakya/types.hpp"            // type_node / type_ref / type_arena
#include "vakya/unification.hpp"       // substitution / unify / apply / generalize / instantiate
#include "vakya/constraints.hpp"       // constraint / constraint_solver / composite_solver / unification_solver
#include "vakya/constraint_solvers.hpp" // rule / graph / egraph constraint solvers + smt.hpp via smt.hpp
#include "vakya/smt.hpp"               // smt_backend concept + no_smt_backend stub
#include "vakya/type_checking.hpp"     // type_environment / typing_rule / type_check / validation_result
#include "vakya/type_inference.hpp"    // infer / infer_error / infer_cache_t
#include "vakya/rewrite.hpp"           // guarded_rule / always_true_guard
#include "vakya/validation.hpp"        // validator / validation_report
#include "vakya/diagnostics.hpp"       // diagnostic / diagnostic_sink / null_sink / collecting_sink

// V3 constraint-reasoning stack (opt-in; each header is independently includable)
#include "vakya/types/type_registry.hpp"    // runtime type-descriptor registry
#include "vakya/types/capability.hpp"       // capability_descriptor + mask
#include "vakya/types/effect.hpp"           // effect_descriptor + mask
#include "vakya/types/shape.hpp"            // shape algebra (shape_type_tag, matmul/broadcast constraints)
#include "vakya/constraint_registry.hpp"    // constraint_descriptor + registry + solve_batch
#include "vakya/analysis_store.hpp"         // analysis_record + analysis_store
#include "vakya/analysis.hpp"               // analyze() driver
#include "vakya/typed_pattern.hpp"          // typed<>/trait<> combinators over pattern.hpp
#include "vakya/type_rewrite.hpp"           // type-level rewriting via egraph
#include "vakya/verify.hpp"                 // formal verification via SMT
#include "vakya/query.hpp"                  // lazy semantic query engine
