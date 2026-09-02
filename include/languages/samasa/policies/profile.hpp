#pragma once

#include "memo_policy.hpp"
#include "trace_policy.hpp"

namespace lang::samasa {
    // Named configuration points make cost visible at the call site.  They are
    // type-only and do not pull CST/recovery/tooling into lexer-only consumers.
    template <class Memo = no_memo, class Trace = no_trace>
    struct parse_profile {
        using memo_policy = Memo;
        using trace_policy = Trace;
    };

    using fast_profile = parse_profile<>;
    using traced_profile = parse_profile<no_memo, collecting_trace>;
}
