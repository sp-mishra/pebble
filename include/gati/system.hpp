#pragma once
// ============================================================================
// gati/system.hpp — System Concept & Static SystemStack Execution Schedule
// ============================================================================
// Zero virtual functions, static tuple folding in declaration order.
// ============================================================================

#include "ecs.hpp"
#include "event.hpp"
#include "parallel.hpp"
#include "mem/arena.hpp"

#include <cstdint>
#include <tuple>
#include <utility>

namespace gati {

// Context passed to every system on each fixed step
struct StepContext {
    Scalar                      dt;
    std::uint64_t               frame;
    EventBus&                   events;
    smriti::pools::LinearArena& scratch;
    ParallelExecutor&           executor;
};

template <typename S>
concept System = requires(S s, World& w, StepContext ctx) {
    { s.run(w, ctx) } -> std::same_as<void>;
};

// Ordered static composition of systems. Order is the schedule.
template <System... Systems>
class SystemStack {
public:
    SystemStack() = default;
    explicit SystemStack(Systems... s) : systems_(std::move(s)...) {}

    void run(World& w, StepContext ctx) {
        std::apply([&](auto&... sys) { (sys.run(w, ctx), ...); }, systems_);
    }

    template <typename S>
    [[nodiscard]] S& get() noexcept {
        return std::get<S>(systems_);
    }

    template <typename S>
    [[nodiscard]] const S& get() const noexcept {
        return std::get<S>(systems_);
    }

private:
    [[no_unique_address]] std::tuple<Systems...> systems_;
};

} // namespace gati
