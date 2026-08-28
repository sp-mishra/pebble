#pragma once

// samasa/core/parse_options.hpp — Runtime parse configuration (see parse_output.hpp).
//
// parse_options lives in parse_output.hpp alongside parse_output<SK,TK>.
// This header re-exports it for compatibility with includes that reference
// parse_options.hpp directly.

#include "parse_output.hpp"

namespace lang::samasa {
    // parse_options is defined in parse_output.hpp
    // default_parse_options is kept as an alias for backwards compatibility.
    using default_parse_options = parse_options;
}
