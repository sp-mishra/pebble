#pragma once
// ============================================================================
// gati/event.hpp — Frame-Coherent Messaging over Lock-Free RingBuffer
// ============================================================================
// One lockfree::RingBuffer<E, 4096> per event type.
// ============================================================================

#include "math.hpp"
#include "containers/lockfree/RingBuffer.hpp"

#include <cstdint>
#include <memory>
#include <typeindex>
#include <unordered_map>

namespace gati {

inline constexpr std::size_t kEventQueueCapacity = 4096;

class EventBus {
public:
    template <typename E>
    void publish(const E& e) {
        (void)channel<E>().try_push(e);
    }

    template <typename E, typename Fn>
    std::size_t drain(Fn&& fn) {
        auto& q = channel<E>();
        std::size_t n = 0;
        while (auto v = q.try_pop()) {
            fn(*v);
            ++n;
        }
        return n;
    }

    template <typename E>
    void clear() {
        auto& q = channel<E>();
        while (q.try_pop()) {}
    }

private:
    template <typename E>
    lockfree::RingBuffer<E, kEventQueueCapacity>& channel() {
        const std::type_index ti{typeid(E)};
        auto it = channels_.find(ti);
        if (it == channels_.end()) {
            auto sp = std::make_shared<lockfree::RingBuffer<E, kEventQueueCapacity>>();
            it = channels_.emplace(ti, std::move(sp)).first;
        }
        return *static_cast<lockfree::RingBuffer<E, kEventQueueCapacity>*>(it->second.get());
    }

    std::unordered_map<std::type_index, std::shared_ptr<void>> channels_;
};

enum class ContactPhase : std::uint8_t {
    Enter,
    Stay,
    Exit
};

// Built-in gameplay & physics event payloads
struct ContactEvent {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    Vec2 normal{0.0f, 0.0f};
    Scalar depth = 0.0f;
    Vec2 point{0.0f, 0.0f};
};

struct ContactPhaseEvent {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    ContactPhase phase{ContactPhase::Enter};
    Vec2 normal{0.0f, 0.0f};
    Scalar depth = 0.0f;
    Vec2 point{0.0f, 0.0f};
};

struct TriggerEvent  { std::uint32_t entity, trigger; bool entering; };
struct FractureEvent { std::uint32_t entity; std::uint32_t shard_count; };

// ── Contact Lifecycle Tracker (Enter / Stay / Exit stateful manager) ─────────────
class ContactStateTracker {
public:
    void update(EventBus& bus) {
        std::unordered_map<std::uint64_t, ContactEvent> current_contacts;

        // Drain raw contacts from physics/narrowphase
        bus.drain<ContactEvent>([&](const ContactEvent& ce) {
            const std::uint64_t key = make_key(ce.a, ce.b);
            current_contacts[key] = ce;
        });

        // 1. Detect Enter & Stay
        for (const auto& [key, ce] : current_contacts) {
            if (active_contacts_.find(key) == active_contacts_.end()) {
                // New collision -> Enter
                bus.publish(ContactPhaseEvent{ce.a, ce.b, ContactPhase::Enter, ce.normal, ce.depth, ce.point});
            } else {
                // Existing collision -> Stay
                bus.publish(ContactPhaseEvent{ce.a, ce.b, ContactPhase::Stay, ce.normal, ce.depth, ce.point});
            }
        }

        // 2. Detect Exit
        for (const auto& [key, ce] : active_contacts_) {
            if (current_contacts.find(key) == current_contacts.end()) {
                bus.publish(ContactPhaseEvent{ce.a, ce.b, ContactPhase::Exit, ce.normal, 0.0f, ce.point});
            }
        }

        active_contacts_ = std::move(current_contacts);
    }

private:
    static constexpr std::uint64_t make_key(std::uint32_t a, std::uint32_t b) noexcept {
        const auto u1 = static_cast<std::uint64_t>(std::min(a, b));
        const auto u2 = static_cast<std::uint64_t>(std::max(a, b));
        return (u1 << 32) | u2;
    }

    std::unordered_map<std::uint64_t, ContactEvent> active_contacts_;
};

} // namespace gati
