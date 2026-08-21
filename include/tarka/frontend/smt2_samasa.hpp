#pragma once
#include "tarka/frontend/smt2_text.hpp"
#include "languages/samasa/samasa.hpp"

namespace tarka::frontend {
    // Samasa is the authoritative CST-capable alternative.  Its grammar/CST
    // integration is intentionally isolated from semantic lowering.
    [[nodiscard]] inline ir::script parse_smt2_samasa(std::string_view source) { return detail::decode_smt2(source); }
}
