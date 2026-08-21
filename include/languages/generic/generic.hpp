#pragma once

// languages/generic/generic.hpp — Umbrella include for the full generic language layer.
//
// C++26-capable, header-only, no virtual, no public macros. Namespace: lang
//
// Hierarchy:
//   core/      — identity, diagnostics, reflection, source_location,
//                rich_diagnostic, parse_stats     (zero-dep primitives)
//   tree/      — spans, event_log, green_arena, static_buffers  (CST substrate)
//   ir/        — node, ir_module, interning, lowering            (generic IR layer)
//   host/      — descriptors, effects, registry   (host-side registration)
//   module/    — module_system, import_resolver   (module resolution + import pipeline)
//   semantic/  — symbol_table, rules, proof       (semantic analysis)
//   ast/       — ast_arena                        (generic flat AST node store)
//   lexer/     — digit_sep                        (lexer utilities)
//
// Pick individual sub-headers for faster compilation; use this umbrella when
// you need the full layer.

#include "languages/generic/core/identity.hpp"
#include "languages/generic/core/diagnostics.hpp"
#include "languages/generic/core/reflection.hpp"
#include "languages/generic/core/source_location.hpp"
#include "languages/generic/core/rich_diagnostic.hpp"
#include "languages/generic/core/parse_stats.hpp"
#include "languages/generic/tree/spans.hpp"
#include "languages/generic/tree/event_log.hpp"
#include "languages/generic/tree/green_arena.hpp"
#include "languages/generic/tree/static_buffers.hpp"
#include "languages/generic/ir/node.hpp"
#include "languages/generic/ir/ir_module.hpp"
#include "languages/generic/ir/interning.hpp"
#include "languages/generic/ir/lowering.hpp"
#include "languages/generic/host/descriptors.hpp"
#include "languages/generic/host/effects.hpp"
#include "languages/generic/host/registry.hpp"
#include "languages/generic/module/module_system.hpp"
#include "languages/generic/module/import_resolver.hpp"
#include "languages/generic/semantic/symbol_table.hpp"
#include "languages/generic/semantic/proof.hpp"
#include "languages/generic/semantic/rules.hpp"
#include "languages/generic/ast/ast_arena.hpp"
#include "languages/generic/lexer/digit_sep.hpp"
