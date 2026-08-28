#pragma once

#include "pravaha/pravaha.hpp"

#include <concepts>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace pravaha::execution {
    template <class Expr, class Backend = InlineBackend>
    struct sender {
        using expr_type = Expr;
        using backend_type = Backend;
        Expr expr;
        std::shared_ptr<Backend> backend{};
        std::optional<PravahaError> validation_error{};
    };

    template <class Sender>
        requires requires(Sender& s) { s.expr; }
    [[nodiscard]] decltype(auto) get_expr(Sender& sender_value) {
        return (sender_value.expr);
    }

    template <class Sender>
        requires requires(const Sender& s) { s.expr; }
    [[nodiscard]] decltype(auto) get_expr(const Sender& sender_value) {
        return (sender_value.expr);
    }

    template <class Sender>
        requires requires(Sender&& s) { s.expr; }
    [[nodiscard]] decltype(auto) get_expr(Sender&& sender_value) {
        return std::move(sender_value.expr);
    }

    template <class T>
    concept sender_like = requires(T s) {
        typename std::remove_cvref_t<T>::expr_type;
        typename std::remove_cvref_t<T>::backend_type;
        get_expr(s);
    };

    template <class T>
    concept receiver_like = requires(T r, const PravahaError& error) {
        r.set_value();
        r.set_error(error);
        r.set_stopped();
    };

    template <class Sender, class Receiver>
    struct operation_state {
        Sender sender;
        Receiver receiver;
    };

    template <class Sender, class Fn>
    struct upon_error_sender {
        using predecessor_type = Sender;
        using fn_type = Fn;
        using expr_type = Sender::expr_type;
        using backend_type = Sender::backend_type;
        Sender predecessor;
        Fn fn;
        std::shared_ptr<backend_type> backend{};
        std::optional<PravahaError> validation_error{};
    };

    template <class Sender, class Fn>
    struct upon_stopped_sender {
        using predecessor_type = Sender;
        using fn_type = Fn;
        using expr_type = Sender::expr_type;
        using backend_type = Sender::backend_type;
        Sender predecessor;
        Fn fn;
        std::shared_ptr<backend_type> backend{};
        std::optional<PravahaError> validation_error{};
    };

    template <class Sender, class Fn>
    [[nodiscard]] decltype(auto) get_expr(upon_error_sender<Sender, Fn>& sender_value) {
        return get_expr(sender_value.predecessor);
    }

    template <class Sender, class Fn>
    [[nodiscard]] decltype(auto) get_expr(const upon_error_sender<Sender, Fn>& sender_value) {
        return get_expr(sender_value.predecessor);
    }

    template <class Sender, class Fn>
    [[nodiscard]] decltype(auto) get_expr(upon_error_sender<Sender, Fn>&& sender_value) {
        return get_expr(std::move(sender_value.predecessor));
    }

    template <class Sender, class Fn>
    [[nodiscard]] decltype(auto) get_expr(upon_stopped_sender<Sender, Fn>& sender_value) {
        return get_expr(sender_value.predecessor);
    }

    template <class Sender, class Fn>
    [[nodiscard]] decltype(auto) get_expr(const upon_stopped_sender<Sender, Fn>& sender_value) {
        return get_expr(sender_value.predecessor);
    }

    template <class Sender, class Fn>
    [[nodiscard]] decltype(auto) get_expr(upon_stopped_sender<Sender, Fn>&& sender_value) {
        return get_expr(std::move(sender_value.predecessor));
    }

    template <class Sender>
    [[nodiscard]] auto take_backend(Sender& sender_value) {
        using backend_type = std::remove_cvref_t<Sender>::backend_type;
        if constexpr (requires { sender_value.backend; }) {
            return sender_value.backend;
        }
        else {
            return std::shared_ptr<backend_type>{};
        }
    }

    template <class Sender>
    [[nodiscard]] auto take_backend(const Sender& sender_value) {
        using backend_type = std::remove_cvref_t<Sender>::backend_type;
        if constexpr (requires { sender_value.backend; }) {
            return sender_value.backend;
        }
        else {
            return std::shared_ptr<backend_type>{};
        }
    }

    template <class Sender>
    [[nodiscard]] auto take_validation_error(Sender& sender_value) {
        if constexpr (requires { sender_value.validation_error; }) {
            return sender_value.validation_error;
        }
        else {
            return std::optional<PravahaError>{};
        }
    }

    template <class Sender>
    [[nodiscard]] auto take_validation_error(const Sender& sender_value) {
        if constexpr (requires { sender_value.validation_error; }) {
            return sender_value.validation_error;
        }
        else {
            return std::optional<PravahaError>{};
        }
    }

    template <class SenderA, class SenderB>
    [[nodiscard]] bool same_backend_instance(const SenderA& sender_a, const SenderB& sender_b) {
        auto backend_a = take_backend(sender_a);
        auto backend_b = take_backend(sender_b);
        if (!backend_a && !backend_b) {
            return true;
        }
        if (!backend_a || !backend_b) {
            return false;
        }
        return backend_a.get() == backend_b.get();
    }

    template <class Receiver, class Fn>
    struct upon_error_receiver {
        Receiver receiver;
        Fn fn;

        void set_value() { receiver.set_value(); }

        void set_error(const PravahaError& error) {
            if constexpr (std::invocable<Fn&, PravahaError>) {
                fn(error);
            }
            else if constexpr (std::invocable<Fn&, const PravahaError&>) {
                fn(error);
            }
            else {
                fn();
            }
            receiver.set_error(error);
        }

        void set_stopped() { receiver.set_stopped(); }
    };

    template <class Receiver, class Fn>
    struct upon_stopped_receiver {
        Receiver receiver;
        Fn fn;

        void set_value() { receiver.set_value(); }
        void set_error(const PravahaError& error) { receiver.set_error(error); }

        void set_stopped() {
            fn();
            receiver.set_stopped();
        }
    };

    template <class Sender, class Receiver>
        requires sender_like<std::remove_cvref_t<Sender>> && receiver_like<std::remove_cvref_t<Receiver>>
    [[nodiscard]] auto connect(Sender&& sender_value, Receiver&& receiver_value) {
        using sender_type = std::decay_t<Sender>;
        using receiver_type = std::decay_t<Receiver>;
        return operation_state<sender_type, receiver_type>{
            std::forward<Sender>(sender_value),
            std::forward<Receiver>(receiver_value)
        };
    }

    template <class Sender, class Receiver>
        requires sender_like<Sender> && receiver_like<Receiver>
    void start(operation_state<Sender, Receiver>& op) {
        if constexpr (requires { op.sender.validation_error; }) {
            if (op.sender.validation_error.has_value()) {
                op.receiver.set_error(*op.sender.validation_error);
                return;
            }
        }

        auto complete_from_submission = [&](auto& submission) {
            if (!submission.has_value()) {
                op.receiver.set_error(submission.error());
                return;
            }
            if (submission->succeeded()) {
                op.receiver.set_value();
                return;
            }
            if (submission->final_state == TaskState::Canceled || submission->final_state == TaskState::Skipped) {
                op.receiver.set_stopped();
                return;
            }
            op.receiver.set_error(PravahaError{ErrorKind::TaskFailed, "execution failed"});
        };

        if constexpr (requires { op.sender.backend; }) {
            if (op.sender.backend) {
                Runner < typename Sender::backend_type > runner{*op.sender.backend};
                auto submission = runner.submit(get_expr(std::move(op.sender)));
                complete_from_submission(submission);
                return;
            }
        }

        Runner < typename Sender::backend_type > runner;
        auto submission = runner.submit(get_expr(std::move(op.sender)));
        complete_from_submission(submission);
    }

    template <class Sender, class Fn, class Receiver>
        requires sender_like<Sender> && receiver_like<Receiver>
    void start(operation_state<upon_error_sender<Sender, Fn>, Receiver>& op) {
        auto inner_receiver = upon_error_receiver<Receiver, Fn>{std::move(op.receiver), std::move(op.sender.fn)};
        auto inner_op = connect(op.sender.predecessor, std::move(inner_receiver));
        start(inner_op);
        op.receiver = std::move(inner_op.receiver.receiver);
    }

    template <class Sender, class Fn, class Receiver>
        requires sender_like<Sender> && receiver_like<Receiver>
    void start(operation_state<upon_stopped_sender<Sender, Fn>, Receiver>& op) {
        auto inner_receiver = upon_stopped_receiver<Receiver, Fn>{std::move(op.receiver), std::move(op.sender.fn)};
        auto inner_op = connect(op.sender.predecessor, std::move(inner_receiver));
        start(inner_op);
        op.receiver = std::move(inner_op.receiver.receiver);
    }

    struct sync_wait_result {
        bool value{};
        bool error{};
        bool stopped{};
    };

    template <class T>
    struct sync_wait_value_result {
        bool value{};
        bool error{};
        bool stopped{};
        std::optional<T> result;
    };

    template <class Sender>
        requires sender_like<std::remove_cvref_t<Sender>>
    [[nodiscard]] sync_wait_result sync_wait(Sender&& sender_value) {
        struct sync_wait_receiver {
            sync_wait_result result{};

            void set_value() { result.value = true; }
            void set_error(const PravahaError&) { result.error = true; }
            void set_stopped() { result.stopped = true; }
        };

        auto op = connect(std::forward<Sender>(sender_value), sync_wait_receiver{});
        start(op);
        return op.receiver.result;
    }

    template <class T, class Sender>
        requires sender_like<std::remove_cvref_t<Sender>>
    [[nodiscard]] sync_wait_value_result<T> sync_wait_value(Sender&& sender_value) {
        sync_wait_value_result<T> out{};
        using sender_type = std::remove_cvref_t<Sender>;

        if constexpr (requires(sender_type s) {
            seq(get_expr(s), task("sync_wait_value_probe", [](T v) { return v; }));
        }) {
            std::optional<T> captured;
            auto tapped = then(
                std::forward<Sender>(sender_value),
                "sync_wait_value",
                [&captured](T v) {
                    captured = std::move(v);
                    return *captured;
                }
            );
            auto base = sync_wait(std::move(tapped));
            out.value = base.value;
            out.error = base.error;
            out.stopped = base.stopped;
            if (out.value) {
                out.result = std::move(captured);
            }
            return out;
        }

        auto base = sync_wait(std::forward<Sender>(sender_value));
        out.value = base.value;
        out.error = base.error;
        out.stopped = base.stopped;
        return out;
    }

    template <class Backend>
    struct scheduler {
        std::shared_ptr<Backend> backend;

        scheduler()
            : backend(std::make_shared<Backend>()) {}

        explicit scheduler(Backend backend_value)
            : backend(std::make_shared<Backend>(std::move(backend_value))) {}

        [[nodiscard]] Backend& get_backend() const noexcept {
            return *backend;
        }
    };

    using inline_scheduler = scheduler<InlineBackend>;
    using jthread_scheduler = scheduler<JThreadBackend>;

    template <class Backend>
    [[nodiscard]] auto schedule(scheduler<Backend> sched) {
        auto expr = task("schedule", [] {});
        using expr_type = decltype(expr);
        return sender<expr_type, Backend>{
            std::move(expr),
            std::move(sched.backend)
        };
    }

    template <class Sender, class Fn>
        requires sender_like<std::remove_cvref_t<Sender>> && std::move_constructible<std::decay_t<Fn>>
    [[nodiscard]] auto then(Sender&& predecessor, std::string_view name, Fn&& fn) {
        using sender_type = std::decay_t<Sender>;
        sender_type predecessor_sender = std::forward<Sender>(predecessor);
        auto backend = take_backend(predecessor_sender);
        auto validation_error = take_validation_error(predecessor_sender);
        auto continuation = task(name, std::forward<Fn>(fn));
        auto expr = seq(get_expr(std::move(predecessor_sender)), std::move(continuation));
        using expr_type = decltype(expr);
        return sender<expr_type, typename sender_type::backend_type>{
            std::move(expr),
            std::move(backend),
            std::move(validation_error)
        };
    }

    template <class Sender, class Fn>
        requires sender_like<std::remove_cvref_t<Sender>> && std::move_constructible<std::decay_t<Fn>>
    [[nodiscard]] auto then(Sender&& predecessor, Fn&& fn) {
        return then(std::forward<Sender>(predecessor), std::string_view{"then"}, std::forward<Fn>(fn));
    }

    template <class SenderA, class SenderB, class... SenderRest>
        requires sender_like<std::remove_cvref_t<SenderA>>
        && sender_like<std::remove_cvref_t<SenderB>>
        && (sender_like<std::remove_cvref_t<SenderRest>> && ...)
    [[nodiscard]] auto when_all(SenderA&& sender_a, SenderB&& sender_b, SenderRest&&... sender_rest) {
        using sender_a_type = std::decay_t<SenderA>;
        sender_a_type sender_a_value = std::forward<SenderA>(sender_a);
        auto backend = take_backend(sender_a_value);
        static_assert(std::same_as<typename std::decay_t<SenderB>::backend_type, typename sender_a_type::backend_type>);
        static_assert(
            (std::same_as<typename std::decay_t<SenderRest>::backend_type, typename sender_a_type::backend_type> && ...
            ));
        bool consistent_backend = same_backend_instance(sender_a_value, sender_b)
            && (same_backend_instance(sender_a_value, sender_rest) && ...);
        std::optional<PravahaError> validation_error;
        if (!consistent_backend) {
            validation_error = PravahaError{
                ErrorKind::ExecutorUnavailable,
                "when_all requires matching backend instances"
            };
            backend.reset();
        }
        auto expr = all_of(
            get_expr(std::move(sender_a_value)),
            get_expr(std::forward<SenderB>(sender_b)),
            get_expr(std::forward<SenderRest>(sender_rest))...
        );
        using expr_type = decltype(expr);
        return sender<expr_type, typename sender_a_type::backend_type>{
            std::move(expr),
            std::move(backend),
            std::move(validation_error)
        };
    }

    template <class Sender, class Fn>
        requires sender_like<std::remove_cvref_t<Sender>> && std::move_constructible<std::decay_t<Fn>>
    [[nodiscard]] auto tap_error(Sender&& predecessor, Fn&& fn) {
        using sender_type = std::decay_t<Sender>;
        using fn_type = std::decay_t<Fn>;
        sender_type predecessor_sender = std::forward<Sender>(predecessor);
        auto backend = take_backend(predecessor_sender);
        auto validation_error = take_validation_error(predecessor_sender);
        return upon_error_sender<sender_type, fn_type>{
            std::move(predecessor_sender),
            std::forward<Fn>(fn),
            std::move(backend),
            std::move(validation_error)
        };
    }

    template <class Sender, class Fn>
        requires sender_like<std::remove_cvref_t<Sender>> && std::move_constructible<std::decay_t<Fn>>
    [[nodiscard]] auto tap_stopped(Sender&& predecessor, Fn&& fn) {
        using sender_type = std::decay_t<Sender>;
        using fn_type = std::decay_t<Fn>;
        sender_type predecessor_sender = std::forward<Sender>(predecessor);
        auto backend = take_backend(predecessor_sender);
        auto validation_error = take_validation_error(predecessor_sender);
        return upon_stopped_sender<sender_type, fn_type>{
            std::move(predecessor_sender),
            std::forward<Fn>(fn),
            std::move(backend),
            std::move(validation_error)
        };
    }

    template <class Sender, class Fn>
        requires sender_like<std::remove_cvref_t<Sender>> && std::move_constructible<std::decay_t<Fn>>
    [[nodiscard]] auto upon_error(Sender&& predecessor, Fn&& fn) {
        return tap_error(std::forward<Sender>(predecessor), std::forward<Fn>(fn));
    }

    template <class Sender, class Fn>
        requires sender_like<std::remove_cvref_t<Sender>> && std::move_constructible<std::decay_t<Fn>>
    [[nodiscard]] auto upon_stopped(Sender&& predecessor, Fn&& fn) {
        return tap_stopped(std::forward<Sender>(predecessor), std::forward<Fn>(fn));
    }

    template <class Backend = InlineBackend, class Expr>
    [[nodiscard]] auto from_expr(Expr&& expr) {
        using expr_type = std::decay_t<Expr>;
        return sender<expr_type, Backend>{
            std::forward<Expr>(expr),
            std::shared_ptr<Backend>{}
        };
    }

    template <class Backend, class Expr>
    [[nodiscard]] auto from_expr(scheduler<Backend> sched, Expr&& expr) {
        using expr_type = std::decay_t<Expr>;
        return sender<expr_type, Backend>{
            std::forward<Expr>(expr),
            std::move(sched.backend)
        };
    }
}
