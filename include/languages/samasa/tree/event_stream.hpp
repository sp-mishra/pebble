#pragma once

// samasa/tree/event_stream.hpp — samasa adapters over the generic event_log substrate.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// Stage 3: event_stream<SK> is now an alias of lang::event_log<SK, samasa_diag_code>.
//          event_kind and parse_event<SK> alias the generic equivalents.
//
// All marker/rollback/tombstone semantics live in lang::event_log (single owner).
// samasa call-sites are unchanged — same names, same method signatures.

#include "../core/diagnostic.hpp"
#include "languages/generic/tree/event_log.hpp"

namespace lang::samasa {

    // event_kind is owned by the generic layer.
    using lang::event_kind;

    // parse_event<SK> — fix DiagCode to samasa_diag_code.
    template <class SyntaxKind>
    using parse_event = lang::parse_event<SyntaxKind, samasa_diag_code>;

    // event_stream<SK> — full marker/rollback/tombstone log.
    // Methods: begin / token / error / end / snapshot / rollback / depth /
    //          event_count / all / reserve — all resolved from lang::event_log.
    template <class SyntaxKind>
    using event_stream = lang::event_log<SyntaxKind, samasa_diag_code>;

} // namespace lang::samasa
