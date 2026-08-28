#pragma once
// ============================================================================
// pravaha.hpp - C++23 Task-Graph Orchestration Engine
// ============================================================================

#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <limits>
#include <typeindex>
#include <utility>
#include <variant>
#include <vector>
#include <unordered_map>

#include "containers/graph/LiteGraph.hpp"
#include "containers/graph/LiteGraphAlgorithms.hpp"
#include "containers/tree/NAryTree.hpp"
#include "meta/meta.hpp"
#include "vakya/vakya.hpp"

// Transitional implementation detail for the historical symbolic frontend.
// This is scoped to Pravaha; it does not create a dependency on Lithe.
namespace pravaha { namespace lithe = ::vakya; }

namespace pravaha {
    // ============================================================================
    //  SECTION 1: ERROR HANDLING
    // ============================================================================

    enum class ErrorKind {
        ParseError, ValidationError, CycleDetected, SymbolNotFound,
        TypeMismatch, ExecutorUnavailable, DomainConstraintViolation,
        PayloadNotSerializable, PayloadNotTransferable, TaskFailed,
        TaskCanceled, QueueRejected, Timeout, InternalError,
        ResourceExhausted, InvalidArgument
    };

    struct PravahaError : std::exception {
        ErrorKind kind;
        std::string message;
        std::string task_identity;
        std::source_location location;

        PravahaError(ErrorKind k, std::string_view msg, std::string_view task_id = {},
                     const std::source_location loc = std::source_location::current())
            : kind{k}, message{msg}, task_identity{task_id}, location{loc} {}

        [[nodiscard]] const char* what() const noexcept override {
            return message.c_str();
        }

        // Source locations are diagnostic-only.  Task outcomes compare by
        // their semantic error payload, which also satisfies std::expected's
        // C++23 comparison constraints on current macOS libc++.
        [[nodiscard]] bool operator==(const PravahaError& other) const noexcept {
            return kind == other.kind && message == other.message &&
                   task_identity == other.task_identity;
        }

        static PravahaError make(ErrorKind k, std::string_view msg,
                                 const std::source_location loc = std::source_location::current()) {
            return PravahaError{k, msg, {}, loc};
        }

        static PravahaError make_for_task(ErrorKind k, std::string_view msg, std::string_view task_id,
                                          const std::source_location loc = std::source_location::current()) {
            return PravahaError{k, msg, task_id, loc};
        }
    };

    template <class T>
    using Outcome = std::expected<T, PravahaError>;

    using Unit = std::monostate;

    template <class T>
    struct is_outcome : std::false_type {};

    template <class T>
    struct is_outcome<Outcome<T>> : std::true_type {};

    template <class T>
    inline constexpr bool is_outcome_v = is_outcome<std::remove_cvref_t<T>>::value;

    template <class T>
    [[nodiscard]] std::size_t runtime_type_hash() {
        using Normalized = std::remove_cvref_t<T>;
        if constexpr (requires { meta::schema_hash<Normalized>(); }) {
            return meta::schema_hash<Normalized>();
        }
        else {
            return std::type_index(typeid(Normalized)).hash_code();
        }
    }

    struct ResultSlot {
        using destroy_fn_t = void(*)(void*) noexcept;
        using copy_fn_t = void*(*)(const void*);
        using make_vector_fn_t = void*(*)();
        using append_to_vector_fn_t = void(*)(void*, const void*);

        std::size_t type_hash{};
        void* storage{nullptr};
        destroy_fn_t destroy_fn{nullptr};
        copy_fn_t copy_fn{nullptr};
        make_vector_fn_t make_vector_fn{nullptr};
        append_to_vector_fn_t append_to_vector_fn{nullptr};
        destroy_fn_t destroy_vector_fn{nullptr};
        std::size_t vector_type_hash{};

        ResultSlot() noexcept = default;

        ResultSlot(const ResultSlot&) = delete;

        ResultSlot& operator=(const ResultSlot&) = delete;

        ResultSlot(ResultSlot&& other) noexcept
            : type_hash{other.type_hash}, storage{other.storage}, destroy_fn{other.destroy_fn},
              copy_fn{other.copy_fn}, make_vector_fn{other.make_vector_fn},
              append_to_vector_fn{other.append_to_vector_fn},
              destroy_vector_fn{other.destroy_vector_fn}, vector_type_hash{other.vector_type_hash} {
            other.type_hash = 0;
            other.storage = nullptr;
            other.destroy_fn = nullptr;
            other.copy_fn = nullptr;
            other.make_vector_fn = nullptr;
            other.append_to_vector_fn = nullptr;
            other.destroy_vector_fn = nullptr;
            other.vector_type_hash = 0;
        }

        ResultSlot& operator=(ResultSlot&& other) noexcept {
            if (this == &other) {
                return *this;
            }
            reset();
            type_hash = other.type_hash;
            storage = other.storage;
            destroy_fn = other.destroy_fn;
            copy_fn = other.copy_fn;
            make_vector_fn = other.make_vector_fn;
            append_to_vector_fn = other.append_to_vector_fn;
            destroy_vector_fn = other.destroy_vector_fn;
            vector_type_hash = other.vector_type_hash;
            other.type_hash = 0;
            other.storage = nullptr;
            other.destroy_fn = nullptr;
            other.copy_fn = nullptr;
            other.make_vector_fn = nullptr;
            other.append_to_vector_fn = nullptr;
            other.destroy_vector_fn = nullptr;
            other.vector_type_hash = 0;
            return *this;
        }

        ~ResultSlot() noexcept {
            reset();
        }

        [[nodiscard]] bool empty() const noexcept {
            return storage == nullptr;
        }

        void reset() noexcept {
            if (destroy_fn && storage) {
                destroy_fn(storage);
            }
            type_hash = 0;
            storage = nullptr;
            destroy_fn = nullptr;
            copy_fn = nullptr;
            make_vector_fn = nullptr;
            append_to_vector_fn = nullptr;
            destroy_vector_fn = nullptr;
            vector_type_hash = 0;
        }

        template <class T, class... Args>
        void emplace(Args&&... args) {
            using Normalized = std::remove_cvref_t<T>;
            reset();
            storage = new Normalized(std::forward<Args>(args)...);
            destroy_fn = [](void* ptr) noexcept {
                delete static_cast<Normalized*>(ptr);
            };
            if constexpr (std::copy_constructible<Normalized>) {
                copy_fn = [](const void* ptr) -> void* {
                    return new Normalized(*static_cast<const Normalized*>(ptr));
                };
                make_vector_fn = []() -> void* {
                    return new std::vector<Normalized>();
                };
                append_to_vector_fn = [](void* vec, const void* ptr) {
                    static_cast<std::vector<Normalized>*>(vec)->push_back(*static_cast<const Normalized*>(ptr));
                };
                destroy_vector_fn = [](void* vec) noexcept {
                    delete static_cast<std::vector<Normalized>*>(vec);
                };
                vector_type_hash = runtime_type_hash<std::vector<Normalized>>();
            }
            type_hash = runtime_type_hash<Normalized>();
        }

        [[nodiscard]] bool copy_from(const ResultSlot& other) {
            if (other.storage == nullptr || other.copy_fn == nullptr) {
                return false;
            }
            reset();
            storage = other.copy_fn(other.storage);
            destroy_fn = other.destroy_fn;
            copy_fn = other.copy_fn;
            make_vector_fn = other.make_vector_fn;
            append_to_vector_fn = other.append_to_vector_fn;
            destroy_vector_fn = other.destroy_vector_fn;
            type_hash = other.type_hash;
            vector_type_hash = other.vector_type_hash;
            return true;
        }

        [[nodiscard]] bool aggregate_from(const std::span<const ResultSlot* const> slots) {
            if (slots.empty()) {
                return false;
            }
            const ResultSlot* exemplar = slots.front();
            if (!exemplar || exemplar->storage == nullptr || exemplar->make_vector_fn == nullptr || exemplar->
                append_to_vector_fn == nullptr || exemplar->destroy_vector_fn == nullptr || exemplar->vector_type_hash
                == 0) {
                return false;
            }
            void* vec_storage = exemplar->make_vector_fn();
            for (const ResultSlot* slot : slots) {
                if (!slot || slot->storage == nullptr || slot->type_hash != exemplar->type_hash) {
                    exemplar->destroy_vector_fn(vec_storage);
                    return false;
                }
                exemplar->append_to_vector_fn(vec_storage, slot->storage);
            }
            reset();
            storage = vec_storage;
            destroy_fn = exemplar->destroy_vector_fn;
            type_hash = exemplar->vector_type_hash;
            return true;
        }

        template <class T>
        [[nodiscard]] T* get_if() noexcept {
            using Normalized = std::remove_cvref_t<T>;
            if (type_hash != runtime_type_hash<Normalized>() || storage == nullptr) {
                return nullptr;
            }
            return static_cast<Normalized*>(storage);
        }

        template <class T>
        [[nodiscard]] const T* get_if() const noexcept {
            using Normalized = std::remove_cvref_t<T>;
            if (type_hash != runtime_type_hash<Normalized>() || storage == nullptr) {
                return nullptr;
            }
            return static_cast<const Normalized*>(storage);
        }
    };

    // ============================================================================
    //  SECTION 2: ENUMERATIONS
    // ============================================================================

    enum class TaskState { Created, Ready, Scheduled, Running, Succeeded, Failed, Canceled, Skipped };

    enum class JoinPolicyKind { AllOrNothing, CollectAll, AnySuccess, Quorum };

    enum class ExecutionDomain { Inline, CPU, IO, Fiber, Coroutine, External };

    enum class RetryDecision { FailFinal, RetryImmediate };

    enum class TaskPriority { Low, Normal, High };

    struct JoinPolicy {
        JoinPolicyKind kind{JoinPolicyKind::AllOrNothing};
        std::size_t quorum_required{0};
    };

    // ============================================================================
    //  SECTION 3: PAYLOAD CONCEPTS
    // ============================================================================

    template <typename T>
    concept Payload = std::move_constructible<T> && std::destructible<T>;

    template <typename T>
    concept LocalPayload = Payload<T> && std::move_constructible<T>;

    template <typename T>
    concept TransferablePayload = Payload<T>
        && std::is_trivially_copyable_v<T>
        && std::is_standard_layout_v<T>;

    template <typename T>
    concept CopyablePayload = Payload<T> && std::copy_constructible<T>;

    namespace detail {
        template <typename T>
        consteval bool is_serializable_payload_check() {
            if constexpr (!std::is_trivially_copyable_v<T> || !std::is_standard_layout_v<T>) return false;
            else if constexpr (meta::Reflectable<T>) return meta::is_zero_copy_serializable<T>();
            else return true;
        }

        template <class T>
        struct callable_traits;

        template <class R, class... Args>
        struct callable_traits<R(*)(Args...)> {
            using result_type = R;
            static constexpr std::size_t arity = sizeof...(Args);

            template <std::size_t Index>
            using arg_t = std::tuple_element_t<Index, std::tuple<Args...>>;
        };

        template <class C, class R, class... Args>
        struct callable_traits<R(C::*)(Args...)> {
            using result_type = R;
            static constexpr std::size_t arity = sizeof...(Args);

            template <std::size_t Index>
            using arg_t = std::tuple_element_t<Index, std::tuple<Args...>>;
        };

        template <class C, class R, class... Args>
        struct callable_traits<R(C::*)(Args...) const> {
            using result_type = R;
            static constexpr std::size_t arity = sizeof...(Args);

            template <std::size_t Index>
            using arg_t = std::tuple_element_t<Index, std::tuple<Args...>>;
        };

        template <class F>
        struct callable_traits : callable_traits<decltype(&std::remove_cvref_t<F>::operator())> {};
    } // namespace detail

    template <typename T>
    concept SerializablePayload = Payload<T> && detail::is_serializable_payload_check<T>();

    template <typename F, typename Result, typename... Args>
    concept InvocableTask = std::invocable<F, Args...> &&
        std::same_as<std::invoke_result_t<F, Args...>, Outcome<Result>>;

    // ============================================================================
    //  SECTION 4: CANCELLATION PRIMITIVES
    // ============================================================================

    struct CancellationState {
        std::atomic<bool> requested{false};
        void request() noexcept { requested.store(true, std::memory_order_release); }
        [[nodiscard]] bool is_requested() const noexcept { return requested.load(std::memory_order_acquire); }
    };

    class CancellationToken {
        std::shared_ptr<CancellationState> state_{};

    public:
        CancellationToken() noexcept = default;

        explicit CancellationToken(std::shared_ptr<CancellationState> state) noexcept : state_{std::move(state)} {}

        [[nodiscard]] bool stop_requested() const noexcept { return state_ && state_->is_requested(); }
        [[nodiscard]] bool has_state() const noexcept { return state_ != nullptr; }
    };

    class CancellationSource {
        std::shared_ptr<CancellationState> state_;

    public:
        CancellationSource() : state_{std::make_shared<CancellationState>()} {}

        [[nodiscard]] CancellationToken token() const noexcept { return CancellationToken{state_}; }
        void request_stop() noexcept { state_->request(); }
        [[nodiscard]] bool stop_requested() const noexcept { return state_->is_requested(); }
    };

    class CancellationScope {
        CancellationSource local_source_;
        CancellationToken parent_token_;

    public:
        CancellationScope() = default;

        explicit CancellationScope(CancellationToken parent) noexcept : parent_token_{std::move(parent)} {}

        [[nodiscard]] bool stop_requested() const noexcept {
            return local_source_.stop_requested() || parent_token_.stop_requested();
        }

        [[nodiscard]] CancellationToken token() const noexcept { return local_source_.token(); }
        void request_stop() noexcept { local_source_.request_stop(); }
    };

    // ============================================================================
    //  SECTION 5: TASK COMMAND - STATIC TYPE ERASURE
    // ============================================================================

    class TaskCommand {
        static constexpr std::size_t buffer_size = 128;
        static constexpr std::size_t buffer_align = alignof(std::max_align_t);
        using invoke_fn_t = Outcome<Unit>(*)(void*, ResultSlot*, ResultSlot*) noexcept;
        using move_fn_t = void(*)(void*, void*) noexcept;
        using destroy_fn_t = void(*)(void*) noexcept;

        alignas(buffer_align) unsigned char storage_[buffer_size]{};
        invoke_fn_t invoke_fn_{nullptr};
        move_fn_t move_fn_{nullptr};
        destroy_fn_t destroy_fn_{nullptr};
        std::string debug_name_{};
        TaskPriority priority_{TaskPriority::Normal};

        template <typename R>
        static Outcome<Unit> store_successful_result(R&& value, ResultSlot* result_slot) {
            using NormalizedR = std::remove_cvref_t<R>;
            if constexpr (std::is_void_v<NormalizedR>) {
                if (result_slot) {
                    result_slot->emplace<Unit>(Unit{});
                }
                return Outcome<Unit>{Unit{}};
            }
            else if constexpr (std::same_as<NormalizedR, Unit>) {
                if (result_slot) {
                    result_slot->emplace<Unit>(std::forward<R>(value));
                }
                return Outcome<Unit>{Unit{}};
            }
            else if constexpr (std::same_as<NormalizedR, Outcome<Unit>>) {
                auto out = std::forward<R>(value);
                if (!out.has_value()) {
                    return std::unexpected(out.error());
                }
                if (result_slot) {
                    result_slot->emplace<Unit>(Unit{});
                }
                return Outcome<Unit>{Unit{}};
            }
            else if constexpr (is_outcome_v<NormalizedR>) {
                auto out = std::forward<R>(value);
                if (!out.has_value()) {
                    return std::unexpected(out.error());
                }
                using ValueType = NormalizedR::value_type;
                if (result_slot) {
                    result_slot->emplace<ValueType>(std::move(out.value()));
                }
                return Outcome<Unit>{Unit{}};
            }
            else {
                if (result_slot) {
                    result_slot->emplace<NormalizedR>(std::forward<R>(value));
                }
                return Outcome<Unit>{Unit{}};
            }
        }

        template <typename F>
        static Outcome<Unit> invoke_impl(void* s, ResultSlot* result_slot, ResultSlot* input_slot) noexcept {
            try {
                F& fn = *std::launder(static_cast<F*>(s));
                if constexpr (std::invocable<F&>) {
                    if constexpr (std::is_void_v<std::invoke_result_t<F&>>) {
                        fn();
                        return store_successful_result(Unit{}, result_slot);
                    }
                    else {
                        return store_successful_result(fn(), result_slot);
                    }
                }
                else if constexpr (detail::callable_traits<F>::arity == 1) {
                    using Arg = detail::callable_traits<F>::template arg_t<0>;
                    using InputType = std::remove_cvref_t<Arg>;
                    if (!input_slot) {
                        return std::unexpected(PravahaError{ErrorKind::TaskFailed, "task input unavailable"});
                    }
                    const auto* input = input_slot->get_if<InputType>();
                    if (!input) {
                        return std::unexpected(PravahaError{ErrorKind::TypeMismatch, "task input type mismatch"});
                    }
                    if constexpr (std::is_void_v<std::invoke_result_t<F&, Arg>>) {
                        std::invoke(fn, static_cast<Arg>(*input));
                        return store_successful_result(Unit{}, result_slot);
                    }
                    else {
                        return store_successful_result(std::invoke(fn, static_cast<Arg>(*input)), result_slot);
                    }
                }
                else {
                    return Outcome<Unit>{Unit{}};
                }
            }
            catch (const std::exception& e) {
                return std::unexpected(PravahaError{ErrorKind::TaskFailed, e.what()});
            }
            catch (...) {
                return std::unexpected(PravahaError{ErrorKind::TaskFailed, "unknown exception"});
            }
        }

        template <typename F>
        static void move_impl(void* d, void* s) noexcept {
            ::new(d) F(std::move(*std::launder(reinterpret_cast<F*>(s))));
            std::launder(reinterpret_cast<F*>(s))->~F();
        }

        template <typename F>
        static void destroy_impl(void* s) noexcept { std::launder(reinterpret_cast<F*>(s))->~F(); }

        struct forwarding_target_t {
            TaskCommand* target{nullptr};
        };

        static Outcome<Unit> invoke_forwarding_impl(void* s, ResultSlot* result_slot, ResultSlot* input_slot) noexcept {
            const auto* forwarding = std::launder(reinterpret_cast<forwarding_target_t*>(s));
            if (forwarding == nullptr || forwarding->target == nullptr) {
                return std::unexpected(PravahaError{
                    ErrorKind::TaskFailed, "TaskCommand forwarding target unavailable"
                });
            }
            return forwarding->target->run(result_slot, input_slot);
        }

        void destroy_current() noexcept {
            if (destroy_fn_) {
                destroy_fn_(storage_);
                invoke_fn_ = nullptr;
                move_fn_ = nullptr;
                destroy_fn_ = nullptr;
            }
        }

    public:
        TaskCommand() noexcept = default;

        TaskCommand(const TaskCommand&) = delete;

        TaskCommand& operator=(const TaskCommand&) = delete;

        TaskCommand(TaskCommand&& o) noexcept : invoke_fn_{o.invoke_fn_}, move_fn_{o.move_fn_},
                                                destroy_fn_{o.destroy_fn_}, debug_name_{std::move(o.debug_name_)},
                                                priority_{o.priority_} {
            if (move_fn_) {
                move_fn_(storage_, o.storage_);
                o.invoke_fn_ = nullptr;
                o.move_fn_ = nullptr;
                o.destroy_fn_ = nullptr;
                o.debug_name_.clear();
                o.priority_ = TaskPriority::Normal;
            }
        }

        TaskCommand& operator=(TaskCommand&& o) noexcept {
            if (this != &o) {
                destroy_current();
                invoke_fn_ = o.invoke_fn_;
                move_fn_ = o.move_fn_;
                destroy_fn_ = o.destroy_fn_;
                debug_name_ = std::move(o.debug_name_);
                priority_ = o.priority_;
                if (move_fn_) {
                    move_fn_(storage_, o.storage_);
                    o.invoke_fn_ = nullptr;
                    o.move_fn_ = nullptr;
                    o.destroy_fn_ = nullptr;
                    o.debug_name_.clear();
                    o.priority_ = TaskPriority::Normal;
                }
            }
            return *this;
        }

        ~TaskCommand() noexcept { destroy_current(); }

        template <typename F> requires std::move_constructible<std::decay_t<F>>
        static TaskCommand make(F&& f, const std::string_view debug_name = {},
                                const TaskPriority priority = TaskPriority::Normal) {
            using S = std::decay_t<F>;
            static_assert(sizeof(S) <= buffer_size,
                          "TaskCommand: callable exceeds inline storage capacity (128 bytes).");
            static_assert(alignof(S) <= buffer_align, "TaskCommand: callable alignment exceeds buffer alignment.");
            TaskCommand cmd;
            ::new(cmd.storage_) S(std::forward<F>(f));
            cmd.invoke_fn_ = &invoke_impl<S>;
            cmd.move_fn_ = &move_impl<S>;
            cmd.destroy_fn_ = &destroy_impl<S>;
            cmd.debug_name_ = std::string{debug_name};
            cmd.priority_ = priority;
            return cmd;
        }

        static TaskCommand make_forwarding(TaskCommand* target, std::string debug_name = {}) {
            static_assert(sizeof(forwarding_target_t) <= buffer_size,
                          "TaskCommand: forwarding target exceeds inline storage capacity (128 bytes).");
            static_assert(alignof(forwarding_target_t) <= buffer_align,
                          "TaskCommand: forwarding target alignment exceeds buffer alignment.");
            TaskCommand cmd;
            ::new(cmd.storage_) forwarding_target_t{target};
            cmd.invoke_fn_ = &invoke_forwarding_impl;
            cmd.move_fn_ = &move_impl<forwarding_target_t>;
            cmd.destroy_fn_ = &destroy_impl<forwarding_target_t>;
            cmd.debug_name_ = std::move(debug_name);
            cmd.priority_ = (target != nullptr) ? target->priority() : TaskPriority::Normal;
            return cmd;
        }

        Outcome<Unit> run(ResultSlot* result_slot = nullptr, ResultSlot* input_slot = nullptr) noexcept {
            if (!invoke_fn_)
                return std::unexpected(PravahaError{
                    ErrorKind::TaskFailed, "TaskCommand::run() called on empty command"
                });
            return invoke_fn_(storage_, result_slot, input_slot);
        }

        [[nodiscard]] bool has_value() const noexcept { return invoke_fn_ != nullptr; }
        [[nodiscard]] bool empty() const noexcept { return invoke_fn_ == nullptr; }
        [[nodiscard]] std::string_view name() const noexcept { return debug_name_; }
        [[nodiscard]] TaskPriority priority() const noexcept { return priority_; }
        [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
    };

    // ============================================================================
    //  SECTION 6: LAZY EXPRESSION DSL
    // ============================================================================

    namespace symbolic {
        struct SymbolicMeta {
            std::string dump;
            std::size_t hash{};
        };

        // Compatibility spelling for code written before Pravaha owned this
        // metadata.  It is an alias only; no Lithe type participates here.
        using LitheFrontendMeta = SymbolicMeta;

        // Stable frontend-identity descriptor at the Lithe -> TaskIr boundary.
        struct SymbolicSource {
            SymbolicMeta frontend;
            std::string debug_name;
        };

        using LitheSymbolicSource = SymbolicSource;

        namespace lithe_frontend {
            LitheFrontendMeta make_task_ref_meta(std::string_view name);

            LitheFrontendMeta make_sequence_meta(std::size_t left_hash, std::size_t right_hash);

            LitheFrontendMeta make_parallel_meta(std::size_t left_hash, std::size_t right_hash);

            LitheFrontendMeta make_collect_all_meta(std::size_t expr_hash);

            LitheFrontendMeta make_any_success_meta(std::size_t expr_hash);

            LitheFrontendMeta make_quorum_meta(std::size_t expr_hash, std::size_t required);

            LitheFrontendMeta make_named_meta(std::size_t expr_hash, std::string_view name);

            LitheFrontendMeta make_domain_meta(std::size_t expr_hash, ExecutionDomain domain);

            LitheFrontendMeta make_priority_meta(std::size_t expr_hash, TaskPriority priority);

            LitheFrontendMeta make_retry_meta(std::size_t expr_hash, std::size_t max_retries);

            LitheFrontendMeta make_timeout_meta(std::size_t expr_hash, std::chrono::nanoseconds timeout);

            LitheFrontendMeta make_parallel_reduce_meta(std::size_t chunk_size, std::size_t range_size,
                                                        bool has_range_size);

            LitheFrontendMeta make_parallel_for_meta(std::size_t chunk_size, std::size_t range_size,
                                                     bool has_range_size);

            LitheFrontendMeta make_parallel_transform_meta(std::size_t chunk_size, std::size_t range_size,
                                                           bool has_range_size);
        } // namespace lithe_frontend
    } // namespace symbolic

    template <typename F>
    class TaskExpr;
    template <typename L, typename R>
    struct SequenceExpr;
    template <typename L, typename R>
    struct ParallelExpr;
    template <typename Expr>
    struct NamedExpr;
    template <typename Expr>
    struct DomainExpr;
    template <typename Expr>
    struct PriorityExpr;
    template <std::size_t N, typename Expr>
    struct RetryExpr;
    template <typename Expr>
    struct TimeoutExpr;
    struct StaticChunkingPolicy;
    struct NAryTreeReductionPolicy;
    template <typename Range, typename Init, typename MapFn, typename ReduceFn, typename ChunkingPolicy =
              StaticChunkingPolicy, typename ReductionPolicy = NAryTreeReductionPolicy>
    struct ParallelReduceExpr;
    template <typename Range, typename BodyFn, typename ChunkingPolicy = StaticChunkingPolicy>
    struct ParallelForExpr;
    template <typename InRange, typename OutRange, typename Fn, typename ChunkingPolicy = StaticChunkingPolicy>
    struct ParallelTransformExpr;

    namespace detail {
        template <typename T>
        struct is_pravaha_expr_impl : std::false_type {};

        template <typename F>
        struct is_pravaha_expr_impl<TaskExpr<F>> : std::true_type {};

        template <typename L, typename R>
        struct is_pravaha_expr_impl<SequenceExpr<L, R>> : std::true_type {};

        template <typename L, typename R>
        struct is_pravaha_expr_impl<ParallelExpr<L, R>> : std::true_type {};

        template <typename Expr>
        struct is_pravaha_expr_impl<NamedExpr<Expr>> : std::true_type {};

        template <typename Expr>
        struct is_pravaha_expr_impl<DomainExpr<Expr>> : std::true_type {};

        template <typename Expr>
        struct is_pravaha_expr_impl<PriorityExpr<Expr>> : std::true_type {};

        template <std::size_t N, typename Expr>
        struct is_pravaha_expr_impl<RetryExpr<N, Expr>> : std::true_type {};

        template <typename Expr>
        struct is_pravaha_expr_impl<TimeoutExpr<Expr>> : std::true_type {};

        template <typename Range, typename Init, typename MapFn, typename ReduceFn, typename ChunkingPolicy, typename
                  ReductionPolicy>
        struct is_pravaha_expr_impl<ParallelReduceExpr<Range, Init, MapFn, ReduceFn, ChunkingPolicy,
                                                       ReductionPolicy>> : std::true_type {};

        template <typename Range, typename BodyFn, typename ChunkingPolicy>
        struct is_pravaha_expr_impl<ParallelForExpr<Range, BodyFn, ChunkingPolicy>> : std::true_type {};

        template <typename InRange, typename OutRange, typename Fn, typename ChunkingPolicy>
        struct is_pravaha_expr_impl<ParallelTransformExpr<InRange, OutRange, Fn, ChunkingPolicy>> : std::true_type {};

        template <typename RangeLike>
        [[nodiscard]] auto range_size_hint(const RangeLike& range) {
            if constexpr (requires { range.size(); }) {
                return std::pair{true, static_cast<std::size_t>(range.size())};
            }
            return std::pair<bool, std::size_t>{false, 0};
        }
    } // namespace detail

    struct ChunkRange {
        std::size_t begin;
        std::size_t end;

        bool operator==(const ChunkRange&) const = default;
    };

    struct StaticChunkingPolicy {
        [[nodiscard]] static std::vector<ChunkRange> chunks(const std::size_t total, const std::size_t chunk_size) {
            const std::size_t normalized_chunk_size = (chunk_size == 0) ? 1 : chunk_size;
            const std::size_t num_chunks = (total == 0)
                                               ? 0
                                               : (total + normalized_chunk_size - 1) / normalized_chunk_size;
            std::vector<ChunkRange> result;
            result.reserve(num_chunks);
            for (std::size_t i = 0; i < num_chunks; ++i) {
                const std::size_t begin = i * normalized_chunk_size;
                const std::size_t end = std::min(begin + normalized_chunk_size, total);
                result.push_back(ChunkRange{begin, end});
            }
            return result;
        }
    };

    template <typename T>
    concept IsPravahaExpr = detail::is_pravaha_expr_impl<std::remove_cvref_t<T>>::value;

    template <typename F>
    class TaskExpr {
        std::string name_;
        F callable_;
        ExecutionDomain domain_;

    public:
        symbolic::LitheFrontendMeta frontend;

        TaskExpr(std::string_view name, F callable, const ExecutionDomain domain = ExecutionDomain::CPU)
            : name_{name}, callable_{std::move(callable)}, domain_{domain},
              frontend{symbolic::lithe_frontend::make_task_ref_meta(name_)} {}

        [[nodiscard]] const std::string& name() const noexcept { return name_; }
        [[nodiscard]] ExecutionDomain domain() const noexcept { return domain_; }
        [[nodiscard]] const F& callable() const noexcept { return callable_; }
        [[nodiscard]] F& callable() noexcept { return callable_; }
    };

    template <typename L, typename R>
    struct SequenceExpr {
        L left;
        R right;
        symbolic::LitheFrontendMeta frontend;

        SequenceExpr(L l, R r)
            : left{std::move(l)}, right{std::move(r)},
              frontend{symbolic::lithe_frontend::make_sequence_meta(left.frontend.hash, right.frontend.hash)} {}
    };

    template <typename L, typename R>
    struct ParallelExpr {
        L left;
        R right;
        JoinPolicy policy{};
        symbolic::LitheFrontendMeta frontend;

        ParallelExpr(L l, R r, const JoinPolicy p = JoinPolicy{})
            : left{std::move(l)}, right{std::move(r)}, policy{p},
              frontend{symbolic::lithe_frontend::make_parallel_meta(left.frontend.hash, right.frontend.hash)} {}
    };

    template <typename T>
    struct ReduceResultHandle {
        std::shared_ptr<std::optional<T>> value;
        std::shared_ptr<std::mutex> mutex;
    };

    template <typename Range, typename Init, typename MapFn, typename ReduceFn, typename ChunkingPolicy, typename
              ReductionPolicy>
    struct ParallelReduceExpr {
        Range range;
        Init init;
        MapFn map_fn;
        ReduceFn reduce_fn;
        std::size_t chunk_size{};
        symbolic::LitheFrontendMeta frontend;
        ReduceResultHandle<Init> result_handle;

        [[nodiscard]] const ReduceResultHandle<Init>& result() const noexcept { return result_handle; }
    };

    template <typename Range, typename BodyFn, typename ChunkingPolicy>
    struct ParallelForExpr {
        Range range;
        BodyFn fn;
        std::size_t chunk_size{};
        symbolic::LitheFrontendMeta frontend;
    };

    template <typename InRange, typename OutRange, typename Fn, typename ChunkingPolicy>
    struct ParallelTransformExpr {
        InRange input;
        OutRange output;
        Fn fn;
        std::size_t chunk_size{};
        symbolic::LitheFrontendMeta frontend;
    };

    template <typename Expr>
    struct NamedExpr {
        Expr expr;
        std::string name;
        symbolic::LitheFrontendMeta frontend;
    };

    template <typename Expr>
    struct DomainExpr {
        Expr expr;
        ExecutionDomain domain;
        symbolic::LitheFrontendMeta frontend;
    };

    template <typename Expr>
    struct PriorityExpr {
        Expr expr;
        TaskPriority priority;
        symbolic::LitheFrontendMeta frontend;
    };

    template <std::size_t N, typename Expr>
    struct RetryExpr {
        Expr expr;
        symbolic::LitheFrontendMeta frontend;
    };

    template <typename Expr>
    struct TimeoutExpr {
        Expr expr;
        std::chrono::nanoseconds timeout{};
        symbolic::LitheFrontendMeta frontend;
    };

    template <typename F> requires std::move_constructible<std::decay_t<F>>
    [[nodiscard]] auto task(std::string_view name, F&& callable) {
        return TaskExpr<std::decay_t<F>>{name, std::forward<F>(callable)};
    }

    template <typename F> requires std::move_constructible<std::decay_t<F>>
    [[nodiscard]] auto task_on(ExecutionDomain domain, std::string_view name, F&& callable) {
        return TaskExpr<std::decay_t<F>>{name, std::forward<F>(callable), domain};
    }

    template <typename F> requires std::move_constructible<std::decay_t<F>>
    [[nodiscard]] auto stage(std::string_view name, F&& callable) {
        return task(name, std::forward<F>(callable));
    }

    template <typename F> requires std::move_constructible<std::decay_t<F>>
    [[nodiscard]] auto stage_on(std::string_view name, ExecutionDomain domain, F&& callable) {
        return task_on(domain, name, std::forward<F>(callable));
    }

    template <typename ChunkingPolicy = StaticChunkingPolicy, typename ReductionPolicy = NAryTreeReductionPolicy,
              typename Range, typename Init, typename MapFn, typename ReduceFn>
    [[nodiscard]] auto lazy_parallel_reduce(
        Range&& range,
        Init init,
        MapFn&& map_fn,
        ReduceFn&& reduce_fn,
        std::size_t chunk_size = 1024
    ) {
        using RangeT = std::decay_t<Range>;
        using InitT = std::decay_t<Init>;
        using MapFnT = std::decay_t<MapFn>;
        using ReduceFnT = std::decay_t<ReduceFn>;

        ParallelReduceExpr<RangeT, InitT, MapFnT, ReduceFnT, ChunkingPolicy, ReductionPolicy> expr{
            std::forward<Range>(range),
            std::move(init),
            std::forward<MapFn>(map_fn),
            std::forward<ReduceFn>(reduce_fn),
            chunk_size,
            {},
            {std::make_shared<std::optional<InitT>>(), std::make_shared<std::mutex>()}
        };

        const auto [has_size, range_size] = detail::range_size_hint(expr.range);
        expr.frontend = symbolic::lithe_frontend::make_parallel_reduce_meta(chunk_size, range_size, has_size);
        return expr;
    }

    template <typename ChunkingPolicy = StaticChunkingPolicy, typename Range, typename BodyFn>
    [[nodiscard]] auto lazy_parallel_for(Range&& range, BodyFn&& body, std::size_t chunk_size = 1024) {
        using RangeT = std::decay_t<Range>;
        using BodyT = std::decay_t<BodyFn>;

        ParallelForExpr<RangeT, BodyT, ChunkingPolicy> expr{
            std::forward<Range>(range),
            std::forward<BodyFn>(body),
            chunk_size,
            {}
        };
        const auto [has_size, range_size] = detail::range_size_hint(expr.range);
        expr.frontend = symbolic::lithe_frontend::make_parallel_for_meta(chunk_size, range_size, has_size);
        return expr;
    }

    template <typename ChunkingPolicy = StaticChunkingPolicy, typename Range, typename BodyFn>
    [[nodiscard]] auto lazy_parallel_for(std::string, Range&& range, std::size_t chunk_size, BodyFn&& body) {
        return lazy_parallel_for<ChunkingPolicy>(std::forward<Range>(range), std::forward<BodyFn>(body), chunk_size);
    }

    template <typename ChunkingPolicy = StaticChunkingPolicy, typename InRange, typename OutRange, typename Fn>
    [[nodiscard]] auto lazy_parallel_transform(InRange&& input, OutRange&& output, Fn&& fn,
                                               std::size_t chunk_size = 1024) {
        using InRangeT = std::decay_t<InRange>;
        using OutRangeT = std::decay_t<OutRange>;
        using FnT = std::decay_t<Fn>;

        ParallelTransformExpr<InRangeT, OutRangeT, FnT, ChunkingPolicy> expr{
            std::forward<InRange>(input),
            std::forward<OutRange>(output),
            std::forward<Fn>(fn),
            chunk_size,
            {}
        };
        const auto [has_size, range_size] = detail::range_size_hint(expr.input);
        expr.frontend = symbolic::lithe_frontend::make_parallel_transform_meta(chunk_size, range_size, has_size);
        return expr;
    }

    template <IsPravahaExpr L, IsPravahaExpr R>
    [[nodiscard]] auto operator|(L&& lhs, R&& rhs) {
        return SequenceExpr<std::decay_t<L>, std::decay_t<R>>{std::forward<L>(lhs), std::forward<R>(rhs)};
    }

    template <IsPravahaExpr A, IsPravahaExpr B>
    [[nodiscard]] auto seq(A&& a, B&& b) {
        return std::forward<A>(a) | std::forward<B>(b);
    }

    template <IsPravahaExpr A, IsPravahaExpr B, IsPravahaExpr C, IsPravahaExpr... Rest>
    [[nodiscard]] auto seq(A&& a, B&& b, C&& c, Rest&&... rest) {
        return seq(
            std::forward<A>(a) | std::forward<B>(b),
            std::forward<C>(c),
            std::forward<Rest>(rest)...
        );
    }

    template <IsPravahaExpr A, IsPravahaExpr B>
    [[nodiscard]] auto pipeline(A&& a, B&& b) {
        return seq(std::forward<A>(a), std::forward<B>(b));
    }

    template <IsPravahaExpr A, IsPravahaExpr B, IsPravahaExpr C, IsPravahaExpr... Rest>
    [[nodiscard]] auto pipeline(A&& a, B&& b, C&& c, Rest&&... rest) {
        return seq(
            std::forward<A>(a),
            std::forward<B>(b),
            std::forward<C>(c),
            std::forward<Rest>(rest)...
        );
    }

    template <IsPravahaExpr L, IsPravahaExpr R>
    [[nodiscard]] auto operator&(L&& lhs, R&& rhs) {
        return ParallelExpr<std::decay_t<L>, std::decay_t<R>>{std::forward<L>(lhs), std::forward<R>(rhs)};
    }

    template <IsPravahaExpr A, IsPravahaExpr B>
    [[nodiscard]] auto par(A&& a, B&& b) {
        return std::forward<A>(a) & std::forward<B>(b);
    }

    template <IsPravahaExpr A, IsPravahaExpr B, IsPravahaExpr C, IsPravahaExpr... Rest>
    [[nodiscard]] auto par(A&& a, B&& b, C&& c, Rest&&... rest) {
        return par(
            std::forward<A>(a) & std::forward<B>(b),
            std::forward<C>(c),
            std::forward<Rest>(rest)...
        );
    }

    template <IsPravahaExpr A, IsPravahaExpr B>
    [[nodiscard]] auto branch(A&& a, B&& b) {
        return par(std::forward<A>(a), std::forward<B>(b));
    }

    template <IsPravahaExpr A, IsPravahaExpr B, IsPravahaExpr C, IsPravahaExpr... Rest>
    [[nodiscard]] auto branch(A&& a, B&& b, C&& c, Rest&&... rest) {
        return par(
            std::forward<A>(a),
            std::forward<B>(b),
            std::forward<C>(c),
            std::forward<Rest>(rest)...
        );
    }

    template <IsPravahaExpr A, IsPravahaExpr B>
    [[nodiscard]] auto all_of(A&& a, B&& b) {
        return par(std::forward<A>(a), std::forward<B>(b));
    }

    template <IsPravahaExpr A, IsPravahaExpr B, IsPravahaExpr C, IsPravahaExpr... Rest>
    [[nodiscard]] auto all_of(A&& a, B&& b, C&& c, Rest&&... rest) {
        return par(
            std::forward<A>(a),
            std::forward<B>(b),
            std::forward<C>(c),
            std::forward<Rest>(rest)...
        );
    }

    template <typename L, typename R>
    [[nodiscard]] auto collect_all(ParallelExpr<L, R> expr) {
        expr.policy = JoinPolicy{JoinPolicyKind::CollectAll, 0};
        expr.frontend = symbolic::lithe_frontend::make_collect_all_meta(expr.frontend.hash);
        return expr;
    }

    template <IsPravahaExpr A, IsPravahaExpr B>
    [[nodiscard]] auto collect_all_of(A&& a, B&& b) {
        return collect_all(par(std::forward<A>(a), std::forward<B>(b)));
    }

    template <IsPravahaExpr A, IsPravahaExpr B, IsPravahaExpr C, IsPravahaExpr... Rest>
    [[nodiscard]] auto collect_all_of(A&& a, B&& b, C&& c, Rest&&... rest) {
        return collect_all(par(
            std::forward<A>(a),
            std::forward<B>(b),
            std::forward<C>(c),
            std::forward<Rest>(rest)...
        ));
    }


    template <typename L, typename R>
    [[nodiscard]] auto any_success(ParallelExpr<L, R> expr) {
        expr.policy = JoinPolicy{JoinPolicyKind::AnySuccess, 0};
        expr.frontend = symbolic::lithe_frontend::make_any_success_meta(expr.frontend.hash);
        return expr;
    }

    template <IsPravahaExpr A, IsPravahaExpr B>
    [[nodiscard]] auto any_success_of(A&& a, B&& b) {
        return any_success(par(std::forward<A>(a), std::forward<B>(b)));
    }

    template <IsPravahaExpr A, IsPravahaExpr B, IsPravahaExpr C, IsPravahaExpr... Rest>
    [[nodiscard]] auto any_success_of(A&& a, B&& b, C&& c, Rest&&... rest) {
        return any_success(par(
            std::forward<A>(a),
            std::forward<B>(b),
            std::forward<C>(c),
            std::forward<Rest>(rest)...
        ));
    }

    template <std::size_t N, typename L, typename R>
    [[nodiscard]] auto quorum(ParallelExpr<L, R> expr) {
        static_assert(N > 0);
        expr.policy = JoinPolicy{JoinPolicyKind::Quorum, N};
        expr.frontend = symbolic::lithe_frontend::make_quorum_meta(expr.frontend.hash, N);
        return expr;
    }

    template <std::size_t N, IsPravahaExpr A, IsPravahaExpr B>
    [[nodiscard]] auto quorum_of(A&& a, B&& b) {
        static_assert(N > 0);
        return quorum<N>(par(std::forward<A>(a), std::forward<B>(b)));
    }

    template <std::size_t N, IsPravahaExpr A, IsPravahaExpr B, IsPravahaExpr C, IsPravahaExpr... Rest>
    [[nodiscard]] auto quorum_of(A&& a, B&& b, C&& c, Rest&&... rest) {
        static_assert(N > 0);
        return quorum<N>(par(
            std::forward<A>(a),
            std::forward<B>(b),
            std::forward<C>(c),
            std::forward<Rest>(rest)...
        ));
    }

    template <IsPravahaExpr Expr>
    [[nodiscard]] auto with_name(std::string name, Expr&& expr) {
        using ExprT = std::decay_t<Expr>;
        ExprT wrapped = std::forward<Expr>(expr);
        auto frontend = symbolic::lithe_frontend::make_named_meta(wrapped.frontend.hash, name);
        return NamedExpr<ExprT>{std::move(wrapped), std::move(name), std::move(frontend)};
    }

    template <IsPravahaExpr Expr>
    [[nodiscard]] auto on_domain(ExecutionDomain domain, Expr&& expr) {
        using ExprT = std::decay_t<Expr>;
        ExprT wrapped = std::forward<Expr>(expr);
        auto frontend = symbolic::lithe_frontend::make_domain_meta(wrapped.frontend.hash, domain);
        return DomainExpr<ExprT>{std::move(wrapped), domain, std::move(frontend)};
    }

    template <IsPravahaExpr Expr>
    [[nodiscard]] auto with_priority(TaskPriority priority, Expr&& expr) {
        using ExprT = std::decay_t<Expr>;
        ExprT wrapped = std::forward<Expr>(expr);
        auto frontend = symbolic::lithe_frontend::make_priority_meta(wrapped.frontend.hash, priority);
        return PriorityExpr<ExprT>{std::move(wrapped), priority, std::move(frontend)};
    }

    template <std::size_t N, IsPravahaExpr Expr>
    [[nodiscard]] auto with_retry(Expr&& expr) {
        using ExprT = std::decay_t<Expr>;
        ExprT wrapped = std::forward<Expr>(expr);
        auto frontend = symbolic::lithe_frontend::make_retry_meta(wrapped.frontend.hash, N);
        return RetryExpr<N, ExprT>{std::move(wrapped), std::move(frontend)};
    }

    template <IsPravahaExpr Expr>
    [[nodiscard]] auto with_timeout(std::chrono::nanoseconds timeout, Expr&& expr) {
        using ExprT = std::decay_t<Expr>;
        ExprT wrapped = std::forward<Expr>(expr);
        auto frontend = symbolic::lithe_frontend::make_timeout_meta(wrapped.frontend.hash, timeout);
        return TimeoutExpr<ExprT>{std::move(wrapped), timeout, std::move(frontend)};
    }

    template <class Rep, class Period, IsPravahaExpr Expr>
    [[nodiscard]] auto with_timeout(std::chrono::duration<Rep, Period> timeout, Expr&& expr) {
        return with_timeout(std::chrono::duration_cast<std::chrono::nanoseconds>(timeout), std::forward<Expr>(expr));
    }


    // ============================================================================
    //  SECTION 7: TASK IR
    // ============================================================================
    // LitheSymbolicSource is the stable frontend identity boundary.
    // TaskIr remains execution IR.
    // LiteGraph remains causality graph.
    // Lithe does not own execution.

    struct TaskId {
        std::size_t value;
        static constexpr std::size_t invalid_value = ~std::size_t{0};

        constexpr TaskId() noexcept : value{invalid_value} {}

        constexpr explicit TaskId(const std::size_t v) noexcept : value{v} {}

        [[nodiscard]] constexpr bool is_valid() const noexcept { return value != invalid_value; }

        constexpr bool operator==(const TaskId&) const noexcept = default;

        constexpr auto operator<=>(const TaskId&) const noexcept = default;
    };

    inline constexpr TaskId invalid_task_id{};

    enum class EventKind {
        TaskReady,
        TaskScheduled,
        TaskStarted,
        TaskCompleted,
        PayloadForwarded,
        TaskFailed,
        TaskSkipped,
        TaskCanceled,
        JoinResolved,
        GraphLowered,
        GraphValidated
    };

    struct TaskEvent {
        EventKind kind{};
        TaskId task_id{};
        std::string_view task_name{};
        TaskState state{};
        std::size_t frontend_hash{};
        std::uint64_t timestamp_ns{};
        TaskId from_task_id{};
        TaskId to_task_id{};
        std::size_t payload_type_hash{};
    };

    struct JoinEvent {
        EventKind kind{EventKind::JoinResolved};
        std::size_t group_id{};
        JoinPolicy policy{};
        bool success{};
        std::size_t expected{};
        std::size_t succeeded{};
        std::size_t failed{};
        std::size_t canceled{};
        std::size_t skipped{};
        std::uint64_t timestamp_ns{};
    };

    struct GraphEvent {
        EventKind kind{};
        std::size_t node_count{};
        std::size_t edge_count{};
        std::size_t join_group_count{};
        std::uint64_t timestamp_ns{};
    };

    struct NoObserver {
        static constexpr bool enabled = false;

        static void on_task_event(const TaskEvent&) noexcept {}

        static void on_join_event(const JoinEvent&) noexcept {}

        static void on_graph_event(const GraphEvent&) noexcept {}
    };

    template <class T>
    concept RetryPolicy = requires(const PravahaError& error, std::size_t attempt_count, std::size_t max_retries) {
        { T::on_failure(error, attempt_count, max_retries) } -> std::same_as<RetryDecision>;
    };

    struct NoRetryPolicy {
        static RetryDecision on_failure(const PravahaError&, std::size_t, std::size_t) noexcept {
            return RetryDecision::FailFinal;
        }
    };

    template <class T>
    concept TimeoutPolicy = requires(std::chrono::nanoseconds timeout) {
        { T::on_timeout(timeout) } -> std::same_as<bool>;
    };

    struct CooperativeTimeoutPolicy {
        static bool on_timeout(std::chrono::nanoseconds) noexcept {
            return true;
        }
    };

    struct NoTimeoutPolicy {
        static bool on_timeout(std::chrono::nanoseconds) noexcept {
            return false;
        }
    };

    enum class SubmitDecision {
        Accept,
        Reject,
        WouldBlock
    };

    template <class T>
    concept FlowControlPolicy = requires(const PravahaError& error) {
        { T::on_submit_rejected(error) } -> std::same_as<SubmitDecision>;
    };

    struct RejectOnFullPolicy {
        static SubmitDecision on_submit_rejected(const PravahaError&) noexcept {
            return SubmitDecision::Reject;
        }
    };

    // ============================================================================
    //  BUDGET POLICY — fuel-limited execution
    // ============================================================================

    // NoBudgetPolicy: zero-overhead sentinel; all budget branches are dead-code.
    struct NoBudgetPolicy {
        [[nodiscard]] static constexpr bool fuel_exhausted() noexcept { return false; }
    };

    // BudgetPolicy: minimal concept used by Runner — only requires fuel_exhausted().
    // FullBudgetPolicy (sandbox field check) is defined inside
    // PRAVAHA_LITHE_RUNTIME_INTEGRATION because it references lithe types.
    template <class T>
    concept BudgetPolicy = requires(T p) {
        { p.fuel_exhausted() } -> std::same_as<bool>;
    };

    template <class T>
    concept ObserverPolicy =
        requires(T t, const TaskEvent& te, const JoinEvent& je, const GraphEvent& ge) {
            { T::enabled } -> std::convertible_to<const bool&>;
            { t.on_task_event(te) } noexcept;
            { t.on_join_event(je) } noexcept;
            { t.on_graph_event(ge) } noexcept;
        };

    enum class EdgeKind { Sequence, Data, Cancellation, Join };

    struct GraphNodePayload {
        TaskId task_id;
        std::string name;
        bool operator==(const GraphNodePayload& o) const noexcept { return task_id == o.task_id && name == o.name; }
    };

    struct GraphEdgePayload {
        EdgeKind kind;
        bool operator==(const GraphEdgePayload& o) const noexcept { return kind == o.kind; }
    };
} // namespace pravaha

namespace std {
    template <>
    struct hash<pravaha::GraphNodePayload> {
        std::size_t operator()(const pravaha::GraphNodePayload& p) const noexcept {
            return std::hash<std::size_t>{}(p.task_id.value) ^ (std::hash<std::string>{}(p.name) << 1);
        }
    };

    template <>
    struct hash<pravaha::GraphEdgePayload> {
        std::size_t operator()(const pravaha::GraphEdgePayload& p) const noexcept {
            return std::hash<int>{}(static_cast<int>(p.kind));
        }
    };
} // namespace std

namespace pravaha {
    using ExecutionGraph = litegraph::Graph<GraphNodePayload, GraphEdgePayload, litegraph::Directed>;

    struct PayloadMeta {
        bool output_checked{false};
        bool output_transferable{false};
        bool output_serializable{false};
        std::string output_type_name;
    };

    struct TypeContract {
        bool checked{};
        std::size_t type_hash{};
        std::string type_name;
        bool vector_checked{};
        std::size_t vector_type_hash{};
        std::string vector_type_name;
    };

    template <class T>
    TypeContract make_type_contract() {
        using Normalized = std::remove_cvref_t<T>;
        TypeContract contract;
        contract.checked = true;
        contract.type_hash = runtime_type_hash<Normalized>();
        if constexpr (requires { meta::type_name<Normalized>(); }) {
            contract.type_name = std::string(meta::type_name<Normalized>());
        }
        else {
            contract.type_name = typeid(Normalized).name();
        }
        if constexpr (std::copy_constructible<Normalized>) {
            using Vectorized = std::vector<Normalized>;
            contract.vector_checked = true;
            contract.vector_type_hash = runtime_type_hash<Vectorized>();
            if constexpr (requires { meta::type_name<Vectorized>(); }) {
                contract.vector_type_name = std::string(meta::type_name<Vectorized>());
            }
            else {
                contract.vector_type_name = typeid(Vectorized).name();
            }
        }
        return contract;
    }

    struct IrNode {
        TaskId id;
        std::string name;
        ExecutionDomain domain{ExecutionDomain::CPU};
        TaskState state{TaskState::Created};
        TaskCommand command;
        TaskPriority priority{TaskPriority::Normal};
        PayloadMeta payload_meta;
        TypeContract input_contract;
        TypeContract output_contract;
        std::size_t max_retries{0};
        std::chrono::nanoseconds timeout{0};
        std::size_t frontend_hash{};
        std::string frontend_dump;

        IrNode() = default;

        IrNode(const TaskId id_, std::string name_, const ExecutionDomain dom_, TaskCommand cmd_)
            : id{id_}, name{std::move(name_)}, domain{dom_}, state{TaskState::Created}, command{std::move(cmd_)} {}
    };

    struct IrEdge {
        TaskId from;
        TaskId to;
        EdgeKind kind;

        IrEdge() = default;

        IrEdge(const TaskId f, const TaskId t, const EdgeKind k) noexcept : from{f}, to{t}, kind{k} {}
    };

    struct IrJoinGroup {
        std::vector<TaskId> members;
        JoinPolicy policy{};
        std::size_t frontend_hash{};
        std::string frontend_dump;
    };

    struct TaskIr {
        std::vector<IrNode> nodes;
        std::vector<IrEdge> edges;
        std::vector<IrJoinGroup> join_groups;
        mutable std::optional<ExecutionGraph> cached_graph;
        mutable std::optional<bool> cached_graph_has_cycle;

        void invalidate_graph_cache() const noexcept {
            cached_graph.reset();
            cached_graph_has_cycle.reset();
        }

        Outcome<const ExecutionGraph*> dependency_graph() const {
            if (cached_graph.has_value()) {
                return &cached_graph.value();
            }

            ExecutionGraph dep_graph;
            dep_graph.reserve_nodes(nodes.size());

            std::unordered_map<std::size_t, litegraph::NodeId> id_map;
            id_map.reserve(nodes.size());

            for (const auto& node : nodes) {
                const litegraph::NodeId nid = dep_graph.add_node(GraphNodePayload{node.id, node.name});
                id_map[node.id.value] = nid;
            }

            for (const auto& edge : edges) {
                if (edge.kind != EdgeKind::Sequence && edge.kind != EdgeKind::Data) {
                    continue;
                }
                auto from_it = id_map.find(edge.from.value);
                auto to_it = id_map.find(edge.to.value);
                if (from_it == id_map.end() || to_it == id_map.end()) {
                    return std::unexpected(PravahaError{
                        ErrorKind::ValidationError,
                        "Invalid edge endpoint: TaskId not found in IR nodes"
                    });
                }
                dep_graph.add_edge(from_it->second, to_it->second, GraphEdgePayload{edge.kind});
            }

            cached_graph = std::move(dep_graph);
            return &cached_graph.value();
        }

        Outcome<bool> dependency_graph_has_cycle() const {
            if (cached_graph_has_cycle.has_value()) {
                return cached_graph_has_cycle.value();
            }
            auto graph_result = dependency_graph();
            if (!graph_result.has_value()) {
                return std::unexpected(graph_result.error());
            }
            cached_graph_has_cycle = litegraph::has_cycle(*graph_result.value());
            return cached_graph_has_cycle.value();
        }

        TaskId add_node(std::string name, ExecutionDomain domain, TaskCommand cmd,
                        const std::size_t frontend_hash = 0, std::string frontend_dump = {}) {
            invalidate_graph_cache();
            TaskId id{nodes.size()};
            nodes.emplace_back(id, std::move(name), domain, std::move(cmd));
            nodes.back().frontend_hash = frontend_hash;
            nodes.back().frontend_dump = std::move(frontend_dump);
            return id;
        }

        void add_edge(TaskId from, TaskId to, EdgeKind kind) {
            invalidate_graph_cache();
            edges.emplace_back(from, to, kind);
        }

        void add_join_group(std::vector<TaskId> members, const JoinPolicy policy,
                            const std::size_t frontend_hash = 0, std::string frontend_dump = {}) {
            invalidate_graph_cache();
            join_groups.push_back(IrJoinGroup{std::move(members), policy, frontend_hash, std::move(frontend_dump)});
        }

        [[nodiscard]] std::size_t node_count() const noexcept { return nodes.size(); }
        [[nodiscard]] std::size_t edge_count() const noexcept { return edges.size(); }
    };

    struct NAryTreeReductionPolicy {
        template <typename T, typename ReduceFn>
        static TaskId lower(
            const std::vector<TaskId>& chunk_node_ids,
            std::shared_ptr<ReduceFn> reduce_fn,
            std::shared_ptr<std::vector<T>> values,
            std::shared_ptr<std::optional<T>> out,
            std::shared_ptr<std::mutex> out_mutex,
            TaskIr& ir,
            const std::size_t frontend_hash,
            const std::string& frontend_dump
        ) {
            std::vector<TaskId> slot_node(values->size(), invalid_task_id);
            for (std::size_t i = 0; i < chunk_node_ids.size(); ++i) {
                slot_node[i] = chunk_node_ids[i];
            }

            std::vector<std::size_t> level_slots;
            level_slots.reserve(chunk_node_ids.size());
            for (std::size_t i = 0; i < chunk_node_ids.size(); ++i) {
                level_slots.push_back(i);
            }

            NAryTree<std::size_t> shape_tree(0);
            auto* root = shape_tree.get_root();
            for (std::size_t i = 0; i < chunk_node_ids.size(); ++i) {
                shape_tree.insert(root, i);
            }

            std::size_t next_slot = chunk_node_ids.size();
            while (level_slots.size() > 1) {
                std::vector<std::size_t> next_level;
                next_level.reserve((level_slots.size() + 1) / 2);
                for (std::size_t i = 0; i + 1 < level_slots.size(); i += 2) {
                    const std::size_t left_slot = level_slots[i];
                    const std::size_t right_slot = level_slots[i + 1];
                    const std::size_t parent_slot = next_slot++;

                    auto reduce_cmd = TaskCommand::make([left_slot, right_slot, parent_slot, values, reduce_fn]() {
                        (*values)[parent_slot] = std::invoke(*reduce_fn, (*values)[left_slot], (*values)[right_slot]);
                    });
                    const TaskId reduce_id = ir.add_node(
                        "parallel_reduce.reduce." + std::to_string(parent_slot),
                        ExecutionDomain::CPU,
                        std::move(reduce_cmd),
                        frontend_hash,
                        frontend_dump
                    );
                    ir.add_edge(slot_node[left_slot], reduce_id, EdgeKind::Sequence);
                    ir.add_edge(slot_node[right_slot], reduce_id, EdgeKind::Sequence);
                    slot_node[parent_slot] = reduce_id;
                    next_level.push_back(parent_slot);

                    auto* parent = shape_tree.insert(root, parent_slot);
                    shape_tree.insert(parent, left_slot);
                    shape_tree.insert(parent, right_slot);
                }
                if (level_slots.size() % 2 == 1) {
                    next_level.push_back(level_slots.back());
                }
                level_slots = std::move(next_level);
            }

            const std::size_t root_slot = level_slots.empty() ? 0 : level_slots.front();
            auto final_cmd = TaskCommand::make([root_slot, values, out, out_mutex]() {
                if (out && out_mutex) {
                    std::lock_guard lock(*out_mutex);
                    *out = (*values)[root_slot];
                }
            });
            const TaskId final_id = ir.add_node(
                "parallel_reduce.final",
                ExecutionDomain::CPU,
                std::move(final_cmd),
                frontend_hash,
                frontend_dump
            );
            if (!level_slots.empty() && slot_node[root_slot].is_valid()) {
                ir.add_edge(slot_node[root_slot], final_id, EdgeKind::Sequence);
            }
            return final_id;
        }
    };

    struct FlatReductionPolicy {
        template <typename T, typename ReduceFn>
        static TaskId lower(
            const std::vector<TaskId>& chunk_node_ids,
            std::shared_ptr<ReduceFn> reduce_fn,
            std::shared_ptr<std::vector<T>> values,
            std::shared_ptr<std::optional<T>> out,
            std::shared_ptr<std::mutex> out_mutex,
            TaskIr& ir,
            const std::size_t frontend_hash,
            const std::string& frontend_dump
        ) {
            const std::size_t chunk_count = chunk_node_ids.size();
            auto final_cmd = TaskCommand::make([chunk_count, values, reduce_fn, out, out_mutex]() {
                if (chunk_count == 0) {
                    return;
                }
                T combined = (*values)[0];
                for (std::size_t i = 1; i < chunk_count; ++i) {
                    combined = std::invoke(*reduce_fn, std::move(combined), (*values)[i]);
                }
                if (out && out_mutex) {
                    std::lock_guard lock(*out_mutex);
                    *out = std::move(combined);
                }
            });
            const TaskId final_id = ir.add_node(
                "parallel_reduce.final",
                ExecutionDomain::CPU,
                std::move(final_cmd),
                frontend_hash,
                frontend_dump
            );
            for (const auto chunk_id : chunk_node_ids) {
                ir.add_edge(chunk_id, final_id, EdgeKind::Sequence);
            }
            return final_id;
        }
    };

    // ============================================================================
    //  SECTION 7.5: LOWERING
    // ============================================================================

    namespace detail {
        inline TaskId add_node_from_source(TaskIr& ir, const symbolic::LitheSymbolicSource& source,
                                           const ExecutionDomain domain, TaskCommand cmd) {
            return ir.add_node(source.debug_name, domain, std::move(cmd), source.frontend.hash, source.frontend.dump);
        }

        template <class T>
        struct unwrap_outcome {
            using type = T;
        };

        template <class T>
        struct unwrap_outcome<Outcome<T>> {
            using type = T;
        };

        template <class T>
        using unwrap_outcome_t = unwrap_outcome<std::remove_cvref_t<T>>::type;


        template <class F>
        constexpr bool has_single_input_v = (callable_traits<std::remove_cvref_t<F>>::arity == 1);

        template <class F>
        TypeContract make_input_contract() {
            if constexpr (has_single_input_v<F>) {
                using Arg = callable_traits<std::remove_cvref_t<F>>::template arg_t<0>;
                return make_type_contract<Arg>();
            }
            return TypeContract{};
        }

        template <class F>
        using callable_result_t = callable_traits<std::remove_cvref_t<F>>::result_type;

        template <class F>
        using callable_payload_t = std::conditional_t<
            std::is_void_v<callable_result_t<F>>,
            Unit,
            unwrap_outcome_t<callable_result_t<F>>
        >;

        struct LowerResult {
            std::vector<TaskId> starts;
            std::vector<TaskId> terminals;
            TaskIr ir;
        };

        inline LowerResult merge_into(LowerResult& dst, LowerResult src) {
            const std::size_t offset = dst.ir.nodes.size();
            for (auto& node : src.ir.nodes) {
                node.id = TaskId{node.id.value + offset};
                dst.ir.nodes.push_back(std::move(node));
            }
            for (auto& edge : src.ir.edges) {
                edge.from = TaskId{edge.from.value + offset};
                edge.to = TaskId{edge.to.value + offset};
                dst.ir.edges.push_back(std::move(edge));
            }
            for (auto& jg : src.ir.join_groups) {
                for (auto& m : jg.members) m = TaskId{m.value + offset};
                dst.ir.join_groups.push_back(std::move(jg));
            }
            std::vector<TaskId> rs;
            rs.reserve(src.starts.size());
            for (const auto id : src.starts) rs.emplace_back(id.value + offset);
            std::vector<TaskId> rt;
            rt.reserve(src.terminals.size());
            for (const auto id : src.terminals) rt.emplace_back(id.value + offset);
            return LowerResult{std::move(rs), std::move(rt), {}};
        }

        template <typename F>
        LowerResult lower_impl(TaskExpr<F> expr);

        template <typename L, typename R>
        LowerResult lower_impl(SequenceExpr<L, R> expr);

        template <typename L, typename R>
        LowerResult lower_impl(ParallelExpr<L, R> expr);

        template <typename Expr>
        LowerResult lower_impl(NamedExpr<Expr> expr);

        template <typename Expr>
        LowerResult lower_impl(DomainExpr<Expr> expr);

        template <typename Expr>
        LowerResult lower_impl(PriorityExpr<Expr> expr);

        template <std::size_t N, typename Expr>
        LowerResult lower_impl(RetryExpr<N, Expr> expr);

        template <typename Expr>
        LowerResult lower_impl(TimeoutExpr<Expr> expr);

        template <typename Range, typename Init, typename MapFn, typename ReduceFn, typename ChunkingPolicy, typename
                  ReductionPolicy>
        LowerResult lower_impl(ParallelReduceExpr<Range, Init, MapFn, ReduceFn, ChunkingPolicy, ReductionPolicy> expr);

        template <typename Range, typename BodyFn, typename ChunkingPolicy>
        LowerResult lower_impl(ParallelForExpr<Range, BodyFn, ChunkingPolicy> expr);

        template <typename InRange, typename OutRange, typename Fn, typename ChunkingPolicy>
        LowerResult lower_impl(ParallelTransformExpr<InRange, OutRange, Fn, ChunkingPolicy> expr);

        template <typename T>
        PayloadMeta make_payload_meta() {
            using LogicalT = unwrap_outcome_t<T>;
            PayloadMeta pm;
            pm.output_checked = true;
            if constexpr (std::is_trivially_copyable_v<LogicalT>&& std::is_standard_layout_v<LogicalT>) {
                if constexpr (meta::Reflectable<LogicalT>) {
                    pm.output_transferable = meta::is_binary_stable<LogicalT>();
                    pm.output_serializable = meta::is_zero_copy_serializable<LogicalT>();
                }
                else {
                    pm.output_transferable = true;
                    pm.output_serializable = true;
                }
            }
            else {
                pm.output_transferable = false;
                pm.output_serializable = false;
            }
            pm.output_type_name = std::string(meta::type_name<LogicalT>());
            return pm;
        }

        template <typename F>
        LowerResult lower_impl(TaskExpr<F> expr) {
            LowerResult result;
            const symbolic::LitheSymbolicSource source{expr.frontend, expr.name()};
            TaskCommand cmd = TaskCommand::make(std::move(expr.callable()), expr.name());
            TaskId id = add_node_from_source(result.ir, source, expr.domain(), std::move(cmd));
            using OutputT = callable_payload_t<F>;
            result.ir.nodes.back().payload_meta = make_payload_meta<OutputT>();
            result.ir.nodes.back().output_contract = make_type_contract<OutputT>();
            result.ir.nodes.back().input_contract = make_input_contract<F>();
            result.starts.push_back(id);
            result.terminals.push_back(id);
            return result;
        }

        template <typename L, typename R>
        LowerResult lower_impl(SequenceExpr<L, R> expr) {
            const symbolic::LitheSymbolicSource group_source{expr.frontend, "dsl.sequence"};
            (void)group_source;
            auto left_result = lower_impl(std::move(expr.left));
            auto right_result = lower_impl(std::move(expr.right));
            auto right_remap = merge_into(left_result, std::move(right_result));
            for (auto t : left_result.terminals)
                for (auto s : right_remap.starts)
                    left_result.ir.add_edge(t, s, EdgeKind::Sequence);
            left_result.terminals = std::move(right_remap.terminals);
            return left_result;
        }

        template <typename L, typename R>
        LowerResult lower_impl(ParallelExpr<L, R> expr) {
            const symbolic::LitheSymbolicSource group_source{expr.frontend, "dsl.parallel"};
            (void)group_source;
            auto left_result = lower_impl(std::move(expr.left));
            auto right_result = lower_impl(std::move(expr.right));
            // Capture left terminals before merge (they are the left branch members)
            const std::vector<TaskId> left_terminals = left_result.terminals;
            auto right_remap = merge_into(left_result, std::move(right_result));
            for (auto s : right_remap.starts) left_result.starts.push_back(s);
            for (auto t : right_remap.terminals) left_result.terminals.push_back(t);
            std::vector<TaskId> members;
            members.reserve(left_terminals.size());
            for (auto t : left_terminals) members.push_back(t);
            for (auto t : right_remap.terminals) members.push_back(t);
            left_result.ir.add_join_group(std::move(members), expr.policy, expr.frontend.hash, expr.frontend.dump);
            return left_result;
        }

        template <typename Expr>
        LowerResult lower_impl(NamedExpr<Expr> expr) {
            auto result = lower_impl(std::move(expr.expr));
            if (result.ir.nodes.size() == 1) {
                result.ir.nodes.front().name = std::move(expr.name);
            }
            return result;
        }

        template <typename Expr>
        LowerResult lower_impl(DomainExpr<Expr> expr) {
            auto result = lower_impl(std::move(expr.expr));
            std::vector<bool> updated(result.ir.nodes.size(), false);
            for (auto id : result.starts) {
                if (id.value < result.ir.nodes.size()) {
                    result.ir.nodes[id.value].domain = expr.domain;
                    updated[id.value] = true;
                }
            }
            for (auto id : result.terminals) {
                if (id.value < result.ir.nodes.size() && !updated[id.value]) {
                    result.ir.nodes[id.value].domain = expr.domain;
                }
            }
            return result;
        }

        template <typename Expr>
        LowerResult lower_impl(PriorityExpr<Expr> expr) {
            auto result = lower_impl(std::move(expr.expr));
            std::vector<bool> updated(result.ir.nodes.size(), false);
            for (auto id : result.starts) {
                if (id.value < result.ir.nodes.size()) {
                    result.ir.nodes[id.value].priority = expr.priority;
                    updated[id.value] = true;
                }
            }
            for (auto id : result.terminals) {
                if (id.value < result.ir.nodes.size() && !updated[id.value]) {
                    result.ir.nodes[id.value].priority = expr.priority;
                }
            }
            return result;
        }

        template <std::size_t N, typename Expr>
        LowerResult lower_impl(RetryExpr<N, Expr> expr) {
            auto result = lower_impl(std::move(expr.expr));
            std::vector<bool> updated(result.ir.nodes.size(), false);
            for (auto id : result.starts) {
                if (id.value < result.ir.nodes.size()) {
                    result.ir.nodes[id.value].max_retries = N;
                    updated[id.value] = true;
                }
            }
            for (auto id : result.terminals) {
                if (id.value < result.ir.nodes.size() && !updated[id.value]) {
                    result.ir.nodes[id.value].max_retries = N;
                }
            }
            return result;
        }

        template <typename Expr>
        LowerResult lower_impl(TimeoutExpr<Expr> expr) {
            auto result = lower_impl(std::move(expr.expr));
            std::vector<bool> updated(result.ir.nodes.size(), false);
            for (auto id : result.starts) {
                if (id.value < result.ir.nodes.size()) {
                    result.ir.nodes[id.value].timeout = expr.timeout;
                    updated[id.value] = true;
                }
            }
            for (auto id : result.terminals) {
                if (id.value < result.ir.nodes.size() && !updated[id.value]) {
                    result.ir.nodes[id.value].timeout = expr.timeout;
                }
            }
            return result;
        }

        template <typename Range, typename Init, typename MapFn, typename ReduceFn, typename ChunkingPolicy, typename
                  ReductionPolicy>
        LowerResult lower_impl(ParallelReduceExpr<Range, Init, MapFn, ReduceFn, ChunkingPolicy, ReductionPolicy> expr) {
            static_assert(requires(const Range& r) { r.size(); r[std::size_t{}]; });

            LowerResult result;
            const auto total = static_cast<std::size_t>(expr.range.size());
            const auto chunk_ranges = ChunkingPolicy::chunks(total, expr.chunk_size);

            auto range_ptr = std::make_shared<Range>(std::move(expr.range));
            auto map_ptr = std::make_shared<MapFn>(std::move(expr.map_fn));
            auto reduce_ptr = std::make_shared<ReduceFn>(std::move(expr.reduce_fn));
            const Init init_value = expr.init;

            if (total == 0) {
                auto values = std::make_shared<std::vector<Init>>(1, init_value);
                auto out = expr.result_handle.value;
                auto out_mutex = expr.result_handle.mutex;
                auto final_cmd = TaskCommand::make([values, out, out_mutex]() {
                    if (out && out_mutex) {
                        std::lock_guard<std::mutex> lock(*out_mutex);
                        *out = (*values)[0];
                    }
                });
                const TaskId final_id = result.ir.add_node(
                    "parallel_reduce.final",
                    ExecutionDomain::CPU,
                    std::move(final_cmd),
                    expr.frontend.hash,
                    expr.frontend.dump
                );
                result.starts.push_back(final_id);
                result.terminals.push_back(final_id);
                return result;
            }

            const std::size_t num_chunks = chunk_ranges.size();
            auto values = std::make_shared<std::vector<Init>>(std::max<std::size_t>(1, 2 * num_chunks), init_value);
            std::vector<TaskId> chunk_node_ids;
            chunk_node_ids.reserve(num_chunks);

            for (std::size_t i = 0; i < num_chunks; ++i) {
                const std::size_t begin = chunk_ranges[i].begin;
                const std::size_t end = chunk_ranges[i].end;
                auto chunk_cmd = TaskCommand::make(
                    [i, begin, end, values, range_ptr, map_ptr, reduce_ptr, init_value]() {
                        Init partial = init_value;
                        for (std::size_t idx = begin; idx < end; ++idx) {
                            partial = std::invoke(*reduce_ptr, std::move(partial),
                                                  std::invoke(*map_ptr, (*range_ptr)[idx]));
                        }
                        (*values)[i] = std::move(partial);
                    });
                TaskId chunk_id = result.ir.add_node(
                    "parallel_reduce.chunk." + std::to_string(i),
                    ExecutionDomain::CPU,
                    std::move(chunk_cmd),
                    expr.frontend.hash,
                    expr.frontend.dump
                );
                result.starts.push_back(chunk_id);
                chunk_node_ids.push_back(chunk_id);
            }

            const TaskId final_id = ReductionPolicy::template lower<Init, ReduceFn>(
                chunk_node_ids,
                reduce_ptr,
                values,
                expr.result_handle.value,
                expr.result_handle.mutex,
                result.ir,
                expr.frontend.hash,
                expr.frontend.dump
            );
            result.terminals.push_back(final_id);
            return result;
        }

        template <typename Range, typename BodyFn, typename ChunkingPolicy>
        LowerResult lower_impl(ParallelForExpr<Range, BodyFn, ChunkingPolicy> expr) {
            static_assert(std::ranges::random_access_range<Range>);
            static_assert(std::ranges::sized_range<Range>);
            static_assert(std::invocable<BodyFn&, decltype(std::declval<Range&>()[std::size_t{}])>);

            LowerResult result;
            const auto total = static_cast<std::size_t>(expr.range.size());
            const auto chunk_ranges = ChunkingPolicy::chunks(total, expr.chunk_size);
            const std::size_t num_chunks = chunk_ranges.size();

            auto range_ptr = std::make_shared<Range>(std::move(expr.range));
            auto body_ptr = std::make_shared<BodyFn>(std::move(expr.fn));
            if (num_chunks == 0) {
                auto cmd = TaskCommand::make([]() {});
                const TaskId id = result.ir.add_node(
                    "parallel_for.empty",
                    ExecutionDomain::CPU,
                    std::move(cmd),
                    expr.frontend.hash,
                    expr.frontend.dump
                );
                result.starts.push_back(id);
                result.terminals.push_back(id);
                return result;
            }

            for (std::size_t i = 0; i < num_chunks; ++i) {
                const std::size_t begin = chunk_ranges[i].begin;
                const std::size_t end = chunk_ranges[i].end;
                auto cmd = TaskCommand::make([range_ptr, body_ptr, begin, end]() {
                    for (std::size_t idx = begin; idx < end; ++idx) {
                        std::invoke(*body_ptr, (*range_ptr)[idx]);
                    }
                });
                TaskId id = result.ir.add_node(
                    "parallel_for.chunk." + std::to_string(i),
                    ExecutionDomain::CPU,
                    std::move(cmd),
                    expr.frontend.hash,
                    expr.frontend.dump
                );
                result.starts.push_back(id);
                result.terminals.push_back(id);
            }
            return result;
        }

        template <typename InRange, typename OutRange, typename Fn, typename ChunkingPolicy>
        LowerResult lower_impl(ParallelTransformExpr<InRange, OutRange, Fn, ChunkingPolicy> expr) {
            static_assert(std::ranges::random_access_range<InRange>);
            static_assert(std::ranges::sized_range<InRange>);
            static_assert(std::ranges::random_access_range<OutRange>);
            static_assert(std::invocable<Fn&, std::ranges::range_reference_t<InRange>>);
            static_assert(std::assignable_from<
                std::ranges::range_reference_t<OutRange>,
                std::invoke_result_t<Fn&, std::ranges::range_reference_t<InRange>>
            >);

            LowerResult result;
            const auto total = static_cast<std::size_t>(expr.input.size());
            const auto chunk_ranges = ChunkingPolicy::chunks(total, expr.chunk_size);
            const std::size_t num_chunks = chunk_ranges.size();

            if constexpr (std::ranges::sized_range<OutRange>) {
                const auto output_total = static_cast<std::size_t>(expr.output.size());
                if (output_total < total) {
                    auto cmd = TaskCommand::make([]() {
                        throw std::runtime_error("parallel_transform output range smaller than input range");
                    });
                    const TaskId id = result.ir.add_node(
                        "parallel_transform.invalid_output_size",
                        ExecutionDomain::CPU,
                        std::move(cmd),
                        expr.frontend.hash,
                        expr.frontend.dump
                    );
                    result.starts.push_back(id);
                    result.terminals.push_back(id);
                    return result;
                }
            }

            auto input_ptr = std::make_shared<InRange>(std::move(expr.input));
            auto output_ptr = std::make_shared<OutRange>(std::move(expr.output));
            auto fn_ptr = std::make_shared<Fn>(std::move(expr.fn));
            if (num_chunks == 0) {
                auto cmd = TaskCommand::make([]() {});
                const TaskId id = result.ir.add_node(
                    "parallel_transform.empty",
                    ExecutionDomain::CPU,
                    std::move(cmd),
                    expr.frontend.hash,
                    expr.frontend.dump
                );
                result.starts.push_back(id);
                result.terminals.push_back(id);
                return result;
            }

            for (std::size_t i = 0; i < num_chunks; ++i) {
                const std::size_t begin = chunk_ranges[i].begin;
                const std::size_t end = chunk_ranges[i].end;
                auto cmd = TaskCommand::make([input_ptr, output_ptr, fn_ptr, begin, end]() {
                    auto input_it = std::ranges::begin(*input_ptr);
                    auto output_it = std::ranges::begin(*output_ptr);
                    for (std::size_t idx = begin; idx < end; ++idx) {
                        auto input_offset = static_cast<std::ranges::range_difference_t<InRange>>(idx);
                        auto output_offset = static_cast<std::ranges::range_difference_t<OutRange>>(idx);
                        *(output_it + output_offset) = std::invoke(*fn_ptr, *(input_it + input_offset));
                    }
                });
                TaskId id = result.ir.add_node(
                    "parallel_transform.chunk." + std::to_string(i),
                    ExecutionDomain::CPU,
                    std::move(cmd),
                    expr.frontend.hash,
                    expr.frontend.dump
                );
                result.starts.push_back(id);
                result.terminals.push_back(id);
            }
            return result;
        }
    } // namespace detail

    template <IsPravahaExpr Expr>
    Outcome<TaskIr> lower_to_ir(Expr&& expr) {
        auto result = detail::lower_impl(std::forward<Expr>(expr));
        for (const auto& group : result.ir.join_groups) {
            if (group.policy.kind != JoinPolicyKind::Quorum) {
                continue;
            }
            if (group.policy.quorum_required == 0) {
                std::string msg = "quorum validation failed: policy=Quorum quorum_required=0 branch_count="
                    + std::to_string(group.members.size());
                if (group.frontend_hash != 0) {
                    msg += " frontend_hash=" + std::to_string(group.frontend_hash);
                }
                return std::unexpected(PravahaError{
                    ErrorKind::ValidationError,
                    std::move(msg)
                });
            }
            if (group.policy.quorum_required > group.members.size()) {
                std::string msg = "quorum validation failed: policy=Quorum quorum_required="
                    + std::to_string(group.policy.quorum_required)
                    + " branch_count=" + std::to_string(group.members.size());
                if (group.frontend_hash != 0) {
                    msg += " frontend_hash=" + std::to_string(group.frontend_hash);
                }
                return std::unexpected(PravahaError{
                    ErrorKind::ValidationError,
                    std::move(msg)
                });
            }
        }
        return std::move(result.ir);
    }

    // ============================================================================
    //  SECTION 8: LITEGRAPH VALIDATION LAYER
    // ============================================================================


    // ---------------------------------------------------------------------------
    // 8.3 to_litegraph - convert TaskIr to ExecutionGraph
    // ---------------------------------------------------------------------------
    inline Outcome<ExecutionGraph> to_litegraph(const TaskIr& ir) {
        ExecutionGraph graph;
        graph.reserve_nodes(ir.nodes.size());
        graph.reserve_edges(ir.edges.size());

        // Map TaskId -> litegraph::NodeId
        std::unordered_map<std::size_t, litegraph::NodeId> id_map;
        id_map.reserve(ir.nodes.size());

        for (const auto& node : ir.nodes) {
            const litegraph::NodeId nid = graph.add_node(GraphNodePayload{node.id, node.name});
            id_map[node.id.value] = nid;
        }

        for (const auto& edge : ir.edges) {
            auto from_it = id_map.find(edge.from.value);
            auto to_it = id_map.find(edge.to.value);
            if (from_it == id_map.end() || to_it == id_map.end()) {
                return std::unexpected(PravahaError{
                    ErrorKind::ValidationError,
                    "Invalid edge endpoint: TaskId not found in IR nodes"
                });
            }
            graph.add_edge(from_it->second, to_it->second, GraphEdgePayload{edge.kind});
        }

        return graph;
    }

    // ---------------------------------------------------------------------------
    // 8.4 validate_ir_with_litegraph - cycle detection and structural validation
    // ---------------------------------------------------------------------------
    inline Outcome<Unit> validate_ir_with_litegraph(const TaskIr& ir) {
        auto has_cycle = ir.dependency_graph_has_cycle();
        if (!has_cycle.has_value()) {
            return std::unexpected(has_cycle.error());
        }
        if (has_cycle.value()) {
            return std::unexpected(PravahaError{
                ErrorKind::CycleDetected,
                "Cycle detected in task dependency graph"
            });
        }

        return Unit{};
    }

    inline Outcome<Unit> validate_data_contracts(const TaskIr& ir) {
        auto find_node = [&](const TaskId id) -> const IrNode* {
            if (id.value < ir.nodes.size() && ir.nodes[id.value].id == id) {
                return &ir.nodes[id.value];
            }
            const auto it = std::ranges::find_if(ir.nodes, [&](const IrNode& node) {
                return node.id == id;
            });
            if (it == ir.nodes.end()) {
                return nullptr;
            }
            return &*it;
        };

        auto edge_controlled_by_join = [&](const TaskId from, const TaskId to) -> bool {
            for (std::size_t gid = 0; gid < ir.join_groups.size(); ++gid) {
                const auto& group = ir.join_groups[gid];
                if (group.policy.kind != JoinPolicyKind::AnySuccess
                    && group.policy.kind != JoinPolicyKind::Quorum
                    && group.policy.kind != JoinPolicyKind::CollectAll) {
                    continue;
                }
                bool contains_from = false;
                for (auto member : group.members) {
                    if (member == from) {
                        contains_from = true;
                        break;
                    }
                }
                if (!contains_from) {
                    continue;
                }
                for (auto member : group.members) {
                    for (const auto& edge : ir.edges) {
                        if ((edge.kind == EdgeKind::Sequence || edge.kind == EdgeKind::Data)
                            && edge.from == member
                            && edge.to == to) {
                            return true;
                        }
                    }
                }
            }
            return false;
        };

        auto group_contains_member = [](const IrJoinGroup& group, const TaskId id) -> bool {
            return std::ranges::any_of(group.members, [&id](const auto& member) {
                return member == id;
            });
        };

        for (std::size_t gid = 0; gid < ir.join_groups.size(); ++gid) {
            const auto& group = ir.join_groups[gid];
            if (group.policy.kind != JoinPolicyKind::CollectAll) {
                continue;
            }

            const IrNode* exemplar = nullptr;
            for (const auto member : group.members) {
                const IrNode* node = find_node(member);
                if (node == nullptr) {
                    return std::unexpected(PravahaError{
                        ErrorKind::ValidationError,
                        "Invalid join member: TaskId not found in IR nodes"
                    });
                }
                if (!node->output_contract.checked) {
                    continue;
                }
                if (exemplar == nullptr) {
                    exemplar = node;
                    continue;
                }
                if (node->output_contract.type_hash != exemplar->output_contract.type_hash) {
                    std::string msg = "collect_all validation failed: heterogeneous branch payloads in join group";
                    if (group.frontend_hash != 0) {
                        msg += " frontend_hash=" + std::to_string(group.frontend_hash);
                    }
                    return std::unexpected(PravahaError{
                        ErrorKind::ValidationError,
                        std::move(msg)
                    });
                }
            }

            if (exemplar == nullptr) {
                continue;
            }

            if (!exemplar->output_contract.vector_checked) {
                std::string msg = "collect_all validation failed: branch payload is not vector-aggregatable";
                if (group.frontend_hash != 0) {
                    msg += " frontend_hash=" + std::to_string(group.frontend_hash);
                }
                return std::unexpected(PravahaError{
                    ErrorKind::ValidationError,
                    std::move(msg)
                });
            }

            std::vector<TaskId> downstream;
            for (const auto& edge : ir.edges) {
                if (edge.kind != EdgeKind::Sequence && edge.kind != EdgeKind::Data) {
                    continue;
                }
                if (!group_contains_member(group, edge.from)) {
                    continue;
                }
                if (std::ranges::find(downstream, edge.to) == downstream.end()) {
                    downstream.push_back(edge.to);
                }
            }

            for (const auto succ_id : downstream) {
                const IrNode* succ = find_node(succ_id);
                if (succ == nullptr) {
                    return std::unexpected(PravahaError{
                        ErrorKind::ValidationError,
                        "Invalid edge endpoint: TaskId not found in IR nodes"
                    });
                }
                if (!succ->input_contract.checked) {
                    continue;
                }
                if (succ->input_contract.type_hash == exemplar->output_contract.vector_type_hash) {
                    continue;
                }
                std::string message = "Type mismatch on collect_all join to '" + succ->name
                    + "': output '" + exemplar->output_contract.vector_type_name
                    + "' does not match input '" + succ->input_contract.type_name + "'";
                return std::unexpected(PravahaError{
                    ErrorKind::TypeMismatch,
                    std::move(message),
                    succ->name
                });
            }
        }

        for (const auto& node : ir.nodes) {
            if (!node.input_contract.checked) {
                continue;
            }
            std::size_t inbound_non_join_edges = 0;
            for (const auto& edge : ir.edges) {
                if (edge.to != node.id) {
                    continue;
                }
                if (edge.kind != EdgeKind::Sequence && edge.kind != EdgeKind::Data) {
                    continue;
                }
                if (edge_controlled_by_join(edge.from, edge.to)) {
                    continue;
                }
                ++inbound_non_join_edges;
                if (inbound_non_join_edges > 1) {
                    return std::unexpected(PravahaError{
                        ErrorKind::ValidationError,
                        "Multiple predecessors for one-input task are not supported: '" + node.name + "'",
                        node.name
                    });
                }
            }
        }

        for (const auto& edge : ir.edges) {
            if (edge.kind != EdgeKind::Sequence && edge.kind != EdgeKind::Data) {
                continue;
            }

            const IrNode* from = find_node(edge.from);
            const IrNode* to = find_node(edge.to);
            if (from == nullptr || to == nullptr) {
                return std::unexpected(PravahaError{
                    ErrorKind::ValidationError,
                    "Invalid edge endpoint: TaskId not found in IR nodes"
                });
            }

            if (!from->output_contract.checked || !to->input_contract.checked) {
                continue;
            }

            if (edge_controlled_by_join(edge.from, edge.to)) {
                continue;
            }

            if (from->output_contract.type_hash == to->input_contract.type_hash) {
                continue;
            }

            std::string message = "Type mismatch on edge '" + from->name + "' -> '" + to->name
                + "': output '" + from->output_contract.type_name
                + "' does not match input '" + to->input_contract.type_name + "'";
            return std::unexpected(PravahaError{
                ErrorKind::TypeMismatch,
                std::move(message),
                to->name
            });
        }

        return Unit{};
    }

    // ---------------------------------------------------------------------------
    // 8.5 topological_order - compute execution order using LiteGraph
    // ---------------------------------------------------------------------------
    inline Outcome<std::vector<TaskId>> topological_order(const TaskIr& ir) {
        auto has_cycle = ir.dependency_graph_has_cycle();
        if (!has_cycle.has_value()) {
            return std::unexpected(has_cycle.error());
        }
        if (has_cycle.value()) {
            return std::unexpected(PravahaError{
                ErrorKind::CycleDetected,
                "Cycle detected - topological order undefined"
            });
        }

        auto graph_result = ir.dependency_graph();
        if (!graph_result.has_value()) {
            return std::unexpected(graph_result.error());
        }
        const auto& dep_graph = *graph_result.value();

        std::vector<litegraph::NodeId> topo = litegraph::topological_sort(dep_graph);

        std::vector<TaskId> result;
        result.reserve(topo.size());
        for (const auto& nid : topo) {
            const auto& payload = dep_graph.node_data(nid);
            result.push_back(payload.task_id);
        }

        return result;
    }

    // ============================================================================
    //  SECTION 9: RUNTIME STATE & SCHEDULER
    // ============================================================================

    struct RunResult {
        TaskState final_state{TaskState::Succeeded};
        std::vector<TaskState> node_states;
        std::vector<PravahaError> errors;

        [[nodiscard]] bool succeeded() const noexcept {
            return final_state == TaskState::Succeeded;
        }

        [[nodiscard]] bool failed() const noexcept {
            return final_state == TaskState::Failed;
        }
    };

    struct JoinRuntimeState {
        JoinPolicy policy{};
        std::size_t expected{};
        std::size_t succeeded{};
        std::size_t failed{};
        std::size_t canceled{};
        std::size_t skipped{};
        bool resolved{};
        bool success{};
    };

    struct RuntimeState {
        std::vector<TaskState> node_states;
        std::vector<ResultSlot> result_slots;
        std::vector<std::size_t> attempt_counts;
        std::vector<std::uint64_t> timeout_deadline_ns;
        std::vector<std::size_t> remaining_deps;
        std::vector<std::vector<std::size_t>> successors;
        std::vector<std::vector<std::size_t>> predecessors;
        std::vector<JoinRuntimeState> joins;
        std::vector<ResultSlot> join_result_slots;
        std::vector<std::optional<std::size_t>> join_result_sources;
        std::vector<std::vector<bool>> join_member_recorded;
        std::vector<bool> join_failure_reported;
        std::vector<std::vector<std::size_t>> node_join_groups;
        std::vector<PravahaError> errors;
        std::vector<IrJoinGroup> const* join_groups{nullptr};
        CancellationSource cancellation_source{};
        CancellationToken cancellation_token{};
        bool canceled{false};

        static RuntimeState build(const TaskIr& ir) {
            RuntimeState rs;
            const std::size_t n = ir.nodes.size();
            rs.node_states.resize(n, TaskState::Created);
            rs.result_slots.resize(n);
            rs.attempt_counts.resize(n, 0);
            rs.timeout_deadline_ns.resize(n, 0);
            rs.remaining_deps.resize(n, 0);
            rs.successors.resize(n);
            rs.predecessors.resize(n);
            rs.node_join_groups.resize(n);
            rs.join_groups = &ir.join_groups;

            rs.joins.reserve(ir.join_groups.size());
            rs.join_result_slots.resize(ir.join_groups.size());
            rs.join_result_sources.resize(ir.join_groups.size());
            rs.join_member_recorded.resize(ir.join_groups.size());
            rs.join_failure_reported.assign(ir.join_groups.size(), false);
            for (std::size_t gid = 0; gid < ir.join_groups.size(); ++gid) {
                const auto& group = ir.join_groups[gid];
                JoinRuntimeState join;
                join.policy = group.policy;
                join.expected = group.members.size();
                join.resolved = (join.expected == 0);
                join.success = (join.expected == 0);
                rs.joins.push_back(join);
                rs.join_member_recorded[gid].assign(group.members.size(), false);
                for (const auto member : group.members) {
                    if (member.value < n) {
                        rs.node_join_groups[member.value].push_back(gid);
                    }
                }
            }

            for (const auto& edge : ir.edges) {
                if (edge.kind != EdgeKind::Sequence && edge.kind != EdgeKind::Data) continue;
                auto from = edge.from.value;
                auto to = edge.to.value;
                if (from < n && to < n) {
                    rs.remaining_deps[to]++;
                    rs.successors[from].push_back(to);
                    rs.predecessors[to].push_back(from);
                }
            }

            for (std::size_t i = 0; i < n; ++i) {
                if (rs.remaining_deps[i] == 0) {
                    rs.node_states[i] = TaskState::Ready;
                }
            }

            rs.cancellation_token = rs.cancellation_source.token();

            return rs;
        }

        [[nodiscard]] std::size_t attempt_count(const TaskId id) const {
            if (!id.is_valid() || id.value >= attempt_counts.size()) {
                return 0;
            }
            return attempt_counts[id.value];
        }

        [[nodiscard]] bool cancellation_requested() const noexcept {
            return canceled || cancellation_source.stop_requested() || cancellation_token.stop_requested();
        }

        void bind_cancellation(CancellationToken token) {
            cancellation_token = std::move(token);
            if (cancellation_token.stop_requested()) {
                request_cancellation();
            }
        }

        [[nodiscard]] bool is_in_collect_all_group(const std::size_t idx) const {
            if (!join_groups) return false;
            const TaskId tid{idx};
            for (const auto& jg : *join_groups) {
                if (jg.policy.kind != JoinPolicyKind::CollectAll) continue;
                for (const auto& m : jg.members) {
                    if (m == tid) return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool is_in_any_success_group(const std::size_t idx) const {
            if (!join_groups) return false;
            const TaskId tid{idx};
            for (const auto& jg : *join_groups) {
                if (jg.policy.kind != JoinPolicyKind::AnySuccess) continue;
                for (const auto& m : jg.members) {
                    if (m == tid) return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool is_in_quorum_group(const std::size_t idx) const {
            if (!join_groups) return false;
            const TaskId tid{idx};
            for (const auto& jg : *join_groups) {
                if (jg.policy.kind != JoinPolicyKind::Quorum) continue;
                for (const auto& m : jg.members) {
                    if (m == tid) return true;
                }
            }
            return false;
        }

        static bool is_terminal_state(const TaskState state) {
            return state == TaskState::Succeeded
                || state == TaskState::Failed
                || state == TaskState::Canceled
                || state == TaskState::Skipped;
        }

        static void resolve_join(JoinRuntimeState& join) {
            const std::size_t terminal = join.succeeded + join.failed + join.canceled + join.skipped;
            const std::size_t remaining_possible = (join.expected > terminal) ? (join.expected - terminal) : 0;

            switch (join.policy.kind) {
            case JoinPolicyKind::AllOrNothing:
                if (join.failed + join.canceled + join.skipped > 0) {
                    join.resolved = true;
                    join.success = false;
                    return;
                }
                if (join.succeeded >= join.expected) {
                    join.resolved = true;
                    join.success = true;
                }
                return;

            case JoinPolicyKind::CollectAll:
                if (terminal >= join.expected) {
                    join.resolved = true;
                    join.success = (join.succeeded == join.expected);
                }
                return;

            case JoinPolicyKind::AnySuccess:
                if (join.succeeded >= 1) {
                    join.resolved = true;
                    join.success = true;
                    return;
                }
                if (terminal >= join.expected) {
                    join.resolved = true;
                    join.success = false;
                }
                return;

            case JoinPolicyKind::Quorum: {
                const std::size_t required = join.policy.quorum_required;
                if (join.succeeded >= required) {
                    join.resolved = true;
                    join.success = true;
                    return;
                }
                if (join.succeeded + remaining_possible < required) {
                    join.resolved = true;
                    join.success = false;
                }
            }
            }
        }

        [[nodiscard]] std::string join_failure_message(std::size_t group_id) const {
            if (group_id >= joins.size()) {
                return "join failed";
            }
            const auto& join = joins[group_id];
            std::string msg;
            switch (join.policy.kind) {
            case JoinPolicyKind::AllOrNothing:
                msg = "AllOrNothing failed because at least one branch failed";
                break;
            case JoinPolicyKind::CollectAll:
                msg = "CollectAll completed with one or more failures";
                break;
            case JoinPolicyKind::AnySuccess:
                msg = "AnySuccess failed because no branch succeeded";
                break;
            case JoinPolicyKind::Quorum:
                msg = "Quorum failed because quorum became impossible";
                break;
            }
            msg += " policy=";
            switch (join.policy.kind) {
            case JoinPolicyKind::AllOrNothing:
                msg += "AllOrNothing";
                break;
            case JoinPolicyKind::CollectAll:
                msg += "CollectAll";
                break;
            case JoinPolicyKind::AnySuccess:
                msg += "AnySuccess";
                break;
            case JoinPolicyKind::Quorum:
                msg += "Quorum";
                break;
            }
            msg += " quorum_required=" + std::to_string(join.policy.quorum_required)
                + " succeeded=" + std::to_string(join.succeeded)
                + " failed=" + std::to_string(join.failed)
                + " canceled=" + std::to_string(join.canceled)
                + " skipped=" + std::to_string(join.skipped);
            if (join_groups && group_id < join_groups->size() && (*join_groups)[group_id].frontend_hash != 0) {
                msg += " frontend_hash=" + std::to_string((*join_groups)[group_id].frontend_hash);
            }
            return msg;
        }

        [[nodiscard]] bool join_group_controls_successor(const std::size_t group_id, std::size_t succ_idx) const {
            if (!join_groups || group_id >= join_groups->size()) return false;
            const auto& members = (*join_groups)[group_id].members;
            return std::ranges::any_of(members, [&](const auto& member) {
                if (member.value >= successors.size()) return false;
                const auto& succs = successors[member.value];
                return std::ranges::find(succs, succ_idx) != succs.end();
            });
        }

        [[nodiscard]] bool predecessor_satisfied_for_successor(const std::size_t pred_idx,
                                                               const std::size_t succ_idx) const {
            if (pred_idx >= node_states.size() || succ_idx >= node_states.size()) return false;

            if (node_states[pred_idx] == TaskState::Succeeded) return true;

            if (pred_idx < node_join_groups.size()) {
                for (const auto group_id : node_join_groups[pred_idx]) {
                    if (group_id >= joins.size()) continue;
                    if (!join_group_controls_successor(group_id, succ_idx)) continue;
                    const auto& join = joins[group_id];
                    const bool releasable_kind =
                        join.policy.kind == JoinPolicyKind::AnySuccess
                        || join.policy.kind == JoinPolicyKind::Quorum;
                    if (releasable_kind && join.resolved && join.success) {
                        return true;
                    }
                }
            }

            return false;
        }

        [[nodiscard]] bool successor_ready_by_semantics(std::size_t succ_idx) const {
            if (succ_idx >= node_states.size()) return false;
            if (node_states[succ_idx] != TaskState::Created) return false;

            return !std::ranges::any_of(predecessors[succ_idx], [&](auto pred_idx) {
                return !predecessor_satisfied_for_successor(pred_idx, succ_idx);
            });
        }

        void release_successful_join_successors(const std::size_t group_id) {
            if (!join_groups || group_id >= joins.size() || group_id >= join_groups->size()) return;

            if (cancellation_requested()) return;

            const auto& join = joins[group_id];
            if (!join.resolved || !join.success) return;
            if (join.policy.kind != JoinPolicyKind::AnySuccess
                && join.policy.kind != JoinPolicyKind::Quorum)
                return;

            for (const auto& members = (*join_groups)[group_id].members; const auto member : members) {
                if (member.value >= successors.size()) continue;
                for (const auto succ : successors[member.value]) {
                    if (successor_ready_by_semantics(succ)) {
                        node_states[succ] = TaskState::Ready;
                    }
                }
            }
        }

        void capture_first_success_payload(const std::size_t group_id, std::size_t node_index) {
            if (!join_groups || group_id >= joins.size() || group_id >= join_result_slots.size()) return;
            if (node_index >= result_slots.size()) return;
            const auto& join = joins[group_id];
            if (join.policy.kind != JoinPolicyKind::AnySuccess) return;
            if (!join_result_slots[group_id].empty()) return;
            const auto& source = result_slots[node_index];
            if (source.empty()) return;
            if (join_result_slots[group_id].copy_from(source)) {
                join_result_sources[group_id] = node_index;
            }
        }

        void capture_quorum_resolving_payload(const std::size_t group_id, std::size_t node_index) {
            if (!join_groups || group_id >= joins.size() || group_id >= join_result_slots.size()) return;
            if (node_index >= result_slots.size()) return;
            const auto& join = joins[group_id];
            if (join.policy.kind != JoinPolicyKind::Quorum) return;
            if (!join_result_slots[group_id].empty()) return;
            const auto& source = result_slots[node_index];
            if (source.empty()) return;
            if (join_result_slots[group_id].copy_from(source)) {
                join_result_sources[group_id] = node_index;
            }
        }

        void materialize_collect_all_payload(const std::size_t group_id) {
            if (!join_groups || group_id >= joins.size() || group_id >= join_result_slots.size() || group_id >=
                join_groups->size())
                return;
            if (!join_result_slots[group_id].empty()) return;
            const auto& members = (*join_groups)[group_id].members;
            std::vector<const ResultSlot*> slots;
            slots.reserve(members.size());
            for (const auto member : members) {
                if (member.value >= result_slots.size() || member.value >= node_states.size()) {
                    return;
                }
                if (node_states[member.value] != TaskState::Succeeded) {
                    return;
                }
                const auto& slot = result_slots[member.value];
                if (slot.empty()) {
                    return;
                }
                slots.push_back(&slot);
            }
            if (join_result_slots[group_id].aggregate_from(slots) && !members.empty()) {
                join_result_sources[group_id] = members.front().value;
            }
        }

        void record_join_terminal(const std::size_t group_id, const TaskState terminal_state,
                                  const std::optional<std::size_t> terminal_node_index = std::nullopt) {
            if (group_id >= joins.size()) return;
            if (!is_terminal_state(terminal_state)) return;

            auto& join = joins[group_id];
            const bool was_resolved = join.resolved;
            if (join.resolved && join.success
                && (join.policy.kind == JoinPolicyKind::AnySuccess || join.policy.kind == JoinPolicyKind::Quorum)) {
                return;
            }

            switch (terminal_state) {
            case TaskState::Succeeded:
                ++join.succeeded;
                break;
            case TaskState::Failed:
                ++join.failed;
                break;
            case TaskState::Canceled:
                ++join.canceled;
                break;
            case TaskState::Skipped:
                ++join.skipped;
                break;
            default:
                return;
            }

            resolve_join(join);
            if (!was_resolved && join.resolved && join.success) {
                if (join.policy.kind == JoinPolicyKind::CollectAll) {
                    materialize_collect_all_payload(group_id);
                }
                else if (join.policy.kind == JoinPolicyKind::Quorum
                    && terminal_state == TaskState::Succeeded
                    && terminal_node_index.has_value()) {
                    capture_quorum_resolving_payload(group_id, *terminal_node_index);
                }
                release_successful_join_successors(group_id);
            }
            if (!was_resolved && join.resolved && !join.success
                && group_id < join_failure_reported.size() && !join_failure_reported[group_id]) {
                join_failure_reported[group_id] = true;
                errors.emplace_back(ErrorKind::TaskFailed, join_failure_message(group_id));
            }
        }

        void record_join_terminal_for_member(const std::size_t group_id, std::size_t node_index,
                                             const TaskState terminal_state) {
            if (!join_groups || group_id >= joins.size() || group_id >= join_member_recorded.size()) return;
            const auto& members = (*join_groups)[group_id].members;
            const auto it = std::ranges::find_if(members,
                                                 [node_index](const TaskId id) { return id.value == node_index; });
            if (it == members.end()) return;
            const auto member_index = static_cast<std::size_t>(std::distance(members.begin(), it));
            if (member_index >= join_member_recorded[group_id].size()) return;
            if (join_member_recorded[group_id][member_index]) return;
            join_member_recorded[group_id][member_index] = true;
            if (terminal_state == TaskState::Succeeded) {
                capture_first_success_payload(group_id, node_index);
            }
            record_join_terminal(group_id, terminal_state, node_index);
        }

        void record_join_terminal_for_node(const std::size_t node_index, const TaskState terminal_state) {
            if (node_index >= node_join_groups.size()) return;
            for (const auto group_id : node_join_groups[node_index]) {
                record_join_terminal_for_member(group_id, node_index, terminal_state);
            }
        }

        void mark_succeeded(const std::size_t idx) {
            if (idx >= node_states.size()) return;
            if (is_terminal_state(node_states[idx])) return;
            node_states[idx] = TaskState::Succeeded;
            record_join_terminal_for_node(idx, TaskState::Succeeded);
            decrement_downstream(idx);
        }

        void skip_downstream_from_cancellation(const std::size_t idx) {
            if (idx >= successors.size()) return;
            for (const auto succ : successors[idx]) {
                if (succ >= node_states.size()) continue;
                if (auto& succ_state = node_states[succ];
                    succ_state == TaskState::Created || succ_state == TaskState::Ready || succ_state ==
                    TaskState::Scheduled) {
                    succ_state = TaskState::Skipped;
                    record_join_terminal_for_node(succ, TaskState::Skipped);
                    skip_downstream_from_cancellation(succ);
                }
            }
        }

        bool mark_canceled_from_request(const std::size_t idx) {
            if (idx >= node_states.size()) return false;
            if (auto& state = node_states[idx]; state == TaskState::Created || state == TaskState::Ready || state ==
                TaskState::Scheduled) {
                state = TaskState::Canceled;
                record_join_terminal_for_node(idx, TaskState::Canceled);
                skip_downstream_from_cancellation(idx);
                return true;
            }
            return false;
        }

        bool mark_canceled(const std::size_t idx) {
            return mark_canceled_from_request(idx);
        }

        bool request_cancellation() {
            canceled = true;
            cancellation_source.request_stop();
            bool changed = false;
            for (std::size_t i = 0; i < node_states.size(); ++i) {
                if (const auto state = node_states[i];
                    state == TaskState::Created || state == TaskState::Ready || state == TaskState::Scheduled) {
                    changed = mark_canceled_from_request(i) || changed;
                }
            }
            return changed;
        }

        bool synchronize_cancellation() {
            if (!cancellation_requested()) {
                return false;
            }
            return request_cancellation();
        }

        void mark_failed_collect_all(const std::size_t idx, PravahaError err) {
            if (idx >= node_states.size()) return;
            if (is_terminal_state(node_states[idx])) return;
            node_states[idx] = TaskState::Failed;
            record_join_terminal_for_node(idx, TaskState::Failed);
            errors.push_back(std::move(err));
            // Don't skip siblings — decrement downstream dep counters so group can complete
            decrement_downstream(idx);
        }

        void mark_failed_any_success(const std::size_t idx, PravahaError err) {
            if (idx >= node_states.size()) return;
            if (is_terminal_state(node_states[idx])) return;
            node_states[idx] = TaskState::Failed;
            record_join_terminal_for_node(idx, TaskState::Failed);
            errors.push_back(std::move(err));
            decrement_downstream(idx);
        }

        void mark_failed_quorum(const std::size_t idx, PravahaError err) {
            if (idx >= node_states.size()) return;
            if (is_terminal_state(node_states[idx])) return;
            node_states[idx] = TaskState::Failed;
            record_join_terminal_for_node(idx, TaskState::Failed);
            errors.push_back(std::move(err));
            decrement_downstream(idx);
        }

        void mark_failed(const std::size_t idx, PravahaError err) {
            if (idx >= node_states.size()) return;
            if (is_terminal_state(node_states[idx])) return;
            if (is_in_collect_all_group(idx)) {
                mark_failed_collect_all(idx, std::move(err));
            }
            else if (is_in_any_success_group(idx)) {
                mark_failed_any_success(idx, std::move(err));
            }
            else if (is_in_quorum_group(idx)) {
                mark_failed_quorum(idx, std::move(err));
            }
            else {
                node_states[idx] = TaskState::Failed;
                record_join_terminal_for_node(idx, TaskState::Failed);
                errors.push_back(std::move(err));
                skip_downstream(idx);
            }
        }

        void decrement_downstream(const std::size_t idx) {
            for (const auto succ : successors[idx]) {
                if (remaining_deps[succ] > 0) {
                    remaining_deps[succ]--;
                    if (remaining_deps[succ] == 0 && node_states[succ] == TaskState::Created) {
                        if (cancellation_requested()) {
                            mark_canceled(succ);
                        }
                        else if (has_failed_predecessor(succ) || !blocking_join_success_for_successor(succ)) {
                            node_states[succ] = TaskState::Skipped;
                            record_join_terminal_for_node(succ, TaskState::Skipped);
                            skip_downstream(succ);
                        }
                        else {
                            node_states[succ] = TaskState::Ready;
                        }
                    }
                }
            }
        }

        [[nodiscard]] bool has_failed_predecessor(const std::size_t succ_idx) const {
            // Check all nodes that have succ_idx as a successor
            for (std::size_t i = 0; i < successors.size(); ++i) {
                for (const auto s : successors[i]) {
                    if (s == succ_idx && node_states[i] == TaskState::Failed) {
                        if (predecessor_controlled_by_blocking_join(i)) {
                            continue;
                        }
                        return true;
                    }
                }
            }
            return false;
        }

        [[nodiscard]] bool predecessor_controlled_by_blocking_join(const std::size_t pred_idx) const {
            if (pred_idx >= node_join_groups.size()) return false;
            return std::ranges::any_of(node_join_groups[pred_idx], [&](auto group_id) {
                if (group_id >= joins.size()) return false;
                const auto kind = joins[group_id].policy.kind;
                return kind == JoinPolicyKind::AllOrNothing
                    || kind == JoinPolicyKind::CollectAll
                    || kind == JoinPolicyKind::AnySuccess
                    || kind == JoinPolicyKind::Quorum;
            });
        }

        [[nodiscard]] bool blocking_join_success_for_successor(const std::size_t succ_idx) const {
            if (succ_idx >= predecessors.size()) return true;
            for (const auto pred_idx : predecessors[succ_idx]) {
                if (pred_idx >= node_join_groups.size()) continue;
                for (const auto group_id : node_join_groups[pred_idx]) {
                    if (group_id >= joins.size()) continue;
                    const auto& join = joins[group_id];
                    if (join.policy.kind != JoinPolicyKind::AllOrNothing
                        && join.policy.kind != JoinPolicyKind::CollectAll
                        && join.policy.kind != JoinPolicyKind::AnySuccess
                        && join.policy.kind != JoinPolicyKind::Quorum)
                        continue;
                    if (!join.resolved || !join.success) return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool failed_node_tolerated_by_successful_join(const std::size_t idx) const {
            if (idx >= node_join_groups.size()) return false;
            return std::ranges::any_of(node_join_groups[idx], [&](auto group_id) {
                if (group_id >= joins.size()) return false;
                const auto& join = joins[group_id];
                return (join.policy.kind == JoinPolicyKind::AnySuccess
                        || join.policy.kind == JoinPolicyKind::Quorum)
                    && join.resolved && join.success;
            });
        }

        void skip_downstream(const std::size_t idx) {
            for (const auto succ : successors[idx]) {
                if (node_states[succ] == TaskState::Created || node_states[succ] == TaskState::Ready || node_states[
                    succ] == TaskState::Scheduled) {
                    node_states[succ] = TaskState::Skipped;
                    record_join_terminal_for_node(succ, TaskState::Skipped);
                    skip_downstream(succ);
                }
            }
        }

        [[nodiscard]] bool has_ready() const {
            return std::ranges::any_of(node_states, [](const auto s) {
                return s == TaskState::Ready;
            });
        }

        [[nodiscard]] std::size_t next_ready() const {
            for (std::size_t i = 0; i < node_states.size(); ++i)
                if (node_states[i] == TaskState::Ready) return i;
            return ~std::size_t{0};
        }

        [[nodiscard]] RunResult finalize() const {
            RunResult result;
            result.node_states = node_states;
            result.errors = errors;
            result.final_state = TaskState::Succeeded;
            for (std::size_t i = 0; i < node_states.size(); ++i) {
                const auto s = node_states[i];
                if (s == TaskState::Failed) {
                    if (!failed_node_tolerated_by_successful_join(i)) {
                        result.final_state = TaskState::Failed;
                        break;
                    }
                }
                if (s == TaskState::Canceled) {
                    result.final_state = TaskState::Canceled;
                    break;
                }
            }
            return result;
        }
    };

    // ============================================================================
    //  SECTION 9.5: DOMAIN CONSTRAINT VALIDATION (uses meta.hpp)
    // ============================================================================

    namespace domain_traits {
        template <class T>
        consteval bool pravaha_zero_copy_serializable() {
            if constexpr (!std::is_trivially_copyable_v<T> || !std::is_standard_layout_v<T>) return false;
            else if constexpr (meta::Reflectable<T>) return meta::is_zero_copy_serializable<T>();
            else return true;
        }

        template <class T>
        consteval bool pravaha_binary_stable() {
            if constexpr (!std::is_trivially_copyable_v<T> || !std::is_standard_layout_v<T>) return false;
            else if constexpr (meta::Reflectable<T>) return meta::is_binary_stable<T>();
            else return std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>;
        }

        template <class T>
        consteval bool is_transferable() {
            return pravaha_binary_stable<T>();
        }

        template <class T>
        consteval bool is_serializable_for_external() {
            return pravaha_zero_copy_serializable<T>() || pravaha_binary_stable<T>();
        }
    } // namespace domain_traits

    namespace detail {
        template <typename F>
        struct infer_output_type {
            using type = callable_payload_t<F>;
        };

        template <typename F>
        using inferred_output_t = infer_output_type<F>::type;
    } // namespace detail

    inline Outcome<Unit> validate_domain_constraints(const TaskIr& ir) {
        for (const auto& node : ir.nodes) {
            if (node.domain == ExecutionDomain::External) {
                if (node.payload_meta.output_checked) {
                    if (!node.payload_meta.output_transferable && !node.payload_meta.output_serializable) {
                        std::string msg = "External domain requires transferable or serializable output: " + node.
                            payload_meta.output_type_name;
                        if (node.frontend_hash != 0) {
                            msg += " [frontend_hash=" + std::to_string(node.frontend_hash) + "]";
                        }
                        return std::unexpected(PravahaError{
                            ErrorKind::DomainConstraintViolation,
                            std::move(msg),
                            node.name
                        });
                    }
                }
            }
        }
        return Unit{};
    }

    // ============================================================================
    //  SECTION 10: INLINE BACKEND & RUNNER
    // ============================================================================

    class InlineBackend {
        bool stop_requested_{false};

    public:
        InlineBackend() = default;

        void submit(TaskCommand cmd) const noexcept {
            if (!stop_requested_) {
                cmd.run();
            }
        }

        void drain() noexcept {}

        void request_stop() noexcept { stop_requested_ = true; }
        [[nodiscard]] bool stopped() const noexcept { return stop_requested_; }
    };

    // ============================================================================
    //  SECTION 10.5: JTHREAD BACKEND
    // ============================================================================

    class JThreadBackend {
        mutable std::mutex mutex_;
        std::condition_variable_any cv_work_;
        std::condition_variable_any cv_drain_;
        std::deque<TaskCommand> queue_;
        std::size_t queue_capacity_{0};
        std::atomic<bool> stop_requested_{false};
        std::atomic<std::size_t> in_flight_{0};
        std::vector<std::jthread> workers_;

        void worker_loop(const std::stop_token& stoken) {
            while (true) {
                TaskCommand cmd;
                {
                    std::unique_lock lock(mutex_);
                    cv_work_.wait(lock, stoken, [this]() { return !queue_.empty(); });
                    if (stoken.stop_requested() && queue_.empty()) return;
                    if (queue_.empty()) return;
                    auto best_it = queue_.begin();
                    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
                        if (static_cast<int>(it->priority()) > static_cast<int>(best_it->priority())) {
                            best_it = it;
                        }
                    }
                    cmd = std::move(*best_it);
                    queue_.erase(best_it);
                }
                cmd.run();
                {
                    std::lock_guard lock(mutex_);
                    in_flight_.fetch_sub(1, std::memory_order_release);
                }
                cv_drain_.notify_all();
            }
        }

    public:
        explicit JThreadBackend(std::size_t worker_count = 0, const std::size_t queue_capacity = 0)
            : queue_capacity_{queue_capacity} {
            if (worker_count == 0) {
                worker_count = std::thread::hardware_concurrency();
                if (worker_count == 0) worker_count = 1;
            }
            workers_.reserve(worker_count);
            for (std::size_t i = 0; i < worker_count; ++i) {
                workers_.emplace_back([this](const std::stop_token& st) { worker_loop(st); });
            }
        }

        ~JThreadBackend() {
            stop_requested_.store(true, std::memory_order_release);
            for (auto& w : workers_) w.request_stop();
            cv_work_.notify_all();
            workers_.clear(); // joins all threads before mutex/cv destroyed
        }

        JThreadBackend(const JThreadBackend&) = delete;

        JThreadBackend& operator=(const JThreadBackend&) = delete;

        JThreadBackend(JThreadBackend&&) = delete;

        JThreadBackend& operator=(JThreadBackend&&) = delete;

        bool submit(TaskCommand cmd) {
            {
                std::lock_guard lock(mutex_);
                if (stop_requested_.load(std::memory_order_acquire)) {
                    return false;
                }
                if (queue_capacity_ != 0 && queue_.size() >= queue_capacity_) {
                    return false;
                }
                in_flight_.fetch_add(1, std::memory_order_release);
                queue_.push_back(std::move(cmd));
            }
            cv_work_.notify_one();
            return true;
        }

        void drain() {
            std::unique_lock lock(mutex_);
            cv_drain_.wait(lock, [this]() {
                return in_flight_.load(std::memory_order_acquire) == 0 && queue_.empty();
            });
        }

        void request_stop() noexcept {
            stop_requested_.store(true, std::memory_order_release);
            for (auto& w : workers_) w.request_stop();
            cv_work_.notify_all();
            cv_drain_.notify_all();
        }

        [[nodiscard]] bool stopped() const noexcept {
            return stop_requested_.load(std::memory_order_acquire);
        }

        [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }
    };

    namespace detail {
        template <class Backend>
        [[nodiscard]] bool backend_is_stopped(Backend& backend) {
            if constexpr (requires(Backend& b) {
                { b.stopped() } -> std::convertible_to<bool>;
            }) {
                return static_cast<bool>(backend.stopped());
            }
            else {
                return false;
            }
        }

        template <class Backend>
        Outcome<Unit> submit_to_backend(Backend& backend, TaskCommand cmd) {
            if (backend_is_stopped(backend)) {
                return std::unexpected(PravahaError{
                    ErrorKind::QueueRejected, "backend rejected task submission: stopped"
                });
            }

            using submit_result_t = decltype(std::declval<Backend&>().submit(std::declval<TaskCommand>()));

            if constexpr (std::same_as<submit_result_t, Outcome<Unit>>) {
                return backend.submit(std::move(cmd));
            }
            else if constexpr (std::same_as<submit_result_t, bool>) {
                if (!backend.submit(std::move(cmd))) {
                    return std::unexpected(PravahaError{ErrorKind::QueueRejected, "backend rejected task submission"});
                }
                return Outcome<Unit>{Unit{}};
            }
            else {
                backend.submit(std::move(cmd));
                if (backend_is_stopped(backend)) {
                    return std::unexpected(PravahaError{
                        ErrorKind::QueueRejected, "backend rejected task submission: stopped"
                    });
                }
                return Outcome<Unit>{Unit{}};
            }
        }

        template <class Backend>
        void notify_backend_cancellation_state_changed(Backend& backend) {
            if constexpr (requires(Backend& b) { b.notify_cancellation_state_changed(); }) {
                backend.notify_cancellation_state_changed();
            }
        }

        template <class Backend>
        class BackendCancellationTokenScope {
            Backend* backend_{nullptr};

        public:
            BackendCancellationTokenScope(Backend& backend, const CancellationToken* token) {
                if constexpr (requires(Backend& b, const CancellationToken* t) {
                    b.set_active_cancellation_token(t);
                }) {
                    backend_ = &backend;
                    backend_->set_active_cancellation_token(token);
                }
            }

            ~BackendCancellationTokenScope() {
                if constexpr (requires(Backend& b, const CancellationToken* t) {
                    b.set_active_cancellation_token(t);
                }) {
                    if (backend_ == nullptr) {
                        return;
                    }
                    backend_->set_active_cancellation_token(nullptr);
                }
            }
        };

        struct SharedSchedulerState {
            RuntimeState rt;
            std::mutex mutex;
            std::condition_variable cv_done;
            std::size_t total_nodes{0};
            std::size_t terminal_count{0};
            std::size_t completed_count{0};
            std::size_t scheduled_last_pass{0};

            [[nodiscard]] bool all_terminal() const { return terminal_count >= total_nodes; }

            static bool is_terminal(const TaskState s) {
                return s == TaskState::Succeeded || s == TaskState::Failed ||
                    s == TaskState::Skipped || s == TaskState::Canceled;
            }

            void count_terminals() {
                terminal_count = 0;
                for (const auto s : rt.node_states)
                    if (is_terminal(s)) ++terminal_count;
                completed_count = terminal_count;
            }

            void note_scheduled_in_last_pass(const std::size_t count) {
                scheduled_last_pass = count;
            }

            [[nodiscard]] std::size_t active_count() const {
                std::size_t active = 0;
                for (const auto s : rt.node_states) {
                    if (s == TaskState::Scheduled || s == TaskState::Running) {
                        ++active;
                    }
                }
                return active;
            }
        };
    } // namespace detail

    // GraphAlgorithmPolicy owns DAG validation and ordering strategy.
    // Default implementation delegates to LiteGraph/LiteGraphAlgorithms helpers.
    // Alternative policies may add custom cycle checks, incremental validation,
    // or critical-path-aware ordering while keeping Runner unchanged.
    struct DefaultGraphAlgorithmPolicy {
        static Outcome<Unit> validate(const TaskIr& ir) {
            return validate_ir_with_litegraph(ir);
        }

        static Outcome<std::vector<TaskId>> topological_order(const TaskIr& ir) {
            return pravaha::topological_order(ir);
        }
    };

    // Backward-compatible alias during transition to algorithm-policy naming.
    using DefaultGraphValidationPolicy = DefaultGraphAlgorithmPolicy;

    // ReadyPolicy owns schedulability checks for each node.
    // Default implementation uses dependency count, cancellation flag, and node
    // state; alternative policies may add priority/resource/domain-aware readiness.
    struct DefaultReadyPolicy {
        template <class RuntimeStateLike>
        static bool is_ready(const RuntimeStateLike& state, std::size_t index) {
            if (state.canceled) return false;
            if (index >= state.node_states.size() || index >= state.remaining_deps.size()) return false;
            const auto node_state = state.node_states[index];
            if (node_state == TaskState::Ready) return true;
            if (node_state != TaskState::Created) return false;
            return state.remaining_deps[index] == 0;
        }
    };

    // NoProgressPolicy owns deadlock/no-progress handling.
    // Default implementation force-fails unresolved non-terminal nodes and records
    // InternalError; alternative policies may add diagnostics, wait-for analysis,
    // or timeout-based handling.
    struct DefaultNoProgressPolicy {
        template <class SharedSchedulerStateLike>
        static bool handle_no_progress(SharedSchedulerStateLike& sstate) {
            if (sstate.completed_count >= sstate.total_nodes) return false;
            if (sstate.active_count() > 0) return false;
            if (sstate.scheduled_last_pass > 0) return false;

            bool changed = false;
            for (auto& s : sstate.rt.node_states) {
                if (!SharedSchedulerStateLike::is_terminal(s)) {
                    s = TaskState::Failed;
                    changed = true;
                }
            }
            if (!changed) return false;

            sstate.rt.errors.push_back(PravahaError{
                ErrorKind::InternalError,
                "scheduler made no progress; unresolved non-terminal nodes remain"
            });
            sstate.count_terminals();
            return true;
        }
    };

    template <
        typename Backend = InlineBackend,
        typename GraphAlgorithmPolicy = DefaultGraphAlgorithmPolicy,
        typename ReadyPolicy = DefaultReadyPolicy,
        typename NoProgressPolicy = DefaultNoProgressPolicy,
        ObserverPolicy Observer = NoObserver,
        RetryPolicy RetryPolicyT = NoRetryPolicy,
        TimeoutPolicy TimeoutPolicyT = CooperativeTimeoutPolicy,
        FlowControlPolicy FlowControlPolicyT = RejectOnFullPolicy,
        BudgetPolicy BudgetPolicyT = NoBudgetPolicy>
    class Runner {
        Backend* backend_{nullptr};
        Backend owned_backend_;
        std::mutex run_state_mutex_;
        std::weak_ptr<detail::SharedSchedulerState> active_state_;
        BudgetPolicyT budget_policy_{};
        Observer observer_{};

    public:
        using observer_type = Observer;
        using retry_policy_type = RetryPolicyT;
        using timeout_policy_type = TimeoutPolicyT;
        using flow_control_policy_type = FlowControlPolicyT;
        using budget_policy_type = BudgetPolicyT;

        [[nodiscard]] BudgetPolicyT& budget_policy() noexcept { return budget_policy_; }
        [[nodiscard]] Observer& observer() noexcept { return observer_; }

        Runner() : owned_backend_{}, backend_{&owned_backend_} {}

        explicit Runner(Backend& b) : backend_{&b} {}

        template <IsPravahaExpr Expr>
        Outcome<RunResult> submit(Expr&& expr) {
            return submit(std::forward<Expr>(expr), CancellationToken{});
        }

        template <IsPravahaExpr Expr>
        Outcome<RunResult> submit(Expr&& expr, CancellationToken cancellation_token) {
            auto ir_result = lower_to_ir(std::forward<Expr>(expr));
            if (!ir_result.has_value()) return std::unexpected(ir_result.error());
            auto& ir = ir_result.value();

            if constexpr (Observer::enabled) {
                observer_.on_graph_event(GraphEvent{
                    EventKind::GraphLowered,
                    ir.nodes.size(),
                    ir.edges.size(),
                    ir.join_groups.size(),
                    now_ns()
                });
            }

            auto validation = GraphAlgorithmPolicy::validate(ir);
            if (!validation.has_value()) return std::unexpected(validation.error());

            if constexpr (Observer::enabled) {
                observer_.on_graph_event(GraphEvent{
                    EventKind::GraphValidated,
                    ir.nodes.size(),
                    ir.edges.size(),
                    ir.join_groups.size(),
                    now_ns()
                });
            }

            if (auto contract_check = validate_data_contracts(ir); !contract_check.has_value())
                return std::unexpected(
                    contract_check.error());

            if (auto domain_check = validate_domain_constraints(ir); !domain_check.has_value())
                return std::unexpected(
                    domain_check.error());

            return execute(ir, std::move(cancellation_token));
        }

        void request_stop() noexcept {
            backend_->request_stop();

            std::shared_ptr<detail::SharedSchedulerState> sstate;
            {
                std::lock_guard lock(run_state_mutex_);
                sstate = active_state_.lock();
            }
            if (!sstate) {
                return;
            }

            {
                std::lock_guard lock(sstate->mutex);
                sstate->rt.request_cancellation();
                sstate->count_terminals();
                sstate->note_scheduled_in_last_pass(0);
            }
            sstate->cv_done.notify_all();
        }

        Backend& backend_ref() noexcept { return *backend_; }

    private:
        static std::uint64_t now_ns() noexcept {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        static bool timeout_requests_cancellation(std::chrono::nanoseconds timeout) {
            return TimeoutPolicyT::on_timeout(timeout);
        }

        void emit_task_event(const TaskIr& ir, const RuntimeState& rt, const std::size_t idx, const EventKind kind) {
            if constexpr (Observer::enabled) {
                observer_.on_task_event(TaskEvent{
                    kind,
                    ir.nodes[idx].id,
                    ir.nodes[idx].name,
                    rt.node_states[idx],
                    ir.nodes[idx].frontend_hash,
                    now_ns()
                });
            }
        }

        void emit_payload_forward_event(const TaskIr& ir, const RuntimeState& rt, const std::size_t from_idx,
                                        const std::size_t to_idx, const std::size_t payload_type_hash) {
            if constexpr (Observer::enabled) {
                if (from_idx >= ir.nodes.size() || to_idx >= ir.nodes.size()) {
                    return;
                }
                observer_.on_task_event(TaskEvent{
                    EventKind::PayloadForwarded,
                    ir.nodes[to_idx].id,
                    ir.nodes[to_idx].name,
                    rt.node_states[to_idx],
                    ir.nodes[to_idx].frontend_hash,
                    now_ns(),
                    ir.nodes[from_idx].id,
                    ir.nodes[to_idx].id,
                    payload_type_hash
                });
            }
        }

        void emit_skip_cancel_transitions(const TaskIr& ir, const std::vector<TaskState>& before,
                                          const RuntimeState& rt) {
            if constexpr (Observer::enabled) {
                for (std::size_t i = 0; i < rt.node_states.size(); ++i) {
                    if (i >= before.size() || before[i] == rt.node_states[i]) {
                        continue;
                    }
                    if (rt.node_states[i] == TaskState::Skipped) {
                        emit_task_event(ir, rt, i, EventKind::TaskSkipped);
                    }
                    else if (rt.node_states[i] == TaskState::Canceled) {
                        emit_task_event(ir, rt, i, EventKind::TaskCanceled);
                    }
                }
            }
        }

        void emit_join_resolved_transitions(const std::vector<JoinRuntimeState>& before, const RuntimeState& rt) {
            if constexpr (Observer::enabled) {
                for (std::size_t gid = 0; gid < rt.joins.size(); ++gid) {
                    if (gid >= before.size()) {
                        continue;
                    }
                    if (before[gid].resolved || !rt.joins[gid].resolved) {
                        continue;
                    }
                    const auto& join = rt.joins[gid];
                    observer_.on_join_event(JoinEvent{
                        EventKind::JoinResolved,
                        gid,
                        join.policy,
                        join.success,
                        join.expected,
                        join.succeeded,
                        join.failed,
                        join.canceled,
                        join.skipped,
                        now_ns()
                    });
                }
            }
        }

        Outcome<RunResult> execute(TaskIr& ir, CancellationToken cancellation_token) {
            auto sstate = std::make_shared<detail::SharedSchedulerState>();
            sstate->rt = RuntimeState::build(ir);
            sstate->total_nodes = ir.nodes.size();

            {
                std::lock_guard lock(sstate->mutex);
                std::vector<TaskState> before_states;
                std::vector<JoinRuntimeState> before_joins;
                if constexpr (Observer::enabled) {
                    before_states = sstate->rt.node_states;
                    before_joins = sstate->rt.joins;
                }
                sstate->rt.bind_cancellation(std::move(cancellation_token));
                sstate->rt.synchronize_cancellation();
                emit_skip_cancel_transitions(ir, before_states, sstate->rt);
                emit_join_resolved_transitions(before_joins, sstate->rt);
                sstate->count_terminals();
            }

            {
                std::lock_guard lock(run_state_mutex_);
                active_state_ = sstate;
            }

            // Submit initially ready nodes
            schedule_ready(ir, sstate);

            // Deadlock guard: no terminal completion possible if scheduler has no live work.
            bool no_progress_forced = false;
            {
                std::lock_guard lock(sstate->mutex);
                no_progress_forced = NoProgressPolicy::handle_no_progress(*sstate);
            }
            if (no_progress_forced) {
                sstate->cv_done.notify_all();
            }

            // Wait for all nodes to reach terminal state
            {
                std::unique_lock lock(sstate->mutex);
                sstate->cv_done.wait(lock, [&]() { return sstate->all_terminal(); });
            }

            {
                std::lock_guard lock(run_state_mutex_);
                if (const auto current = active_state_.lock(); current.get() == sstate.get()) {
                    active_state_.reset();
                }
            }

            return sstate->rt.finalize();
        }

        void schedule_ready(TaskIr& ir, const std::shared_ptr<detail::SharedSchedulerState>& sstate) {
            // Budget check: cancel all pending work when fuel is exhausted.
            if constexpr (BudgetPolicy<BudgetPolicyT>) {
                if (budget_policy_.fuel_exhausted()) {
                    std::lock_guard lock(sstate->mutex);
                    for (std::size_t i = 0; i < sstate->rt.node_states.size(); ++i) {
                        const auto s = sstate->rt.node_states[i];
                        if (s == TaskState::Created || s == TaskState::Ready) {
                            sstate->rt.node_states[i] = TaskState::Canceled;
                            sstate->rt.errors.push_back(
                                PravahaError{
                                    ErrorKind::ResourceExhausted, "fuel exhausted",
                                    i < ir.nodes.size() ? ir.nodes[i].name : ""
                                });
                        }
                    }
                    sstate->count_terminals();
                    sstate->cv_done.notify_all();
                    return;
                }
            }

            // Collect ready indices under lock
            std::vector<std::size_t> ready_indices;
            {
                std::lock_guard lock(sstate->mutex);
                std::vector<TaskState> before_states;
                std::vector<JoinRuntimeState> before_joins;
                if constexpr (Observer::enabled) {
                    before_states = sstate->rt.node_states;
                    before_joins = sstate->rt.joins;
                }
                sstate->rt.synchronize_cancellation();
                emit_skip_cancel_transitions(ir, before_states, sstate->rt);
                emit_join_resolved_transitions(before_joins, sstate->rt);

                for (std::size_t i = 0; i < sstate->rt.node_states.size(); ++i) {
                    if (ReadyPolicy::is_ready(sstate->rt, i)) {
                        emit_task_event(ir, sstate->rt, i, EventKind::TaskReady);
                        sstate->rt.node_states[i] = TaskState::Scheduled;
                        if (ir.nodes[i].timeout.count() > 0) {
                            const auto timeout_ns = static_cast<std::uint64_t>(ir.nodes[i].timeout.count());
                            sstate->rt.timeout_deadline_ns[i] = now_ns() + timeout_ns;
                        }
                        else {
                            sstate->rt.timeout_deadline_ns[i] = 0;
                        }
                        emit_task_event(ir, sstate->rt, i, EventKind::TaskScheduled);
                        ready_indices.push_back(i);
                    }
                }
                sstate->note_scheduled_in_last_pass(ready_indices.size());
                sstate->count_terminals();
            }

            for (const auto idx : ready_indices) {
                submit_node(ir, idx, sstate);
            }
        }

        void submit_node(TaskIr& ir, std::size_t idx, std::shared_ptr<detail::SharedSchedulerState> sstate) {
            // Wrap the node execution in a TaskCommand that calls back into scheduler
            auto* node_cmd_ptr = &ir.nodes[idx].command;
            auto* ir_ptr = &ir;
            auto wrapped = TaskCommand::make([this, idx, node_cmd_ptr, ir_ptr, sstate]() mutable {
                bool run_node = false;
                ResultSlot* input_slot = nullptr;
                std::size_t payload_source_idx = std::numeric_limits<std::size_t>::max();
                {
                    std::lock_guard lock(sstate->mutex);
                    std::vector<TaskState> before_states;
                    std::vector<JoinRuntimeState> before_joins;
                    if constexpr (Observer::enabled) {
                        before_states = sstate->rt.node_states;
                        before_joins = sstate->rt.joins;
                    }
                    sstate->rt.synchronize_cancellation();
                    if (sstate->rt.cancellation_requested() && sstate->rt.node_states[idx] == TaskState::Scheduled) {
                        sstate->rt.mark_canceled(idx);
                    }
                    if (!RuntimeState::is_terminal_state(sstate->rt.node_states[idx])
                        && sstate->rt.node_states[idx] == TaskState::Scheduled
                        && ir_ptr->nodes[idx].timeout.count() > 0
                        && sstate->rt.timeout_deadline_ns[idx] != 0
                        && now_ns() >= sstate->rt.timeout_deadline_ns[idx]
                        && timeout_requests_cancellation(ir_ptr->nodes[idx].timeout)) {
                        sstate->rt.mark_canceled(idx);
                    }
                    if (!RuntimeState::is_terminal_state(sstate->rt.node_states[idx])) {
                        std::size_t payload_type_hash = 0;
                        if (ir_ptr->nodes[idx].input_contract.checked) {
                            bool has_controlling_join = false;
                            std::vector visited_join(sstate->rt.joins.size(), false);
                            for (auto pred_idx : sstate->rt.predecessors[idx]) {
                                if (pred_idx >= sstate->rt.node_join_groups.size()) {
                                    continue;
                                }
                                for (auto group_id : sstate->rt.node_join_groups[pred_idx]) {
                                    if (group_id >= sstate->rt.joins.size() || group_id >= visited_join.size() ||
                                        visited_join[group_id]) {
                                        continue;
                                    }
                                    visited_join[group_id] = true;
                                    if (!sstate->rt.join_group_controls_successor(group_id, idx)) {
                                        continue;
                                    }
                                    has_controlling_join = true;
                                    if (const auto& join = sstate->rt.joins[group_id];
                                        !join.resolved || !join.success) {
                                        continue;
                                    }
                                    if (group_id >= sstate->rt.join_result_slots.size()) {
                                        continue;
                                    }
                                    auto& candidate = sstate->rt.join_result_slots[group_id];
                                    if (candidate.empty()) {
                                        continue;
                                    }
                                    if (candidate.type_hash != ir_ptr->nodes[idx].input_contract.type_hash) {
                                        continue;
                                    }
                                    input_slot = &candidate;
                                    if constexpr (Observer::enabled) {
                                        if (group_id < sstate->rt.join_result_sources.size() && sstate->rt.
                                            join_result_sources[group_id].has_value()) {
                                            payload_source_idx = *sstate->rt.join_result_sources[group_id];
                                        }
                                        else {
                                            payload_source_idx = pred_idx;
                                        }
                                        payload_type_hash = candidate.type_hash;
                                    }
                                    break;
                                }
                                if (input_slot != nullptr) {
                                    break;
                                }
                            }

                            if (!has_controlling_join) {
                                std::size_t successful_predecessors = 0;
                                std::size_t selected_pred = std::numeric_limits<std::size_t>::max();
                                for (auto pred_idx : sstate->rt.predecessors[idx]) {
                                    if (pred_idx >= sstate->rt.node_states.size()) {
                                        continue;
                                    }
                                    if (sstate->rt.node_states[pred_idx] != TaskState::Succeeded) {
                                        continue;
                                    }
                                    ++successful_predecessors;
                                    if (selected_pred == std::numeric_limits<std::size_t>::max()) {
                                        selected_pred = pred_idx;
                                    }
                                }

                                if (successful_predecessors != 1) {
                                    sstate->rt.mark_failed(
                                        idx,
                                        PravahaError{
                                            ErrorKind::ValidationError,
                                            "exactly one successful predecessor required for one-input task",
                                            ir_ptr->nodes[idx].name
                                        }
                                    );
                                    emit_task_event(*ir_ptr, sstate->rt, idx, EventKind::TaskFailed);
                                }
                                else if (selected_pred >= sstate->rt.result_slots.size()) {
                                    sstate->rt.mark_failed(idx, PravahaError{
                                                               ErrorKind::TypeMismatch, "task input type mismatch",
                                                               ir_ptr->nodes[idx].name
                                                           });
                                    emit_task_event(*ir_ptr, sstate->rt, idx, EventKind::TaskFailed);
                                }
                                else {
                                    auto& candidate = sstate->rt.result_slots[selected_pred];
                                    if (candidate.empty() || candidate.type_hash != ir_ptr->nodes[idx].input_contract.
                                        type_hash) {
                                        sstate->rt.mark_failed(idx, PravahaError{
                                                                   ErrorKind::TypeMismatch, "task input type mismatch",
                                                                   ir_ptr->nodes[idx].name
                                                               });
                                        emit_task_event(*ir_ptr, sstate->rt, idx, EventKind::TaskFailed);
                                    }
                                    else {
                                        input_slot = &candidate;
                                        if constexpr (Observer::enabled) {
                                            payload_source_idx = selected_pred;
                                            payload_type_hash = candidate.type_hash;
                                        }
                                    }
                                }
                            }
                            if (input_slot == nullptr && !
                                RuntimeState::is_terminal_state(sstate->rt.node_states[idx])) {
                                sstate->rt.mark_failed(idx, PravahaError{
                                                           ErrorKind::TypeMismatch, "task input type mismatch",
                                                           ir_ptr->nodes[idx].name
                                                       });
                                emit_task_event(*ir_ptr, sstate->rt, idx, EventKind::TaskFailed);
                            }
                        }
                        if (!RuntimeState::is_terminal_state(sstate->rt.node_states[idx])) {
                            sstate->rt.node_states[idx] = TaskState::Running;
                            emit_task_event(*ir_ptr, sstate->rt, idx, EventKind::TaskStarted);
                            if constexpr (Observer::enabled) {
                                if (input_slot != nullptr
                                    && payload_source_idx < ir_ptr->nodes.size()
                                    && payload_type_hash != 0) {
                                    emit_payload_forward_event(*ir_ptr, sstate->rt, payload_source_idx, idx,
                                                               payload_type_hash);
                                }
                            }
                            run_node = true;
                        }
                    }
                    emit_skip_cancel_transitions(*ir_ptr, before_states, sstate->rt);
                    emit_join_resolved_transitions(before_joins, sstate->rt);
                    sstate->count_terminals();
                }

                if (!run_node) {
                    sstate->cv_done.notify_all();
                    return;
                }

                detail::notify_backend_cancellation_state_changed(*backend_);
                detail::BackendCancellationTokenScope<Backend> cancellation_scope{
                    *backend_, &sstate->rt.cancellation_token
                };

                // Run the actual node command
                auto result = node_cmd_ptr->run(&sstate->rt.result_slots[idx], input_slot);

                // Update scheduler state
                std::vector<std::size_t> newly_ready;
                bool no_progress_forced = false;
                {
                    std::lock_guard lock(sstate->mutex);
                    std::vector<TaskState> before_states;
                    std::vector<JoinRuntimeState> before_joins;
                    if constexpr (Observer::enabled) {
                        before_states = sstate->rt.node_states;
                        before_joins = sstate->rt.joins;
                    }
                    if (result.has_value()) {
                        sstate->rt.mark_succeeded(idx);
                        emit_task_event(*ir_ptr, sstate->rt, idx, EventKind::TaskCompleted);
                    }
                    else {
                        auto error = std::move(result.error());
                        const auto decision = RetryPolicyT::on_failure(
                            error,
                            sstate->rt.attempt_counts[idx],
                            ir_ptr->nodes[idx].max_retries
                        );
                        const bool can_retry = decision == RetryDecision::RetryImmediate
                            && sstate->rt.attempt_counts[idx] < ir_ptr->nodes[idx].max_retries
                            && !sstate->rt.cancellation_requested();
                        if (can_retry) {
                            ++sstate->rt.attempt_counts[idx];
                            sstate->rt.node_states[idx] = TaskState::Ready;
                        }
                        else {
                            sstate->rt.mark_failed(idx, std::move(error));
                            emit_task_event(*ir_ptr, sstate->rt, idx, EventKind::TaskFailed);
                        }
                    }
                    sstate->rt.synchronize_cancellation();
                    emit_skip_cancel_transitions(*ir_ptr, before_states, sstate->rt);
                    emit_join_resolved_transitions(before_joins, sstate->rt);
                    sstate->count_terminals();

                    // Collect newly ready nodes
                    for (std::size_t i = 0; i < sstate->rt.node_states.size(); ++i) {
                        if (ReadyPolicy::is_ready(sstate->rt, i)) {
                            emit_task_event(*ir_ptr, sstate->rt, i, EventKind::TaskReady);
                            sstate->rt.node_states[i] = TaskState::Scheduled;
                            if (ir_ptr->nodes[i].timeout.count() > 0) {
                                const auto timeout_ns = static_cast<std::uint64_t>(ir_ptr->nodes[i].timeout.count());
                                sstate->rt.timeout_deadline_ns[i] = now_ns() + timeout_ns;
                            }
                            else {
                                sstate->rt.timeout_deadline_ns[i] = 0;
                            }
                            emit_task_event(*ir_ptr, sstate->rt, i, EventKind::TaskScheduled);
                            newly_ready.push_back(i);
                        }
                    }
                    sstate->note_scheduled_in_last_pass(newly_ready.size());
                    sstate->count_terminals();

                    // Deadlock guard after completion scheduling pass.
                    no_progress_forced = NoProgressPolicy::handle_no_progress(*sstate);
                }

                detail::notify_backend_cancellation_state_changed(*backend_);

                // Submit newly ready nodes
                for (auto nidx : newly_ready) {
                    submit_node(*ir_ptr, nidx, sstate);
                }

                // Notify if all done
                if (no_progress_forced) {
                    sstate->cv_done.notify_all();
                }
                sstate->cv_done.notify_all();
            }, {}, ir.nodes[idx].priority);

            auto submit_result = detail::submit_to_backend(*backend_, std::move(wrapped));
            if (!submit_result.has_value()) {
                {
                    std::lock_guard lock(sstate->mutex);
                    std::vector<TaskState> before_states;
                    std::vector<JoinRuntimeState> before_joins;
                    if constexpr (Observer::enabled) {
                        before_states = sstate->rt.node_states;
                        before_joins = sstate->rt.joins;
                    }
                    if (sstate->rt.cancellation_requested()) {
                        sstate->rt.mark_canceled(idx);
                    }
                    else {
                        PravahaError queue_error{
                            ErrorKind::QueueRejected,
                            submit_result.error().message,
                            ir.nodes[idx].name
                        };
                        const auto flow_decision = FlowControlPolicyT::on_submit_rejected(queue_error);
                        if (flow_decision == SubmitDecision::Reject) {
                            sstate->rt.mark_failed(idx, std::move(queue_error));
                            emit_task_event(ir, sstate->rt, idx, EventKind::TaskFailed);
                        }
                        else {
                            if (!RuntimeState::is_terminal_state(sstate->rt.node_states[idx])) {
                                sstate->rt.node_states[idx] = TaskState::Ready;
                                sstate->rt.timeout_deadline_ns[idx] = 0;
                            }
                            sstate->note_scheduled_in_last_pass(0);
                        }
                    }
                    sstate->rt.synchronize_cancellation();
                    emit_skip_cancel_transitions(ir, before_states, sstate->rt);
                    emit_join_resolved_transitions(before_joins, sstate->rt);
                    sstate->count_terminals();
                    (void)NoProgressPolicy::handle_no_progress(*sstate);
                }
                sstate->cv_done.notify_all();
            }
        }
    };

    // Specialization behavior: InlineBackend submit returns Outcome, JThreadBackend submit is void.
    // We need the Runner to work with both. The wrapped TaskCommand handles everything internally.

    // ============================================================================
    //  SECTION 11: TEXTUAL PIPELINE PARSING (v0.1 bridge using Lithe)
    // ============================================================================
    // For v0.1, Pravaha keeps a small runtime tokenizer/parser.
    // Lithe provides the canonical symbolic frontend identity layer:
    // task references, sequence, parallel, collect_all, and pipeline
    // structures are captured as Lithe expressions and reduced to
    // dump/hash metadata.
    // Lithe does not execute tasks and does not replace TaskIr or Runner.


    namespace symbolic::lithe_frontend {
        struct pipeline_tag {};

        struct task_ref_tag {};

        struct sequence_tag {};

        struct parallel_tag {};

        struct collect_all_tag {};

        struct any_success_tag {};

        struct quorum_tag {};

        struct named_tag {};

        struct domain_tag {};

        struct priority_tag {};

        struct retry_tag {};

        struct timeout_tag {};

        struct parallel_reduce_tag {};

        struct parallel_for_tag {};

        struct parallel_transform_tag {};

        struct annotation_tag {};

        struct keyword_tag {};

        struct identifier_tag {};

        struct token_tag {};

        struct TokenCapture {
            std::string kind;
            std::string text;
            std::size_t begin{};
            std::size_t end{};

            bool operator==(const TokenCapture&) const = default;
        };
    } // namespace symbolic::lithe_frontend
} // namespace pravaha

namespace std {
    template <>
    struct hash<pravaha::symbolic::lithe_frontend::TokenCapture> {
        std::size_t operator()(const pravaha::symbolic::lithe_frontend::TokenCapture& tok) const noexcept {
            std::size_t h = std::hash<std::string>{}(tok.kind);
            h ^= (std::hash<std::string>{}(tok.text) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
            h ^= (std::hash<std::size_t>{}(tok.begin) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
            h ^= (std::hash<std::size_t>{}(tok.end) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
            return h;
        }
    };
} // namespace std

namespace vakya::emit {
    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::pipeline_tag> {
        static constexpr const char* value = "pravaha.pipeline";
    };

    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::task_ref_tag> {
        static constexpr const char* value = "pravaha.task_ref";
    };

    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::sequence_tag> {
        static constexpr const char* value = "pravaha.sequence";
    };

    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::parallel_tag> {
        static constexpr const char* value = "pravaha.parallel";
    };

    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::collect_all_tag> {
        static constexpr const char* value = "pravaha.collect_all";
    };

    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::any_success_tag> {
        static constexpr const char* value = "pravaha.any_success";
    };

    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::quorum_tag> {
        static constexpr const char* value = "pravaha.quorum";
    };

    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::named_tag> {
        static constexpr const char* value = "pravaha.named";
    };

    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::domain_tag> {
        static constexpr const char* value = "pravaha.domain";
    };

    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::priority_tag> {
        static constexpr const char* value = "pravaha.priority";
    };

    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::retry_tag> {
        static constexpr const char* value = "pravaha.retry";
    };

    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::timeout_tag> {
        static constexpr const char* value = "pravaha.timeout";
    };

    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::parallel_reduce_tag> {
        static constexpr const char* value = "pravaha.parallel_reduce";
    };

    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::parallel_for_tag> {
        static constexpr const char* value = "pravaha.parallel_for";
    };

    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::parallel_transform_tag> {
        static constexpr const char* value = "pravaha.parallel_transform";
    };

    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::annotation_tag> {
        static constexpr const char* value = "pravaha.annotation";
    };

    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::keyword_tag> {
        static constexpr const char* value = "pravaha.keyword";
    };

    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::identifier_tag> {
        static constexpr const char* value = "pravaha.identifier";
    };

    template <>
    struct tag_name<pravaha::symbolic::lithe_frontend::token_tag> {
        static constexpr const char* value = "pravaha.token";
    };
} // namespace vakya::emit

namespace pravaha { namespace symbolic { namespace lithe_frontend {
            // The textual task DSL needs only Vākya's structural tree facilities.
            // It deliberately does not depend on Lithe's compiler, passes, or
            // backends.  Keep this alias local while the historical public
            // `lithe_frontend` spelling is migrated in a source-compatible way.
            namespace lithe = ::vakya;
            template <class Expr>
            LitheFrontendMeta make_meta(const Expr& expr);

            struct CapturedToken {
                TokenCapture capture;
                std::string lithe_dump;
                std::size_t lithe_hash{};
            };

            struct PipelineHeaderParse {
                std::string name;
                std::size_t body_start_offset{};
                CapturedToken pipeline_keyword;
                CapturedToken pipeline_name;
                std::string lithe_dump;
                std::size_t lithe_hash{};
            };

            struct ParallelIntroParse {
                std::size_t body_start_offset{};
                CapturedToken parallel_keyword;
                std::string lithe_dump;
                std::size_t lithe_hash{};
            };

            inline auto token_expr(TokenCapture capture) {
                return lithe::make_node<token_tag>(lithe::as_expr(std::move(capture)));
            }

            inline auto keyword_expr(std::string text, const std::size_t begin, const std::size_t end) {
                return lithe::make_node<keyword_tag>(
                    token_expr(TokenCapture{"keyword", std::move(text), begin, end})
                );
            }

            inline auto identifier_expr(std::string text, const std::size_t begin, const std::size_t end) {
                return lithe::make_node<identifier_tag>(
                    token_expr(TokenCapture{"identifier", std::move(text), begin, end})
                );
            }

            inline auto task_ref_expr(std::string name) {
                return lithe::make_node<task_ref_tag>(lithe::as_expr(std::move(name)));
            }

            inline auto task_ref_expr(std::string name, const std::size_t begin, const std::size_t end) {
                (void)begin;
                (void)end;
                return task_ref_expr(std::move(name));
            }

            template <class LeftExpr, class RightExpr>
            auto sequence_expr(LeftExpr&& left, RightExpr&& right) {
                return lithe::make_node<sequence_tag>(
                    std::forward<LeftExpr>(left),
                    std::forward<RightExpr>(right)
                );
            }

            template <class LeftExpr, class RightExpr>
            auto parallel_expr(LeftExpr&& left, RightExpr&& right) {
                return lithe::make_node<parallel_tag>(
                    std::forward<LeftExpr>(left),
                    std::forward<RightExpr>(right)
                );
            }

            template <class Expr>
            auto collect_all_expr(Expr&& expr) {
                return lithe::make_node<collect_all_tag>(
                    std::forward<Expr>(expr)
                );
            }

            template <class Expr>
            auto any_success_expr(Expr&& expr) {
                return lithe::make_node<any_success_tag>(
                    std::forward<Expr>(expr)
                );
            }

            template <class Expr>
            auto quorum_expr(Expr&& expr, std::size_t required) {
                return lithe::make_node<quorum_tag>(
                    lithe::as_expr(required),
                    std::forward<Expr>(expr)
                );
            }

            template <class ExprHashExpr, class NameExpr>
            auto named_expr(ExprHashExpr&& expr_hash, NameExpr&& name) {
                return lithe::make_node<named_tag>(
                    std::forward<ExprHashExpr>(expr_hash),
                    std::forward<NameExpr>(name)
                );
            }

            template <class ExprHashExpr, class DomainExprType>
            auto domain_expr(ExprHashExpr&& expr_hash, DomainExprType&& domain) {
                return lithe::make_node<domain_tag>(
                    std::forward<ExprHashExpr>(expr_hash),
                    std::forward<DomainExprType>(domain)
                );
            }

            template <class ExprHashExpr, class PriorityExprType>
            auto priority_expr(ExprHashExpr&& expr_hash, PriorityExprType&& priority) {
                return lithe::make_node<priority_tag>(
                    std::forward<ExprHashExpr>(expr_hash),
                    std::forward<PriorityExprType>(priority)
                );
            }

            template <class ExprHashExpr, class RetryExprType>
            auto retry_expr(ExprHashExpr&& expr_hash, RetryExprType&& retries) {
                return lithe::make_node<retry_tag>(
                    std::forward<ExprHashExpr>(expr_hash),
                    std::forward<RetryExprType>(retries)
                );
            }

            template <class ExprHashExpr, class TimeoutExprType>
            auto timeout_expr(ExprHashExpr&& expr_hash, TimeoutExprType&& timeout) {
                return lithe::make_node<timeout_tag>(
                    std::forward<ExprHashExpr>(expr_hash),
                    std::forward<TimeoutExprType>(timeout)
                );
            }

            inline auto parallel_reduce_expr(std::size_t chunk_size, std::size_t range_size, bool has_range_size) {
                return lithe::make_node<parallel_reduce_tag>(
                    lithe::as_expr(chunk_size),
                    lithe::as_expr(range_size),
                    lithe::as_expr(has_range_size)
                );
            }

            inline auto parallel_for_expr(std::size_t chunk_size, std::size_t range_size, bool has_range_size) {
                return lithe::make_node<parallel_for_tag>(
                    lithe::as_expr(chunk_size),
                    lithe::as_expr(range_size),
                    lithe::as_expr(has_range_size)
                );
            }

            inline auto parallel_transform_expr(std::size_t chunk_size, std::size_t range_size, bool has_range_size) {
                return lithe::make_node<parallel_transform_tag>(
                    lithe::as_expr(chunk_size),
                    lithe::as_expr(range_size),
                    lithe::as_expr(has_range_size)
                );
            }

            inline auto annotation_expr(std::size_t expr_hash, std::string key, std::string value) {
                return lithe::make_node<annotation_tag>(
                    lithe::as_expr(expr_hash),
                    lithe::as_expr(std::move(key)),
                    lithe::as_expr(std::move(value))
                );
            }

            template <class BodyExpr>
            auto pipeline_expr(std::string name, BodyExpr&& body_expr) {
                return lithe::make_node<pipeline_tag>(
                    lithe::make_node<identifier_tag>(lithe::as_expr(std::move(name))),
                    std::forward<BodyExpr>(body_expr)
                );
            }

            // ── Keyword string constants ──────────────────────────────────────────────
            inline constexpr std::string_view kw_pipeline = "pipeline";
            inline constexpr std::string_view kw_then = "then";
            inline constexpr std::string_view kw_parallel = "parallel";
            inline constexpr std::string_view kw_collect_all = "collect_all";
            inline constexpr std::string_view kw_any_success = "any_success";
            inline constexpr std::string_view kw_quorum = "quorum";

            // ── Annotation key constants ──────────────────────────────────────────────
            inline constexpr std::string_view ann_domain = "domain";
            inline constexpr std::string_view ann_retry = "retry";
            inline constexpr std::string_view ann_timeout = "timeout";
            inline constexpr std::string_view ann_priority = "priority";

            // ── Domain value constants ────────────────────────────────────────────────
            inline constexpr std::string_view domain_inline = "inline";
            inline constexpr std::string_view domain_cpu = "cpu";
            inline constexpr std::string_view domain_io = "io";

            // ── Priority value constants ──────────────────────────────────────────────
            inline constexpr std::string_view priority_low = "low";
            inline constexpr std::string_view priority_normal = "normal";
            inline constexpr std::string_view priority_high = "high";

            inline bool keyword_matches(const std::string_view token, const std::string_view keyword) {
                return !token.empty() && !keyword.empty() && token == keyword;
            }

            inline bool is_pipeline_keyword(const std::string_view token) {
                return keyword_matches(token, kw_pipeline);
            }

            inline bool is_then_keyword(const std::string_view token) {
                return keyword_matches(token, kw_then);
            }

            inline bool is_parallel_keyword(const std::string_view token) {
                return keyword_matches(token, kw_parallel);
            }

            inline bool is_parallel_keyword_misspelling(const std::string_view token) {
                return token.starts_with(kw_parallel) && !is_parallel_keyword(token);
            }

            inline bool is_collect_all_keyword(const std::string_view token) {
                return keyword_matches(token, kw_collect_all);
            }

            inline bool is_any_success_keyword(const std::string_view token) {
                return keyword_matches(token, kw_any_success);
            }

            inline bool is_quorum_keyword(const std::string_view token) {
                return keyword_matches(token, kw_quorum);
            }

            inline bool is_supported_annotation_key(const std::string_view token) {
                return token == ann_domain || token == ann_retry || token == ann_timeout || token == ann_priority;
            }

            inline bool is_reserved_keyword(const std::string_view token) {
                return is_pipeline_keyword(token)
                    || is_then_keyword(token)
                    || is_parallel_keyword(token)
                    || is_collect_all_keyword(token)
                    || is_any_success_keyword(token)
                    || is_quorum_keyword(token);
            }

            inline bool identifier_matches(std::string_view token) {
                if (token.empty() || is_reserved_keyword(token)) return false;
                if (!std::isalpha(static_cast<unsigned char>(token[0])) && token[0] != '_') return false;
                return !std::ranges::any_of(token, [](const char c) {
                    return !std::isalnum(static_cast<unsigned char>(c)) && c != '_';
                });
            }

            inline Outcome<CapturedToken> capture_keyword(
                std::string_view text,
                std::size_t offset,
                std::string_view keyword,
                std::string_view kind
            ) {
                if (keyword.empty()) {
                    return std::unexpected(PravahaError{ErrorKind::ParseError, "expected non-empty keyword"});
                }

                std::size_t pos = offset;
                while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
                    ++pos;
                }
                if (pos >= text.size()) {
                    return std::unexpected(PravahaError{
                        ErrorKind::ParseError, "expected keyword '" + std::string(keyword) + "'"
                    });
                }

                const std::size_t begin = pos;
                while (pos < text.size() && !std::isspace(static_cast<unsigned char>(text[pos]))
                    && text[pos] != '{' && text[pos] != '}' && text[pos] != ',') {
                    ++pos;
                }
                const std::size_t end = pos;
                const auto token = text.substr(begin, end - begin);

                if (!keyword_matches(token, keyword)) {
                    return std::unexpected(PravahaError{
                        ErrorKind::ParseError, "expected keyword '" + std::string(keyword) + "'"
                    });
                }

                CapturedToken captured;
                captured.capture = TokenCapture{std::string(kind), std::string(token), begin, end};
                const auto expr = lithe::make_node<keyword_tag>(lithe::as_expr(std::string(token)));
                const auto [dump, hash] = make_meta(expr);
                captured.lithe_dump = dump;
                captured.lithe_hash = hash;
                return captured;
            }

            inline Outcome<CapturedToken> capture_identifier(
                std::string_view text,
                std::size_t offset,
                std::string_view kind
            ) {
                std::size_t pos = offset;
                while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
                    ++pos;
                }
                if (pos >= text.size()) {
                    return std::unexpected(PravahaError{ErrorKind::ParseError, "expected identifier"});
                }

                const std::size_t begin = pos;
                while (pos < text.size() && !std::isspace(static_cast<unsigned char>(text[pos]))
                    && text[pos] != '{' && text[pos] != '}' && text[pos] != ',') {
                    ++pos;
                }
                const std::size_t end = pos;
                const auto token = text.substr(begin, end - begin);

                if (!identifier_matches(token)) {
                    return std::unexpected(PravahaError{ErrorKind::ParseError, "expected identifier"});
                }

                CapturedToken captured;
                captured.capture = TokenCapture{std::string(kind), std::string(token), begin, end};
                const auto expr = lithe::make_node<identifier_tag>(lithe::as_expr(std::string(token)));
                const auto [dump, hash] = make_meta(expr);
                captured.lithe_dump = dump;
                captured.lithe_hash = hash;
                return captured;
            }

            inline Outcome<PipelineHeaderParse> parse_pipeline_header(std::string_view text) {
                std::size_t pos = 0;
                while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
                    ++pos;
                }

                auto kw = capture_keyword(text, pos, kw_pipeline, "pipeline_keyword");
                if (!kw.has_value()) {
                    return std::unexpected(PravahaError{ErrorKind::ParseError, "expected keyword 'pipeline'"});
                }

                pos = kw->capture.end;
                if (pos >= text.size() || !std::isspace(static_cast<unsigned char>(text[pos]))) {
                    return std::unexpected(PravahaError{ErrorKind::ParseError, "expected pipeline name"});
                }

                std::size_t name_scan = pos;
                while (name_scan < text.size() && std::isspace(static_cast<unsigned char>(text[name_scan]))) {
                    ++name_scan;
                }
                if (name_scan >= text.size()) {
                    return std::unexpected(PravahaError{ErrorKind::ParseError, "expected pipeline name"});
                }

                std::size_t token_end = name_scan;
                while (token_end < text.size() && !std::isspace(static_cast<unsigned char>(text[token_end]))
                    && text[token_end] != '{' && text[token_end] != '}' && text[token_end] != ',') {
                    ++token_end;
                }
                const auto name_tok = text.substr(name_scan, token_end - name_scan);
                if (is_reserved_keyword(name_tok)) {
                    return std::unexpected(PravahaError{
                        ErrorKind::ParseError,
                        "reserved keyword cannot be used as pipeline name"
                    });
                }

                auto name = capture_identifier(text, pos, "pipeline_name");
                if (!name.has_value()) {
                    return std::unexpected(PravahaError{ErrorKind::ParseError, "expected pipeline name"});
                }

                pos = name->capture.end;
                while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
                    ++pos;
                }
                if (pos >= text.size() || text[pos] != '{') {
                    return std::unexpected(PravahaError{ErrorKind::ParseError, "expected '{' after pipeline name"});
                }

                const auto header_expr = lithe::make_node<pipeline_tag>(
                    keyword_expr(kw->capture.text, kw->capture.begin, kw->capture.end),
                    identifier_expr(name->capture.text, name->capture.begin, name->capture.end)
                );

                PipelineHeaderParse out;
                out.name = name->capture.text;
                out.body_start_offset = pos + 1;
                out.pipeline_keyword = std::move(*kw);
                out.pipeline_name = std::move(*name);
                const auto [dump, hash] = make_meta(header_expr);
                out.lithe_dump = dump;
                out.lithe_hash = hash;
                return out;
            }

            inline Outcome<ParallelIntroParse> parse_parallel_intro(std::string_view text, std::size_t offset) {
                auto kw = capture_keyword(text, offset, kw_parallel, "parallel_keyword");
                if (!kw.has_value()) {
                    return std::unexpected(PravahaError{ErrorKind::ParseError, "expected keyword 'parallel'"});
                }

                std::size_t pos = kw->capture.end;
                while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
                    ++pos;
                }
                if (pos >= text.size() || text[pos] != '{') {
                    return std::unexpected(PravahaError{ErrorKind::ParseError, "expected '{' after parallel"});
                }

                const auto intro_expr = lithe::make_node<parallel_tag>(
                    keyword_expr(kw->capture.text, kw->capture.begin, kw->capture.end)
                );

                ParallelIntroParse out;
                out.body_start_offset = pos + 1;
                out.parallel_keyword = std::move(*kw);
                const auto [dump, hash] = make_meta(intro_expr);
                out.lithe_dump = dump;
                out.lithe_hash = hash;
                return out;
            }
        } // namespace lithe_frontend

        namespace lithe_frontend {
            template <class Expr>
            LitheFrontendMeta make_meta(const Expr& expr) {
                auto meta = LitheFrontendMeta{lithe::emit::dump(expr), lithe::emit::structural_hash(expr)};
                // Mix dump-derived entropy so runtime identity stays sensitive to canonical Lithe shape.
                const auto dump_hash = std::hash<std::string>{}(meta.dump);
                meta.hash ^= (dump_hash + 0x9e3779b97f4a7c15ULL + (meta.hash << 6) + (meta.hash >> 2));
                return meta;
            }

            inline LitheFrontendMeta make_task_ref_meta(const std::string_view name) {
                const auto expr = task_ref_expr(std::string{name});
                return make_meta(expr);
            }

            inline LitheFrontendMeta make_sequence_meta(std::size_t left_hash, std::size_t right_hash) {
                const auto expr = sequence_expr(lithe::as_expr(left_hash), lithe::as_expr(right_hash));
                return make_meta(expr);
            }

            inline LitheFrontendMeta make_parallel_meta(std::size_t left_hash, std::size_t right_hash) {
                const auto expr = parallel_expr(lithe::as_expr(left_hash), lithe::as_expr(right_hash));
                return make_meta(expr);
            }

            inline LitheFrontendMeta make_collect_all_meta(std::size_t expr_hash) {
                const auto expr = collect_all_expr(lithe::as_expr(expr_hash));
                return make_meta(expr);
            }

            inline LitheFrontendMeta make_any_success_meta(std::size_t expr_hash) {
                const auto expr = any_success_expr(lithe::as_expr(expr_hash));
                return make_meta(expr);
            }

            inline LitheFrontendMeta make_quorum_meta(std::size_t expr_hash, const std::size_t required) {
                const auto expr = quorum_expr(lithe::as_expr(expr_hash), required);
                return make_meta(expr);
            }

            inline LitheFrontendMeta make_named_meta(std::size_t expr_hash, const std::string_view name) {
                const auto expr = named_expr(lithe::as_expr(expr_hash), lithe::as_expr(std::string{name}));
                return make_meta(expr);
            }

            inline LitheFrontendMeta make_domain_meta(std::size_t expr_hash, ExecutionDomain domain) {
                const auto expr = domain_expr(lithe::as_expr(expr_hash),
                                              lithe::as_expr(static_cast<std::size_t>(domain)));
                return make_meta(expr);
            }

            inline LitheFrontendMeta make_retry_meta(std::size_t expr_hash, std::size_t max_retries) {
                const auto expr = retry_expr(lithe::as_expr(expr_hash), lithe::as_expr(max_retries));
                return make_meta(expr);
            }

            inline LitheFrontendMeta make_priority_meta(std::size_t expr_hash, TaskPriority priority) {
                const auto expr = priority_expr(lithe::as_expr(expr_hash),
                                                lithe::as_expr(static_cast<std::size_t>(priority)));
                return make_meta(expr);
            }

            inline LitheFrontendMeta make_timeout_meta(std::size_t expr_hash, const std::chrono::nanoseconds timeout) {
                const auto expr = timeout_expr(lithe::as_expr(expr_hash),
                                               lithe::as_expr(static_cast<long long>(timeout.count())));
                return make_meta(expr);
            }

            inline LitheFrontendMeta make_annotation_meta(const std::size_t expr_hash, std::string key,
                                                          std::string value) {
                const auto expr = annotation_expr(expr_hash, std::move(key), std::move(value));
                return make_meta(expr);
            }

            inline LitheFrontendMeta make_task_ref_meta(const std::string_view name,
                                                        const std::unordered_map<std::string, std::string>&
                                                        annotations) {
                auto meta = make_task_ref_meta(name);

                if (const auto it = annotations.find(std::string{ann_domain}); it != annotations.end()) {
                    meta = make_annotation_meta(meta.hash, std::string{ann_domain}, it->second);
                }
                if (const auto it = annotations.find(std::string{ann_retry}); it != annotations.end()) {
                    meta = make_annotation_meta(meta.hash, std::string{ann_retry}, it->second);
                }
                if (const auto it = annotations.find(std::string{ann_timeout}); it != annotations.end()) {
                    meta = make_annotation_meta(meta.hash, std::string{ann_timeout}, it->second);
                }
                if (const auto it = annotations.find(std::string{ann_priority}); it != annotations.end()) {
                    meta = make_annotation_meta(meta.hash, std::string{ann_priority}, it->second);
                }

                return meta;
            }

            inline LitheFrontendMeta make_parallel_reduce_meta(const std::size_t chunk_size,
                                                               const std::size_t range_size,
                                                               const bool has_range_size) {
                const auto expr = parallel_reduce_expr(chunk_size, range_size, has_range_size);
                auto meta = make_meta(expr);
                meta.hash ^= (std::hash<std::size_t>{}(chunk_size) + 0x9e3779b97f4a7c15ULL + (meta.hash << 6) + (
                    meta.hash >> 2));
                meta.hash ^= (std::hash<std::size_t>{}(range_size) + 0x9e3779b97f4a7c15ULL + (meta.hash << 6) + (
                    meta.hash >> 2));
                meta.hash ^= (std::hash<bool>{}(has_range_size) + 0x9e3779b97f4a7c15ULL + (meta.hash << 6) + (
                    meta.hash >> 2));
                return meta;
            }

            inline LitheFrontendMeta make_parallel_for_meta(const std::size_t chunk_size, const std::size_t range_size,
                                                            const bool has_range_size) {
                const auto expr = parallel_for_expr(chunk_size, range_size, has_range_size);
                auto meta = make_meta(expr);
                meta.hash ^= (std::hash<std::size_t>{}(chunk_size) + 0x9e3779b97f4a7c15ULL + (meta.hash << 6) + (
                    meta.hash >> 2));
                meta.hash ^= (std::hash<std::size_t>{}(range_size) + 0x9e3779b97f4a7c15ULL + (meta.hash << 6) + (
                    meta.hash >> 2));
                meta.hash ^= (std::hash<bool>{}(has_range_size) + 0x9e3779b97f4a7c15ULL + (meta.hash << 6) + (
                    meta.hash >> 2));
                return meta;
            }

            inline LitheFrontendMeta make_parallel_transform_meta(const std::size_t chunk_size,
                                                                  const std::size_t range_size,
                                                                  const bool has_range_size) {
                const auto expr = parallel_transform_expr(chunk_size, range_size, has_range_size);
                auto meta = make_meta(expr);
                meta.hash ^= (std::hash<std::size_t>{}(chunk_size) + 0x9e3779b97f4a7c15ULL + (meta.hash << 6) + (
                    meta.hash >> 2));
                meta.hash ^= (std::hash<std::size_t>{}(range_size) + 0x9e3779b97f4a7c15ULL + (meta.hash << 6) + (
                    meta.hash >> 2));
                meta.hash ^= (std::hash<bool>{}(has_range_size) + 0x9e3779b97f4a7c15ULL + (meta.hash << 6) + (
                    meta.hash >> 2));
                return meta;
            }
        } // namespace lithe_frontend

        using AnnotationMap = std::unordered_map<std::string, std::string>;

        struct SymbolicTaskExpr {
            std::string name;
            LitheFrontendMeta frontend;
            std::size_t source_begin{};
            std::size_t source_end{};
            AnnotationMap annotations{};
        };

        struct SymbolicSequenceExpr;
        struct SymbolicParallelExpr;

        using SymbolicExpr = std::variant<
            SymbolicTaskExpr,
            std::unique_ptr<SymbolicSequenceExpr>,
            std::unique_ptr<SymbolicParallelExpr>
        >;

        struct SymbolicSequenceExpr {
            SymbolicExpr left;
            SymbolicExpr right;
            LitheFrontendMeta frontend;
        };

        struct SymbolicParallelExpr {
            std::vector<SymbolicExpr> branches;
            JoinPolicy policy{};
            LitheFrontendMeta frontend;
        };

        struct SymbolicPipeline {
            std::string name;
            SymbolicExpr root;
            LitheFrontendMeta frontend;
        };

        inline LitheFrontendMeta stored_frontend_meta_for_symbolic_expr(const SymbolicExpr& expr) {
            if (auto* task = std::get_if<SymbolicTaskExpr>(&expr)) {
                return task->frontend;
            }
            if (auto* seq_ptr = std::get_if<std::unique_ptr<SymbolicSequenceExpr>>(&expr)) {
                if (*seq_ptr) return (*seq_ptr)->frontend;
                return {};
            }
            if (auto* par_ptr = std::get_if<std::unique_ptr<SymbolicParallelExpr>>(&expr)) {
                if (*par_ptr) return (*par_ptr)->frontend;
                return {};
            }
            return {};
        }

        inline LitheFrontendMeta make_frontend_meta_for_symbolic_expr(const SymbolicExpr& expr) {
            // Canonical path: symbolic nodes already carry Lithe-derived metadata.
            auto stored = stored_frontend_meta_for_symbolic_expr(expr);
            if (stored.hash != 0 && !stored.dump.empty()) {
                return stored;
            }

            // Compatibility fallback for partially-initialized symbolic nodes.
            if (auto* task = std::get_if<SymbolicTaskExpr>(&expr)) {
                const auto task_expr = lithe_frontend::task_ref_expr(task->name);
                return lithe_frontend::make_meta(task_expr);
            }

            if (auto* seq_ptr = std::get_if<std::unique_ptr<SymbolicSequenceExpr>>(&expr)) {
                const auto& seq = **seq_ptr;
                auto [left_dump, left_hash] = make_frontend_meta_for_symbolic_expr(seq.left);
                auto [right_dump, right_hash] = make_frontend_meta_for_symbolic_expr(seq.right);
                const auto seq_expr = lithe_frontend::sequence_expr(
                    lithe::as_expr(left_hash),
                    lithe::as_expr(right_hash)
                );
                return lithe_frontend::make_meta(seq_expr);
            }

            if (auto* par_ptr = std::get_if<std::unique_ptr<SymbolicParallelExpr>>(&expr)) {
                const auto& par = **par_ptr;
                if (par.branches.empty()) {
                    return {};
                }

                auto first_meta = make_frontend_meta_for_symbolic_expr(par.branches.front());
                auto acc_meta = first_meta;

                for (std::size_t i = 1; i < par.branches.size(); ++i) {
                    auto [dump, hash] = make_frontend_meta_for_symbolic_expr(par.branches[i]);
                    const auto par_expr = lithe_frontend::parallel_expr(
                        lithe::as_expr(acc_meta.hash),
                        lithe::as_expr(hash)
                    );
                    acc_meta = lithe_frontend::make_meta(par_expr);
                }

                if (par.policy.kind == JoinPolicyKind::CollectAll) {
                    return lithe_frontend::make_collect_all_meta(acc_meta.hash);
                }
                if (par.policy.kind == JoinPolicyKind::AnySuccess) {
                    return lithe_frontend::make_any_success_meta(acc_meta.hash);
                }
                if (par.policy.kind == JoinPolicyKind::Quorum) {
                    return lithe_frontend::make_quorum_meta(acc_meta.hash, par.policy.quorum_required);
                }

                return acc_meta;
            }

            return {};
        }

        // Lithe-validated keyword set
        inline bool is_keyword(const std::string_view token) {
            return lithe_frontend::is_reserved_keyword(token);
        }

        inline bool is_valid_identifier(const std::string_view token) {
            return lithe_frontend::identifier_matches(token);
        }

        inline Outcome<Unit> validate_task_identifier(const std::string_view token) {
            if (lithe_frontend::is_reserved_keyword(token)) {
                return std::unexpected(PravahaError{
                    ErrorKind::ParseError,
                    "reserved keyword cannot be used as task identifier: " + std::string(token)
                });
            }
            if (!lithe_frontend::identifier_matches(token)) {
                return std::unexpected(PravahaError{
                    ErrorKind::ParseError,
                    "invalid identifier: " + std::string(token)
                });
            }
            return Unit{};
        }

        namespace detail {
            struct Parser {
                std::string_view src;
                std::size_t pos{0};

                void skip_ws() { while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) ++pos; }

                std::string_view peek_token() {
                    skip_ws();
                    if (pos >= src.size()) return {};
                    if (src[pos] == '{' || src[pos] == '}' || src[pos] == ',') return src.substr(pos, 1);
                    const std::size_t start = pos;
                    while (pos < src.size() && !std::isspace(static_cast<unsigned char>(src[pos]))
                        && src[pos] != '{' && src[pos] != '}' && src[pos] != ',')
                        ++pos;
                    const auto tok = src.substr(start, pos - start);
                    pos = start; // don't consume
                    return tok;
                }

                std::string_view consume_token() {
                    skip_ws();
                    if (pos >= src.size()) return {};
                    if (src[pos] == '{' || src[pos] == '}' || src[pos] == ',') return src.substr(pos++, 1);
                    const std::size_t start = pos;
                    while (pos < src.size() && !std::isspace(static_cast<unsigned char>(src[pos]))
                        && src[pos] != '{' && src[pos] != '}' && src[pos] != ',')
                        ++pos;
                    return src.substr(start, pos - start);
                }

                bool expect(const std::string_view expected) {
                    skip_ws();
                    const auto tok = consume_token();
                    return tok == expected;
                }

                Outcome<AnnotationMap> parse_optional_task_annotations() {
                    skip_ws();
                    if (pos >= src.size() || src[pos] != '[') {
                        return AnnotationMap{};
                    }

                    ++pos;
                    AnnotationMap annotations;
                    bool saw_any = false;

                    while (true) {
                        skip_ws();
                        if (pos >= src.size()) {
                            return std::unexpected(PravahaError{
                                ErrorKind::ParseError, "missing closing ']' in task annotation"
                            });
                        }
                        if (src[pos] == ']') {
                            if (!saw_any) {
                                return std::unexpected(PravahaError{ErrorKind::ParseError, "missing annotation key"});
                            }
                            ++pos;
                            return annotations;
                        }

                        const std::size_t key_begin = pos;
                        while (pos < src.size() && (
                            std::isalnum(static_cast<unsigned char>(src[pos])) || src[pos] == '_')) {
                            ++pos;
                        }
                        if (key_begin == pos) {
                            return std::unexpected(PravahaError{ErrorKind::ParseError, "missing annotation key"});
                        }

                        const auto key_view = src.substr(key_begin, pos - key_begin);
                        if (!lithe_frontend::is_supported_annotation_key(key_view)) {
                            return std::unexpected(PravahaError{
                                ErrorKind::ParseError, "unknown annotation key: " + std::string(key_view)
                            });
                        }

                        skip_ws();
                        if (pos >= src.size() || src[pos] != '=') {
                            return std::unexpected(PravahaError{ErrorKind::ParseError, "missing annotation value"});
                        }
                        ++pos;
                        skip_ws();

                        const std::size_t value_begin = pos;
                        while (pos < src.size() && !std::isspace(static_cast<unsigned char>(src[pos])) && src[pos] !=
                            ',' && src[pos] != ']') {
                            ++pos;
                        }
                        if (value_begin == pos) {
                            return std::unexpected(PravahaError{ErrorKind::ParseError, "missing annotation value"});
                        }

                        std::string key{key_view};
                        std::string value{src.substr(value_begin, pos - value_begin)};

                        if (annotations.contains(key)) {
                            return std::unexpected(PravahaError{
                                ErrorKind::ParseError, "duplicate annotation key: " + key
                            });
                        }
                        annotations.emplace(std::move(key), std::move(value));
                        saw_any = true;

                        skip_ws();
                        if (pos >= src.size()) {
                            return std::unexpected(PravahaError{
                                ErrorKind::ParseError, "missing closing ']' in task annotation"
                            });
                        }
                        if (src[pos] == ',') {
                            ++pos;
                            continue;
                        }
                        if (src[pos] == ']') {
                            ++pos;
                            return annotations;
                        }

                        return std::unexpected(PravahaError{ErrorKind::ParseError, "malformed task annotation list"});
                    }
                }

                Outcome<SymbolicTaskExpr> parse_task_with_optional_annotations(
                    std::string_view consumed, std::size_t begin, std::size_t end) {
                    auto annotations = parse_optional_task_annotations();
                    if (!annotations.has_value()) {
                        return std::unexpected(annotations.error());
                    }

                    SymbolicTaskExpr task;
                    task.name = std::string(consumed);
                    task.source_begin = begin;
                    task.source_end = end;
                    task.annotations = std::move(annotations.value());
                    task.frontend = lithe_frontend::make_task_ref_meta(task.name, task.annotations);
                    return task;
                }

                Outcome<SymbolicExpr> parse_parallel(JoinPolicy policy = JoinPolicy{}) {
                    std::vector<SymbolicExpr> branches;
                    while (true) {
                        skip_ws();
                        auto tok = peek_token();
                        if (tok == "}") {
                            consume_token();
                            break;
                        }
                        if (tok.empty())
                            return std::unexpected(PravahaError{
                                ErrorKind::ParseError, "Unexpected end in parallel block"
                            });
                        if (!branches.empty()) {
                            if (tok == ",") consume_token();
                            else
                                return std::unexpected(PravahaError{
                                    ErrorKind::ParseError, "Expected ',' between parallel branches"
                                });
                            tok = peek_token();
                        }
                        if (auto id_ok = validate_task_identifier(tok); !id_ok.has_value())
                            return std::unexpected(
                                id_ok.error());
                        auto consumed = consume_token();
                        const std::size_t end = pos;
                        const std::size_t begin = end - consumed.size();
                        auto task = parse_task_with_optional_annotations(consumed, begin, end);
                        if (!task.has_value()) return std::unexpected(task.error());
                        branches.emplace_back(std::move(task.value()));
                    }
                    if (branches.empty())
                        return std::unexpected(PravahaError{
                            ErrorKind::ParseError, "Empty parallel block"
                        });

                    auto first_meta = stored_frontend_meta_for_symbolic_expr(branches.front());
                    auto par_meta = first_meta;
                    for (std::size_t i = 1; i < branches.size(); ++i) {
                        const auto next_meta = stored_frontend_meta_for_symbolic_expr(branches[i]);
                        const auto par_expr = lithe_frontend::parallel_expr(
                            lithe::as_expr(par_meta.hash),
                            lithe::as_expr(next_meta.hash)
                        );
                        par_meta = lithe_frontend::make_meta(par_expr);
                    }

                    if (policy.kind == JoinPolicyKind::CollectAll) {
                        par_meta = lithe_frontend::make_collect_all_meta(par_meta.hash);
                    }
                    else if (policy.kind == JoinPolicyKind::AnySuccess) {
                        par_meta = lithe_frontend::make_any_success_meta(par_meta.hash);
                    }
                    else if (policy.kind == JoinPolicyKind::Quorum) {
                        par_meta = lithe_frontend::make_quorum_meta(par_meta.hash, policy.quorum_required);
                    }

                    auto par = std::make_unique<SymbolicParallelExpr>();
                    par->branches = std::move(branches);
                    par->policy = policy;
                    par->frontend = std::move(par_meta);
                    SymbolicExpr result{std::move(par)};
                    return result;
                }

                Outcome<std::size_t> parse_quorum_required(const std::string_view token) {
                    if (token.empty()) {
                        return std::unexpected(PravahaError{ErrorKind::ParseError, "expected quorum value"});
                    }
                    if (!std::ranges::all_of(token, [](const unsigned char c) { return std::isdigit(c); })) {
                        return std::unexpected(PravahaError{ErrorKind::ParseError, "expected quorum value"});
                    }
                    std::size_t required = 0;
                    try {
                        required = static_cast<std::size_t>(std::stoull(std::string(token)));
                    }
                    catch (...) {
                        return std::unexpected(PravahaError{ErrorKind::ParseError, "expected quorum value"});
                    }
                    if (required == 0) {
                        return std::unexpected(PravahaError{ErrorKind::ParseError, "quorum requires value > 0"});
                    }
                    return required;
                }

                Outcome<SymbolicExpr> parse_step() {
                    skip_ws();
                    auto tok = peek_token();
                    if (lithe_frontend::is_parallel_keyword(tok)) {
                        auto intro = lithe_frontend::parse_parallel_intro(src, pos);
                        if (!intro.has_value()) {
                            return std::unexpected(PravahaError{
                                ErrorKind::ParseError,
                                "reserved keyword cannot be used as task identifier: parallel"
                            });
                        }
                        pos = intro->body_start_offset;
                        return parse_parallel(JoinPolicy{JoinPolicyKind::AllOrNothing, 0});
                    }

                    if (lithe_frontend::is_collect_all_keyword(tok)) {
                        auto kw = lithe_frontend::capture_keyword(src, pos, "collect_all", "collect_all_keyword");
                        if (!kw.has_value()) {
                            return std::unexpected(
                                PravahaError{ErrorKind::ParseError, "expected keyword 'collect_all'"});
                        }
                        pos = kw->capture.end;
                        skip_ws();
                        if (pos >= src.size() || src[pos] != '{') {
                            return std::unexpected(
                                PravahaError{ErrorKind::ParseError, "expected '{' after collect_all"});
                        }
                        ++pos;
                        return parse_parallel(JoinPolicy{JoinPolicyKind::CollectAll, 0});
                    }

                    if (lithe_frontend::is_any_success_keyword(tok)) {
                        auto kw = lithe_frontend::capture_keyword(src, pos, "any_success", "any_success_keyword");
                        if (!kw.has_value()) {
                            return std::unexpected(
                                PravahaError{ErrorKind::ParseError, "expected keyword 'any_success'"});
                        }
                        pos = kw->capture.end;
                        skip_ws();
                        if (pos >= src.size() || src[pos] != '{') {
                            return std::unexpected(
                                PravahaError{ErrorKind::ParseError, "expected '{' after any_success"});
                        }
                        ++pos;
                        return parse_parallel(JoinPolicy{JoinPolicyKind::AnySuccess, 0});
                    }

                    if (lithe_frontend::is_quorum_keyword(tok)) {
                        auto kw = lithe_frontend::capture_keyword(src, pos, "quorum", "quorum_keyword");
                        if (!kw.has_value()) {
                            return std::unexpected(PravahaError{ErrorKind::ParseError, "expected keyword 'quorum'"});
                        }
                        pos = kw->capture.end;
                        skip_ws();
                        auto required_tok = consume_token();
                        auto required = parse_quorum_required(required_tok);
                        if (!required.has_value()) {
                            return std::unexpected(required.error());
                        }
                        skip_ws();
                        if (pos >= src.size() || src[pos] != '{') {
                            return std::unexpected(PravahaError{
                                ErrorKind::ParseError, "expected '{' after quorum value"
                            });
                        }
                        ++pos;
                        return parse_parallel(JoinPolicy{JoinPolicyKind::Quorum, required.value()});
                    }

                    // If a token looks like a misspelled parallel block intro and is
                    // directly followed by '{', report a parallel-keyword diagnostic.
                    if (lithe_frontend::is_parallel_keyword_misspelling(tok)) {
                        const auto saved = pos;
                        (void)consume_token();
                        skip_ws();
                        const bool has_open_brace = (pos < src.size() && src[pos] == '{');
                        pos = saved;
                        if (has_open_brace) {
                            return std::unexpected(PravahaError{ErrorKind::ParseError, "expected keyword 'parallel'"});
                        }
                    }

                    if (auto id_ok = validate_task_identifier(tok); !id_ok.has_value())
                        return std::unexpected(
                            id_ok.error());
                    auto consumed = consume_token();
                    const std::size_t end = pos;
                    const std::size_t begin = end - consumed.size();
                    auto task = parse_task_with_optional_annotations(consumed, begin, end);
                    if (!task.has_value()) return std::unexpected(task.error());
                    return SymbolicExpr{std::move(task.value())};
                }

                Outcome<SymbolicExpr> parse_sequence() {
                    auto first = parse_step();
                    if (!first.has_value()) return first;
                    auto current = std::move(first.value());
                    while (true) {
                        skip_ws();
                        if (auto tok = peek_token(); !lithe_frontend::is_then_keyword(tok)) break;
                        consume_token();
                        auto next = parse_step();
                        if (!next.has_value()) return next;
                        auto seq = std::make_unique<SymbolicSequenceExpr>();
                        seq->left = std::move(current);
                        seq->right = std::move(next.value());
                        const auto [l_dump, l_hash] = stored_frontend_meta_for_symbolic_expr(seq->left);
                        const auto [r_dump, r_hash] = stored_frontend_meta_for_symbolic_expr(seq->right);
                        const auto seq_expr = lithe_frontend::sequence_expr(
                            lithe::as_expr(l_hash),
                            lithe::as_expr(r_hash)
                        );
                        seq->frontend = lithe_frontend::make_meta(seq_expr);
                        current = SymbolicExpr{std::move(seq)};
                    }
                    return current;
                }

                Outcome<SymbolicPipeline> parse_pipeline() {
                    auto header = lithe_frontend::parse_pipeline_header(src);
                    if (!header.has_value()) return std::unexpected(header.error());
                    pos = header->body_start_offset;

                    auto body = parse_sequence();
                    if (!body.has_value()) return std::unexpected(body.error());
                    if (!expect("}")) return std::unexpected(PravahaError{ErrorKind::ParseError, "Expected '}'"});

                    auto root = std::move(body.value());
                    const auto [dump, hash] = stored_frontend_meta_for_symbolic_expr(root);
                    const auto pipeline_expr = lithe_frontend::pipeline_expr(
                        header->name,
                        lithe::as_expr(hash)
                    );

                    return SymbolicPipeline{
                        std::move(header->name),
                        std::move(root),
                        lithe_frontend::make_meta(pipeline_expr)
                    };
                }
            };
        } // namespace detail
    } // namespace symbolic

    inline Outcome<symbolic::SymbolicPipeline> parse_pipeline(const std::string_view text) {
        symbolic::detail::Parser parser{text};
        return parser.parse_pipeline();
    }

    // ============================================================================
    //  SECTION 11.5: SYMBOL REGISTRY & SYMBOLIC LOWERING
    // ============================================================================

    class SymbolRegistry {
        struct Entry {
            std::string name;
            TaskCommand cmd;
            TypeContract input_contract;
            TypeContract output_contract;
            ExecutionDomain domain{ExecutionDomain::CPU};
        };

        std::vector<Entry> entries_;

    public:
        SymbolRegistry() = default;

        template <typename F> requires std::move_constructible<std::decay_t<F>>
        void register_task(std::string name, F&& f) {
            std::string debug_name{name};
            using OutputT = detail::callable_payload_t<F>;
            TaskCommand cmd = TaskCommand::make(std::forward<F>(f), debug_name);
            entries_.push_back(Entry{
                std::move(name),
                std::move(cmd),
                detail::make_input_contract<F>(),
                make_type_contract<OutputT>(),
                ExecutionDomain::CPU
            });
        }

        template <typename F> requires std::move_constructible<std::decay_t<F>>
        void register_task(std::string name, const ExecutionDomain domain, F&& f) {
            std::string debug_name{name};
            using OutputT = detail::callable_payload_t<F>;
            TaskCommand cmd = TaskCommand::make(std::forward<F>(f), debug_name);
            entries_.push_back(Entry{
                std::move(name),
                std::move(cmd),
                detail::make_input_contract<F>(),
                make_type_contract<OutputT>(),
                domain
            });
        }

        void register_command(std::string name, TaskCommand cmd) {
            entries_.push_back(Entry{
                std::move(name), std::move(cmd), TypeContract{}, TypeContract{}, ExecutionDomain::CPU
            });
        }

        void register_command(std::string name, const ExecutionDomain domain, TaskCommand cmd) {
            entries_.push_back(Entry{std::move(name), std::move(cmd), TypeContract{}, TypeContract{}, domain});
        }

        bool find(const std::string& name, TaskCommand*& cmd, const TypeContract*& input_contract,
                  const TypeContract*& output_contract, const ExecutionDomain*& domain) {
            for (auto& e : entries_) {
                if (e.name == name) {
                    cmd = &e.cmd;
                    input_contract = &e.input_contract;
                    output_contract = &e.output_contract;
                    domain = &e.domain;
                    return true;
                }
            }
            cmd = nullptr;
            input_contract = nullptr;
            output_contract = nullptr;
            domain = nullptr;
            return false;
        }
    };

    namespace detail {
        inline Outcome<ExecutionDomain> parse_symbolic_domain_annotation(const std::string_view value) {
            if (value == symbolic::lithe_frontend::domain_inline) return ExecutionDomain::Inline;
            if (value == symbolic::lithe_frontend::domain_cpu) return ExecutionDomain::CPU;
            if (value == symbolic::lithe_frontend::domain_io) return ExecutionDomain::IO;
            return std::unexpected(PravahaError{
                ErrorKind::ValidationError,
                "invalid domain annotation value: " + std::string(value)
            });
        }

        inline Outcome<std::size_t> parse_symbolic_retry_annotation(const std::string_view value) {
            if (value.empty()) {
                return std::unexpected(PravahaError{
                    ErrorKind::ValidationError,
                    "invalid retry annotation value: " + std::string(value)
                });
            }
            if (!std::ranges::all_of(value, [](const unsigned char c) { return std::isdigit(c); })) {
                return std::unexpected(PravahaError{
                    ErrorKind::ValidationError,
                    "invalid retry annotation value: " + std::string(value)
                });
            }

            unsigned long long parsed = 0;
            try {
                parsed = std::stoull(std::string(value));
            }
            catch (...) {
                return std::unexpected(PravahaError{
                    ErrorKind::ValidationError,
                    "invalid retry annotation value: " + std::string(value)
                });
            }

            const auto narrowed = static_cast<std::size_t>(parsed);
            if (static_cast<unsigned long long>(narrowed) != parsed) {
                return std::unexpected(PravahaError{
                    ErrorKind::ValidationError,
                    "invalid retry annotation value: " + std::string(value)
                });
            }

            return narrowed;
        }

        inline Outcome<std::chrono::nanoseconds> parse_symbolic_timeout_annotation(const std::string_view value) {
            if (value.empty()) {
                return std::unexpected(PravahaError{
                    ErrorKind::ValidationError,
                    "invalid timeout annotation value: " + std::string(value)
                });
            }

            std::size_t unit_len = 0;
            unsigned long long multiplier = 0;
            if (value.size() >= 2 && value.ends_with("ns")) {
                unit_len = 2;
                multiplier = 1ULL;
            }
            else if (value.size() >= 2 && value.ends_with("us")) {
                unit_len = 2;
                multiplier = 1000ULL;
            }
            else if (value.size() >= 2 && value.ends_with("ms")) {
                unit_len = 2;
                multiplier = 1000ULL * 1000ULL;
            }
            else if (value.ends_with("s")) {
                unit_len = 1;
                multiplier = 1000ULL * 1000ULL * 1000ULL;
            }
            else {
                return std::unexpected(PravahaError{
                    ErrorKind::ValidationError,
                    "invalid timeout annotation value: " + std::string(value)
                });
            }

            const auto numeric = value.substr(0, value.size() - unit_len);
            if (numeric.empty() || !std::ranges::all_of(numeric,
                                                        [](const unsigned char c) { return std::isdigit(c); })) {
                return std::unexpected(PravahaError{
                    ErrorKind::ValidationError,
                    "invalid timeout annotation value: " + std::string(value)
                });
            }

            unsigned long long parsed = 0;
            try {
                parsed = std::stoull(std::string(numeric));
            }
            catch (...) {
                return std::unexpected(PravahaError{
                    ErrorKind::ValidationError,
                    "invalid timeout annotation value: " + std::string(value)
                });
            }

            constexpr auto max_rep = static_cast<unsigned long long>(std::numeric_limits<
                std::chrono::nanoseconds::rep>::max());
            if (parsed > max_rep / multiplier) {
                return std::unexpected(PravahaError{
                    ErrorKind::ValidationError,
                    "invalid timeout annotation value: " + std::string(value)
                });
            }

            const auto count_ns = parsed * multiplier;
            return std::chrono::nanoseconds{static_cast<std::chrono::nanoseconds::rep>(count_ns)};
        }

        inline Outcome<TaskPriority> parse_symbolic_priority_annotation(const std::string_view value) {
            if (value == symbolic::lithe_frontend::priority_low) return TaskPriority::Low;
            if (value == symbolic::lithe_frontend::priority_normal) return TaskPriority::Normal;
            if (value == symbolic::lithe_frontend::priority_high) return TaskPriority::High;
            return std::unexpected(PravahaError{
                ErrorKind::ValidationError,
                "invalid priority annotation value: " + std::string(value)
            });
        }

        inline Outcome<Unit> lower_symbolic_expr(const symbolic::SymbolicExpr& expr, SymbolRegistry& reg,
                                                 TaskIr& ir, std::vector<TaskId>& starts,
                                                 std::vector<TaskId>& terminals) {
            if (auto* task = std::get_if<symbolic::SymbolicTaskExpr>(&expr)) {
                const symbolic::LitheSymbolicSource source{task->frontend, task->name};
                TaskCommand* cmd_ptr = nullptr;
                const TypeContract* input_contract = nullptr;
                const TypeContract* output_contract = nullptr;
                const ExecutionDomain* default_domain = nullptr;
                if (!reg.find(task->name, cmd_ptr, input_contract, output_contract, default_domain)) {
                    std::string msg = "Symbol not found: " + task->name;
                    if (source.frontend.hash != 0) {
                        msg += " [frontend_hash=" + std::to_string(source.frontend.hash) + "]";
                    }
                    return std::unexpected(PravahaError{ErrorKind::SymbolNotFound, std::move(msg), task->name});
                }
                ExecutionDomain node_domain = default_domain ? *default_domain : ExecutionDomain::CPU;
                if (auto it = task->annotations.find(std::string{symbolic::lithe_frontend::ann_domain});
                    it != task->annotations.end()) {
                    auto parsed = parse_symbolic_domain_annotation(it->second);
                    if (!parsed.has_value()) {
                        return std::unexpected(PravahaError{
                            ErrorKind::ValidationError,
                            parsed.error().message,
                            task->name
                        });
                    }
                    node_domain = parsed.value();
                }
                std::size_t node_max_retries = 0;
                if (auto it = task->annotations.find(std::string{symbolic::lithe_frontend::ann_retry});
                    it != task->annotations.end()) {
                    auto parsed = parse_symbolic_retry_annotation(it->second);
                    if (!parsed.has_value()) {
                        return std::unexpected(PravahaError{
                            ErrorKind::ValidationError,
                            parsed.error().message,
                            task->name
                        });
                    }
                    node_max_retries = parsed.value();
                }
                std::chrono::nanoseconds node_timeout{0};
                if (auto it = task->annotations.find(std::string{symbolic::lithe_frontend::ann_timeout});
                    it != task->annotations.end()) {
                    auto parsed = parse_symbolic_timeout_annotation(it->second);
                    if (!parsed.has_value()) {
                        return std::unexpected(PravahaError{
                            ErrorKind::ValidationError,
                            parsed.error().message,
                            task->name
                        });
                    }
                    node_timeout = parsed.value();
                }
                auto node_priority = TaskPriority::Normal;
                if (auto it = task->annotations.find(std::string{symbolic::lithe_frontend::ann_priority});
                    it != task->annotations.end()) {
                    auto parsed = parse_symbolic_priority_annotation(it->second);
                    if (!parsed.has_value()) {
                        return std::unexpected(PravahaError{
                            ErrorKind::ValidationError,
                            parsed.error().message,
                            task->name
                        });
                    }
                    node_priority = parsed.value();
                }
                // Create a placeholder command that delegates to the registered one
                auto* raw = cmd_ptr;
                auto wrapper = TaskCommand::make_forwarding(raw, task->name);
                TaskId id = add_node_from_source(ir, source, node_domain, std::move(wrapper));
                if (input_contract) {
                    ir.nodes.back().input_contract = *input_contract;
                }
                if (output_contract) {
                    ir.nodes.back().output_contract = *output_contract;
                }
                ir.nodes.back().max_retries = node_max_retries;
                ir.nodes.back().timeout = node_timeout;
                ir.nodes.back().priority = node_priority;
                starts.push_back(id);
                terminals.push_back(id);
                return Unit{};
            }

            if (auto* seq_ptr = std::get_if<std::unique_ptr<symbolic::SymbolicSequenceExpr>>(&expr)) {
                auto& [left, right, frontend] = **seq_ptr;
                const symbolic::LitheSymbolicSource group_source{frontend, "symbolic.sequence"};
                (void)group_source;
                std::vector<TaskId> left_starts, left_terminals, right_starts, right_terminals;
                auto lr = lower_symbolic_expr(left, reg, ir, left_starts, left_terminals);
                if (!lr.has_value()) return lr;
                auto rr = lower_symbolic_expr(right, reg, ir, right_starts, right_terminals);
                if (!rr.has_value()) return rr;
                for (auto t : left_terminals) for (auto s : right_starts) ir.add_edge(t, s, EdgeKind::Sequence);
                starts = std::move(left_starts);
                terminals = std::move(right_terminals);
                return Unit{};
            }

            if (auto* par_ptr = std::get_if<std::unique_ptr<symbolic::SymbolicParallelExpr>>(&expr)) {
                auto& [branches, policy, frontend] = **par_ptr;
                const symbolic::LitheSymbolicSource group_source{frontend, "symbolic.parallel"};
                (void)group_source;
                for (auto& branch : branches) {
                    std::vector<TaskId> bs, bt;
                    if (auto br = lower_symbolic_expr(branch, reg, ir, bs, bt); !br.has_value()) return br;
                    for (auto s : bs) starts.push_back(s);
                    for (auto t : bt) terminals.push_back(t);
                }
                if (policy.kind == JoinPolicyKind::Quorum) {
                    if (policy.quorum_required == 0) {
                        std::string msg = "quorum validation failed: policy=Quorum quorum_required=0 branch_count="
                            + std::to_string(terminals.size());
                        if (frontend.hash != 0) {
                            msg += " frontend_hash=" + std::to_string(frontend.hash);
                        }
                        return std::unexpected(PravahaError{
                            ErrorKind::ValidationError,
                            std::move(msg)
                        });
                    }
                    if (policy.quorum_required > terminals.size()) {
                        std::string msg = "quorum validation failed: policy=Quorum quorum_required="
                            + std::to_string(policy.quorum_required)
                            + " branch_count=" + std::to_string(terminals.size());
                        if (frontend.hash != 0) {
                            msg += " frontend_hash=" + std::to_string(frontend.hash);
                        }
                        return std::unexpected(PravahaError{
                            ErrorKind::ValidationError,
                            std::move(msg)
                        });
                    }
                }
                ir.add_join_group(terminals, policy, frontend.hash, frontend.dump);
                return Unit{};
            }

            return std::unexpected(PravahaError{ErrorKind::ParseError, "Unknown symbolic expression type"});
        }
    } // namespace detail

    inline Outcome<TaskIr> lower_symbolic_pipeline(const symbolic::SymbolicPipeline& pipeline, SymbolRegistry& reg) {
        TaskIr ir;
        std::vector<TaskId> starts, terminals;
        if (auto result = detail::lower_symbolic_expr(pipeline.root, reg, ir, starts, terminals); !result.has_value())
            return std::unexpected(result.error());
        return ir;
    }

    // ============================================================================
    //  SECTION 12: PARALLEL_FOR (NAryTree hierarchy)
    // ============================================================================

    struct AlgorithmTreeNode {
        std::string name;
        std::size_t begin{0};
        std::size_t end{0};

        bool operator==(const AlgorithmTreeNode&) const = default;
    };

    using AlgorithmTree = NAryTree<AlgorithmTreeNode>;

    template <typename F>
    struct ParallelForResult {
        TaskIr ir;
        AlgorithmTree hierarchy;
        std::size_t chunk_count{0};
    };

    template <typename Range, typename F>
        requires std::invocable<F&, std::size_t, std::size_t>
    auto parallel_for_eager(const std::string& name, Range&& range, std::size_t chunk_size, F&& body) {
        auto& range_ref = range;
        auto body_fn = std::forward<F>(body);
        const std::size_t total = range_ref.size();
        if (chunk_size == 0) chunk_size = 1;
        std::size_t num_chunks = (total + chunk_size - 1) / chunk_size;

        // Build NAryTree hierarchy
        AlgorithmTree tree(AlgorithmTreeNode{name, 0, total});
        auto* root_node = tree.get_root();

        TaskIr ir;

        for (std::size_t i = 0; i < num_chunks; ++i) {
            std::size_t b = i * chunk_size;
            std::size_t e = std::min(b + chunk_size, total);

            // Add to NAryTree hierarchy
            tree.insert(root_node, AlgorithmTreeNode{name + "_chunk_" + std::to_string(i), b, e});

            std::invoke(body_fn, b, e);
        }

        return ParallelForResult<std::decay_t<F>>{std::move(ir), std::move(tree), num_chunks};
    }

    template <typename F>
    Outcome<TaskIr> lower_parallel_for(ParallelForResult<F>& pf) {
        return std::move(pf.ir);
    }

    template <typename ChunkingPolicy = StaticChunkingPolicy, typename Range, typename F>
    auto parallel_for(std::string name, Range&& range, std::size_t chunk_size, F&& body) {
        return lazy_parallel_for<ChunkingPolicy>(std::move(name), std::forward<Range>(range), chunk_size,
                                                 std::forward<F>(body));
    }

    template <typename Range, typename F>
    decltype(auto) parallel_transform_eager(Range&& range, std::size_t chunk_size, F&& transform) {
        auto& range_ref = range;
        auto transform_fn = std::forward<F>(transform);
        if (chunk_size == 0) chunk_size = 1;
        const auto total = static_cast<std::size_t>(range_ref.size());

        for (std::size_t begin = 0; begin < total; begin += chunk_size) {
            const std::size_t end = std::min(begin + chunk_size, total);
            for (std::size_t idx = begin; idx < end; ++idx) {
                std::invoke(transform_fn, range_ref[idx]);
            }
        }

        return std::forward<Range>(range);
    }

    template <typename ChunkingPolicy = StaticChunkingPolicy, typename InRange, typename OutRange, typename F>
    [[nodiscard]] auto parallel_transform(InRange&& input, OutRange&& output, F&& transform,
                                          std::size_t chunk_size = 1024) {
        return lazy_parallel_transform<ChunkingPolicy>(
            std::forward<InRange>(input),
            std::forward<OutRange>(output),
            std::forward<F>(transform),
            chunk_size
        );
    }

    template <typename T>
    struct ParallelReduceResult {
        T value;
        AlgorithmTree hierarchy;
        std::size_t chunk_count{0};
        bool has_error{false};
        PravahaError error{ErrorKind::InternalError, ""};
    };

    namespace detail {
        template <typename Backend, typename GraphAlgorithmPolicy, typename ReadyPolicy, typename NoProgressPolicy,
                  typename Observer, typename RetryPolicyT, typename TimeoutPolicyT, typename FlowControlPolicyT,
                  typename BudgetPolicyT,
                  typename Range, typename T, typename ReduceFn, typename CombineFn>
            requires std::invocable<ReduceFn, T, std::size_t, std::size_t>
            && std::invocable<CombineFn, T, T>
            && std::copy_constructible<T>
        auto parallel_reduce_eager_impl(
            Runner<Backend, GraphAlgorithmPolicy, ReadyPolicy, NoProgressPolicy, Observer, RetryPolicyT, TimeoutPolicyT,
                   FlowControlPolicyT, BudgetPolicyT>& runner,
            Range& range,
            T init,
            ReduceFn&& reduce_fn,
            CombineFn&& combine_fn,
            std::size_t chunk_size)
            -> Outcome<ParallelReduceResult<T>> {
            std::size_t total = range.size();

            if (total == 0) {
                AlgorithmTree tree(AlgorithmTreeNode{"reduce_root", 0, 0});
                return ParallelReduceResult<T>{
                    init, std::move(tree), 0, false, PravahaError{ErrorKind::InternalError, ""}
                };
            }

            if (chunk_size == 0) chunk_size = 1;
            std::size_t num_chunks = (total + chunk_size - 1) / chunk_size;

            AlgorithmTree tree(AlgorithmTreeNode{"reduce_root", 0, total});
            auto* root_node = tree.get_root();
            auto* combine_node = tree.insert(root_node, AlgorithmTreeNode{"combine", 0, total});

            auto partials = std::make_shared<std::vector<T>>(num_chunks, init);
            auto error_flag = std::make_shared<std::atomic<bool>>(false);
            auto first_error = std::make_shared<PravahaError>(ErrorKind::InternalError, "");
            auto error_mutex = std::make_shared<std::mutex>();

            TaskIr ir;
            for (std::size_t i = 0; i < num_chunks; ++i) {
                std::size_t b = i * chunk_size;
                std::size_t e = std::min(b + chunk_size, total);
                tree.insert(combine_node, AlgorithmTreeNode{"chunk_" + std::to_string(i), b, e});

                auto chunk_cmd = TaskCommand::make(
                    [i, b, e, init_val = init, &reduce_fn, partials, error_flag, first_error, error_mutex]() mutable {
                        try {
                            T partial = reduce_fn(init_val, b, e);
                            (*partials)[i] = std::move(partial);
                        }
                        catch (const std::exception& ex) {
                            error_flag->store(true, std::memory_order_release);
                            std::lock_guard lk(*error_mutex);
                            *first_error = PravahaError{
                                ErrorKind::TaskFailed,
                                std::string{"parallel_reduce chunk failed: "} + ex.what(),
                                "chunk_" + std::to_string(i)
                            };
                            throw;
                        }
                    });
                ir.add_node("chunk_" + std::to_string(i), ExecutionDomain::CPU, std::move(chunk_cmd));
            }

            auto sstate = std::make_shared<SharedSchedulerState>();
            sstate->rt = RuntimeState::build(ir);
            sstate->total_nodes = ir.nodes.size();
            sstate->count_terminals();

            {
                std::vector<std::size_t> ready_indices;
                {
                    std::lock_guard lock(sstate->mutex);
                    for (std::size_t i = 0; i < sstate->rt.node_states.size(); ++i) {
                        if (ReadyPolicy::is_ready(sstate->rt, i)) {
                            sstate->rt.node_states[i] = TaskState::Scheduled;
                            ready_indices.push_back(i);
                        }
                    }
                    sstate->note_scheduled_in_last_pass(ready_indices.size());
                    sstate->count_terminals();
                }

                for (auto idx : ready_indices) {
                    auto* node_cmd_ptr = &ir.nodes[idx].command;
                    auto wrapped = TaskCommand::make(
                        [idx, node_cmd_ptr, sstate]() mutable {
                            auto result = node_cmd_ptr->run();
                            {
                                std::lock_guard lock(sstate->mutex);
                                if (result.has_value()) {
                                    sstate->rt.mark_succeeded(idx);
                                }
                                else {
                                    sstate->rt.mark_failed(idx, std::move(result.error()));
                                }
                                sstate->count_terminals();
                            }
                            sstate->cv_done.notify_all();
                        });
                    runner.backend_ref().submit(std::move(wrapped));
                }
            }

            {
                std::unique_lock lock(sstate->mutex);
                sstate->cv_done.wait(lock, [&]() { return sstate->all_terminal(); });
            }

            if (auto run_result = sstate->rt.finalize(); run_result.final_state == TaskState::Failed) {
                std::lock_guard lk(*error_mutex);
                return std::unexpected(*first_error);
            }

            T combined = (*partials)[0];
            for (std::size_t i = 1; i < num_chunks; ++i) {
                combined = combine_fn(std::move(combined), std::move((*partials)[i]));
            }

            return ParallelReduceResult<T>{
                std::move(combined),
                std::move(tree),
                num_chunks,
                false,
                PravahaError{ErrorKind::InternalError, ""}
            };
        }
    } // namespace detail

    template <typename Backend, typename GraphAlgorithmPolicy, typename ReadyPolicy, typename NoProgressPolicy,
              typename Observer, typename RetryPolicyT, typename TimeoutPolicyT, typename FlowControlPolicyT,
              typename BudgetPolicyT,
              typename Range, typename T, typename ReduceFn, typename CombineFn>
        requires std::invocable<ReduceFn, T, std::size_t, std::size_t>
        && std::invocable<CombineFn, T, T>
        && std::copy_constructible<T>
    auto parallel_reduce_eager(
        Runner<Backend, GraphAlgorithmPolicy, ReadyPolicy, NoProgressPolicy, Observer, RetryPolicyT, TimeoutPolicyT,
               FlowControlPolicyT, BudgetPolicyT>& runner,
        Range& range,
        T init,
        ReduceFn&& reduce_fn,
        CombineFn&& combine_fn,
        std::size_t chunk_size)
        -> Outcome<ParallelReduceResult<T>> {
        return detail::parallel_reduce_eager_impl(
            runner,
            range,
            std::move(init),
            std::forward<ReduceFn>(reduce_fn),
            std::forward<CombineFn>(combine_fn),
            chunk_size
        );
    }


    template <typename ChunkingPolicy = StaticChunkingPolicy, typename ReductionPolicy = NAryTreeReductionPolicy,
              typename Range, typename Init, typename MapFn, typename ReduceFn>
    auto parallel_reduce(
        Range&& range,
        Init init,
        MapFn&& map_fn,
        ReduceFn&& reduce_fn,
        std::size_t chunk_size = 1024) {
        return lazy_parallel_reduce<ChunkingPolicy, ReductionPolicy>(
            std::forward<Range>(range),
            std::move(init),
            std::forward<MapFn>(map_fn),
            std::forward<ReduceFn>(reduce_fn),
            chunk_size
        );
    }
} // namespace pravaha
