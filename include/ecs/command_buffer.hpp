#pragma once
// ============================================================================
// ecs/command_buffer.hpp — Linear Arena Deferred Mutation Buffer
// ============================================================================
// Records structural entity and component mutations into a contiguous LinearArena,
// eliminating all std::function heap allocations and mutex contention during recording.
//
// Zero virtual functions, zero macros, header-only C++23.
// ============================================================================

#include "entity.hpp"
#include "component_store.hpp"
#include "storage_policy.hpp"
#include "mem/arena.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace pebble::ecs {

enum class CmdOp : std::uint8_t {
    Spawn,
    Despawn,
    Add,
    Remove,
    Emplace
};

struct CmdHeader {
    CmdOp op;
    std::uint32_t type_id = 0;
    Entity entity{};
    std::size_t payload_size = 0;
    void (*destruct)(void* payload) noexcept = nullptr;
};

class CommandBuffer {
public:
    explicit CommandBuffer(std::size_t initial_bytes = 64 * 1024)
        : arena_(initial_bytes) {}

    ~CommandBuffer() {
        clear();
    }

    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;

    CommandBuffer(CommandBuffer&& other) noexcept
        : arena_(std::move(other.arena_)),
          headers_(std::move(other.headers_)),
          payloads_(std::move(other.payloads_)) {}

    CommandBuffer& operator=(CommandBuffer&& other) noexcept {
        if (this != &other) {
            clear();
            headers_ = std::move(other.headers_);
            payloads_ = std::move(other.payloads_);
        }
        return *this;
    }

    void* allocate_bytes(std::size_t size, std::size_t align) {
        std::lock_guard<std::mutex> lock(mutex_);
        void* mem = arena_.allocate(size, align);
        if (!mem) {
            mem = std::malloc(size);
        }
        return mem;
    }

    [[nodiscard]] Entity spawn() {
        Entity placeholder{};
        std::lock_guard<std::mutex> lock(mutex_);
        headers_.push_back(CmdHeader{
            .op = CmdOp::Spawn,
            .type_id = 0,
            .entity = placeholder,
            .payload_size = 0,
            .destruct = nullptr
        });
        payloads_.push_back(nullptr);
        spawn_out_.push_back(placeholder);
        return spawn_out_.back();
    }

    [[nodiscard]] std::span<Entity> spawn_batch(std::size_t n) {
        if (n == 0) return {};
        std::lock_guard<std::mutex> lock(mutex_);
        // Allocate entity batch directly from the LinearArena so the returned
        // span remains pointer-stable until execute() / clear() resets the arena.
        void* mem = arena_.allocate(sizeof(Entity) * n, alignof(Entity));
        Entity* batch = mem ? static_cast<Entity*>(mem)
                            : static_cast<Entity*>(std::malloc(sizeof(Entity) * n));
        for (std::size_t i = 0; i < n; ++i) {
            new (batch + i) Entity{};
            spawn_out_.push_back(batch[i]);
            headers_.push_back(CmdHeader{
                .op = CmdOp::Spawn,
                .type_id = 0,
                .entity = Entity{},
                .payload_size = 0,
                .destruct = nullptr
            });
            payloads_.push_back(nullptr);
        }
        return std::span<Entity>(batch, n);
    }

    void despawn(Entity e) {
        std::lock_guard<std::mutex> lock(mutex_);
        headers_.push_back(CmdHeader{
            .op = CmdOp::Despawn,
            .type_id = 0,
            .entity = e,
            .payload_size = 0,
            .destruct = nullptr
        });
        payloads_.push_back(nullptr);
    }

    template <Component C>
    void add(Entity e, C c) {
        void* mem = allocate_bytes(sizeof(C), alignof(C));
        new (mem) C(std::move(c));

        (void)ComponentTypeId<C>::id();

        std::lock_guard<std::mutex> lock(mutex_);
        headers_.push_back(CmdHeader{
            .op = CmdOp::Add,
            .type_id = ComponentTypeId<C>::id(),
            .entity = e,
            .payload_size = sizeof(C),
            .destruct = [](void* payload) noexcept {
                static_cast<C*>(payload)->~C();
            }
        });
        payloads_.push_back(mem);
    }

    template <Component C, typename... Args>
    void emplace(Entity e, Args&&... args) {
        add<C>(e, C(std::forward<Args>(args)...));
    }

    template <Component C>
    void remove(Entity e) {
        std::lock_guard<std::mutex> lock(mutex_);
        headers_.push_back(CmdHeader{
            .op = CmdOp::Remove,
            .type_id = ComponentTypeId<C>::id(),
            .entity = e,
            .payload_size = 0,
            .destruct = nullptr
        });
        payloads_.push_back(nullptr);
    }

    template <typename WorldT>
    void flush(WorldT& w) {
        std::vector<CmdHeader> local_hdrs;
        std::vector<void*> local_payloads;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            local_hdrs.swap(headers_);
            local_payloads.swap(payloads_);
        }

        for (std::size_t i = 0; i < local_hdrs.size(); ++i) {
            const auto& hdr = local_hdrs[i];
            void* payload = local_payloads[i];
            if (hdr.op == CmdOp::Spawn) {
                (void)w.spawn();
            } else if (hdr.op == CmdOp::Despawn) {
                w.despawn(hdr.entity);
            } else if (hdr.op == CmdOp::Add && payload) {
                w.add_by_type_id(hdr.entity, hdr.type_id, payload);
            } else if (hdr.op == CmdOp::Remove) {
                w.remove_by_type_id(hdr.entity, hdr.type_id);
            }
            if (payload && hdr.destruct) {
                hdr.destruct(payload);
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        arena_.reset();
        spawn_out_.clear();
    }

    [[nodiscard]] bool empty() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return headers_.empty();
    }

    void clear() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < headers_.size(); ++i) {
            if (payloads_[i] && headers_[i].destruct) {
                headers_[i].destruct(payloads_[i]);
            }
        }
        headers_.clear();
        payloads_.clear();
        arena_.reset();
    }

    void merge(std::vector<CmdHeader>&& hdrs, std::vector<void*>&& payloads) {
        std::lock_guard<std::mutex> lock(mutex_);
        headers_.insert(headers_.end(),
                        std::make_move_iterator(hdrs.begin()),
                        std::make_move_iterator(hdrs.end()));
        payloads_.insert(payloads_.end(),
                         std::make_move_iterator(payloads.begin()),
                         std::make_move_iterator(payloads.end()));
    }

private:
    mutable std::mutex mutex_;
    smriti::pools::LinearArena arena_;
    std::vector<CmdHeader> headers_;
    std::vector<void*> payloads_;
    std::vector<Entity> spawn_out_;
};

static_assert(!std::is_polymorphic_v<CommandBuffer>, "CommandBuffer must have zero virtual functions");

// ── LocalCommandBuffer: Thread-Local Zero-Lock Recording Context ─────────────

class LocalCommandBuffer {
public:
    explicit LocalCommandBuffer(CommandBuffer& global)
        : global_(global) {}

    ~LocalCommandBuffer() {
        if (!headers_.empty()) {
            global_.merge(std::move(headers_), std::move(payloads_));
        }
    }

    void despawn(Entity e) {
        headers_.push_back(CmdHeader{
            .op = CmdOp::Despawn,
            .type_id = 0,
            .entity = e,
            .payload_size = 0,
            .destruct = nullptr
        });
        payloads_.push_back(nullptr);
    }

    template <Component C>
    void add(Entity e, C c) {
        void* mem = global_.allocate_bytes(sizeof(C), alignof(C));
        new (mem) C(std::move(c));

        (void)ComponentTypeId<C>::id();

        headers_.push_back(CmdHeader{
            .op = CmdOp::Add,
            .type_id = ComponentTypeId<C>::id(),
            .entity = e,
            .payload_size = sizeof(C),
            .destruct = [](void* payload) noexcept {
                static_cast<C*>(payload)->~C();
            }
        });
        payloads_.push_back(mem);
    }

    template <Component C, typename... Args>
    void emplace(Entity e, Args&&... args) {
        add<C>(e, C(std::forward<Args>(args)...));
    }

    template <Component C>
    void remove(Entity e) {
        headers_.push_back(CmdHeader{
            .op = CmdOp::Remove,
            .type_id = ComponentTypeId<C>::id(),
            .entity = e,
            .payload_size = 0,
            .destruct = nullptr
        });
        payloads_.push_back(nullptr);
    }

private:
    CommandBuffer& global_;
    std::vector<CmdHeader> headers_;
    std::vector<void*> payloads_;
};

static_assert(!std::is_polymorphic_v<LocalCommandBuffer>, "LocalCommandBuffer must have zero virtual functions");

} // namespace pebble::ecs
