#pragma once
// ============================================================================
// spandana/timeline.hpp — Node-Based Declarative Animation Timeline
// ============================================================================
// Actions register ResourceKeys for automatic dependency & parallelism inference.
//
// Design charter (Pebble): zero virtual, zero heap on the hot path, static
// policy dispatch. Actions are value types modelling the `AnimationAction`
// concept; the Timeline erases each one into a fixed inline buffer via a manual
// free-function vtable — no `virtual`, no RTTI, no `shared_ptr`, no allocation.
// Nodes live in a `static_vector`, dependency inference is O(1) through an
// inline open-addressed resource table, and execution is a single pass gated by
// a compile-time `ExecPolicy` (Serial by default, Parallel opt-in via pravaha).
// ============================================================================

#include "resource_key.hpp"
#include "easing.hpp"
#include "concepts.hpp"
#include "containers/static/static_vector.hpp"
#include <algorithm>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace pebble::spandana {
    // ----------------------------------------------------------------------------
    // Action — SBO type-erased holder for any AnimationAction value.
    // ----------------------------------------------------------------------------
    // The erased action is stored inline in `Buf` bytes. Dispatch goes through a
    // static, per-type `Vtable` of free functions (no virtual, no RTTI). Actions
    // larger than the inline buffer fail a static_assert rather than silently
    // heap-allocating — Pebble forbids hidden heap on the hot path. Bump the
    // `InlineBytes` template parameter of the owning Timeline if a fatter action is
    // genuinely needed. The default (192) fits every EDSL action Spandana ships —
    // the largest is `SetMaterialAction` at 152 bytes (it stores a full
    // `gati::MaterialComponent` by value) — with headroom.
    template <std::size_t InlineBytes = 192, std::size_t InlineAlign = alignof(std::max_align_t)>
    class BasicAction {
    public:
        struct Vtable {
            void (*update)(void*, float, float) noexcept;
            void (*on_start)(void*) noexcept;
            void (*on_complete)(void*) noexcept;
            float (*duration)(const void*) noexcept;
            ResourceKey (*resource_key)(const void*) noexcept;
            void (*move_construct)(void*, void*) noexcept; // (dst, src)
            void (*destroy)(void*) noexcept;
        };

        BasicAction() = default;

        template <typename A, typename Decayed = std::remove_cvref_t<A>>
            requires (AnimationAction<Decayed> && !std::is_same_v<Decayed, BasicAction>)
        BasicAction(A&& action) noexcept {
            static_assert(sizeof(Decayed) <= InlineBytes,
                          "spandana action exceeds Timeline inline buffer; raise Timeline<InlineBytes>.");
            static_assert(alignof(Decayed) <= InlineAlign,
                          "spandana action over-aligned for Timeline inline buffer.");
            static_assert(std::is_nothrow_move_constructible_v<Decayed>,
                          "spandana action must be nothrow move-constructible for zero-overhead storage.");
            ::new(static_cast<void*>(&storage_)) Decayed(std::forward<A>(action));
            vtable_ = vtable_for<Decayed>();
        }

        BasicAction(BasicAction&& other) noexcept {
            if (other.vtable_) {
                other.vtable_->move_construct(&storage_, &other.storage_);
                vtable_ = other.vtable_;
                other.reset();
            }
        }

        BasicAction& operator=(BasicAction&& other) noexcept {
            if (this != &other) {
                reset();
                if (other.vtable_) {
                    other.vtable_->move_construct(&storage_, &other.storage_);
                    vtable_ = other.vtable_;
                    other.reset();
                }
            }
            return *this;
        }

        BasicAction(const BasicAction&) = delete;
        BasicAction& operator=(const BasicAction&) = delete;

        ~BasicAction() { reset(); }

        [[nodiscard]] bool valid() const noexcept { return vtable_ != nullptr; }

        void update(float progress, float dt) noexcept { if (vtable_) vtable_->update(&storage_, progress, dt); }
        void on_start() noexcept { if (vtable_) vtable_->on_start(&storage_); }
        void on_complete() noexcept { if (vtable_) vtable_->on_complete(&storage_); }
        [[nodiscard]] float duration() const noexcept { return vtable_ ? vtable_->duration(&storage_) : 0.0f; }

        [[nodiscard]] ResourceKey resource_key() const noexcept {
            return vtable_ ? vtable_->resource_key(&storage_) : kWorldResource;
        }

    private:
        void reset() noexcept {
            if (vtable_) {
                vtable_->destroy(&storage_);
                vtable_ = nullptr;
            }
        }

        template <typename A>
        static const Vtable* vtable_for() noexcept {
            static constexpr Vtable vt{
                .update = [](void* p, float progress, float dt) noexcept {
                    static_cast<A*>(p)->update(progress, dt);
                },
                .on_start = [](void* p) noexcept {
                    if constexpr (HasOnStart<A>) static_cast<A*>(p)->on_start();
                },
                .on_complete = [](void* p) noexcept {
                    if constexpr (HasOnComplete<A>) static_cast<A*>(p)->on_complete();
                },
                .duration = [](const void* p) noexcept -> float {
                    return static_cast<const A*>(p)->duration();
                },
                .resource_key = [](const void* p) noexcept -> ResourceKey {
                    return static_cast<const A*>(p)->resource_key();
                },
                .move_construct = [](void* dst, void* src) noexcept {
                    ::new(dst) A(std::move(*static_cast<A*>(src)));
                },
                .destroy = [](void* p) noexcept { static_cast<A*>(p)->~A(); },
            };
            return &vt;
        }

        alignas(InlineAlign) std::byte storage_[InlineBytes]{};
        const Vtable* vtable_ = nullptr;
    };

    using Action = BasicAction<>;

    // ----------------------------------------------------------------------------
    // Execution policies — compile-time selected, zero-overhead when Serial.
    // ----------------------------------------------------------------------------
    struct SerialExec {
        // Advance every node in insertion order on the calling thread.
        template <typename NodeRange, typename Fn>
        static void for_each(NodeRange& nodes, Fn&& fn) {
            for (auto& node : nodes) fn(node);
        }
    };

    // ----------------------------------------------------------------------------
    // TimelineNode — one erased action plus its scheduling state.
    // ----------------------------------------------------------------------------
    template <typename ActionT>
    struct BasicTimelineNode {
        ActionT action;
        float start_time = 0.0f;
        float elapsed = 0.0f;
        bool started = false;
        bool finished = false;

        [[nodiscard]] float end_time() const noexcept {
            return start_time + action.duration();
        }
    };

    using TimelineNode = BasicTimelineNode<Action>;

    // ----------------------------------------------------------------------------
    // Timeline — declarative animation scheduler.
    // ----------------------------------------------------------------------------
    // Template params default so the common case is a bare `Timeline`:
    //   - Capacity    : max concurrent actions (inline, no heap)
    //   - Exec        : execution policy (Serial default; supply a parallel policy)
    //   - InlineBytes : per-action inline storage budget (default 192 — fits every
    //                   shipped EDSL action; largest is SetMaterialAction at 152B)
    // ----------------------------------------------------------------------------
    template <std::size_t Capacity = 64,
              typename Exec = SerialExec,
              std::size_t InlineBytes = 192>
    class BasicTimeline {
    public:
        using node_type = BasicTimelineNode<BasicAction<InlineBytes>>;
        using action_type = BasicAction<InlineBytes>;

        BasicTimeline() = default;

        // Add an action with automatic resource dependency inference.
        // Accepts either an AnimationAction value directly or an ActionBuilder
        // (finalized via `.build()`).
        template <typename ArgT>
        BasicTimeline& add(ArgT&& arg) {
            if constexpr (ActionBuilder<ArgT>) {
                return add_action(std::forward<ArgT>(arg).build());
            }
            else {
                return add_action(std::forward<ArgT>(arg));
            }
        }

        // Variadic convenience add.
        template <typename First, typename... Rest>
            requires (sizeof...(Rest) > 0)
        BasicTimeline& add(First&& first, Rest&&... rest) {
            add(std::forward<First>(first));
            add(std::forward<Rest>(rest)...);
            return *this;
        }

        void update(float dt) {
            if (finished_) return;

            current_time_ += dt;
            bool all_done = true;

            // Single pass: start pending nodes whose start_time has arrived, advance
            // active nodes, and complete nodes reaching their end.
            Exec::for_each(nodes_, [&](node_type& node) {
                if (node.finished) return;

                if (!node.started) {
                    if (current_time_ < node.start_time) {
                        all_done = false;
                        return;
                    }
                    node.started = true;
                    node.action.on_start();
                }

                const float dur = node.action.duration();
                node.elapsed = std::min(dur, current_time_ - node.start_time);
                const float progress = (dur > 0.0f) ? std::min(1.0f, node.elapsed / dur) : 1.0f;

                node.action.update(progress, dt);

                if (node.elapsed >= dur) {
                    node.finished = true;
                    node.action.on_complete();
                }
                else {
                    all_done = false;
                }
            });

            finished_ = all_done && (current_time_ >= total_duration_);
        }

        [[nodiscard]] bool is_finished() const noexcept { return finished_; }
        [[nodiscard]] float total_duration() const noexcept { return total_duration_; }
        [[nodiscard]] float current_time() const noexcept { return current_time_; }
        [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
        [[nodiscard]] bool overflow() const noexcept { return nodes_.overflow(); }

        void reset() noexcept {
            current_time_ = 0.0f;
            finished_ = false;
            for (auto& node : nodes_) {
                node.elapsed = 0.0f;
                node.started = false;
                node.finished = false;
            }
        }

    private:
        // Inline open-addressed resource table for O(1) dependency inference.
        // Maps a ResourceKey to the latest end_time of any action writing it, so a
        // newly added action chains after the current tail on the same key while
        // remaining parallel to disjoint keys. Capacity is 2x node capacity to keep
        // the linear-probe load factor low; lives inline (zero heap).
        static constexpr std::size_t kSlots = Capacity * 2;

        struct DepSlot {
            ResourceKey key{};
            float latest_end = 0.0f;
            bool occupied = false;
        };

        [[nodiscard]] float latest_end_for(const ResourceKey& key) const noexcept {
            if constexpr (kSlots == 0) return 0.0f;
            const std::size_t h = std::hash<ResourceKey>{}(key) % kSlots;
            for (std::size_t i = 0; i < kSlots; ++i) {
                const DepSlot& s = dep_[(h + i) % kSlots];
                if (!s.occupied) return 0.0f;
                if (s.key == key) return s.latest_end;
            }
            return 0.0f;
        }

        void record_end_for(const ResourceKey& key, float end_time) noexcept {
            if constexpr (kSlots == 0) return;
            const std::size_t h = std::hash<ResourceKey>{}(key) % kSlots;
            for (std::size_t i = 0; i < kSlots; ++i) {
                DepSlot& s = dep_[(h + i) % kSlots];
                if (!s.occupied) {
                    s = DepSlot{.key = key, .latest_end = end_time, .occupied = true};
                    return;
                }
                if (s.key == key) {
                    s.latest_end = std::max(s.latest_end, end_time);
                    return;
                }
            }
            // Table full (should not happen: kSlots > Capacity). Dependency for this
            // key degrades to parallel; correctness of already-recorded keys intact.
        }

        template <typename A>
        BasicTimeline& add_action(A&& act) {
            action_type erased{std::forward<A>(act)};
            const ResourceKey key = erased.resource_key();
            const float dur = erased.duration();

            // O(1) dependency inference: chain after the latest end on the same key.
            const float start_t = latest_end_for(key);
            const float end_t = start_t + dur;

            const bool ok = nodes_.push_back(node_type{
                .action = std::move(erased),
                .start_time = start_t,
                .elapsed = 0.0f,
                .started = false,
                .finished = false,
            });
            if (!ok) return *this; // capacity reached; overflow() reports it

            record_end_for(key, end_t);
            total_duration_ = std::max(total_duration_, end_t);
            return *this;
        }

        containers::static_vector<node_type, Capacity> nodes_{};
        DepSlot dep_[kSlots > 0 ? kSlots : 1]{};
        float total_duration_ = 0.0f;
        float current_time_ = 0.0f;
        bool finished_ = false;
    };

    // Default Timeline: 64 concurrent actions, serial execution, 96-byte actions.
    using Timeline = BasicTimeline<>;
} // namespace pebble::spandana
