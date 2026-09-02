#pragma once

// Cached grammar facts.  Consumers should use this rather than independently
// instantiating validation/FIRST/FOLLOW analyses for the same grammar.
#include "validation.hpp"
#include "expected_sets.hpp"

namespace lang::samasa {
    template <class Grammar>
    struct grammar_metadata {
        static constexpr auto validation = validate_grammar<Grammar>();
        static constexpr auto first = first_sets<Grammar>();
        static constexpr auto follow = follow_sets<Grammar>();
        static constexpr std::size_t rule_count = Grammar::rule_count;
        static constexpr bool valid = validation.ok();
    };
}
