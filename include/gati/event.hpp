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

// Built-in gameplay & physics event payloads
struct ContactEvent {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    Vec2 normal{0.0f, 0.0f};
    Scalar depth = 0.0f;
    Vec2 point{0.0f, 0.0f};
};
struct TriggerEvent  { std::uint32_t entity, trigger; bool entering; };
struct FractureEvent { std::uint32_t entity; std::uint32_t shard_count; };

} // namespace gati
