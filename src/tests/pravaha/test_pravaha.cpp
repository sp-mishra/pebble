// ============================================================================
// test_pravaha.cpp — Unit tests for pravaha.hpp
// ============================================================================

#include "catch_amalgamated.hpp"
#include "pravaha/pravaha.hpp"

#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <deque>
#include <future>
#include <mutex>
#include <chrono>
#include <thread>

// ============================================================================
// Test types for concept validation
// ============================================================================

struct TrivialPoint {
    double x;
    double y;
};

struct NonTrivial {
    std::string name;
    int value;
};

struct MoveOnly {
    std::unique_ptr<int> data;

    MoveOnly() = default;

    MoveOnly(MoveOnly&&) = default;

    MoveOnly& operator=(MoveOnly&&) = default;

    MoveOnly(const MoveOnly&) = delete;

    MoveOnly& operator=(const MoveOnly&) = delete;
};

struct WithPointer {
    int* ptr;
    int val;
};

struct IntWrapper {
    int value;
};

int triple_value(int v) {
    return v * 3;
}

struct AddTwoFunctor {
    int operator()(int v) const {
        return v + 2;
    }
};

struct OutcomeAddOneFunctor {
    pravaha::Outcome<int> operator()(int v) const {
        return v + 1;
    }
};

// ============================================================================
// SECTION 1: ErrorKind Enum
// ============================================================================

TEST_CASE (



"ErrorKind values are distinct"
,
"[pravaha][error]"
)
 {
    STATIC_REQUIRE(pravaha::ErrorKind::ParseError != pravaha::ErrorKind::ValidationError);
    STATIC_REQUIRE(pravaha::ErrorKind::CycleDetected != pravaha::ErrorKind::SymbolNotFound);
    STATIC_REQUIRE(pravaha::ErrorKind::TypeMismatch != pravaha::ErrorKind::ExecutorUnavailable);
    STATIC_REQUIRE(pravaha::ErrorKind::TaskFailed != pravaha::ErrorKind::TaskCanceled);
    STATIC_REQUIRE(pravaha::ErrorKind::QueueRejected != pravaha::ErrorKind::Timeout);
    STATIC_REQUIRE(pravaha::ErrorKind::InternalError != pravaha::ErrorKind::ParseError);
}

TEST_CASE (



"ErrorKind covers all categories"
,
"[pravaha][error]"
)
 {
    auto check = [](pravaha::ErrorKind k) { return static_cast<int>(k) >= 0; };
    REQUIRE(check(pravaha::ErrorKind::ParseError));
    REQUIRE(check(pravaha::ErrorKind::ValidationError));
    REQUIRE(check(pravaha::ErrorKind::CycleDetected));
    REQUIRE(check(pravaha::ErrorKind::SymbolNotFound));
    REQUIRE(check(pravaha::ErrorKind::TypeMismatch));
    REQUIRE(check(pravaha::ErrorKind::ExecutorUnavailable));
    REQUIRE(check(pravaha::ErrorKind::DomainConstraintViolation));
    REQUIRE(check(pravaha::ErrorKind::PayloadNotSerializable));
    REQUIRE(check(pravaha::ErrorKind::PayloadNotTransferable));
    REQUIRE(check(pravaha::ErrorKind::TaskFailed));
    REQUIRE(check(pravaha::ErrorKind::TaskCanceled));
    REQUIRE(check(pravaha::ErrorKind::QueueRejected));
    REQUIRE(check(pravaha::ErrorKind::Timeout));
    REQUIRE(check(pravaha::ErrorKind::InternalError));
}

// ============================================================================
// SECTION 2: PravahaError
// ============================================================================

TEST_CASE (



"PravahaError construction"
,
"[pravaha][error]"
)
 {
    auto err = pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "something went wrong", "task_42"};
    REQUIRE(err.kind == pravaha::ErrorKind::TaskFailed);
    REQUIRE(err.message == "something went wrong");
    REQUIRE(err.task_identity == "task_42");
    REQUIRE(err.location.line() > 0);
}

TEST_CASE (



"PravahaError::make factory"
,
"[pravaha][error]"
)
 {
    auto err = pravaha::PravahaError::make(pravaha::ErrorKind::CycleDetected, "cycle found in DAG");
    REQUIRE(err.kind == pravaha::ErrorKind::CycleDetected);
    REQUIRE(err.message == "cycle found in DAG");
    REQUIRE(err.task_identity.empty());
}

TEST_CASE (



"PravahaError::make_for_task factory"
,
"[pravaha][error]"
)
 {
    auto err = pravaha::PravahaError::make_for_task(pravaha::ErrorKind::Timeout, "deadline exceeded", "expensive_task");
    REQUIRE(err.kind == pravaha::ErrorKind::Timeout);
    REQUIRE(err.message == "deadline exceeded");
    REQUIRE(err.task_identity == "expensive_task");
}

// ============================================================================
// SECTION 3: Outcome<T>
// ============================================================================

TEST_CASE (



"Outcome<int> success"
,
"[pravaha][outcome]"
)
 {
    pravaha::Outcome<int> result{42};
    REQUIRE(result.has_value());
    REQUIRE(result.value() == 42);
}

TEST_CASE (



"Outcome<int> error"
,
"[pravaha][outcome]"
)
 {
    pravaha::Outcome<int> result = std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "oops"});
    REQUIRE(!result.has_value());
    REQUIRE(result.error().kind == pravaha::ErrorKind::TaskFailed);
    REQUIRE(result.error().message == "oops");
}

TEST_CASE (



"Outcome<Unit> success"
,
"[pravaha][outcome]"
)
 {
    pravaha::Outcome<pravaha::Unit> result{pravaha::Unit{}};
    REQUIRE(result.has_value());
}

TEST_CASE (



"Outcome<string> success"
,
"[pravaha][outcome]"
)
 {
    pravaha::Outcome<std::string> result{"hello"};
    REQUIRE(result.has_value());
    REQUIRE(result.value() == "hello");
}

TEST_CASE (



"Outcome monadic chaining with and_then"
,
"[pravaha][outcome]"
)
 {
    auto step1 = []() -> pravaha::Outcome<int> { return 10; };
    auto step2 = [](int v) -> pravaha::Outcome<int> { return v * 2; };
    auto result = step1().and_then(step2);
    REQUIRE(result.has_value());
    REQUIRE(result.value() == 20);
}

TEST_CASE (



"Outcome monadic chaining propagates error"
,
"[pravaha][outcome]"
)
 {
    auto step1 = []() -> pravaha::Outcome<int> {
        return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::InternalError, "fail"});
    };
    auto step2 = [](int v) -> pravaha::Outcome<int> { return v * 2; };
    auto result = step1().and_then(step2);
    REQUIRE(!result.has_value());
    REQUIRE(result.error().kind == pravaha::ErrorKind::InternalError);
}

// ============================================================================
// SECTION 4: TaskState Enum
// ============================================================================

TEST_CASE (



"TaskState values"
,
"[pravaha][state]"
)
 {
    STATIC_REQUIRE(pravaha::TaskState::Created != pravaha::TaskState::Ready);
    STATIC_REQUIRE(pravaha::TaskState::Scheduled != pravaha::TaskState::Running);
    STATIC_REQUIRE(pravaha::TaskState::Succeeded != pravaha::TaskState::Failed);
    STATIC_REQUIRE(pravaha::TaskState::Canceled != pravaha::TaskState::Skipped);
}

TEST_CASE (



"TaskState all values accessible"
,
"[pravaha][state]"
)
 {
    auto states = std::array{
        pravaha::TaskState::Created, pravaha::TaskState::Ready,
        pravaha::TaskState::Scheduled, pravaha::TaskState::Running,
        pravaha::TaskState::Succeeded, pravaha::TaskState::Failed,
        pravaha::TaskState::Canceled, pravaha::TaskState::Skipped
    };
    REQUIRE(states.size() == 8);
    for (std::size_t i = 0; i < states.size(); ++i)
        for (std::size_t j = i + 1; j < states.size(); ++j)
            REQUIRE(states[i] != states[j]);
}

TEST_CASE (



"EventKind values are distinct"
,
"[pravaha][observer]"
)
 {
    STATIC_REQUIRE(pravaha::EventKind::TaskReady != pravaha::EventKind::TaskScheduled);
    STATIC_REQUIRE(pravaha::EventKind::TaskStarted != pravaha::EventKind::TaskCompleted);
    STATIC_REQUIRE(pravaha::EventKind::TaskCompleted != pravaha::EventKind::PayloadForwarded);
    STATIC_REQUIRE(pravaha::EventKind::TaskFailed != pravaha::EventKind::TaskSkipped);
    STATIC_REQUIRE(pravaha::EventKind::TaskCanceled != pravaha::EventKind::JoinResolved);
    STATIC_REQUIRE(pravaha::EventKind::GraphLowered != pravaha::EventKind::GraphValidated);
}

TEST_CASE (



"Observer event payloads are constructible"
,
"[pravaha][observer]"
)
 {
    const pravaha::TaskEvent task_event{
        pravaha::EventKind::TaskReady,
        pravaha::TaskId{7},
        "task_a",
        pravaha::TaskState::Ready,
        123
    };
    REQUIRE(task_event.kind == pravaha::EventKind::TaskReady);
    REQUIRE(task_event.task_id == pravaha::TaskId{7});
    REQUIRE(task_event.task_name == "task_a");
    REQUIRE(task_event.state == pravaha::TaskState::Ready);
    REQUIRE(task_event.frontend_hash == 123);

    const pravaha::JoinEvent join_event{
        pravaha::EventKind::JoinResolved,
        2,
        pravaha::JoinPolicy{pravaha::JoinPolicyKind::CollectAll, 0},
        true,
        4,
        3,
        1,
        0,
        0
    };
    REQUIRE(join_event.kind == pravaha::EventKind::JoinResolved);
    REQUIRE(join_event.group_id == 2);
    REQUIRE(join_event.policy.kind == pravaha::JoinPolicyKind::CollectAll);
    REQUIRE(join_event.success);
    REQUIRE(join_event.expected == 4);
    REQUIRE(join_event.succeeded == 3);
    REQUIRE(join_event.failed == 1);

    const pravaha::GraphEvent graph_event{
        pravaha::EventKind::GraphLowered,
        10,
        12,
        3
    };
    REQUIRE(graph_event.kind == pravaha::EventKind::GraphLowered);
    REQUIRE(graph_event.node_count == 10);
    REQUIRE(graph_event.edge_count == 12);
    REQUIRE(graph_event.join_group_count == 3);
}

TEST_CASE (



"NoObserver satisfies observer policy and no-op methods compile"
,
"[pravaha][observer]"
)
 {
    STATIC_REQUIRE(pravaha::ObserverPolicy<pravaha::NoObserver>);

    const pravaha::TaskEvent task_event{};
    const pravaha::JoinEvent join_event{};
    const pravaha::GraphEvent graph_event{};

    REQUIRE_FALSE(pravaha::NoObserver::enabled);
    pravaha::NoObserver::on_task_event(task_event);
    pravaha::NoObserver::on_join_event(join_event);
    pravaha::NoObserver::on_graph_event(graph_event);
    SUCCEED();
}

TEST_CASE (



"NoRetryPolicy satisfies retry policy concept"
,
"[pravaha][retry_policy][compile]"
)
 {
    STATIC_REQUIRE(pravaha::RetryPolicy<pravaha::NoRetryPolicy>);
    const auto decision = pravaha::NoRetryPolicy::on_failure(
        pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "x"},
        0,
        0
    );
    REQUIRE(decision == pravaha::RetryDecision::FailFinal);
}

TEST_CASE (



"Runner accepts custom retry policy type"
,
"[pravaha][retry_policy][compile]"
)
 {
    struct AlwaysFailFinalPolicy {
        static pravaha::RetryDecision on_failure(const pravaha::PravahaError &, std::size_t, std::size_t) noexcept {
            return pravaha::RetryDecision::FailFinal;
        }
    };

    STATIC_REQUIRE(pravaha::RetryPolicy<AlwaysFailFinalPolicy>);
    pravaha::Runner<pravaha::InlineBackend, pravaha::DefaultGraphAlgorithmPolicy, pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy, pravaha::NoObserver, AlwaysFailFinalPolicy> runner;
    auto result = runner.submit(pravaha::task("a", []() {
    }));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
}

TEST_CASE (



"CooperativeTimeoutPolicy satisfies timeout policy concept"
,
"[pravaha][timeout_policy][compile]"
)
 {
    STATIC_REQUIRE(pravaha::TimeoutPolicy<pravaha::CooperativeTimeoutPolicy>);
    REQUIRE(pravaha::CooperativeTimeoutPolicy::on_timeout(std::chrono::nanoseconds{1}));
}

TEST_CASE (



"NoTimeoutPolicy satisfies timeout policy concept"
,
"[pravaha][timeout_policy][compile]"
)
 {
    STATIC_REQUIRE(pravaha::TimeoutPolicy<pravaha::NoTimeoutPolicy>);
    REQUIRE_FALSE(pravaha::NoTimeoutPolicy::on_timeout(std::chrono::nanoseconds{1}));
}

TEST_CASE (



"Runner accepts custom timeout policy type"
,
"[pravaha][timeout_policy][compile]"
)
 {
    struct NeverTimeoutPolicy {
        static bool on_timeout(std::chrono::nanoseconds) noexcept {
            return false;
        }
    };

    STATIC_REQUIRE(pravaha::TimeoutPolicy<NeverTimeoutPolicy>);
    pravaha::Runner<pravaha::InlineBackend, pravaha::DefaultGraphAlgorithmPolicy, pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy, pravaha::NoObserver, pravaha::NoRetryPolicy, NeverTimeoutPolicy> runner;
    auto result = runner.submit(pravaha::task("a", []() {
    }));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
}

TEST_CASE (



"RejectOnFullPolicy satisfies flow control policy concept"
,
"[pravaha][flow_control_policy][compile]"
)
 {
    STATIC_REQUIRE(pravaha::FlowControlPolicy<pravaha::RejectOnFullPolicy>);
    const auto decision = pravaha::RejectOnFullPolicy::on_submit_rejected(
        pravaha::PravahaError{pravaha::ErrorKind::QueueRejected, "q"}
    );
    REQUIRE(decision == pravaha::SubmitDecision::Reject);
}

TEST_CASE (



"Runner accepts custom flow control policy type"
,
"[pravaha][flow_control_policy][compile]"
)
 {
    struct WouldBlockPolicy {
        static pravaha::SubmitDecision on_submit_rejected(const pravaha::PravahaError &) noexcept {
            return pravaha::SubmitDecision::WouldBlock;
        }
    };

    STATIC_REQUIRE(pravaha::FlowControlPolicy<WouldBlockPolicy>);
    pravaha::Runner<pravaha::InlineBackend, pravaha::DefaultGraphAlgorithmPolicy, pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy, pravaha::NoObserver, pravaha::NoRetryPolicy, pravaha::NoTimeoutPolicy,
        WouldBlockPolicy> runner;
    auto result = runner.submit(pravaha::task("a", []() {
    }));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
}

// ============================================================================
// SECTION 5: JoinPolicyKind and ExecutionDomain Enums
// ============================================================================

TEST_CASE (



"JoinPolicyKind values"
,
"[pravaha][policy]"
)
 {
    STATIC_REQUIRE(pravaha::JoinPolicyKind::AllOrNothing != pravaha::JoinPolicyKind::CollectAll);
    STATIC_REQUIRE(pravaha::JoinPolicyKind::AnySuccess != pravaha::JoinPolicyKind::Quorum);
}

TEST_CASE (



"JoinPolicy data model"
,
"[pravaha][policy]"
)
 {
    const pravaha::JoinPolicy any_success{pravaha::JoinPolicyKind::AnySuccess, 0};
    REQUIRE(any_success.kind == pravaha::JoinPolicyKind::AnySuccess);
    REQUIRE(any_success.quorum_required == 0);

    const pravaha::JoinPolicy quorum{pravaha::JoinPolicyKind::Quorum, 2};
    REQUIRE(quorum.kind == pravaha::JoinPolicyKind::Quorum);
    REQUIRE(quorum.quorum_required == 2);
}

TEST_CASE (



"ExecutionDomain values"
,
"[pravaha][domain]"
)
 {
    auto domains = std::array{
        pravaha::ExecutionDomain::Inline, pravaha::ExecutionDomain::CPU,
        pravaha::ExecutionDomain::IO, pravaha::ExecutionDomain::Fiber,
        pravaha::ExecutionDomain::Coroutine, pravaha::ExecutionDomain::External
    };
    REQUIRE(domains.size() == 6);
    for (std::size_t i = 0; i < domains.size(); ++i)
        for (std::size_t j = i + 1; j < domains.size(); ++j)
            REQUIRE(domains[i] != domains[j]);
}

// ============================================================================
// SECTION 6: Payload Concept
// ============================================================================

TEST_CASE (



"Payload concept - basic types"
,
"[pravaha][concepts]"
)
 {
    STATIC_REQUIRE(pravaha::Payload<int>);
    STATIC_REQUIRE(pravaha::Payload<double>);
    STATIC_REQUIRE(pravaha::Payload<std::string>);
    STATIC_REQUIRE(pravaha::Payload<std::vector<int>>);
    STATIC_REQUIRE(pravaha::Payload<TrivialPoint>);
    STATIC_REQUIRE(pravaha::Payload<NonTrivial>);
    STATIC_REQUIRE(pravaha::Payload<MoveOnly>);
}

TEST_CASE (



"LocalPayload concept"
,
"[pravaha][concepts]"
)
 {
    STATIC_REQUIRE(pravaha::LocalPayload<int>);
    STATIC_REQUIRE(pravaha::LocalPayload<std::string>);
    STATIC_REQUIRE(pravaha::LocalPayload<MoveOnly>);
    STATIC_REQUIRE(pravaha::LocalPayload<TrivialPoint>);
}

// ============================================================================
// SECTION 7: TransferablePayload Concept
// ============================================================================

TEST_CASE (



"TransferablePayload concept - memory-safe transferable types"
,
"[pravaha][concepts]"
)
 {
    STATIC_REQUIRE(pravaha::TransferablePayload<int>);
    STATIC_REQUIRE(pravaha::TransferablePayload<double>);
    STATIC_REQUIRE(pravaha::TransferablePayload<TrivialPoint>);
}

TEST_CASE (



"TransferablePayload concept - non-trivial or heap-owning types excluded"
,
"[pravaha][concepts]"
)
 {
    STATIC_REQUIRE(!pravaha::TransferablePayload<std::string>);
    STATIC_REQUIRE(!pravaha::TransferablePayload<std::vector<int>>);
    STATIC_REQUIRE(!pravaha::TransferablePayload<NonTrivial>);
}

TEST_CASE (



"TransferablePayload concept - move-only types excluded"
,
"[pravaha][concepts]"
)
 {
    STATIC_REQUIRE(!pravaha::TransferablePayload<MoveOnly>);
}

TEST_CASE (



"CopyablePayload concept - local copyability"
,
"[pravaha][concepts]"
)
 {
    STATIC_REQUIRE(pravaha::CopyablePayload<std::string>);
    STATIC_REQUIRE(pravaha::CopyablePayload<std::vector<int>>);
    STATIC_REQUIRE(!pravaha::CopyablePayload<MoveOnly>);
}

// ============================================================================
// SECTION 8: SerializablePayload Concept
// ============================================================================

TEST_CASE (



"SerializablePayload concept - trivial aggregates"
,
"[pravaha][concepts]"
)
 {
    STATIC_REQUIRE(pravaha::SerializablePayload<TrivialPoint>);
    STATIC_REQUIRE(pravaha::SerializablePayload<IntWrapper>);
    STATIC_REQUIRE(pravaha::SerializablePayload<int>);
    STATIC_REQUIRE(pravaha::SerializablePayload<double>);
}

TEST_CASE (



"SerializablePayload concept - non-trivial types excluded"
,
"[pravaha][concepts]"
)
 {
    STATIC_REQUIRE(!pravaha::SerializablePayload<std::string>);
    STATIC_REQUIRE(!pravaha::SerializablePayload<std::vector<int>>);
    STATIC_REQUIRE(!pravaha::SerializablePayload<NonTrivial>);
    STATIC_REQUIRE(!pravaha::SerializablePayload<MoveOnly>);
}

TEST_CASE (



"SerializablePayload concept - pointer types excluded via meta"
,
"[pravaha][concepts]"
)
 {
    STATIC_REQUIRE(!pravaha::SerializablePayload<WithPointer>);
}

// ============================================================================
// SECTION 9: InvocableTask Concept
// ============================================================================

TEST_CASE (



"InvocableTask concept - valid task callable"
,
"[pravaha][concepts]"
)
 {
    auto valid_task = [](int x) -> pravaha::Outcome<int> { return x * 2; };
    STATIC_REQUIRE(pravaha::InvocableTask<decltype(valid_task), int, int>);
}

TEST_CASE (



"InvocableTask concept - no-arg task"
,
"[pravaha][concepts]"
)
 {
    auto no_arg_task = []() -> pravaha::Outcome<pravaha::Unit> { return pravaha::Unit{}; };
    STATIC_REQUIRE(pravaha::InvocableTask<decltype(no_arg_task), pravaha::Unit>);
}

TEST_CASE (



"InvocableTask concept - wrong return type excluded"
,
"[pravaha][concepts]"
)
 {
    auto wrong_return = [](int x) -> int { return x; };
    STATIC_REQUIRE(!pravaha::InvocableTask<decltype(wrong_return), int, int>);
}

// ============================================================================
// SECTION 10: Unit type
// ============================================================================

TEST_CASE (



"Unit is monostate"
,
"[pravaha][unit]"
)
 {
    STATIC_REQUIRE(std::same_as<pravaha::Unit, std::monostate>);
    pravaha::Unit u1{};
    pravaha::Unit u2{};
    REQUIRE(u1 == u2);
}

// ============================================================================
// SECTION 11: Cancellation Primitives
// ============================================================================

TEST_CASE (



"CancellationToken - fresh token is not canceled"
,
"[pravaha][cancellation]"
)
 {
    pravaha::CancellationSource source;
    auto tok = source.token();
    REQUIRE(!tok.stop_requested());
    REQUIRE(tok.has_state());
}

TEST_CASE (



"CancellationToken - default empty token is not canceled"
,
"[pravaha][cancellation]"
)
 {
    pravaha::CancellationToken empty_tok;
    REQUIRE(!empty_tok.stop_requested());
    REQUIRE(!empty_tok.has_state());
}

TEST_CASE (



"CancellationSource - request_stop changes state"
,
"[pravaha][cancellation]"
)
 {
    pravaha::CancellationSource source;
    auto tok = source.token();
    REQUIRE(!tok.stop_requested());
    REQUIRE(!source.stop_requested());
    source.request_stop();
    REQUIRE(tok.stop_requested());
    REQUIRE(source.stop_requested());
}

TEST_CASE (



"CancellationSource - repeated request_stop is safe"
,
"[pravaha][cancellation]"
)
 {
    pravaha::CancellationSource source;
    auto tok = source.token();
    source.request_stop();
    source.request_stop();
    source.request_stop();
    REQUIRE(tok.stop_requested());
    REQUIRE(source.stop_requested());
}

TEST_CASE (



"CancellationSource - multiple tokens observe same state"
,
"[pravaha][cancellation]"
)
 {
    pravaha::CancellationSource source;
    auto tok1 = source.token();
    auto tok2 = source.token();
    REQUIRE(!tok1.stop_requested());
    REQUIRE(!tok2.stop_requested());
    source.request_stop();
    REQUIRE(tok1.stop_requested());
    REQUIRE(tok2.stop_requested());
}

TEST_CASE (



"CancellationScope - root scope not canceled initially"
,
"[pravaha][cancellation]"
)
 {
    pravaha::CancellationScope scope;
    REQUIRE(!scope.stop_requested());
}

TEST_CASE (



"CancellationScope - local request_stop"
,
"[pravaha][cancellation]"
)
 {
    pravaha::CancellationScope scope;
    scope.request_stop();
    REQUIRE(scope.stop_requested());
}

TEST_CASE (



"CancellationScope - child scope observes parent cancellation"
,
"[pravaha][cancellation]"
)
 {
    pravaha::CancellationSource parent_source;
    pravaha::CancellationScope child_scope{parent_source.token()};
    REQUIRE(!child_scope.stop_requested());
    parent_source.request_stop();
    REQUIRE(child_scope.stop_requested());
}

TEST_CASE (



"CancellationScope - child local cancel does not affect parent"
,
"[pravaha][cancellation]"
)
 {
    pravaha::CancellationSource parent_source;
    pravaha::CancellationScope child_scope{parent_source.token()};
    child_scope.request_stop();
    REQUIRE(child_scope.stop_requested());
    REQUIRE(!parent_source.stop_requested());
}

TEST_CASE (



"CancellationScope - token from scope"
,
"[pravaha][cancellation]"
)
 {
    pravaha::CancellationScope scope;
    auto tok = scope.token();
    REQUIRE(!tok.stop_requested());
    scope.request_stop();
    REQUIRE(tok.stop_requested());
}

TEST_CASE (



"CancellationScope - nested scopes"
,
"[pravaha][cancellation]"
)
 {
    pravaha::CancellationSource root_source;
    pravaha::CancellationScope mid_scope{root_source.token()};
    pravaha::CancellationScope leaf_scope{mid_scope.token()};
    REQUIRE(!leaf_scope.stop_requested());
    mid_scope.request_stop();
    REQUIRE(leaf_scope.stop_requested());
    REQUIRE(!root_source.stop_requested());
}

// ============================================================================
// SECTION 12: TaskCommand
// ============================================================================

TEST_CASE (



"TaskCommand - stores lambda and runs it"
,
"[pravaha][taskcommand]"
)
 {
    int counter = 0;
    auto cmd = pravaha::TaskCommand::make([&counter]() { ++counter; }, "increment");
    REQUIRE(cmd.has_value());
    REQUIRE(!cmd.empty());
    REQUIRE(cmd.name() == "increment");
    auto result = cmd.run();
    REQUIRE(result.has_value());
    REQUIRE(counter == 1);
}

TEST_CASE (



"TaskCommand - callable runs exactly once when run once"
,
"[pravaha][taskcommand]"
)
 {
    int counter = 0;
    auto cmd = pravaha::TaskCommand::make([&counter]() { ++counter; });
    cmd.run();
    REQUIRE(counter == 1);
    cmd.run();
    REQUIRE(counter == 2);
}

TEST_CASE (



"TaskCommand - move-only lambda works"
,
"[pravaha][taskcommand]"
)
 {
    auto ptr = std::make_unique<int>(42);
    int *raw = ptr.get();
    auto cmd = pravaha::TaskCommand::make([p = std::move(ptr)]() mutable { *p += 1; }, "move_only_task");
    REQUIRE(cmd.has_value());
    auto result = cmd.run();
    REQUIRE(result.has_value());
    REQUIRE(*raw == 43);
}

TEST_CASE (



"TaskCommand - moved-from command is empty"
,
"[pravaha][taskcommand]"
)
 {
    int counter = 0;
    auto cmd1 = pravaha::TaskCommand::make([&counter]() { ++counter; });
    REQUIRE(cmd1.has_value());
    auto cmd2 = std::move(cmd1);
    REQUIRE(cmd1.empty());
    REQUIRE(!cmd1.has_value());
    REQUIRE(cmd2.has_value());
    auto result = cmd2.run();
    REQUIRE(result.has_value());
    REQUIRE(counter == 1);
}

TEST_CASE (



"TaskCommand - move assignment"
,
"[pravaha][taskcommand]"
)
 {
    int a_counter = 0, b_counter = 0;
    auto cmd_a = pravaha::TaskCommand::make([&a_counter]() { ++a_counter; });
    auto cmd_b = pravaha::TaskCommand::make([&b_counter]() { ++b_counter; });
    cmd_a = std::move(cmd_b);
    REQUIRE(cmd_b.empty());
    REQUIRE(cmd_a.has_value());
    cmd_a.run();
    REQUIRE(a_counter == 0);
    REQUIRE(b_counter == 1);
}

TEST_CASE (



"TaskCommand - exception becomes error"
,
"[pravaha][taskcommand]"
)
 {
    auto cmd = pravaha::TaskCommand::make([]() { throw std::runtime_error("task exploded"); });
    auto result = cmd.run();
    REQUIRE(!result.has_value());
    REQUIRE(result.error().kind == pravaha::ErrorKind::TaskFailed);
    REQUIRE(result.error().message.find("task exploded") != std::string::npos);
}

TEST_CASE (



"TaskCommand - unknown exception becomes Outcome error"
,
"[pravaha][taskcommand]"
)
 {
    auto cmd = pravaha::TaskCommand::make([]() { throw 42; });
    auto result = cmd.run();
    REQUIRE(!result.has_value());
    REQUIRE(result.error().kind == pravaha::ErrorKind::TaskFailed);
    REQUIRE(result.error().message.find("unknown") != std::string::npos);
}

TEST_CASE (



"TaskCommand - default constructed is empty"
,
"[pravaha][taskcommand]"
)
 {
    pravaha::TaskCommand cmd;
    REQUIRE(cmd.empty());
    REQUIRE(!cmd.has_value());
    REQUIRE(!static_cast<bool>(cmd));
    auto result = cmd.run();
    REQUIRE(!result.has_value());
    REQUIRE(result.error().kind == pravaha::ErrorKind::TaskFailed);
}

TEST_CASE (



"TaskCommand - debug name preserved through move"
,
"[pravaha][taskcommand]"
)
 {
    auto cmd1 = pravaha::TaskCommand::make([]() {
    }, "my_task");
    REQUIRE(cmd1.name() == "my_task");
    auto cmd2 = std::move(cmd1);
    REQUIRE(cmd2.name() == "my_task");
    REQUIRE(cmd1.name().empty());
}

TEST_CASE (



"TaskCommand - bool conversion"
,
"[pravaha][taskcommand]"
)
 {
    pravaha::TaskCommand empty_cmd;
    REQUIRE(!static_cast<bool>(empty_cmd));
    auto full_cmd = pravaha::TaskCommand::make([]() {
    });
    REQUIRE(static_cast<bool>(full_cmd));
}

TEST_CASE (



"TaskCommand - no std::function usage"
,
"[pravaha][taskcommand]"
)
 {
    auto cmd = pravaha::TaskCommand::make([]() {
    });
    REQUIRE(cmd.has_value());
    auto result = cmd.run();
    REQUIRE(result.has_value());
}

TEST_CASE (



"TaskCommand - Outcome<Unit> error is propagated"
,
"[pravaha][taskcommand][regression]"
)
 {
    auto cmd = pravaha::TaskCommand::make([]() -> pravaha::Outcome<pravaha::Unit> {
        return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "propagated failure"});
    });
    auto result = cmd.run();
    REQUIRE(!result.has_value());
    REQUIRE(result.error().kind == pravaha::ErrorKind::TaskFailed);
    REQUIRE(result.error().message == "propagated failure");
}

TEST_CASE (



"TaskCommand - explicit void callable succeeds"
,
"[pravaha][taskcommand][regression]"
)
 {
    int ran = 0;
    auto cmd = pravaha::TaskCommand::make([&ran]() -> void { ++ran; });
    auto result = cmd.run();
    REQUIRE(result.has_value());
    REQUIRE(ran == 1);
}

TEST_CASE (



"TaskCommand - Outcome<string> success is treated as success"
,
"[pravaha][taskcommand][regression]"
)
 {
    auto cmd = pravaha::TaskCommand::make([]() -> pravaha::Outcome<std::string> {
        return std::string{"ok"};
    });
    auto result = cmd.run();
    REQUIRE(result.has_value());
}

TEST_CASE (



"TaskCommand - Outcome<string> error is propagated"
,
"[pravaha][taskcommand][regression]"
)
 {
    auto cmd = pravaha::TaskCommand::make([]() -> pravaha::Outcome<std::string> {
        return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "string failure"});
    });
    auto result = cmd.run();
    REQUIRE(!result.has_value());
    REQUIRE(result.error().kind == pravaha::ErrorKind::TaskFailed);
    REQUIRE(result.error().message == "string failure");
}

TEST_CASE (



"TaskCommand - Outcome<int> error is propagated"
,
"[pravaha][taskcommand][regression]"
)
 {
    auto cmd = pravaha::TaskCommand::make([]() -> pravaha::Outcome<int> {
        return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::InternalError, "int failure"});
    });
    auto result = cmd.run();
    REQUIRE(!result.has_value());
    REQUIRE(result.error().kind == pravaha::ErrorKind::InternalError);
    REQUIRE(result.error().message == "int failure");
}

// ============================================================================
// SECTION 13: Lazy Expression DSL
// ============================================================================

TEST_CASE (



"task() - creating task does not execute callable"
,
"[pravaha][dsl]"
)
 {
    int counter = 0;
    auto a = pravaha::task("a", [&counter]() { ++counter; });
    REQUIRE(counter == 0);
    REQUIRE(a.name() == "a");
    REQUIRE(a.domain() == pravaha::ExecutionDomain::CPU);
}

TEST_CASE (



"stage(\"a\", fn) behaves like task(\"a\", fn)"
,
"[pravaha][dsl][stage]"
)
 {
    int counter = 0;
    auto a = pravaha::stage("a", [&counter]() { ++counter; });
    REQUIRE(counter == 0);
    REQUIRE(a.name() == "a");
    REQUIRE(a.domain() == pravaha::ExecutionDomain::CPU);
}

TEST_CASE (



"stage_on(\"io\", IO, fn) preserves domain"
,
"[pravaha][dsl][stage_on]"
)
 {
    auto t = pravaha::stage_on("io", pravaha::ExecutionDomain::IO, []() {
    });
    REQUIRE(t.name() == "io");
    REQUIRE(t.domain() == pravaha::ExecutionDomain::IO);
}

TEST_CASE (



"with_name on task overrides lowered node name"
,
"[pravaha][dsl][with_name][ir]"
)
 {
    auto expr = pravaha::with_name("x", pravaha::task("a", []() {
    }));
    auto ir_result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(ir_result.has_value());
    REQUIRE(ir_result->nodes.size() == 1);
    REQUIRE(ir_result->nodes[0].name == "x");
}

TEST_CASE (



"with_name around sequence does not rename child task nodes"
,
"[pravaha][dsl][with_name][ir]"
)
 {
    auto expr = pravaha::with_name(
        "group",
        pravaha::task("a", []() {
        }) | pravaha::task("b", []() {
        })
    );
    auto ir_result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(ir_result.has_value());
    REQUIRE(ir_result->nodes.size() == 2);
    REQUIRE(ir_result->nodes[0].name == "a");
    REQUIRE(ir_result->nodes[1].name == "b");
}

TEST_CASE (



"with_name frontend hash differs from wrapped expression"
,
"[pravaha][dsl][with_name][lithe]"
)
 {
    auto base = pravaha::task("a", []() {
    });
    auto named = pravaha::with_name("x", pravaha::task("a", []() {
    }));
    REQUIRE(base.frontend.hash != 0);
    REQUIRE(named.frontend.hash != 0);
    REQUIRE(named.frontend.hash != base.frontend.hash);
}

TEST_CASE (



"with_name preserves execution semantics"
,
"[pravaha][dsl][with_name][runner]"
)
 {
    std::atomic runs{0};
    pravaha::Runner<> runner;
    auto expr = pravaha::with_name("renamed", pravaha::task("a", [&runs]() { runs.fetch_add(1); }));
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(runs.load() == 1);
}

TEST_CASE (



"on_domain on task overrides lowered node domain"
,
"[pravaha][dsl][on_domain][ir]"
)
 {
    auto expr = pravaha::on_domain(pravaha::ExecutionDomain::IO, pravaha::task("a", []() {
    }));
    auto ir_result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(ir_result.has_value());
    REQUIRE(ir_result->nodes.size() == 1);
    REQUIRE(ir_result->nodes[0].domain == pravaha::ExecutionDomain::IO);
}

TEST_CASE (



"on_domain keeps domain validation active"
,
"[pravaha][dsl][on_domain][validation]"
)
 {
    pravaha::Runner<> runner;
    auto expr = pravaha::on_domain(
        pravaha::ExecutionDomain::External,
        pravaha::task("a", []() -> std::string { return "x"; })
    );
    auto result = runner.submit(std::move(expr));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == pravaha::ErrorKind::DomainConstraintViolation);
}

TEST_CASE (



"on_domain frontend hash differs from wrapped expression"
,
"[pravaha][dsl][on_domain][lithe]"
)
 {
    auto base = pravaha::task("a", []() {
    });
    auto dom = pravaha::on_domain(pravaha::ExecutionDomain::IO, pravaha::task("a", []() {
    }));
    REQUIRE(base.frontend.hash != 0);
    REQUIRE(dom.frontend.hash != 0);
    REQUIRE(dom.frontend.hash != base.frontend.hash);
}

TEST_CASE (



"with_name on_domain nesting preserves name and domain"
,
"[pravaha][dsl][on_domain][with_name]"
)
 {
    auto expr = pravaha::with_name(
        "renamed",
        pravaha::on_domain(pravaha::ExecutionDomain::IO, pravaha::task("a", []() {
        }))
    );
    auto ir_result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(ir_result.has_value());
    REQUIRE(ir_result->nodes.size() == 1);
    REQUIRE(ir_result->nodes[0].name == "renamed");
    REQUIRE(ir_result->nodes[0].domain == pravaha::ExecutionDomain::IO);
}

TEST_CASE (



"with_priority metadata reaches IrNode"
,
"[pravaha][dsl][with_priority][ir]"
)
 {
    auto expr = pravaha::with_priority(pravaha::TaskPriority::High, pravaha::task("a", []() {
    }));
    auto ir_result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(ir_result.has_value());
    REQUIRE(ir_result->nodes.size() == 1);
    REQUIRE(ir_result->nodes[0].priority == pravaha::TaskPriority::High);
}

TEST_CASE (



"with_priority frontend hash differs from wrapped expression"
,
"[pravaha][dsl][with_priority][lithe]"
)
 {
    auto base = pravaha::task("a", []() {
    });
    auto priority_expr = pravaha::with_priority(pravaha::TaskPriority::Low, pravaha::task("a", []() {
    }));
    REQUIRE(base.frontend.hash != 0);
    REQUIRE(priority_expr.frontend.hash != 0);
    REQUIRE(priority_expr.frontend.hash != base.frontend.hash);
}

TEST_CASE (



"with_priority preserves execution semantics"
,
"[pravaha][dsl][with_priority][runner]"
)
 {
    std::atomic runs{0};
    pravaha::Runner<> runner;
    auto expr = pravaha::with_priority(pravaha::TaskPriority::High,
                                       pravaha::task("a", [&runs]() { runs.fetch_add(1); }));
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(runs.load() == 1);
}

TEST_CASE (



"with_retry<3> on task sets max_retries metadata"
,
"[pravaha][dsl][with_retry][ir]"
)
 {
    auto expr = pravaha::with_retry<3>(pravaha::task("a", []() {
    }));
    auto ir_result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(ir_result.has_value());
    REQUIRE(ir_result->nodes.size() == 1);
    REQUIRE(ir_result->nodes[0].max_retries == 3);
}

TEST_CASE (



"with_retry<0> compiles and keeps default retry metadata"
,
"[pravaha][dsl][with_retry][ir]"
)
 {
    auto expr = pravaha::with_retry<0>(pravaha::task("a", []() {
    }));
    auto ir_result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(ir_result.has_value());
    REQUIRE(ir_result->nodes.size() == 1);
    REQUIRE(ir_result->nodes[0].max_retries == 0);
}

TEST_CASE (



"with_retry frontend hash differs from wrapped expression"
,
"[pravaha][dsl][with_retry][lithe]"
)
 {
    auto base = pravaha::task("a", []() {
    });
    auto retry = pravaha::with_retry<3>(pravaha::task("a", []() {
    }));
    REQUIRE(base.frontend.hash != 0);
    REQUIRE(retry.frontend.hash != 0);
    REQUIRE(retry.frontend.hash != base.frontend.hash);
}

TEST_CASE (



"retry metadata survives with_name and on_domain nesting"
,
"[pravaha][dsl][with_retry][nesting]"
)
 {
    auto expr = pravaha::with_name(
        "renamed",
        pravaha::on_domain(
            pravaha::ExecutionDomain::IO,
            pravaha::with_retry<5>(pravaha::task("a", []() {
            }))
        )
    );
    auto ir_result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(ir_result.has_value());
    REQUIRE(ir_result->nodes.size() == 1);
    REQUIRE(ir_result->nodes[0].name == "renamed");
    REQUIRE(ir_result->nodes[0].domain == pravaha::ExecutionDomain::IO);
    REQUIRE(ir_result->nodes[0].max_retries == 5);
}

TEST_CASE (



"with_retry preserves execution semantics"
,
"[pravaha][dsl][with_retry][runner]"
)
 {
    std::atomic runs{0};
    pravaha::Runner<> runner;
    auto expr = pravaha::with_retry<3>(pravaha::task("a", [&runs]() { runs.fetch_add(1); }));
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(runs.load() == 1);
}

TEST_CASE (



"with_timeout metadata reaches IrNode"
,
"[pravaha][dsl][with_timeout][ir]"
)
 {
    auto expr = pravaha::with_timeout(std::chrono::milliseconds(5), pravaha::task("a", []() {
    }));
    auto ir_result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(ir_result.has_value());
    REQUIRE(ir_result->nodes.size() == 1);
    REQUIRE(
        ir_result->nodes[0].timeout == std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(5)
        ));
}

TEST_CASE (



"with_timeout accepts zero and duration literals"
,
"[pravaha][dsl][with_timeout][ir]"
)
 {
    using namespace std::chrono_literals;
    auto zero_expr = pravaha::with_timeout(std::chrono::nanoseconds{0}, pravaha::task("a", []() {
    }));
    auto zero_ir = pravaha::lower_to_ir(std::move(zero_expr));
    REQUIRE(zero_ir.has_value());
    REQUIRE(zero_ir->nodes.size() == 1);
    REQUIRE(zero_ir->nodes[0].timeout == std::chrono::nanoseconds{0});

    auto literal_expr = pravaha::with_timeout(5s, pravaha::task("b", []() {
    }));
    auto literal_ir = pravaha::lower_to_ir(std::move(literal_expr));
    REQUIRE(literal_ir.has_value());
    REQUIRE(literal_ir->nodes.size() == 1);
    REQUIRE(literal_ir->nodes[0].timeout == std::chrono::duration_cast<std::chrono::nanoseconds>(5s));
}

TEST_CASE (



"with_timeout frontend hash differs from wrapped expression"
,
"[pravaha][dsl][with_timeout][lithe]"
)
 {
    auto base = pravaha::task("a", []() {
    });
    auto timed = pravaha::with_timeout(std::chrono::milliseconds(5), pravaha::task("a", []() {
    }));
    REQUIRE(base.frontend.hash != 0);
    REQUIRE(timed.frontend.hash != 0);
    REQUIRE(timed.frontend.hash != base.frontend.hash);
}

TEST_CASE (



"with_timeout nested decorators preserve metadata"
,
"[pravaha][dsl][with_timeout][nesting]"
)
 {
    auto expr = pravaha::with_name(
        "renamed",
        pravaha::on_domain(
            pravaha::ExecutionDomain::IO,
            pravaha::with_timeout(std::chrono::milliseconds(7), pravaha::with_retry<2>(pravaha::task("a", []() {
            })))
        )
    );
    auto ir_result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(ir_result.has_value());
    REQUIRE(ir_result->nodes.size() == 1);
    REQUIRE(ir_result->nodes[0].name == "renamed");
    REQUIRE(ir_result->nodes[0].domain == pravaha::ExecutionDomain::IO);
    REQUIRE(ir_result->nodes[0].max_retries == 2);
    REQUIRE(
        ir_result->nodes[0].timeout == std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(7)
        ));
}

TEST_CASE (



"with_timeout preserves execution semantics"
,
"[pravaha][dsl][with_timeout][runner]"
)
 {
    std::atomic runs{0};
    pravaha::Runner<> runner;
    auto expr = pravaha::with_timeout(std::chrono::seconds(1), pravaha::task("a", [&runs]() { runs.fetch_add(1); }));
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(runs.load() == 1);
}

TEST_CASE (



"task() - sequence composition does not execute callable"
,
"[pravaha][dsl]"
)
 {
    int ca = 0, cb = 0;
    auto a = pravaha::task("a", [&ca]() { ++ca; });
    auto b = pravaha::task("b", [&cb]() { ++cb; });
    auto seq = std::move(a) | std::move(b);
    REQUIRE(ca == 0);
    REQUIRE(cb == 0);
    STATIC_REQUIRE(pravaha::IsPravahaExpr<decltype(seq)>);
}

TEST_CASE (



"seq(a,b) executes a before b"
,
"[pravaha][dsl][seq]"
)
 {
    std::vector<int> order;
    pravaha::Runner<> runner;
    auto expr = pravaha::seq(
        pravaha::task("a", [&order]() { order.push_back(1); }),
        pravaha::task("b", [&order]() { order.push_back(2); })
    );
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(order == std::vector<int>{1, 2});
}

TEST_CASE (



"seq(a,b,c) executes in order"
,
"[pravaha][dsl][seq]"
)
 {
    std::vector<int> order;
    pravaha::Runner<> runner;
    auto expr = pravaha::seq(
        pravaha::task("a", [&order]() { order.push_back(1); }),
        pravaha::task("b", [&order]() { order.push_back(2); }),
        pravaha::task("c", [&order]() { order.push_back(3); })
    );
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(order == std::vector<int>{1, 2, 3});
}

TEST_CASE (



"pipeline(stage(a), stage(b)) executes sequence"
,
"[pravaha][dsl][pipeline]"
)
 {
    std::vector<int> order;
    pravaha::Runner<> runner;
    auto expr = pravaha::pipeline(
        pravaha::stage("a", [&order]() { order.push_back(1); }),
        pravaha::stage("b", [&order]() { order.push_back(2); })
    );
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(order == std::vector<int>{1, 2});
}

TEST_CASE (



"seq(a,b) frontend hash parity with operator|"
,
"[pravaha][dsl][seq][lithe]"
)
 {
    auto seq_expr = pravaha::seq(
        pravaha::task("a", []() {
        }),
        pravaha::task("b", []() {
        })
    );
    auto op_expr = pravaha::task("a", []() {
    }) | pravaha::task("b", []() {
    });
    REQUIRE(seq_expr.frontend.hash != 0);
    REQUIRE(seq_expr.frontend.hash == op_expr.frontend.hash);
}

TEST_CASE (



"seq(a, b | c) compiles and lowers"
,
"[pravaha][dsl][seq][ir]"
)
 {
    auto expr = pravaha::seq(
        pravaha::task("a", []() {
        }),
        pravaha::task("b", []() {
        }) | pravaha::task("c", []() {
        })
    );
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
}

TEST_CASE (



"par(a,b) runs both branches"
,
"[pravaha][dsl][par]"
)
 {
    std::atomic a_runs{0};
    std::atomic b_runs{0};
    pravaha::Runner<> runner;
    auto expr = pravaha::par(
        pravaha::task("a", [&a_runs]() { a_runs.fetch_add(1); }),
        pravaha::task("b", [&b_runs]() { b_runs.fetch_add(1); })
    );
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(a_runs.load() == 1);
    REQUIRE(b_runs.load() == 1);
}

TEST_CASE (



"par(a,b,c) lowers successfully"
,
"[pravaha][dsl][par][ir]"
)
 {
    auto expr = pravaha::par(
        pravaha::task("a", []() {
        }),
        pravaha::task("b", []() {
        }),
        pravaha::task("c", []() {
        })
    );
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
}

TEST_CASE (



"branch(stage(a), stage(b)) creates parallel group"
,
"[pravaha][dsl][branch][ir]"
)
 {
    auto expr = pravaha::branch(
        pravaha::stage("a", []() {
        }),
        pravaha::stage("b", []() {
        })
    );
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->join_groups.size() == 1);
    REQUIRE(result->join_groups[0].members.size() == 2);
    REQUIRE(result->join_groups[0].policy.kind == pravaha::JoinPolicyKind::AllOrNothing);
}

TEST_CASE (



"par(a,b) default policy is AllOrNothing"
,
"[pravaha][dsl][par]"
)
 {
    auto expr = pravaha::par(
        pravaha::task("a", []() {
        }),
        pravaha::task("b", []() {
        })
    );
    REQUIRE(expr.policy.kind == pravaha::JoinPolicyKind::AllOrNothing);
    REQUIRE(expr.policy.quorum_required == 0);
}

TEST_CASE (



"par(a,b) frontend hash parity with operator&"
,
"[pravaha][dsl][par][lithe]"
)
 {
    auto par_expr = pravaha::par(
        pravaha::task("a", []() {
        }),
        pravaha::task("b", []() {
        })
    );
    auto op_expr = pravaha::task("a", []() {
    }) & pravaha::task("b", []() {
    });
    REQUIRE(par_expr.frontend.hash != 0);
    REQUIRE(par_expr.frontend.hash == op_expr.frontend.hash);
}

TEST_CASE (



"all_of(a,b) policy is AllOrNothing"
,
"[pravaha][dsl][all_of]"
)
 {
    auto expr = pravaha::all_of(
        pravaha::task("a", []() {
        }),
        pravaha::task("b", []() {
        })
    );
    REQUIRE(expr.policy.kind == pravaha::JoinPolicyKind::AllOrNothing);
    REQUIRE(expr.policy.quorum_required == 0);
}

TEST_CASE (



"all_of(a,b,c) compiles and lowers"
,
"[pravaha][dsl][all_of][ir]"
)
 {
    auto expr = pravaha::all_of(
        pravaha::task("a", []() {
        }),
        pravaha::task("b", []() {
        }),
        pravaha::task("c", []() {
        })
    );
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
}

TEST_CASE (



"all_of failure blocks downstream"
,
"[pravaha][dsl][all_of][runner]"
)
 {
    std::atomic downstream_runs{0};
    pravaha::Runner<> runner;
    auto expr = pravaha::all_of(
                    pravaha::task("a", []() { throw std::runtime_error("fail"); }),
                    pravaha::task("b", []() {
                    })
                ) | pravaha::task("c", [&downstream_runs]() { downstream_runs.fetch_add(1); });
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Failed);
    REQUIRE(downstream_runs.load() == 0);
}

TEST_CASE (



"all_of(a,b) frontend hash is non-zero"
,
"[pravaha][dsl][all_of][lithe]"
)
 {
    auto expr = pravaha::all_of(
        pravaha::task("a", []() {
        }),
        pravaha::task("b", []() {
        })
    );
    REQUIRE(expr.frontend.hash != 0);
}

TEST_CASE (



"collect_all_of(a,b) policy is CollectAll"
,
"[pravaha][dsl][collect_all_of]"
)
 {
    auto expr = pravaha::collect_all_of(
        pravaha::task("a", []() {
        }),
        pravaha::task("b", []() {
        })
    );
    REQUIRE(expr.policy.kind == pravaha::JoinPolicyKind::CollectAll);
    REQUIRE(expr.policy.quorum_required == 0);
}

TEST_CASE (



"collect_all_of(a,b,c) compiles and lowers"
,
"[pravaha][dsl][collect_all_of][ir]"
)
 {
    auto expr = pravaha::collect_all_of(
        pravaha::task("a", []() {
        }),
        pravaha::task("b", []() {
        }),
        pravaha::task("c", []() {
        })
    );
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
}

TEST_CASE (



"collect_all_of(a,b) frontend hash parity with collect_all(par(...))"
,
"[pravaha][dsl][collect_all_of][lithe]"
)
 {
    auto helper_expr = pravaha::collect_all_of(
        pravaha::task("a", []() {
        }),
        pravaha::task("b", []() {
        })
    );
    auto explicit_expr = pravaha::collect_all(
        pravaha::par(
            pravaha::task("a", []() {
            }),
            pravaha::task("b", []() {
            })
        )
    );
    REQUIRE(helper_expr.frontend.hash != 0);
    REQUIRE(helper_expr.frontend.hash == explicit_expr.frontend.hash);
}

TEST_CASE (



"collect_all_of(a returns 1, b returns 2) forwards vector<int>{1,2}"
,
"[pravaha][dsl][collect_all_of][payload]"
)
 {
    std::vector<int> observed;
    pravaha::Runner<> runner;
    auto expr = pravaha::collect_all_of(
                    pravaha::task("a", []() -> int { return 1; }),
                    pravaha::task("b", []() -> int { return 2; })
                ) | pravaha::task("sink", [&observed](std::vector<int> values) {
                    observed = std::move(values);
                });
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == std::vector<int>{1, 2});
}

TEST_CASE (



"collect_all_of with one failure fails run"
,
"[pravaha][dsl][collect_all_of][payload]"
)
 {
    pravaha::Runner<> runner;
    auto expr = pravaha::collect_all_of(
                    pravaha::task("a", []() -> int { return 1; }),
                    pravaha::task("b", []() -> pravaha::Outcome<int> {
                        return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "boom"});
                    })
                ) | pravaha::task("sink", [](std::vector<int>) {
                    return pravaha::Unit{};
                });
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->failed());
}

TEST_CASE (



"collect_all_of heterogeneous payloads fail validation"
,
"[pravaha][dsl][collect_all_of][payload]"
)
 {
    pravaha::Runner<> runner;
    auto expr = pravaha::collect_all_of(
                    pravaha::task("a", []() -> int { return 1; }),
                    pravaha::task("b", []() -> std::string { return "x"; })
                ) | pravaha::task("sink", [](std::vector<int>) {
                    return pravaha::Unit{};
                });
    auto result = runner.submit(std::move(expr));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == pravaha::ErrorKind::ValidationError);
}

TEST_CASE (



"collect_all_of failure runs all branches and blocks downstream"
,
"[pravaha][dsl][collect_all_of][runner]"
)
 {
    std::atomic fail_runs{0};
    std::atomic ok_runs{0};
    std::atomic downstream_runs{0};
    pravaha::Runner<> runner;
    auto expr = pravaha::collect_all_of(
                    pravaha::task("a", [&fail_runs]() {
                        fail_runs.fetch_add(1);
                        throw std::runtime_error("fail");
                    }),
                    pravaha::task("b", [&ok_runs]() { ok_runs.fetch_add(1); })
                ) | pravaha::task("c", [&downstream_runs]() { downstream_runs.fetch_add(1); });
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(fail_runs.load() == 1);
    REQUIRE(ok_runs.load() == 1);
    REQUIRE(result->final_state == pravaha::TaskState::Failed);
    REQUIRE(downstream_runs.load() == 0);
}

TEST_CASE (



"task() - parallel composition does not execute callable"
,
"[pravaha][dsl]"
)
 {
    int ca = 0, cb = 0;
    auto a = pravaha::task("a", [&ca]() { ++ca; });
    auto b = pravaha::task("b", [&cb]() { ++cb; });
    auto par = std::move(a) & std::move(b);
    REQUIRE(ca == 0);
    REQUIRE(cb == 0);
    STATIC_REQUIRE(pravaha::IsPravahaExpr<decltype(par)>);
}

TEST_CASE (



"task_on() - stores selected ExecutionDomain"
,
"[pravaha][dsl]"
)
 {
    auto t = pravaha::task_on(pravaha::ExecutionDomain::IO, "io_task", []() {
    });
    REQUIRE(t.name() == "io_task");
    REQUIRE(t.domain() == pravaha::ExecutionDomain::IO);
}

TEST_CASE (



"collect_all() - marks parallel expr with CollectAll"
,
"[pravaha][dsl]"
)
 {
    auto a = pravaha::task("a", []() {
    });
    auto b = pravaha::task("b", []() {
    });
    auto par = std::move(a) & std::move(b);
    REQUIRE(par.policy.kind == pravaha::JoinPolicyKind::AllOrNothing);
    auto collected = pravaha::collect_all(std::move(par));
    REQUIRE(collected.policy.kind == pravaha::JoinPolicyKind::CollectAll);
}

TEST_CASE (



"any_success() - marks parallel expr with AnySuccess and updates frontend"
,
"[pravaha][dsl][lithe]"
)
 {
    auto plain = pravaha::task("a", [] {
    }) & pravaha::task("b", [] {
    });
    auto any = pravaha::any_success(pravaha::task("a", [] {
    }) & pravaha::task("b", [] {
    }));
    auto collected = pravaha::collect_all(pravaha::task("a", [] {
    }) & pravaha::task("b", [] {
    }));

    REQUIRE(any.policy.kind == pravaha::JoinPolicyKind::AnySuccess);
    REQUIRE(any.policy.quorum_required == 0);
    REQUIRE(any.frontend.hash != 0);
    REQUIRE(any.frontend.hash != plain.frontend.hash);
    REQUIRE(any.frontend.hash != collected.frontend.hash);
}

TEST_CASE (



"any_success_of(a,b) sets AnySuccess policy and preserves frontend hash parity"
,
"[pravaha][dsl][any_success_of][lithe]"
)
 {
    auto helper_expr = pravaha::any_success_of(
        pravaha::task("a", [] {
        }),
        pravaha::task("b", [] {
        })
    );
    auto explicit_expr = pravaha::any_success(
        pravaha::par(
            pravaha::task("a", [] {
            }),
            pravaha::task("b", [] {
            })
        )
    );

    REQUIRE(helper_expr.policy.kind == pravaha::JoinPolicyKind::AnySuccess);
    REQUIRE(helper_expr.policy.quorum_required == 0);
    REQUIRE(helper_expr.frontend.hash != 0);
    REQUIRE(helper_expr.frontend.hash == explicit_expr.frontend.hash);
}

TEST_CASE (



"any_success_of(success, fail) final result succeeds"
,
"[pravaha][runner][any_success_of]"
)
 {
    std::atomic downstream_runs{0};
    pravaha::Runner<> runner;
    auto expr = pravaha::any_success_of(
                    pravaha::task("success", []() {
                    }),
                    pravaha::task("fail", []() -> pravaha::Outcome<pravaha::Unit> {
                        return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "late fail"});
                    })
                ) | pravaha::task("downstream", [&downstream_runs]() { downstream_runs.fetch_add(1); });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(downstream_runs.load() == 1);
}

TEST_CASE (



"quorum_of<1>(a,b).policy.quorum_required == 1"
,
"[pravaha][dsl][quorum_of]"
)
 {
    auto expr = pravaha::quorum_of<1>(
        pravaha::task("a", []() {
        }),
        pravaha::task("b", []() {
        })
    );
    REQUIRE(expr.policy.kind == pravaha::JoinPolicyKind::Quorum);
    REQUIRE(expr.policy.quorum_required == 1);
}

TEST_CASE (



"quorum_of<2>(success, success) succeeds"
,
"[pravaha][runner][quorum_of]"
)
 {
    pravaha::Runner<> runner;
    auto expr = pravaha::quorum_of<2>(
        pravaha::task("a", []() {
        }),
        pravaha::task("b", []() {
        })
    );
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
}

TEST_CASE (



"quorum_of<2>(success, fail) fails"
,
"[pravaha][runner][quorum_of]"
)
 {
    pravaha::Runner<> runner;
    auto expr = pravaha::quorum_of<2>(
        pravaha::task("a", []() {
        }),
        pravaha::task("b", []() -> pravaha::Outcome<pravaha::Unit> {
            return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "boom"});
        })
    );
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->failed());
}

TEST_CASE (



"quorum_of<3>(a,b) fails validation/lowering"
,
"[pravaha][dsl][quorum_of][ir]"
)
 {
    auto expr = pravaha::quorum_of<3>(
        pravaha::task("a", []() {
        }),
        pravaha::task("b", []() {
        })
    );
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == pravaha::ErrorKind::ValidationError);
}

TEST_CASE (



"quorum<N>() - marks parallel expr with Quorum and updates frontend"
,
"[pravaha][dsl][lithe]"
)
 {
    auto plain = pravaha::task("a", [] {
    }) & pravaha::task("b", [] {
    });
    auto collected = pravaha::collect_all(pravaha::task("a", [] {
    }) & pravaha::task("b", [] {
    }));
    auto any = pravaha::any_success(pravaha::task("a", [] {
    }) & pravaha::task("b", [] {
    }));
    auto q1 = pravaha::quorum<1>(pravaha::task("a", [] {
    }) & pravaha::task("b", [] {
    }));
    auto q2 = pravaha::quorum<2>(pravaha::task("a", [] {
    }) & pravaha::task("b", [] {
    }));

    REQUIRE(q1.policy.kind == pravaha::JoinPolicyKind::Quorum);
    REQUIRE(q1.policy.quorum_required == 1);
    REQUIRE(q1.frontend.hash != 0);
    REQUIRE(q1.frontend.hash != plain.frontend.hash);
    REQUIRE(q1.frontend.hash != collected.frontend.hash);
    REQUIRE(q1.frontend.hash != any.frontend.hash);
    REQUIRE(q2.frontend.hash != q1.frontend.hash);
}

TEST_CASE (



"C++ DSL expressions carry lithe-derived frontend identity"
,
"[pravaha][dsl][lithe]"
)
 {
    const auto e1 = pravaha::task("a", [] {
    }) | pravaha::task("b", [] {
    });
    REQUIRE(e1.frontend.hash != 0);
    REQUIRE_FALSE(e1.frontend.dump.empty());

    const auto e1_same = pravaha::task("a", [] {
    }) | pravaha::task("b", [] {
    });
    REQUIRE(e1.frontend.hash == e1_same.frontend.hash);

    const auto e1_reversed = pravaha::task("b", [] {
    }) | pravaha::task("a", [] {
    });
    REQUIRE(e1.frontend.hash != e1_reversed.frontend.hash);

    const auto par = pravaha::task("a", [] {
    }) & pravaha::task("b", [] {
    });
    REQUIRE(par.frontend.hash != e1.frontend.hash);

    const auto collected = pravaha::collect_all(pravaha::task("a", [] {
    }) & pravaha::task("b", [] {
    }));
    REQUIRE(collected.frontend.hash != par.frontend.hash);
}

TEST_CASE (



"Lithe is canonical symbolic identity across textual and C++ DSL"
,
"[pravaha][lithe][regression][canonical]"
)
 {
    const auto text_seq_1 = pravaha::parse_pipeline("pipeline p { a then b }");
    const auto text_seq_2 = pravaha::parse_pipeline("pipeline p { a then b }");
    REQUIRE(text_seq_1.has_value());
    REQUIRE(text_seq_2.has_value());

    const auto text_seq_1_root = pravaha::symbolic::make_frontend_meta_for_symbolic_expr(text_seq_1->root);
    const auto text_seq_2_root = pravaha::symbolic::make_frontend_meta_for_symbolic_expr(text_seq_2->root);
    REQUIRE(text_seq_1_root.hash == text_seq_2_root.hash);
    REQUIRE(text_seq_1->frontend.hash == text_seq_2->frontend.hash);

    const auto text_seq_ba = pravaha::parse_pipeline("pipeline p { b then a }");
    REQUIRE(text_seq_ba.has_value());
    const auto text_seq_ba_root = pravaha::symbolic::make_frontend_meta_for_symbolic_expr(text_seq_ba->root);
    REQUIRE(text_seq_1_root.hash != text_seq_ba_root.hash);

    const auto text_par = pravaha::parse_pipeline("pipeline p { parallel { a, b } }");
    REQUIRE(text_par.has_value());
    const auto text_par_root = pravaha::symbolic::make_frontend_meta_for_symbolic_expr(text_par->root);
    REQUIRE(text_seq_1_root.hash != text_par_root.hash);

    const auto text_name_q = pravaha::parse_pipeline("pipeline q { a }");
    const auto text_name_p = pravaha::parse_pipeline("pipeline p { a }");
    REQUIRE(text_name_q.has_value());
    REQUIRE(text_name_p.has_value());
    REQUIRE(text_name_q->frontend.hash != text_name_p->frontend.hash);

    const auto cpp_seq_ab = pravaha::task("a", [] {
    }) | pravaha::task("b", [] {
    });
    const auto cpp_seq_ab_2 = pravaha::task("a", [] {
    }) | pravaha::task("b", [] {
    });
    const auto cpp_seq_ba = pravaha::task("b", [] {
    }) | pravaha::task("a", [] {
    });
    const auto cpp_par_ab = pravaha::task("a", [] {
    }) & pravaha::task("b", [] {
    });
    REQUIRE(cpp_seq_ab.frontend.hash != 0);
    REQUIRE(cpp_seq_ab.frontend.hash == cpp_seq_ab_2.frontend.hash);
    REQUIRE(cpp_seq_ab.frontend.hash != cpp_seq_ba.frontend.hash);
    REQUIRE(cpp_seq_ab.frontend.hash != cpp_par_ab.frontend.hash);

    REQUIRE(text_seq_1_root.hash == cpp_seq_ab.frontend.hash);

    const auto cpp_collect_all_ab = pravaha::collect_all(pravaha::task("a", [] {
    }) & pravaha::task("b", [] {
    }));
    REQUIRE(cpp_collect_all_ab.frontend.hash != cpp_par_ab.frontend.hash);
}

TEST_CASE (



"SymbolicTaskExpr default annotations are empty"
,
"[pravaha][symbolic][annotation]"
)
 {
    pravaha::symbolic::SymbolicTaskExpr task{
        "fetch",
        pravaha::symbolic::lithe_frontend::make_task_ref_meta("fetch"),
        0,
        5
    };
    REQUIRE(task.annotations.empty());
}

TEST_CASE (



"parse_pipeline task keeps empty annotations"
,
"[pravaha][symbolic][annotation]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch }");
    REQUIRE(parsed.has_value());
    auto *task = std::get_if<pravaha::symbolic::SymbolicTaskExpr>(&parsed->root);
    REQUIRE(task != nullptr);
    REQUIRE(task->name == "fetch");
    REQUIRE(task->annotations.empty());
}

TEST_CASE (



"parse_pipeline task parses retry annotation"
,
"[pravaha][symbolic][annotation][parse]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [retry=3] }");
    REQUIRE(parsed.has_value());
    auto *task = std::get_if<pravaha::symbolic::SymbolicTaskExpr>(&parsed->root);
    REQUIRE(task != nullptr);
    REQUIRE(task->annotations.size() == 1);
    REQUIRE(task->annotations.at("retry") == "3");
}

TEST_CASE (



"parse_pipeline task parses domain annotation"
,
"[pravaha][symbolic][annotation][parse]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [domain=io] }");
    REQUIRE(parsed.has_value());
    auto *task = std::get_if<pravaha::symbolic::SymbolicTaskExpr>(&parsed->root);
    REQUIRE(task != nullptr);
    REQUIRE(task->annotations.size() == 1);
    REQUIRE(task->annotations.at("domain") == "io");
}

TEST_CASE (



"parse_pipeline task parses timeout annotation"
,
"[pravaha][symbolic][annotation][parse]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [timeout=5s] }");
    REQUIRE(parsed.has_value());
    auto *task = std::get_if<pravaha::symbolic::SymbolicTaskExpr>(&parsed->root);
    REQUIRE(task != nullptr);
    REQUIRE(task->annotations.size() == 1);
    REQUIRE(task->annotations.at("timeout") == "5s");
}

TEST_CASE (



"parse_pipeline task parses multiple annotations"
,
"[pravaha][symbolic][annotation][parse]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [domain=io, retry=3, timeout=5s] }");
    REQUIRE(parsed.has_value());
    auto *task = std::get_if<pravaha::symbolic::SymbolicTaskExpr>(&parsed->root);
    REQUIRE(task != nullptr);
    REQUIRE(task->annotations.size() == 3);
    REQUIRE(task->annotations.at("domain") == "io");
    REQUIRE(task->annotations.at("retry") == "3");
    REQUIRE(task->annotations.at("timeout") == "5s");
}

TEST_CASE (



"parse_pipeline malformed task annotation fails"
,
"[pravaha][symbolic][annotation][parse]"
)
 {
    auto missing_close = pravaha::parse_pipeline("pipeline p { fetch [retry=3 }");
    REQUIRE_FALSE(missing_close.has_value());
    REQUIRE(missing_close.error().kind == pravaha::ErrorKind::ParseError);

    auto missing_key = pravaha::parse_pipeline("pipeline p { fetch [=3] }");
    REQUIRE_FALSE(missing_key.has_value());
    REQUIRE(missing_key.error().kind == pravaha::ErrorKind::ParseError);

    auto missing_value = pravaha::parse_pipeline("pipeline p { fetch [retry=] }");
    REQUIRE_FALSE(missing_value.has_value());
    REQUIRE(missing_value.error().kind == pravaha::ErrorKind::ParseError);

    auto unknown_key = pravaha::parse_pipeline("pipeline p { fetch [owner=me] }");
    REQUIRE_FALSE(unknown_key.has_value());
    REQUIRE(unknown_key.error().kind == pravaha::ErrorKind::ParseError);
}

TEST_CASE (



"parse_pipeline duplicate task annotation key fails"
,
"[pravaha][symbolic][annotation][parse]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [retry=3, retry=4] }");
    REQUIRE_FALSE(parsed.has_value());
    REQUIRE(parsed.error().kind == pravaha::ErrorKind::ParseError);
}

TEST_CASE (



"task annotation changes symbolic frontend hash"
,
"[pravaha][symbolic][annotation][lithe]"
)
 {
    auto base = pravaha::parse_pipeline("pipeline p { fetch }");
    auto annotated = pravaha::parse_pipeline("pipeline p { fetch [retry=3] }");
    REQUIRE(base.has_value());
    REQUIRE(annotated.has_value());

    auto *base_task = std::get_if<pravaha::symbolic::SymbolicTaskExpr>(&base->root);
    auto *annotated_task = std::get_if<pravaha::symbolic::SymbolicTaskExpr>(&annotated->root);
    REQUIRE(base_task != nullptr);
    REQUIRE(annotated_task != nullptr);
    REQUIRE(base_task->frontend.hash != annotated_task->frontend.hash);
}

// ============================================================================
// SECTION 14: Task IR - Lowering
// ============================================================================

TEST_CASE (



"TaskId - basic properties"
,
"[pravaha][ir]"
)
 {
    pravaha::TaskId id0{0};
    pravaha::TaskId id1{1};
    pravaha::TaskId invalid{};
    REQUIRE(id0.is_valid());
    REQUIRE(id1.is_valid());
    REQUIRE(!invalid.is_valid());
    REQUIRE(id0 != id1);
    REQUIRE(invalid == pravaha::invalid_task_id);
}

TEST_CASE (



"lower_to_ir - single task lowers to one node"
,
"[pravaha][ir]"
)
 {
    int counter = 0;
    auto a = pravaha::task("a", [&counter]() { ++counter; });
    auto result = pravaha::lower_to_ir(std::move(a));
    REQUIRE(result.has_value());
    auto &ir = result.value();
    REQUIRE(ir.node_count() == 1);
    REQUIRE(ir.edge_count() == 0);
    REQUIRE(ir.nodes[0].name == "a");
    REQUIRE(counter == 0);
}

TEST_CASE (



"lower_to_ir - sequence lowers to two nodes and one Sequence edge"
,
"[pravaha][ir]"
)
 {
    int counter = 0;
    auto a = pravaha::task("a", [&counter]() { ++counter; });
    auto b = pravaha::task("b", [&counter]() { ++counter; });
    auto expr = std::move(a) | std::move(b);
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    auto &ir = result.value();
    REQUIRE(ir.node_count() == 2);
    REQUIRE(ir.edge_count() == 1);
    REQUIRE(ir.edges[0].from == pravaha::TaskId{0});
    REQUIRE(ir.edges[0].to == pravaha::TaskId{1});
    REQUIRE(ir.edges[0].kind == pravaha::EdgeKind::Sequence);
    REQUIRE(counter == 0);
}

TEST_CASE (



"lower_to_ir - parallel lowers to two nodes and no edge"
,
"[pravaha][ir]"
)
 {
    int counter = 0;
    auto a = pravaha::task("a", [&counter]() { ++counter; });
    auto b = pravaha::task("b", [&counter]() { ++counter; });
    auto expr = std::move(a) & std::move(b);
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    auto &ir = result.value();
    REQUIRE(ir.node_count() == 2);
    REQUIRE(ir.edge_count() == 0);
    REQUIRE(counter == 0);
}

TEST_CASE (



"lower_to_ir - (A & B) | C has edges A->C and B->C"
,
"[pravaha][ir]"
)
 {
    int counter = 0;
    auto a = pravaha::task("A", [&counter]() { ++counter; });
    auto b = pravaha::task("B", [&counter]() { ++counter; });
    auto c = pravaha::task("C", [&counter]() { ++counter; });
    auto expr = (std::move(a) & std::move(b)) | std::move(c);
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    auto &ir = result.value();
    REQUIRE(ir.node_count() == 3);
    REQUIRE(ir.edge_count() == 2);
    REQUIRE(counter == 0);
}

TEST_CASE (



"lower_to_ir - commands are runnable after lowering"
,
"[pravaha][ir]"
)
 {
    int counter = 0;
    auto a = pravaha::task("a", [&counter]() { counter += 1; });
    auto result = pravaha::lower_to_ir(std::move(a));
    REQUIRE(result.has_value());
    REQUIRE(counter == 0);
    auto run_result = result.value().nodes[0].command.run();
    REQUIRE(run_result.has_value());
    REQUIRE(counter == 1);
}

TEST_CASE (



"lower_to_ir - C++ DSL task propagates frontend diagnostics"
,
"[pravaha][ir][frontend]"
)
 {
    auto expr = pravaha::task("diag_task", []() {
    });
    auto ir_result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(ir_result.has_value());
    REQUIRE(ir_result->nodes.size() == 1);
    REQUIRE(ir_result->nodes[0].frontend_hash != 0);
    REQUIRE_FALSE(ir_result->nodes[0].frontend_dump.empty());
}

TEST_CASE (



"lower_to_ir - Outcome<Unit> payload metadata unwraps to Unit"
,
"[pravaha][ir][payload]"
)
 {
    auto a = pravaha::task("a", []() -> pravaha::Outcome<pravaha::Unit> { return pravaha::Unit{}; });
    auto result = pravaha::lower_to_ir(std::move(a));
    REQUIRE(result.has_value());
    REQUIRE(result->nodes.size() == 1);
    REQUIRE(result->nodes[0].payload_meta.output_type_name == std::string(meta::type_name<pravaha::Unit>()));
}

TEST_CASE (



"lower_to_ir - task returning int has checked output contract"
,
"[pravaha][ir][contract]"
)
 {
    auto expr = pravaha::task("a", []() -> int { return 1; });
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->nodes.size() == 1);

    const auto expected = pravaha::make_type_contract<int>();
    const auto &contract = result->nodes[0].output_contract;
    REQUIRE(contract.checked);
    REQUIRE(contract.type_hash == expected.type_hash);
    REQUIRE(contract.type_name == expected.type_name);
}

TEST_CASE (



"lower_to_ir - Outcome<string> output contract unwraps to string"
,
"[pravaha][ir][contract]"
)
 {
    auto expr = pravaha::task("a", []() -> pravaha::Outcome<std::string> { return std::string{"ok"}; });
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->nodes.size() == 1);

    const auto expected = pravaha::make_type_contract<std::string>();
    const auto &contract = result->nodes[0].output_contract;
    REQUIRE(contract.checked);
    REQUIRE(contract.type_hash == expected.type_hash);
    REQUIRE(contract.type_name == expected.type_name);
}

TEST_CASE (



"lower_to_ir - void output contract maps to Unit"
,
"[pravaha][ir][contract]"
)
 {
    auto expr = pravaha::task("a", []() -> void {
    });
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->nodes.size() == 1);

    const auto expected = pravaha::make_type_contract<pravaha::Unit>();
    const auto &contract = result->nodes[0].output_contract;
    REQUIRE(contract.checked);
    REQUIRE(contract.type_hash == expected.type_hash);
    REQUIRE(contract.type_name == expected.type_name);
}

TEST_CASE (



"lower_to_ir - zero argument callable has unchecked input contract"
,
"[pravaha][ir][contract]"
)
 {
    auto expr = pravaha::task("a", []() -> int { return 1; });
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->nodes.size() == 1);
    REQUIRE_FALSE(result->nodes[0].input_contract.checked);
    const auto expected_output = pravaha::make_type_contract<int>();
    REQUIRE(result->nodes[0].output_contract.checked);
    REQUIRE(result->nodes[0].output_contract.type_hash == expected_output.type_hash);
}

TEST_CASE (



"lower_to_ir - single argument callable has checked input and checked output contract"
,
"[pravaha][ir][contract]"
)
 {
    auto expr = pravaha::task("a", [](int) -> std::string { return "x"; });
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->nodes.size() == 1);

    const auto expected_input = pravaha::make_type_contract<int>();
    const auto expected_output = pravaha::make_type_contract<std::string>();
    REQUIRE(result->nodes[0].input_contract.checked);
    REQUIRE(result->nodes[0].input_contract.type_hash == expected_input.type_hash);
    REQUIRE(result->nodes[0].output_contract.checked);
    REQUIRE(result->nodes[0].output_contract.type_hash == expected_output.type_hash);
}

TEST_CASE (



"lower_to_ir - const reference input callable has checked input contract"
,
"[pravaha][ir][contract]"
)
 {
    struct MyType {
        int value{};
    };

    auto expr = pravaha::task("a", [](const MyType &) -> pravaha::Outcome<pravaha::Unit> {
        return pravaha::Unit{};
    });
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->nodes.size() == 1);

    const auto expected_input = pravaha::make_type_contract<MyType>();
    const auto expected_output = pravaha::make_type_contract<pravaha::Unit>();
    REQUIRE(result->nodes[0].input_contract.checked);
    REQUIRE(result->nodes[0].input_contract.type_hash == expected_input.type_hash);
    REQUIRE(result->nodes[0].output_contract.checked);
    REQUIRE(result->nodes[0].output_contract.type_hash == expected_output.type_hash);
}

TEST_CASE (



"lower_to_ir - multi argument callable has unchecked input contract"
,
"[pravaha][ir][contract]"
)
 {
    auto expr = pravaha::task("a", [](int, int) -> int { return 1; });
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->nodes.size() == 1);
    REQUIRE_FALSE(result->nodes[0].input_contract.checked);
    const auto expected_output = pravaha::make_type_contract<int>();
    REQUIRE(result->nodes[0].output_contract.checked);
    REQUIRE(result->nodes[0].output_contract.type_hash == expected_output.type_hash);
}

TEST_CASE (



"validate_data_contracts - int output to int input passes"
,
"[pravaha][ir][contract]"
)
 {
    auto expr = pravaha::task("a", []() -> int { return 1; })
                | pravaha::task("b", [](int) -> pravaha::Unit { return {}; });
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    auto validation = pravaha::validate_data_contracts(result.value());
    REQUIRE(validation.has_value());
}

TEST_CASE (



"validate_data_contracts - int output to string input fails TypeMismatch"
,
"[pravaha][ir][contract]"
)
 {
    auto expr = pravaha::task("a", []() -> int { return 1; })
                | pravaha::task("b", [](std::string) -> pravaha::Unit { return {}; });
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    auto validation = pravaha::validate_data_contracts(result.value());
    REQUIRE_FALSE(validation.has_value());
    REQUIRE(validation.error().kind == pravaha::ErrorKind::TypeMismatch);
    REQUIRE(validation.error().message.find("a") != std::string::npos);
    REQUIRE(validation.error().message.find("b") != std::string::npos);
    REQUIRE(validation.error().message.find(pravaha::make_type_contract<int>().type_name) != std::string::npos);
    REQUIRE(validation.error().message.find(pravaha::make_type_contract<std::string>().type_name) != std::string::npos);
}

TEST_CASE (



"validate_data_contracts - unchecked input contract passes"
,
"[pravaha][ir][contract]"
)
 {
    auto expr = pravaha::task("a", []() -> int { return 1; })
                | pravaha::task("b", [](int, int) -> pravaha::Unit { return {}; });
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    auto validation = pravaha::validate_data_contracts(result.value());
    REQUIRE(validation.has_value());
}

TEST_CASE (



"validate_data_contracts - unchecked output contract passes"
,
"[pravaha][ir][contract]"
)
 {
    auto expr = pravaha::task("a", []() -> int { return 1; })
                | pravaha::task("b", [](std::string) -> pravaha::Unit { return {}; });
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->nodes.size() == 2);
    result->nodes[0].output_contract.checked = false;
    auto validation = pravaha::validate_data_contracts(result.value());
    REQUIRE(validation.has_value());
}

TEST_CASE (



"validate_data_contracts - Outcome<int> output to int input passes"
,
"[pravaha][ir][contract]"
)
 {
    auto expr = pravaha::task("a", []() -> pravaha::Outcome<int> { return 7; })
                | pravaha::task("b", [](int) -> pravaha::Unit { return {}; });
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    auto validation = pravaha::validate_data_contracts(result.value());
    REQUIRE(validation.has_value());
}

TEST_CASE (



"validate_data_contracts - Outcome<string> output to int input fails"
,
"[pravaha][ir][contract]"
)
 {
    auto expr = pravaha::task("a", []() -> pravaha::Outcome<std::string> { return std::string{"x"}; })
                | pravaha::task("b", [](int) -> pravaha::Unit { return {}; });
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    auto validation = pravaha::validate_data_contracts(result.value());
    REQUIRE_FALSE(validation.has_value());
    REQUIRE(validation.error().kind == pravaha::ErrorKind::TypeMismatch);
}

TEST_CASE (



"function pointer task preserves one-input value flow and contracts"
,
"[pravaha][callable_traits]"
)
 {
    int observed = 0;
    pravaha::Runner<> runner;
    auto expr = pravaha::task("src", []() -> int { return 4; })
                | pravaha::task("fp", &triple_value)
                | pravaha::task("sink", [&observed](int v) -> pravaha::Unit {
                    observed = v;
                    return {};
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == 12);
}

TEST_CASE (



"functor task preserves Outcome deduction and one-input value flow"
,
"[pravaha][callable_traits]"
)
 {
    int observed = 0;
    pravaha::Runner<> runner;
    auto expr = pravaha::task("src", []() -> int { return 5; })
                | pravaha::task("add_two", AddTwoFunctor{})
                | pravaha::task("add_one_outcome", OutcomeAddOneFunctor{})
                | pravaha::task("sink", [&observed](int v) -> pravaha::Unit {
                    observed = v;
                    return {};
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == 8);
}

TEST_CASE (



"lower_symbolic_pipeline - registry int to int contract passes"
,
"[pravaha][symbolic][contract]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { a then b }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("a", []() -> int { return 1; });
    reg.register_task("b", [](int) -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(ir.has_value());

    auto validation = pravaha::validate_data_contracts(ir.value());
    REQUIRE(validation.has_value());
}

TEST_CASE (



"lower_symbolic_pipeline - registry int to string contract fails TypeMismatch"
,
"[pravaha][symbolic][contract]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { a then b }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("a", []() -> int { return 1; });
    reg.register_task("b", [](std::string) -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(ir.has_value());

    auto validation = pravaha::validate_data_contracts(ir.value());
    REQUIRE_FALSE(validation.has_value());
    REQUIRE(validation.error().kind == pravaha::ErrorKind::TypeMismatch);
}

TEST_CASE (



"lower_symbolic_pipeline - registry Outcome<string> to string contract passes"
,
"[pravaha][symbolic][contract]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { a then b }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("a", []() -> pravaha::Outcome<std::string> { return std::string{"ok"}; });
    reg.register_task("b", [](std::string) -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(ir.has_value());

    auto validation = pravaha::validate_data_contracts(ir.value());
    REQUIRE(validation.has_value());
}

TEST_CASE (



"lower_symbolic_pipeline forwards int payload into downstream SymbolRegistry task"
,
"[pravaha][symbolic][value_flow]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { A then B }");
    REQUIRE(parsed.has_value());

    int observed = 0;
    pravaha::SymbolRegistry reg;
    reg.register_task("A", []() -> int { return 21; });
    reg.register_task("B", [&observed](int value) -> pravaha::Unit {
        observed = value;
        return {};
    });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(ir.has_value());
    REQUIRE(ir->nodes.size() == 2);

    auto state = pravaha::RuntimeState::build(*ir);
    auto a_result = ir->nodes[0].command.run(&state.result_slots[0], nullptr);
    REQUIRE(a_result.has_value());
    auto b_result = ir->nodes[1].command.run(&state.result_slots[1], &state.result_slots[0]);
    REQUIRE(b_result.has_value());
    REQUIRE(observed == 21);
}

TEST_CASE (



"lower_symbolic_pipeline forwards Outcome<string> payload into downstream SymbolRegistry task"
,
"[pravaha][symbolic][value_flow]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { A then B }");
    REQUIRE(parsed.has_value());

    std::string observed;
    pravaha::SymbolRegistry reg;
    reg.register_task("A", []() -> pravaha::Outcome<std::string> { return std::string{"ok"}; });
    reg.register_task("B", [&observed](std::string value) -> pravaha::Unit {
        observed = std::move(value);
        return {};
    });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(ir.has_value());
    REQUIRE(ir->nodes.size() == 2);

    auto state = pravaha::RuntimeState::build(*ir);
    auto a_result = ir->nodes[0].command.run(&state.result_slots[0], nullptr);
    REQUIRE(a_result.has_value());
    auto b_result = ir->nodes[1].command.run(&state.result_slots[1], &state.result_slots[0]);
    REQUIRE(b_result.has_value());
    REQUIRE(observed == "ok");
}

TEST_CASE (



"lower_symbolic_pipeline failure in upstream SymbolRegistry task prevents downstream execution"
,
"[pravaha][symbolic][value_flow]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { A then B }");
    REQUIRE(parsed.has_value());

    int b_runs = 0;
    pravaha::SymbolRegistry reg;
    reg.register_task("A", []() -> pravaha::Outcome<int> {
        return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "boom"});
    });
    reg.register_task("B", [&b_runs](int) -> pravaha::Unit {
        ++b_runs;
        return {};
    });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(ir.has_value());
    REQUIRE(ir->nodes.size() == 2);

    auto state = pravaha::RuntimeState::build(*ir);
    auto a_result = ir->nodes[0].command.run(&state.result_slots[0], nullptr);
    REQUIRE_FALSE(a_result.has_value());
    auto b_result = ir->nodes[1].command.run(&state.result_slots[1], &state.result_slots[0]);
    REQUIRE_FALSE(b_result.has_value());
    REQUIRE(b_runs == 0);
}

TEST_CASE (



"lower_symbolic_pipeline - missing symbol returns SymbolNotFound"
,
"[pravaha][symbolic][contract]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { a then b }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("a", []() -> int { return 1; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE_FALSE(ir.has_value());
    REQUIRE(ir.error().kind == pravaha::ErrorKind::SymbolNotFound);
}

TEST_CASE (



"lower_symbolic_pipeline - domain annotation io lowers to IO"
,
"[pravaha][symbolic][annotation][domain]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [domain=io] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", []() -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(ir.has_value());
    REQUIRE(ir->nodes.size() == 1);
    REQUIRE(ir->nodes[0].domain == pravaha::ExecutionDomain::IO);
    auto domain_validation = pravaha::validate_domain_constraints(*ir);
    REQUIRE(domain_validation.has_value());
}

TEST_CASE (



"lower_symbolic_pipeline - domain annotation cpu lowers to CPU"
,
"[pravaha][symbolic][annotation][domain]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [domain=cpu] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", []() -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(ir.has_value());
    REQUIRE(ir->nodes.size() == 1);
    REQUIRE(ir->nodes[0].domain == pravaha::ExecutionDomain::CPU);
}

TEST_CASE (



"lower_symbolic_pipeline - invalid domain annotation fails ValidationError"
,
"[pravaha][symbolic][annotation][domain]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [domain=gpu] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", []() -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE_FALSE(ir.has_value());
    REQUIRE(ir.error().kind == pravaha::ErrorKind::ValidationError);
}

TEST_CASE (



"lower_symbolic_pipeline - domain annotation overrides SymbolRegistry default domain"
,
"[pravaha][symbolic][annotation][domain]"
)
 {
    auto parsed_default = pravaha::parse_pipeline("pipeline p { fetch }");
    auto parsed_override = pravaha::parse_pipeline("pipeline p { fetch [domain=cpu] }");
    REQUIRE(parsed_default.has_value());
    REQUIRE(parsed_override.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", pravaha::ExecutionDomain::IO, []() -> pravaha::Unit { return {}; });

    auto ir_default = pravaha::lower_symbolic_pipeline(parsed_default.value(), reg);
    REQUIRE(ir_default.has_value());
    REQUIRE(ir_default->nodes.size() == 1);
    REQUIRE(ir_default->nodes[0].domain == pravaha::ExecutionDomain::IO);

    auto ir_override = pravaha::lower_symbolic_pipeline(parsed_override.value(), reg);
    REQUIRE(ir_override.has_value());
    REQUIRE(ir_override->nodes.size() == 1);
    REQUIRE(ir_override->nodes[0].domain == pravaha::ExecutionDomain::CPU);
}

TEST_CASE (



"lower_symbolic_pipeline - retry annotation sets max_retries"
,
"[pravaha][symbolic][annotation][retry]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [retry=3] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", []() -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(ir.has_value());
    REQUIRE(ir->nodes.size() == 1);
    REQUIRE(ir->nodes[0].max_retries == 3);
}

TEST_CASE (



"lower_symbolic_pipeline - retry annotation zero is accepted"
,
"[pravaha][symbolic][annotation][retry]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [retry=0] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", []() -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(ir.has_value());
    REQUIRE(ir->nodes.size() == 1);
    REQUIRE(ir->nodes[0].max_retries == 0);
}

TEST_CASE (



"lower_symbolic_pipeline - retry annotation negative value fails"
,
"[pravaha][symbolic][annotation][retry]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [retry=-1] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", []() -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE_FALSE(ir.has_value());
    REQUIRE(ir.error().kind == pravaha::ErrorKind::ValidationError);
}

TEST_CASE (



"lower_symbolic_pipeline - retry annotation non number fails"
,
"[pravaha][symbolic][annotation][retry]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [retry=abc] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", []() -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE_FALSE(ir.has_value());
    REQUIRE(ir.error().kind == pravaha::ErrorKind::ValidationError);
}

TEST_CASE (



"lower_symbolic_pipeline - retry annotation works with SymbolRegistry task"
,
"[pravaha][symbolic][annotation][retry]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { a then b [retry=2] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("a", []() -> int { return 1; });
    reg.register_task("b", [](int) -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(ir.has_value());
    REQUIRE(ir->nodes.size() == 2);
    REQUIRE(ir->nodes[1].name == "b");
    REQUIRE(ir->nodes[1].max_retries == 2);
}

TEST_CASE (



"lower_symbolic_pipeline - timeout annotation seconds lowers to nanoseconds"
,
"[pravaha][symbolic][annotation][timeout]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [timeout=5s] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", []() -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(ir.has_value());
    REQUIRE(ir->nodes.size() == 1);
    REQUIRE(ir->nodes[0].timeout == std::chrono::seconds(5));
}

TEST_CASE (



"lower_symbolic_pipeline - timeout annotation milliseconds lowers to nanoseconds"
,
"[pravaha][symbolic][annotation][timeout]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [timeout=100ms] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", []() -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(ir.has_value());
    REQUIRE(ir->nodes.size() == 1);
    REQUIRE(ir->nodes[0].timeout == std::chrono::milliseconds(100));
}

TEST_CASE (



"lower_symbolic_pipeline - timeout annotation microseconds lowers to nanoseconds"
,
"[pravaha][symbolic][annotation][timeout]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [timeout=10us] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", []() -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(ir.has_value());
    REQUIRE(ir->nodes.size() == 1);
    REQUIRE(ir->nodes[0].timeout == std::chrono::microseconds(10));
}

TEST_CASE (



"lower_symbolic_pipeline - timeout annotation nanoseconds lowers to nanoseconds"
,
"[pravaha][symbolic][annotation][timeout]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [timeout=1ns] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", []() -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(ir.has_value());
    REQUIRE(ir->nodes.size() == 1);
    REQUIRE(ir->nodes[0].timeout == std::chrono::nanoseconds(1));
}

TEST_CASE (



"lower_symbolic_pipeline - timeout annotation missing unit fails"
,
"[pravaha][symbolic][annotation][timeout]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [timeout=5] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", []() -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE_FALSE(ir.has_value());
    REQUIRE(ir.error().kind == pravaha::ErrorKind::ValidationError);
}

TEST_CASE (



"lower_symbolic_pipeline - timeout annotation non number fails"
,
"[pravaha][symbolic][annotation][timeout]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [timeout=abc] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", []() -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE_FALSE(ir.has_value());
    REQUIRE(ir.error().kind == pravaha::ErrorKind::ValidationError);
}

TEST_CASE (



"lower_symbolic_pipeline - timeout annotation negative fails"
,
"[pravaha][symbolic][annotation][timeout]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [timeout=-1s] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", []() -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE_FALSE(ir.has_value());
    REQUIRE(ir.error().kind == pravaha::ErrorKind::ValidationError);
}

TEST_CASE (



"lower_symbolic_pipeline - priority annotation low lowers to Low"
,
"[pravaha][symbolic][annotation][priority]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [priority=low] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", []() -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(ir.has_value());
    REQUIRE(ir->nodes.size() == 1);
    REQUIRE(ir->nodes[0].priority == pravaha::TaskPriority::Low);
}

TEST_CASE (



"lower_symbolic_pipeline - priority annotation normal lowers to Normal"
,
"[pravaha][symbolic][annotation][priority]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [priority=normal] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", []() -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(ir.has_value());
    REQUIRE(ir->nodes.size() == 1);
    REQUIRE(ir->nodes[0].priority == pravaha::TaskPriority::Normal);
}

TEST_CASE (



"lower_symbolic_pipeline - priority annotation high lowers to High"
,
"[pravaha][symbolic][annotation][priority]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [priority=high] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", []() -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(ir.has_value());
    REQUIRE(ir->nodes.size() == 1);
    REQUIRE(ir->nodes[0].priority == pravaha::TaskPriority::High);
}

TEST_CASE (



"lower_symbolic_pipeline - invalid priority annotation fails"
,
"[pravaha][symbolic][annotation][priority]"
)
 {
    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [priority=urgent] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", []() -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE_FALSE(ir.has_value());
    REQUIRE(ir.error().kind == pravaha::ErrorKind::ValidationError);
}

TEST_CASE (



"text domain annotation matches C++ on_domain metadata and execution"
,
"[pravaha][symbolic][annotation][parity]"
)
 {
    int text_runs = 0;
    int cpp_runs = 0;

    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [domain=io] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", [&]() { ++text_runs; });
    auto text_ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(text_ir.has_value());
    REQUIRE(text_ir->nodes.size() == 1);

    auto cpp_expr = pravaha::on_domain(pravaha::ExecutionDomain::IO, pravaha::task("fetch", [&]() { ++cpp_runs; }));
    auto cpp_ir = pravaha::lower_to_ir(std::move(cpp_expr));
    REQUIRE(cpp_ir.has_value());
    REQUIRE(cpp_ir->nodes.size() == 1);

    REQUIRE(text_ir->nodes[0].domain == pravaha::ExecutionDomain::IO);
    REQUIRE(cpp_ir->nodes[0].domain == pravaha::ExecutionDomain::IO);
    REQUIRE(text_ir->nodes[0].frontend_hash != 0);
    REQUIRE(cpp_ir->nodes[0].frontend_hash != 0);

    REQUIRE(text_ir->nodes[0].command.run().has_value());
    REQUIRE(cpp_ir->nodes[0].command.run().has_value());
    REQUIRE(text_runs == 1);
    REQUIRE(cpp_runs == 1);
}

TEST_CASE (



"text retry annotation matches C++ with_retry metadata and execution"
,
"[pravaha][symbolic][annotation][parity]"
)
 {
    int text_runs = 0;
    int cpp_runs = 0;

    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [retry=3] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", [&]() { ++text_runs; });
    auto text_ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(text_ir.has_value());
    REQUIRE(text_ir->nodes.size() == 1);

    auto cpp_expr = pravaha::with_retry<3>(pravaha::task("fetch", [&]() { ++cpp_runs; }));
    auto cpp_ir = pravaha::lower_to_ir(std::move(cpp_expr));
    REQUIRE(cpp_ir.has_value());
    REQUIRE(cpp_ir->nodes.size() == 1);

    REQUIRE(text_ir->nodes[0].max_retries == 3);
    REQUIRE(cpp_ir->nodes[0].max_retries == 3);
    REQUIRE(text_ir->nodes[0].frontend_hash != 0);
    REQUIRE(cpp_ir->nodes[0].frontend_hash != 0);

    REQUIRE(text_ir->nodes[0].command.run().has_value());
    REQUIRE(cpp_ir->nodes[0].command.run().has_value());
    REQUIRE(text_runs == 1);
    REQUIRE(cpp_runs == 1);
}

TEST_CASE (



"text timeout annotation matches C++ with_timeout metadata and execution"
,
"[pravaha][symbolic][annotation][parity]"
)
 {
    int text_runs = 0;
    int cpp_runs = 0;

    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [timeout=5s] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", [&]() { ++text_runs; });
    auto text_ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(text_ir.has_value());
    REQUIRE(text_ir->nodes.size() == 1);

    auto cpp_expr = pravaha::with_timeout(std::chrono::seconds(5), pravaha::task("fetch", [&]() { ++cpp_runs; }));
    auto cpp_ir = pravaha::lower_to_ir(std::move(cpp_expr));
    REQUIRE(cpp_ir.has_value());
    REQUIRE(cpp_ir->nodes.size() == 1);

    REQUIRE(text_ir->nodes[0].timeout == std::chrono::seconds(5));
    REQUIRE(cpp_ir->nodes[0].timeout == std::chrono::seconds(5));
    REQUIRE(text_ir->nodes[0].frontend_hash != 0);
    REQUIRE(cpp_ir->nodes[0].frontend_hash != 0);

    REQUIRE(text_ir->nodes[0].command.run().has_value());
    REQUIRE(cpp_ir->nodes[0].command.run().has_value());
    REQUIRE(text_runs == 1);
    REQUIRE(cpp_runs == 1);
}

TEST_CASE (



"text priority annotation matches C++ with_priority metadata and execution"
,
"[pravaha][symbolic][annotation][parity]"
)
 {
    int text_runs = 0;
    int cpp_runs = 0;

    auto parsed = pravaha::parse_pipeline("pipeline p { fetch [priority=high] }");
    REQUIRE(parsed.has_value());

    pravaha::SymbolRegistry reg;
    reg.register_task("fetch", [&]() { ++text_runs; });
    auto text_ir = pravaha::lower_symbolic_pipeline(parsed.value(), reg);
    REQUIRE(text_ir.has_value());
    REQUIRE(text_ir->nodes.size() == 1);

    auto cpp_expr = pravaha::with_priority(pravaha::TaskPriority::High, pravaha::task("fetch", [&]() { ++cpp_runs; }));
    auto cpp_ir = pravaha::lower_to_ir(std::move(cpp_expr));
    REQUIRE(cpp_ir.has_value());
    REQUIRE(cpp_ir->nodes.size() == 1);

    REQUIRE(text_ir->nodes[0].priority == pravaha::TaskPriority::High);
    REQUIRE(cpp_ir->nodes[0].priority == pravaha::TaskPriority::High);
    REQUIRE(text_ir->nodes[0].frontend_hash != 0);
    REQUIRE(cpp_ir->nodes[0].frontend_hash != 0);

    REQUIRE(text_ir->nodes[0].command.run().has_value());
    REQUIRE(cpp_ir->nodes[0].command.run().has_value());
    REQUIRE(text_runs == 1);
    REQUIRE(cpp_runs == 1);
}

TEST_CASE (



"lower_to_ir - parallel join group policy defaults to AllOrNothing"
,
"[pravaha][ir][policy]"
)
 {
    auto expr = pravaha::task("a", []() {
    }) & pravaha::task("b", []() {
    });
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->join_groups.size() == 1);
    REQUIRE(result->join_groups[0].members.size() == 2);
    REQUIRE(result->join_groups[0].policy.kind == pravaha::JoinPolicyKind::AllOrNothing);
    REQUIRE(result->join_groups[0].policy.quorum_required == 0);
}

TEST_CASE (



"lower_to_ir - collect_all parallel join group policy is CollectAll"
,
"[pravaha][ir][policy]"
)
 {
    auto expr = pravaha::collect_all(pravaha::task("a", []() {
    }) & pravaha::task("b", []() {
    }));
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->join_groups.size() == 1);
    REQUIRE(result->join_groups[0].policy.kind == pravaha::JoinPolicyKind::CollectAll);
    REQUIRE(result->join_groups[0].policy.quorum_required == 0);
}

TEST_CASE (



"lower_to_ir - any_success parallel join group policy is AnySuccess"
,
"[pravaha][ir][policy]"
)
 {
    auto expr = pravaha::any_success(pravaha::task("a", []() {
    }) & pravaha::task("b", []() {
    }));
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->join_groups.size() == 1);
    REQUIRE(result->join_groups[0].policy.kind == pravaha::JoinPolicyKind::AnySuccess);
    REQUIRE(result->join_groups[0].policy.quorum_required == 0);
}

TEST_CASE (



"lower_to_ir - quorum parallel join group policy preserves quorum_required"
,
"[pravaha][ir][policy]"
)
 {
    auto expr = pravaha::quorum<1>(pravaha::task("a", []() {
    }) & pravaha::task("b", []() {
    }));
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->join_groups.size() == 1);
    REQUIRE(result->join_groups[0].policy.kind == pravaha::JoinPolicyKind::Quorum);
    REQUIRE(result->join_groups[0].policy.quorum_required == 1);
}

TEST_CASE (



"lower_to_ir - quorum larger than branch count fails validation"
,
"[pravaha][ir][policy]"
)
 {
    auto expr = pravaha::quorum<3>(pravaha::task("a", []() {
    }) & pravaha::task("b", []() {
    }));
    auto result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(!result.has_value());
    REQUIRE(result.error().kind == pravaha::ErrorKind::ValidationError);
}

TEST_CASE (



"join runtime state - AllOrNothing success and failure"
,
"[pravaha][runtime][join]"
)
 {
    pravaha::RuntimeState ok;
    ok.joins.push_back(pravaha::JoinRuntimeState{pravaha::JoinPolicy{pravaha::JoinPolicyKind::AllOrNothing, 0}, 2});
    ok.record_join_terminal(0, pravaha::TaskState::Succeeded);
    REQUIRE(ok.joins[0].resolved == false);
    REQUIRE(ok.joins[0].success == false);
    ok.record_join_terminal(0, pravaha::TaskState::Succeeded);
    REQUIRE(ok.joins[0].resolved == true);
    REQUIRE(ok.joins[0].success == true);

    pravaha::RuntimeState fail;
    fail.joins.push_back(pravaha::JoinRuntimeState{pravaha::JoinPolicy{pravaha::JoinPolicyKind::AllOrNothing, 0}, 2});
    fail.record_join_terminal(0, pravaha::TaskState::Failed);
    REQUIRE(fail.joins[0].resolved == true);
    REQUIRE(fail.joins[0].success == false);
}

TEST_CASE (



"join runtime state - CollectAll resolves only when all terminal"
,
"[pravaha][runtime][join]"
)
 {
    pravaha::RuntimeState state;
    state.joins.push_back(pravaha::JoinRuntimeState{pravaha::JoinPolicy{pravaha::JoinPolicyKind::CollectAll, 0}, 2});
    state.record_join_terminal(0, pravaha::TaskState::Succeeded);
    REQUIRE(state.joins[0].resolved == false);
    state.record_join_terminal(0, pravaha::TaskState::Failed);
    REQUIRE(state.joins[0].resolved == true);
    REQUIRE(state.joins[0].success == false);
}

TEST_CASE (



"join runtime state - AnySuccess success and all-fail resolution"
,
"[pravaha][runtime][join]"
)
 {
    pravaha::RuntimeState success_state;
    success_state.joins.push_back(pravaha::JoinRuntimeState{
        pravaha::JoinPolicy{pravaha::JoinPolicyKind::AnySuccess, 0}, 3
    });
    success_state.record_join_terminal(0, pravaha::TaskState::Succeeded);
    REQUIRE(success_state.joins[0].resolved == true);
    REQUIRE(success_state.joins[0].success == true);

    pravaha::RuntimeState fail_state;
    fail_state.joins.push_back(
        pravaha::JoinRuntimeState{pravaha::JoinPolicy{pravaha::JoinPolicyKind::AnySuccess, 0}, 2});
    fail_state.record_join_terminal(0, pravaha::TaskState::Failed);
    REQUIRE(fail_state.joins[0].resolved == false);
    fail_state.record_join_terminal(0, pravaha::TaskState::Skipped);
    REQUIRE(fail_state.joins[0].resolved == true);
    REQUIRE(fail_state.joins[0].success == false);
}

TEST_CASE (



"join runtime state - Quorum success and impossible failure"
,
"[pravaha][runtime][join]"
)
 {
    pravaha::RuntimeState success_state;
    success_state.joins.push_back(pravaha::JoinRuntimeState{
        pravaha::JoinPolicy{pravaha::JoinPolicyKind::Quorum, 2}, 3
    });
    success_state.record_join_terminal(0, pravaha::TaskState::Succeeded);
    REQUIRE(success_state.joins[0].resolved == false);
    success_state.record_join_terminal(0, pravaha::TaskState::Failed);
    REQUIRE(success_state.joins[0].resolved == false);
    success_state.record_join_terminal(0, pravaha::TaskState::Succeeded);
    REQUIRE(success_state.joins[0].resolved == true);
    REQUIRE(success_state.joins[0].success == true);

    pravaha::RuntimeState fail_state;
    fail_state.joins.push_back(pravaha::JoinRuntimeState{pravaha::JoinPolicy{pravaha::JoinPolicyKind::Quorum, 2}, 3});
    fail_state.record_join_terminal(0, pravaha::TaskState::Failed);
    REQUIRE(fail_state.joins[0].resolved == false);
    fail_state.record_join_terminal(0, pravaha::TaskState::Skipped);
    REQUIRE(fail_state.joins[0].resolved == true);
    REQUIRE(fail_state.joins[0].success == false);
}

TEST_CASE (



"join runtime state - member accounting prevents double count"
,
"[pravaha][runtime][join]"
)
 {
    pravaha::TaskIr ir;
    ir.add_node("a", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_node("b", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_join_group({pravaha::TaskId{0}, pravaha::TaskId{1}},
                      pravaha::JoinPolicy{pravaha::JoinPolicyKind::AllOrNothing, 0});

    auto state = pravaha::RuntimeState::build(ir);
    state.record_join_terminal_for_member(0, 0, pravaha::TaskState::Succeeded);
    state.record_join_terminal_for_member(0, 0, pravaha::TaskState::Succeeded);
    REQUIRE(state.joins[0].succeeded == 1);
}

TEST_CASE (



"runtime state attempt counts initialize to zero and match node count"
,
"[pravaha][runtime][retry]"
)
 {
    pravaha::TaskIr ir;
    ir.add_node("a", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_node("b", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_node("c", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));

    auto state = pravaha::RuntimeState::build(ir);
    REQUIRE(state.attempt_counts.size() == ir.nodes.size());
    REQUIRE(state.attempt_count(pravaha::TaskId{0}) == 0);
    REQUIRE(state.attempt_count(pravaha::TaskId{1}) == 0);
    REQUIRE(state.attempt_count(pravaha::TaskId{2}) == 0);
}

TEST_CASE (



"runtime state initializes empty result slots per node"
,
"[pravaha][runtime][result_slot]"
)
 {
    pravaha::TaskIr ir;
    ir.add_node("a", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_node("b", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));

    auto state = pravaha::RuntimeState::build(ir);
    REQUIRE(state.result_slots.size() == ir.nodes.size());
    for (const auto &slot: state.result_slots) {
        REQUIRE(slot.empty());
        REQUIRE(slot.type_hash == 0);
    }
}

TEST_CASE (



"ResultSlot can store int"
,
"[pravaha][runtime][result_slot]"
)
 {
    pravaha::ResultSlot slot;
    REQUIRE(slot.empty());

    slot.emplace<int>(42);

    REQUIRE_FALSE(slot.empty());
    REQUIRE(slot.type_hash == pravaha::make_type_contract<int>().type_hash);
    auto *value = slot.get_if<int>();
    REQUIRE(value != nullptr);
    REQUIRE(*value == 42);
}

TEST_CASE (



"ResultSlot can store string"
,
"[pravaha][runtime][result_slot]"
)
 {
    pravaha::ResultSlot slot;
    slot.emplace<std::string>("hello");

    REQUIRE_FALSE(slot.empty());
    REQUIRE(slot.type_hash == pravaha::make_type_contract<std::string>().type_hash);
    auto *value = slot.get_if<std::string>();
    REQUIRE(value != nullptr);
    REQUIRE(*value == "hello");
}

TEST_CASE (



"ResultSlot type hash matches schema hash contract"
,
"[pravaha][runtime][result_slot]"
)
 {
    struct ReflectablePayload {
        int x;
        double y;
    };

    auto contract = pravaha::make_type_contract<ReflectablePayload>();
    REQUIRE(contract.type_hash == static_cast<std::size_t>(meta::schema_hash<ReflectablePayload>()));

    pravaha::ResultSlot slot;
    slot.emplace<ReflectablePayload>(ReflectablePayload{3, 4.0});
    REQUIRE(slot.type_hash == contract.type_hash);
}

TEST_CASE (



"TaskCommand stores int output in RuntimeState result slot"
,
"[pravaha][runtime][result_slot][output]"
)
 {
    auto ir_result = pravaha::lower_to_ir(pravaha::task("store_int", []() -> int {
        return 17;
    }));
    REQUIRE(ir_result.has_value());
    auto &ir = ir_result.value();

    auto state = pravaha::RuntimeState::build(ir);
    auto run_result = ir.nodes[0].command.run(&state.result_slots[0]);
    REQUIRE(run_result.has_value());
    auto *stored = state.result_slots[0].get_if<int>();
    REQUIRE(stored != nullptr);
    REQUIRE(*stored == 17);
}

TEST_CASE (



"TaskCommand stores string output in RuntimeState result slot"
,
"[pravaha][runtime][result_slot][output]"
)
 {
    auto ir_result = pravaha::lower_to_ir(pravaha::task("store_string", []() -> std::string {
        return "value";
    }));
    REQUIRE(ir_result.has_value());
    auto &ir = ir_result.value();

    auto state = pravaha::RuntimeState::build(ir);
    auto run_result = ir.nodes[0].command.run(&state.result_slots[0]);
    REQUIRE(run_result.has_value());
    auto *stored = state.result_slots[0].get_if<std::string>();
    REQUIRE(stored != nullptr);
    REQUIRE(*stored == "value");
}

TEST_CASE (



"TaskCommand stores Outcome<int> payload in RuntimeState result slot"
,
"[pravaha][runtime][result_slot][output]"
)
 {
    auto ir_result = pravaha::lower_to_ir(pravaha::task("store_outcome_int", []() -> pravaha::Outcome<int> {
        return 23;
    }));
    REQUIRE(ir_result.has_value());
    auto &ir = ir_result.value();

    auto state = pravaha::RuntimeState::build(ir);
    auto run_result = ir.nodes[0].command.run(&state.result_slots[0]);
    REQUIRE(run_result.has_value());
    auto *stored = state.result_slots[0].get_if<int>();
    REQUIRE(stored != nullptr);
    REQUIRE(*stored == 23);
}

TEST_CASE (



"TaskCommand failed Outcome<int> does not store output in RuntimeState result slot"
,
"[pravaha][runtime][result_slot][output]"
)
 {
    auto ir_result = pravaha::lower_to_ir(pravaha::task("fail_outcome_int", []() -> pravaha::Outcome<int> {
        return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "fail"});
    }));
    REQUIRE(ir_result.has_value());
    auto &ir = ir_result.value();

    auto state = pravaha::RuntimeState::build(ir);
    auto run_result = ir.nodes[0].command.run(&state.result_slots[0]);
    REQUIRE_FALSE(run_result.has_value());
    REQUIRE(state.result_slots[0].empty());
    REQUIRE(state.result_slots[0].type_hash == 0);
}

TEST_CASE (



"TaskCommand stores Unit for void output in RuntimeState result slot"
,
"[pravaha][runtime][result_slot][output]"
)
 {
    auto ir_result = pravaha::lower_to_ir(pravaha::task("store_void", []() -> void {
    }));
    REQUIRE(ir_result.has_value());
    auto &ir = ir_result.value();

    auto state = pravaha::RuntimeState::build(ir);
    auto run_result = ir.nodes[0].command.run(&state.result_slots[0]);
    REQUIRE(run_result.has_value());
    auto *stored = state.result_slots[0].get_if<pravaha::Unit>();
    REQUIRE(stored != nullptr);
}

TEST_CASE (



"single predecessor int payload forwards to one-arg task"
,
"[pravaha][runtime][forward]"
)
 {
    int observed = 0;
    pravaha::Runner<> runner;
    auto expr = pravaha::task("a", []() -> int { return 41; })
                | pravaha::task("b", [&observed](int value) -> int {
                    observed = value;
                    return value + 1;
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == 41);
}

TEST_CASE (



"single predecessor string payload forwards to one-arg task"
,
"[pravaha][runtime][forward]"
)
 {
    std::string observed;
    pravaha::Runner<> runner;
    auto expr = pravaha::task("a", []() -> std::string { return "hello"; })
                | pravaha::task("b", [&observed](const std::string &value) -> std::string {
                    observed = value;
                    return value + " world";
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == "hello");
}

TEST_CASE (



"Outcome<int> payload forwards to int input"
,
"[pravaha][runtime][forward]"
)
 {
    int observed = 0;
    pravaha::Runner<> runner;
    auto expr = pravaha::task("a", []() -> pravaha::Outcome<int> { return 13; })
                | pravaha::task("b", [&observed](int value) -> pravaha::Unit {
                    observed = value;
                    return {};
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == 13);
}

TEST_CASE (



"Outcome<string> payload forwards to string input"
,
"[pravaha][runtime][forward]"
)
 {
    std::string observed;
    pravaha::Runner<> runner;
    auto expr = pravaha::task("a", []() -> pravaha::Outcome<std::string> { return std::string{"ok"}; })
                | pravaha::task("b", [&observed](const std::string &value) -> pravaha::Unit {
                    observed = value;
                    return {};
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == "ok");
}

TEST_CASE (



"Outcome<int> failure blocks downstream"
,
"[pravaha][runtime][forward]"
)
 {
    int downstream_runs = 0;
    pravaha::Runner<> runner;
    auto expr = pravaha::task("a", []() -> pravaha::Outcome<int> {
        return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "no value"});
    }) | pravaha::task("b", [&downstream_runs](int) -> pravaha::Unit {
        ++downstream_runs;
        return {};
    });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->failed());
    REQUIRE(downstream_runs == 0);
}

TEST_CASE (



"Outcome<int> stores type hash of int"
,
"[pravaha][runtime][result_slot][output]"
)
 {
    auto ir_result = pravaha::lower_to_ir(pravaha::task("store_outcome_int_hash", []() -> pravaha::Outcome<int> {
        return 23;
    }));
    REQUIRE(ir_result.has_value());
    auto &ir = ir_result.value();

    auto state = pravaha::RuntimeState::build(ir);
    auto run_result = ir.nodes[0].command.run(&state.result_slots[0]);
    REQUIRE(run_result.has_value());
    REQUIRE(state.result_slots[0].type_hash == pravaha::make_type_contract<int>().type_hash);
    REQUIRE(state.result_slots[0].type_hash != pravaha::runtime_type_hash<pravaha::Outcome<int>>());
}

TEST_CASE (



"single predecessor type mismatch fails validation"
,
"[pravaha][runtime][forward]"
)
 {
    pravaha::Runner<> runner;
    auto expr = pravaha::task("a", []() -> int { return 1; })
                | pravaha::task("b", [](std::string) -> pravaha::Unit { return {}; });

    auto result = runner.submit(std::move(expr));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == pravaha::ErrorKind::TypeMismatch);
}

TEST_CASE (



"zero-arg task remains unaffected by forwarding path"
,
"[pravaha][runtime][forward]"
)
 {
    int runs = 0;
    pravaha::Runner<> runner;
    auto expr = pravaha::task("a", []() -> int { return 9; })
                | pravaha::task("b", [&runs]() -> pravaha::Unit {
                    ++runs;
                    return {};
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(runs == 1);
}

TEST_CASE (



"multiple predecessors to one-input task are rejected"
,
"[pravaha][runtime][forward]"
)
 {
    auto expr = (pravaha::task("a", []() -> int { return 1; })
                 & pravaha::task("b", []() -> int { return 2; }))
                | pravaha::task("c", [](int) -> pravaha::Unit { return {}; });

    auto ir = pravaha::lower_to_ir(expr);
    REQUIRE(ir.has_value());
    auto validation = pravaha::validate_data_contracts(*ir);
    REQUIRE_FALSE(validation.has_value());
    REQUIRE(validation.error().kind == pravaha::ErrorKind::ValidationError);

    pravaha::Runner<> runner;
    auto result = runner.submit(std::move(expr));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == pravaha::ErrorKind::ValidationError);
}

TEST_CASE (



"any_success forwards first successful payload"
,
"[pravaha][runtime][join_forward]"
)
 {
    int observed = 0;
    pravaha::Runner<> runner;
    auto expr = pravaha::any_success_of(
                    pravaha::task("a", []() -> int { return 7; }),
                    pravaha::task("b", []() -> int { return 11; })
                ) | pravaha::task("c", [&observed](int value) -> pravaha::Unit {
                    observed = value;
                    return {};
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == 7);
}

TEST_CASE (



"any_success forwards payload when first branch succeeds and second fails"
,
"[pravaha][runtime][join_forward]"
)
 {
    int observed = 0;
    pravaha::Runner<> runner;
    auto expr = pravaha::any_success_of(
                    pravaha::task("a", []() -> int { return 1; }),
                    pravaha::task("b", []() -> pravaha::Outcome<int> {
                        return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "fail"});
                    })
                ) | pravaha::task("c", [&observed](int value) -> pravaha::Unit {
                    observed = value;
                    return {};
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == 1);
}

TEST_CASE (



"any_success forwards payload when first branch fails and second succeeds"
,
"[pravaha][runtime][join_forward]"
)
 {
    int observed = 0;
    pravaha::Runner<> runner;
    auto expr = pravaha::any_success_of(
                    pravaha::task("a", []() -> pravaha::Outcome<int> {
                        return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "fail"});
                    }),
                    pravaha::task("b", []() -> int { return 2; })
                ) | pravaha::task("c", [&observed](int value) -> pravaha::Unit {
                    observed = value;
                    return {};
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == 2);
}

TEST_CASE (



"any_success payload winner is not overwritten by late success"
,
"[pravaha][runtime][join_forward]"
)
 {
    pravaha::TaskIr ir;
    ir.add_node("a", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() -> int { return 1; }));
    ir.add_node("b", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() -> int { return 2; }));
    ir.add_node("c", pravaha::ExecutionDomain::CPU,
                pravaha::TaskCommand::make([](int) -> pravaha::Unit { return {}; }));
    ir.add_edge(pravaha::TaskId{0}, pravaha::TaskId{2}, pravaha::EdgeKind::Sequence);
    ir.add_edge(pravaha::TaskId{1}, pravaha::TaskId{2}, pravaha::EdgeKind::Sequence);
    ir.add_join_group({pravaha::TaskId{0}, pravaha::TaskId{1}},
                      pravaha::JoinPolicy{pravaha::JoinPolicyKind::AnySuccess, 0});

    auto state = pravaha::RuntimeState::build(ir);
    auto run_a = ir.nodes[0].command.run(&state.result_slots[0]);
    REQUIRE(run_a.has_value());
    state.mark_succeeded(0);
    REQUIRE(state.joins[0].resolved);
    REQUIRE(state.joins[0].success);
    auto *first_value = state.join_result_slots[0].get_if<int>();
    REQUIRE(first_value != nullptr);
    REQUIRE(*first_value == 1);

    auto run_b = ir.nodes[1].command.run(&state.result_slots[1]);
    REQUIRE(run_b.has_value());
    state.mark_succeeded(1);
    auto *winner_value = state.join_result_slots[0].get_if<int>();
    REQUIRE(winner_value != nullptr);
    REQUIRE(*winner_value == 1);
}

TEST_CASE (



"quorum forwards payload from branch that resolves quorum"
,
"[pravaha][runtime][join_forward]"
)
 {
    int observed = 0;
    pravaha::Runner<> runner;
    auto expr = pravaha::quorum_of<2>(
                    pravaha::task("a", []() -> int { return 3; }),
                    pravaha::task("b", []() -> int { return 5; })
                ) | pravaha::task("c", [&observed](int value) -> pravaha::Unit {
                    observed = value;
                    return {};
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == 5);
}

TEST_CASE (



"quorum<1> forwards first successful payload when other branch fails"
,
"[pravaha][runtime][join_forward]"
)
 {
    int observed = 0;
    pravaha::Runner<> runner;
    auto expr = pravaha::quorum_of<1>(
                    pravaha::task("a", []() -> int { return 1; }),
                    pravaha::task("b", []() -> pravaha::Outcome<int> {
                        return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "fail"});
                    })
                ) | pravaha::task("c", [&observed](int value) -> pravaha::Unit {
                    observed = value;
                    return {};
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == 1);
}

TEST_CASE (



"quorum<2> with one failure blocks downstream"
,
"[pravaha][runtime][join_forward]"
)
 {
    int downstream_runs = 0;
    pravaha::Runner<> runner;
    auto expr = pravaha::quorum_of<2>(
                    pravaha::task("a", []() -> int { return 1; }),
                    pravaha::task("b", []() -> pravaha::Outcome<int> {
                        return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "fail"});
                    })
                ) | pravaha::task("c", [&downstream_runs](int) -> pravaha::Unit {
                    ++downstream_runs;
                    return {};
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->failed());
    REQUIRE(downstream_runs == 0);
}

TEST_CASE (



"collect_all forwards aggregated payload"
,
"[pravaha][runtime][join_forward]"
)
 {
    std::vector<int> observed;
    pravaha::Runner<> runner;
    auto expr = pravaha::collect_all_of(
                    pravaha::task("a", []() -> int { return 2; }),
                    pravaha::task("b", []() -> int { return 4; })
                ) | pravaha::task("c", [&observed](const std::vector<int> &values) -> pravaha::Unit {
                    observed = values;
                    return {};
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == std::vector<int>{2, 4});
}

TEST_CASE (



"join runtime state - AnySuccess ignores late terminal updates after success"
,
"[pravaha][runtime][join]"
)
 {
    pravaha::RuntimeState state;
    state.joins.push_back(pravaha::JoinRuntimeState{pravaha::JoinPolicy{pravaha::JoinPolicyKind::AnySuccess, 0}, 2});

    state.record_join_terminal(0, pravaha::TaskState::Succeeded);
    REQUIRE(state.joins[0].resolved == true);
    REQUIRE(state.joins[0].success == true);
    REQUIRE(state.joins[0].succeeded == 1);
    REQUIRE(state.joins[0].failed == 0);

    state.record_join_terminal(0, pravaha::TaskState::Failed);
    REQUIRE(state.joins[0].resolved == true);
    REQUIRE(state.joins[0].success == true);
    REQUIRE(state.joins[0].succeeded == 1);
    REQUIRE(state.joins[0].failed == 0);
}

TEST_CASE (



"join runtime state - Quorum ignores late terminal updates after success"
,
"[pravaha][runtime][join]"
)
 {
    pravaha::RuntimeState state;
    state.joins.push_back(pravaha::JoinRuntimeState{pravaha::JoinPolicy{pravaha::JoinPolicyKind::Quorum, 1}, 2});

    state.record_join_terminal(0, pravaha::TaskState::Succeeded);
    REQUIRE(state.joins[0].resolved == true);
    REQUIRE(state.joins[0].success == true);
    REQUIRE(state.joins[0].succeeded == 1);
    REQUIRE(state.joins[0].failed == 0);

    state.record_join_terminal(0, pravaha::TaskState::Failed);
    REQUIRE(state.joins[0].resolved == true);
    REQUIRE(state.joins[0].success == true);
    REQUIRE(state.joins[0].succeeded == 1);
    REQUIRE(state.joins[0].failed == 0);
}

TEST_CASE (



"join runtime state - CollectAll continues counting after partial success"
,
"[pravaha][runtime][join]"
)
 {
    pravaha::RuntimeState state;
    state.joins.push_back(pravaha::JoinRuntimeState{pravaha::JoinPolicy{pravaha::JoinPolicyKind::CollectAll, 0}, 2});

    state.record_join_terminal(0, pravaha::TaskState::Succeeded);
    REQUIRE(state.joins[0].resolved == false);
    REQUIRE(state.joins[0].succeeded == 1);

    state.record_join_terminal(0, pravaha::TaskState::Failed);
    REQUIRE(state.joins[0].resolved == true);
    REQUIRE(state.joins[0].success == false);
    REQUIRE(state.joins[0].succeeded == 1);
    REQUIRE(state.joins[0].failed == 1);
}

TEST_CASE (



"join runtime state - AnySuccess releases downstream on first success"
,
"[pravaha][runtime][join][early-release]"
)
 {
    pravaha::TaskIr ir;
    ir.add_node("fast_success", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_node("long_success", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_node("downstream", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_edge(pravaha::TaskId{0}, pravaha::TaskId{2}, pravaha::EdgeKind::Sequence);
    ir.add_edge(pravaha::TaskId{1}, pravaha::TaskId{2}, pravaha::EdgeKind::Sequence);
    ir.add_join_group({pravaha::TaskId{0}, pravaha::TaskId{1}},
                      pravaha::JoinPolicy{pravaha::JoinPolicyKind::AnySuccess, 0});

    auto state = pravaha::RuntimeState::build(ir);
    state.mark_succeeded(0);

    REQUIRE(state.node_states[2] == pravaha::TaskState::Ready);
}

TEST_CASE (



"join runtime state - Quorum<1> releases downstream on first success"
,
"[pravaha][runtime][join][early-release]"
)
 {
    pravaha::TaskIr ir;
    ir.add_node("fast_success", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_node("long_success", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_node("downstream", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_edge(pravaha::TaskId{0}, pravaha::TaskId{2}, pravaha::EdgeKind::Sequence);
    ir.add_edge(pravaha::TaskId{1}, pravaha::TaskId{2}, pravaha::EdgeKind::Sequence);
    ir.add_join_group({pravaha::TaskId{0}, pravaha::TaskId{1}},
                      pravaha::JoinPolicy{pravaha::JoinPolicyKind::Quorum, 1});

    auto state = pravaha::RuntimeState::build(ir);
    state.mark_succeeded(0);

    REQUIRE(state.node_states[2] == pravaha::TaskState::Ready);
}

TEST_CASE (



"join runtime state - Quorum<2> waits for both successes"
,
"[pravaha][runtime][join][early-release]"
)
 {
    pravaha::TaskIr ir;
    ir.add_node("first_success", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_node("second_success", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_node("downstream", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_edge(pravaha::TaskId{0}, pravaha::TaskId{2}, pravaha::EdgeKind::Sequence);
    ir.add_edge(pravaha::TaskId{1}, pravaha::TaskId{2}, pravaha::EdgeKind::Sequence);
    ir.add_join_group({pravaha::TaskId{0}, pravaha::TaskId{1}},
                      pravaha::JoinPolicy{pravaha::JoinPolicyKind::Quorum, 2});

    auto state = pravaha::RuntimeState::build(ir);
    state.mark_succeeded(0);
    REQUIRE(state.node_states[2] == pravaha::TaskState::Created);

    state.mark_succeeded(1);
    REQUIRE(state.node_states[2] == pravaha::TaskState::Ready);
}

TEST_CASE (



"join runtime state - CollectAll waits for both successes"
,
"[pravaha][runtime][join][early-release]"
)
 {
    pravaha::TaskIr ir;
    ir.add_node("first_success", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_node("second_success", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_node("downstream", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_edge(pravaha::TaskId{0}, pravaha::TaskId{2}, pravaha::EdgeKind::Sequence);
    ir.add_edge(pravaha::TaskId{1}, pravaha::TaskId{2}, pravaha::EdgeKind::Sequence);
    ir.add_join_group({pravaha::TaskId{0}, pravaha::TaskId{1}},
                      pravaha::JoinPolicy{pravaha::JoinPolicyKind::CollectAll, 0});

    auto state = pravaha::RuntimeState::build(ir);
    state.mark_succeeded(0);

    REQUIRE(state.node_states[2] == pravaha::TaskState::Created);
}

TEST_CASE (



"join runtime state - AllOrNothing waits for both successes"
,
"[pravaha][runtime][join][early-release]"
)
 {
    pravaha::TaskIr ir;
    ir.add_node("first_success", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_node("second_success", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_node("downstream", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_edge(pravaha::TaskId{0}, pravaha::TaskId{2}, pravaha::EdgeKind::Sequence);
    ir.add_edge(pravaha::TaskId{1}, pravaha::TaskId{2}, pravaha::EdgeKind::Sequence);
    ir.add_join_group({pravaha::TaskId{0}, pravaha::TaskId{1}},
                      pravaha::JoinPolicy{pravaha::JoinPolicyKind::AllOrNothing, 0});

    auto state = pravaha::RuntimeState::build(ir);
    state.mark_succeeded(0);

    REQUIRE(state.node_states[2] == pravaha::TaskState::Created);
}

// ============================================================================
// SECTION 15: LiteGraph Validation Layer
// ============================================================================

TEST_CASE (



"validate_ir_with_litegraph - valid sequence passes"
,
"[pravaha][litegraph]"
)
 {
    // A | B - simple valid sequence
    auto a = pravaha::task("A", []() {
    });
    auto b = pravaha::task("B", []() {
    });
    auto expr = std::move(a) | std::move(b);
    auto ir_result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(ir_result.has_value());

    auto validation = pravaha::validate_ir_with_litegraph(ir_result.value());
    REQUIRE(validation.has_value());
}

TEST_CASE (



"validate_ir_with_litegraph - invalid edge endpoint fails"
,
"[pravaha][litegraph]"
)
 {
    // Manually construct invalid IR with bad edge endpoint
    pravaha::TaskIr ir;
    ir.add_node("A", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_node("B", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    // Edge referencing non-existent TaskId{99}
    ir.add_edge(pravaha::TaskId{0}, pravaha::TaskId{99}, pravaha::EdgeKind::Sequence);

    auto validation = pravaha::validate_ir_with_litegraph(ir);
    REQUIRE(!validation.has_value());
    REQUIRE(validation.error().kind == pravaha::ErrorKind::ValidationError);
}

TEST_CASE (



"validate_ir_with_litegraph - cycle detected"
,
"[pravaha][litegraph]"
)
 {
    // Manually construct IR with a cycle: A->B->A
    pravaha::TaskIr ir;
    ir.add_node("A", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_node("B", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_edge(pravaha::TaskId{0}, pravaha::TaskId{1}, pravaha::EdgeKind::Sequence);
    ir.add_edge(pravaha::TaskId{1}, pravaha::TaskId{0}, pravaha::EdgeKind::Sequence);

    auto validation = pravaha::validate_ir_with_litegraph(ir);
    REQUIRE(!validation.has_value());
    REQUIRE(validation.error().kind == pravaha::ErrorKind::CycleDetected);
}

TEST_CASE (



"topological_order - A | B returns A before B"
,
"[pravaha][litegraph]"
)
 {
    auto a = pravaha::task("A", []() {
    });
    auto b = pravaha::task("B", []() {
    });
    auto expr = std::move(a) | std::move(b);
    auto ir_result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(ir_result.has_value());

    auto topo_result = pravaha::topological_order(ir_result.value());
    REQUIRE(topo_result.has_value());
    auto &order = topo_result.value();
    REQUIRE(order.size() == 2);

    // Find positions of A and B
    std::size_t a_pos = ~std::size_t{0}, b_pos = ~std::size_t{0};
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] == pravaha::TaskId{0}) a_pos = i;
        if (order[i] == pravaha::TaskId{1}) b_pos = i;
    }
    REQUIRE(a_pos < b_pos); // A must come before B
}

TEST_CASE (



"topological_order - (A & B) | C returns A/B before C"
,
"[pravaha][litegraph]"
)
 {
    auto a = pravaha::task("A", []() {
    });
    auto b = pravaha::task("B", []() {
    });
    auto c = pravaha::task("C", []() {
    });
    auto expr = (std::move(a) & std::move(b)) | std::move(c);
    auto ir_result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(ir_result.has_value());

    auto topo_result = pravaha::topological_order(ir_result.value());
    REQUIRE(topo_result.has_value());
    auto &order = topo_result.value();
    REQUIRE(order.size() == 3);

    // Find positions
    std::size_t a_pos = ~std::size_t{0};
    std::size_t b_pos = ~std::size_t{0};
    std::size_t c_pos = ~std::size_t{0};
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] == pravaha::TaskId{0}) a_pos = i;
        if (order[i] == pravaha::TaskId{1}) b_pos = i;
        if (order[i] == pravaha::TaskId{2}) c_pos = i;
    }
    // A and B must both come before C
    REQUIRE(a_pos < c_pos);
    REQUIRE(b_pos < c_pos);
}

TEST_CASE (



"topological_order - repeated calls are stable"
,
"[pravaha][litegraph]"
)
 {
    auto a = pravaha::task("A", []() {
    });
    auto b = pravaha::task("B", []() {
    });
    auto c = pravaha::task("C", []() {
    });
    auto expr = std::move(a) | std::move(b) | std::move(c);
    auto ir_result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(ir_result.has_value());

    auto order1 = pravaha::topological_order(ir_result.value());
    auto order2 = pravaha::topological_order(ir_result.value());
    REQUIRE(order1.has_value());
    REQUIRE(order2.has_value());
    REQUIRE(order1.value() == order2.value());
}

TEST_CASE (



"validate_ir_with_litegraph - mutation invalidates cached dependency graph"
,
"[pravaha][litegraph][regression]"
)
 {
    pravaha::TaskIr ir;
    ir.add_node("A", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_node("B", pravaha::ExecutionDomain::CPU, pravaha::TaskCommand::make([]() {
    }));
    ir.add_edge(pravaha::TaskId{0}, pravaha::TaskId{1}, pravaha::EdgeKind::Sequence);

    auto first_validation = pravaha::validate_ir_with_litegraph(ir);
    REQUIRE(first_validation.has_value());

    ir.add_edge(pravaha::TaskId{1}, pravaha::TaskId{0}, pravaha::EdgeKind::Sequence);
    auto second_validation = pravaha::validate_ir_with_litegraph(ir);
    REQUIRE(!second_validation.has_value());
    REQUIRE(second_validation.error().kind == pravaha::ErrorKind::CycleDetected);

    auto topo_result = pravaha::topological_order(ir);
    REQUIRE(!topo_result.has_value());
    REQUIRE(topo_result.error().kind == pravaha::ErrorKind::CycleDetected);
}

TEST_CASE (



"to_litegraph - converts valid IR to ExecutionGraph"
,
"[pravaha][litegraph]"
)
 {
    auto a = pravaha::task("A", []() {
    });
    auto b = pravaha::task("B", []() {
    });
    auto expr = std::move(a) | std::move(b);
    auto ir_result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(ir_result.has_value());

    auto graph_result = pravaha::to_litegraph(ir_result.value());
    REQUIRE(graph_result.has_value());
    auto &graph = graph_result.value();
    REQUIRE(graph.node_count() == 2);
    REQUIRE(graph.edge_count() == 1);
}

// ============================================================================
// SECTION 16: Runner & InlineBackend
// ============================================================================

TEST_CASE (



"Runner - single task runs on submit"
,
"[pravaha][runner]"
)
 {
    int counter = 0;
    pravaha::Runner<> runner;
    auto expr = pravaha::task("A", [&counter]() { ++counter; });
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(counter == 1);
    REQUIRE(result.value().final_state == pravaha::TaskState::Succeeded);
    REQUIRE(result.value().node_states.size() == 1);
    REQUIRE(result.value().node_states[0] == pravaha::TaskState::Succeeded);
}

TEST_CASE (



"Runner - sequence runs A before B"
,
"[pravaha][runner]"
)
 {
    std::vector<int> order;
    pravaha::Runner<> runner;
    auto a = pravaha::task("A", [&order]() { order.push_back(1); });
    auto b = pravaha::task("B", [&order]() { order.push_back(2); });
    auto expr = std::move(a) | std::move(b);
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(order.size() == 2);
    REQUIRE(order[0] == 1);
    REQUIRE(order[1] == 2);
    REQUIRE(result.value().final_state == pravaha::TaskState::Succeeded);
}

TEST_CASE (



"Runner - if A fails, B is skipped"
,
"[pravaha][runner]"
)
 {
    int b_counter = 0;
    pravaha::Runner<> runner;
    auto a = pravaha::task("A", []() { throw std::runtime_error("fail"); });
    auto b = pravaha::task("B", [&b_counter]() { ++b_counter; });
    auto expr = std::move(a) | std::move(b);
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(b_counter == 0);
    REQUIRE(result.value().final_state == pravaha::TaskState::Failed);
    REQUIRE(result.value().node_states[0] == pravaha::TaskState::Failed);
    REQUIRE(result.value().node_states[1] == pravaha::TaskState::Skipped);
    REQUIRE(result.value().errors.size() == 1);
}

TEST_CASE (



"Runner - construction is lazy before submit"
,
"[pravaha][runner]"
)
 {
    int counter = 0;
    pravaha::Runner<> runner;
    auto expr = pravaha::task("A", [&counter]() { ++counter; });
    REQUIRE(counter == 0);
    auto result = runner.submit(std::move(expr));
    REQUIRE(counter == 1);
}

TEST_CASE (



"Runner - final result is Failed if chain fails"
,
"[pravaha][runner]"
)
 {
    pravaha::Runner<> runner;
    auto a = pravaha::task("A", []() {
    });
    auto b = pravaha::task("B", []() { throw std::runtime_error("boom"); });
    auto expr = std::move(a) | std::move(b);
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result.value().final_state == pravaha::TaskState::Failed);
}

TEST_CASE (



"Runner - final result is Succeeded if chain succeeds"
,
"[pravaha][runner]"
)
 {
    pravaha::Runner<> runner;
    auto a = pravaha::task("A", []() {
    });
    auto b = pravaha::task("B", []() {
    });
    auto c = pravaha::task("C", []() {
    });
    auto expr = std::move(a) | std::move(b) | std::move(c);
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result.value().final_state == pravaha::TaskState::Succeeded);
    REQUIRE(result.value().node_states.size() == 3);
    for (auto s: result.value().node_states)
        REQUIRE(s == pravaha::TaskState::Succeeded);
}

TEST_CASE (



"Runner - LiteGraph validation is called before execution"
,
"[pravaha][runner]"
)
 {
    int counter = 0;
    pravaha::Runner<> runner;
    auto a = pravaha::task("A", [&counter]() { ++counter; });
    auto b = pravaha::task("B", [&counter]() { ++counter; });
    auto expr = (std::move(a) & std::move(b));
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(counter == 2);
    REQUIRE(result.value().final_state == pravaha::TaskState::Succeeded);
}

TEST_CASE (



"Runner - data contract validation is called before execution"
,
"[pravaha][runner]"
)
 {
    int counter = 0;
    pravaha::Runner<> runner;
    auto expr = pravaha::task("a", [&counter]() -> int {
                    ++counter;
                    return 1;
                })
                | pravaha::task("b", [](std::string) -> pravaha::Unit { return {}; });
    auto result = runner.submit(std::move(expr));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == pravaha::ErrorKind::TypeMismatch);
    REQUIRE(counter == 0);
}

struct ControlledBackend {
    mutable std::mutex mutex;
    std::deque<pravaha::TaskCommand> queue;
    bool stop_requested{false};

    bool submit(pravaha::TaskCommand cmd) {
        std::lock_guard lock(mutex);
        if (stop_requested) return false;
        queue.push_back(std::move(cmd));
        return true;
    }

    void drain() noexcept {}

    void request_stop() noexcept {
        std::lock_guard lock(mutex);
        stop_requested = true;
    }

    [[nodiscard]] bool stopped() const noexcept {
        std::lock_guard lock(mutex);
        return stop_requested;
    }

    [[nodiscard]] std::size_t queue_size() const {
        std::lock_guard lock(mutex);
        return queue.size();
    }

    bool run_front() {
        pravaha::TaskCommand cmd;
        {
            std::lock_guard lock(mutex);
            if (queue.empty()) return false;
            cmd = std::move(queue.front());
            queue.pop_front();
        }
        return cmd.run().has_value();
    }

    bool run_back() {
        pravaha::TaskCommand cmd;
        {
            std::lock_guard lock(mutex);
            if (queue.empty()) return false;
            cmd = std::move(queue.back());
            queue.pop_back();
        }
        return cmd.run().has_value();
    }
};

TEST_CASE (



"Runner cancellation - cancel before submit marks pending nodes canceled and skipped"
,
"[pravaha][runner][cancellation]"
)
 {
    pravaha::CancellationSource source;
    source.request_stop();

    std::atomic ran{0};
    pravaha::Runner<> runner;
    auto expr =
            pravaha::task("A", [&ran]() { ran.fetch_add(1); })
            | pravaha::task("B", [&ran]() { ran.fetch_add(1); })
            | pravaha::task("C", [&ran]() { ran.fetch_add(1); });

    auto result = runner.submit(std::move(expr), source.token());
    REQUIRE(result.has_value());
    REQUIRE(ran.load() == 0);
    REQUIRE(result->final_state == pravaha::TaskState::Canceled);
    REQUIRE(result->node_states.size() == 3);
    REQUIRE(result->node_states[0] == pravaha::TaskState::Canceled);
    REQUIRE(result->node_states[1] == pravaha::TaskState::Skipped);
    REQUIRE(result->node_states[2] == pravaha::TaskState::Skipped);
}

TEST_CASE (



"Runner cancellation - cancel after first task in sequence prevents downstream execution"
,
"[pravaha][runner][cancellation]"
)
 {
    auto wait_for_queue_at_least = [](ControlledBackend &backend, std::size_t min_size) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            if (backend.queue_size() >= min_size) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return backend.queue_size() >= min_size;
    };

    ControlledBackend backend;
    pravaha::Runner runner(backend);
    std::atomic first{0};
    std::atomic second{0};

    auto expr =
            pravaha::task("first", [&first]() { first.fetch_add(1); })
            | pravaha::task("second", [&second]() { second.fetch_add(1); });

    auto fut = std::async(std::launch::async, [&runner, expr = std::move(expr)]() mutable {
        return runner.submit(std::move(expr));
    });

    REQUIRE(wait_for_queue_at_least(backend, 1));
    REQUIRE(backend.run_front());
    runner.request_stop();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready
           && std::chrono::steady_clock::now() < deadline) {
        if (!backend.run_front()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    REQUIRE(fut.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    auto result = fut.get();
    REQUIRE(result.has_value());
    REQUIRE(first.load() == 1);
    REQUIRE(second.load() == 0);
    REQUIRE(result->final_state == pravaha::TaskState::Canceled);
    REQUIRE(result->node_states[1] == pravaha::TaskState::Canceled);
}

TEST_CASE (



"Runner cancellation - cancel during parallel prevents unstarted branches from running"
,
"[pravaha][runner][cancellation]"
)
 {
    auto wait_for_queue_at_least = [](ControlledBackend &backend, std::size_t min_size) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            if (backend.queue_size() >= min_size) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return backend.queue_size() >= min_size;
    };

    ControlledBackend backend;
    pravaha::Runner runner(backend);
    std::atomic a_runs{0};
    std::atomic b_runs{0};

    auto expr =
            pravaha::task("A", [&a_runs]() { a_runs.fetch_add(1); })
            & pravaha::task("B", [&b_runs]() { b_runs.fetch_add(1); });

    auto fut = std::async(std::launch::async, [&runner, expr = std::move(expr)]() mutable {
        return runner.submit(std::move(expr));
    });

    REQUIRE(wait_for_queue_at_least(backend, 2));
    REQUIRE(backend.run_front());
    runner.request_stop();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready
           && std::chrono::steady_clock::now() < deadline) {
        if (!backend.run_front()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    REQUIRE(fut.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    auto result = fut.get();
    REQUIRE(result.has_value());
    REQUIRE(a_runs.load() + b_runs.load() == 1);
    REQUIRE(result->final_state == pravaha::TaskState::Canceled);
    REQUIRE(std::count(result->node_states.begin(), result->node_states.end(), pravaha::TaskState::Canceled) == 1);
}

TEST_CASE (



"Runner cancellation - running task may complete but downstream does not start"
,
"[pravaha][runner][cancellation]"
)
 {
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool started = false;
    bool release = false;

    std::atomic first_runs{0};
    std::atomic second_runs{0};

    pravaha::JThreadBackend backend(1);
    pravaha::Runner runner(backend);

    auto expr =
            pravaha::task("first", [&]() {
                std::unique_lock lock(gate_mutex);
                started = true;
                gate_cv.notify_all();
                gate_cv.wait(lock, [&]() { return release; });
                first_runs.fetch_add(1);
            })
            | pravaha::task("second", [&]() { second_runs.fetch_add(1); });

    auto fut = std::async(std::launch::async, [&runner, expr = std::move(expr)]() mutable {
        return runner.submit(std::move(expr));
    });

    {
        std::unique_lock lock(gate_mutex);
        const auto started_in_time = gate_cv.wait_for(lock, std::chrono::seconds(1), [&]() { return started; });
        REQUIRE(started_in_time);
    }

    runner.request_stop();

    {
        std::lock_guard lock(gate_mutex);
        release = true;
    }
    gate_cv.notify_all();

    REQUIRE(fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    auto result = fut.get();
    REQUIRE(result.has_value());
    REQUIRE(first_runs.load() == 1);
    REQUIRE(second_runs.load() == 0);
    REQUIRE(result->final_state == pravaha::TaskState::Canceled);
    REQUIRE(result->node_states[0] == pravaha::TaskState::Succeeded);
    REQUIRE(result->node_states[1] == pravaha::TaskState::Canceled);
}


TEST_CASE (



"Runner controlled backend - early release semantics"
,
"[pravaha][runner][parallel][early-release][controlled]"
)
 {
    auto wait_for_queue_at_least = [](ControlledBackend &backend, std::size_t min_size) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            if (backend.queue_size() >= min_size) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return backend.queue_size() >= min_size;
    };

    auto finish_runner = [](ControlledBackend &backend, auto &fut) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready
               && std::chrono::steady_clock::now() < deadline) {
            if (!backend.run_front()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return fut.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
    };

    SECTION("any_success_of releases downstream before slow branch") {
        ControlledBackend backend;
        pravaha::Runner runner(backend);
        std::vector<std::string> events;

        auto expr = pravaha::any_success_of(
                        pravaha::task("fast_success", [&] { events.push_back("fast"); }),
                        pravaha::task("slow_success", [&] { events.push_back("slow"); })
                    ) | pravaha::task("downstream", [&] { events.push_back("downstream"); });

        auto fut = std::async(std::launch::async, [&runner, expr = std::move(expr)]() mutable {
            return runner.submit(std::move(expr));
        });

        REQUIRE(wait_for_queue_at_least(backend, 2));
        REQUIRE(backend.run_front());
        REQUIRE(wait_for_queue_at_least(backend, 2));
        REQUIRE(backend.run_back());
        REQUIRE(events.size() >= 2);
        REQUIRE(events[0] == "fast");
        REQUIRE(events[1] == "downstream");

        REQUIRE(finish_runner(backend, fut));
        auto result = fut.get();
        REQUIRE(result.has_value());
        REQUIRE(result->succeeded());
    }

    SECTION("any_success_of late failure does not flip final result") {
        ControlledBackend backend;
        pravaha::Runner runner(backend);
        std::vector<std::string> events;

        auto expr = pravaha::any_success_of(
                        pravaha::task("fast_success", [&] { events.push_back("fast"); }),
                        pravaha::task("late_fail", [&]() -> pravaha::Outcome<pravaha::Unit> {
                            events.push_back("late_fail");
                            return std::unexpected(
                                pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "late failure"});
                        })
                    ) | pravaha::task("downstream", [&] { events.push_back("downstream"); });

        auto fut = std::async(std::launch::async, [&runner, expr = std::move(expr)]() mutable {
            return runner.submit(std::move(expr));
        });

        REQUIRE(wait_for_queue_at_least(backend, 2));
        REQUIRE(backend.run_front());
        REQUIRE(wait_for_queue_at_least(backend, 2));
        REQUIRE(backend.run_back());
        REQUIRE(events.size() >= 2);
        REQUIRE(events[0] == "fast");
        REQUIRE(events[1] == "downstream");

        REQUIRE(finish_runner(backend, fut));
        auto result = fut.get();
        REQUIRE(result.has_value());
        REQUIRE(result->succeeded());
    }

    SECTION("AnySuccess releases downstream before slow branch") {
        ControlledBackend backend;
        pravaha::Runner runner(backend);
        std::vector<std::string> events;

        auto expr = pravaha::any_success(
                        pravaha::task("fast_success", [&] { events.push_back("fast"); })
                        &
                        pravaha::task("slow_success", [&] { events.push_back("slow"); })
                    ) | pravaha::task("downstream", [&] { events.push_back("downstream"); });

        auto fut = std::async(std::launch::async, [&runner, expr = std::move(expr)]() mutable {
            return runner.submit(std::move(expr));
        });

        REQUIRE(wait_for_queue_at_least(backend, 2));
        REQUIRE(backend.run_front());
        REQUIRE(wait_for_queue_at_least(backend, 2));
        REQUIRE(backend.run_back());
        REQUIRE(events.size() >= 2);
        REQUIRE(events[0] == "fast");
        REQUIRE(events[1] == "downstream");

        REQUIRE(finish_runner(backend, fut));
        auto result = fut.get();
        REQUIRE(result.has_value());
        REQUIRE(result->succeeded());
    }

    SECTION("Quorum<1> releases downstream before slow branch") {
        ControlledBackend backend;
        pravaha::Runner runner(backend);
        std::vector<std::string> events;

        auto expr = pravaha::quorum<1>(
                        pravaha::task("fast_success", [&] { events.push_back("fast"); })
                        &
                        pravaha::task("slow_success", [&] { events.push_back("slow"); })
                    ) | pravaha::task("downstream", [&] { events.push_back("downstream"); });

        auto fut = std::async(std::launch::async, [&runner, expr = std::move(expr)]() mutable {
            return runner.submit(std::move(expr));
        });

        REQUIRE(wait_for_queue_at_least(backend, 2));
        REQUIRE(backend.run_front());
        REQUIRE(wait_for_queue_at_least(backend, 2));
        REQUIRE(backend.run_back());
        REQUIRE(events.size() >= 2);
        REQUIRE(events[0] == "fast");
        REQUIRE(events[1] == "downstream");

        REQUIRE(finish_runner(backend, fut));
        auto result = fut.get();
        REQUIRE(result.has_value());
        REQUIRE(result->succeeded());
    }

    SECTION("Quorum<2> waits for both successes") {
        ControlledBackend backend;
        pravaha::Runner runner(backend);
        std::vector<std::string> events;

        auto expr = pravaha::quorum<2>(
                        pravaha::task("a", [&] { events.push_back("a"); })
                        &
                        pravaha::task("b", [&] { events.push_back("b"); })
                    ) | pravaha::task("downstream", [&] { events.push_back("downstream"); });

        auto fut = std::async(std::launch::async, [&runner, expr = std::move(expr)]() mutable {
            return runner.submit(std::move(expr));
        });

        REQUIRE(wait_for_queue_at_least(backend, 2));
        REQUIRE(backend.run_front());
        REQUIRE(backend.queue_size() == 1);
        REQUIRE(events.size() == 1);
        REQUIRE(events[0] == "a");

        REQUIRE(backend.run_front());
        REQUIRE(wait_for_queue_at_least(backend, 1));

        REQUIRE(finish_runner(backend, fut));
        auto result = fut.get();
        REQUIRE(result.has_value());
        REQUIRE(result->succeeded());
    }

    SECTION("CollectAll waits for both successes") {
        ControlledBackend backend;
        pravaha::Runner runner(backend);
        std::vector<std::string> events;

        auto expr = pravaha::collect_all(
                        pravaha::task("a", [&] { events.push_back("a"); })
                        &
                        pravaha::task("b", [&] { events.push_back("b"); })
                    ) | pravaha::task("downstream", [&] { events.push_back("downstream"); });

        auto fut = std::async(std::launch::async, [&runner, expr = std::move(expr)]() mutable {
            return runner.submit(std::move(expr));
        });

        REQUIRE(wait_for_queue_at_least(backend, 2));
        REQUIRE(backend.run_front());
        REQUIRE(backend.queue_size() == 1);
        REQUIRE(events.size() == 1);
        REQUIRE(events[0] == "a");

        REQUIRE(backend.run_front());
        REQUIRE(wait_for_queue_at_least(backend, 1));

        REQUIRE(finish_runner(backend, fut));
        auto result = fut.get();
        REQUIRE(result.has_value());
        REQUIRE(result->succeeded());
    }

    SECTION("AllOrNothing waits for both successes") {
        ControlledBackend backend;
        pravaha::Runner runner(backend);
        std::vector<std::string> events;

        auto expr = (
                        pravaha::task("a", [&] { events.push_back("a"); })
                        &
                        pravaha::task("b", [&] { events.push_back("b"); })
                    ) | pravaha::task("downstream", [&] { events.push_back("downstream"); });

        auto fut = std::async(std::launch::async, [&runner, expr = std::move(expr)]() mutable {
            return runner.submit(std::move(expr));
        });

        REQUIRE(wait_for_queue_at_least(backend, 2));
        REQUIRE(backend.run_front());
        REQUIRE(backend.queue_size() == 1);
        REQUIRE(events.size() == 1);
        REQUIRE(events[0] == "a");

        REQUIRE(backend.run_front());
        REQUIRE(wait_for_queue_at_least(backend, 1));

        REQUIRE(finish_runner(backend, fut));
        auto result = fut.get();
        REQUIRE(result.has_value());
        REQUIRE(result->succeeded());
    }
}

// ============================================================================
// SECTION 17: Parallel Semantics & Dependency Counters
// ============================================================================

TEST_CASE (



"Runner parallel - A & B both run"
,
"[pravaha][runner][parallel]"
)
 {
    int a_ran = 0, b_ran = 0;
    pravaha::Runner<> runner;
    auto a = pravaha::task("A", [&a_ran]() { ++a_ran; });
    auto b = pravaha::task("B", [&b_ran]() { ++b_ran; });
    auto expr = std::move(a) & std::move(b);
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(a_ran == 1);
    REQUIRE(b_ran == 1);
    REQUIRE(result.value().final_state == pravaha::TaskState::Succeeded);
    REQUIRE(result.value().node_states.size() == 2);
    REQUIRE(result.value().node_states[0] == pravaha::TaskState::Succeeded);
    REQUIRE(result.value().node_states[1] == pravaha::TaskState::Succeeded);
}

TEST_CASE (



"Runner parallel - (A & B) | C runs C after both"
,
"[pravaha][runner][parallel]"
)
 {
    std::atomic<int> a_done{0}, b_done{0}, c_start{0};
    pravaha::Runner<> runner;
    auto a = pravaha::task("A", [&]() { a_done.store(1); });
    auto b = pravaha::task("B", [&]() { b_done.store(1); });
    auto c = pravaha::task("C", [&]() { c_start.store(a_done.load() + b_done.load()); });
    auto expr = (std::move(a) & std::move(b)) | std::move(c);
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(c_start.load() == 2);
    REQUIRE(result.value().final_state == pravaha::TaskState::Succeeded);
}

TEST_CASE (



"Runner parallel - failure skips downstream"
,
"[pravaha][runner][parallel]"
)
 {
    std::atomic c_ran{0};
    pravaha::Runner<> runner;
    auto a = pravaha::task("A", []() { throw std::runtime_error("fail"); });
    auto b = pravaha::task("B", [&c_ran]() { ++c_ran; });
    auto expr = std::move(a) | std::move(b);
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(c_ran.load() == 0);
    REQUIRE(result.value().final_state == pravaha::TaskState::Failed);
    REQUIRE(result.value().node_states[1] == pravaha::TaskState::Skipped);
}

TEST_CASE (



"Runner parallel - no task runs twice"
,
"[pravaha][runner][parallel]"
)
 {
    std::vector run_counts(3, 0);
    pravaha::Runner<> runner;
    auto a = pravaha::task("A", [&run_counts]() { ++run_counts[0]; });
    auto b = pravaha::task("B", [&run_counts]() { ++run_counts[1]; });
    auto c = pravaha::task("C", [&run_counts]() { ++run_counts[2]; });
    auto expr = (std::move(a) & std::move(b)) | std::move(c);
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(run_counts[0] == 1);
    REQUIRE(run_counts[1] == 1);
    REQUIRE(run_counts[2] == 1);
}

// ============================================================================
// SECTION 18: CollectAll Join Policy
// ============================================================================

TEST_CASE (



"CollectAll - runs both branches even if A fails"
,
"[pravaha][runner][collectall]"
)
 {
    int a_ran = 0, b_ran = 0;
    pravaha::Runner<> runner;
    auto a = pravaha::task("A", [&a_ran]() {
        ++a_ran;
        throw std::runtime_error("A failed");
    });
    auto b = pravaha::task("B", [&b_ran]() { ++b_ran; });
    auto par = pravaha::collect_all(std::move(a) & std::move(b));
    auto result = runner.submit(std::move(par));
    REQUIRE(result.has_value());
    REQUIRE(a_ran == 1);
    REQUIRE(b_ran == 1);
    REQUIRE(result.value().final_state == pravaha::TaskState::Failed);
}

TEST_CASE (



"CollectAll - successful branches allow downstream continuation"
,
"[pravaha][runner][collectall]"
)
 {
    int c_ran = 0;
    pravaha::Runner<> runner;
    auto a = pravaha::task("A", []() {
    });
    auto b = pravaha::task("B", []() {
    });
    auto c = pravaha::task("C", [&c_ran]() { ++c_ran; });
    auto par = pravaha::collect_all(std::move(a) & std::move(b));
    auto expr = std::move(par) | std::move(c);
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(c_ran == 1);
    REQUIRE(result->final_state == pravaha::TaskState::Succeeded);
    REQUIRE(result->node_states[2] == pravaha::TaskState::Succeeded);
}

TEST_CASE (



"CollectAll - errors from failing branches are recorded"
,
"[pravaha][runner][collectall]"
)
 {
    pravaha::Runner<> runner;
    auto a = pravaha::task("A", []() { throw std::runtime_error("err_A"); });
    auto b = pravaha::task("B", []() { throw std::runtime_error("err_B"); });
    auto par = pravaha::collect_all(std::move(a) & std::move(b));
    auto result = runner.submit(std::move(par));
    REQUIRE(result.has_value());
    REQUIRE(result.value().errors.size() >= 2);
    const bool has_collect_all = std::any_of(result->errors.begin(), result->errors.end(), [](const auto &e) {
        return e.message.find("CollectAll") != std::string::npos
               || e.message.find("collect_all") != std::string::npos;
    });
    REQUIRE(has_collect_all);
    REQUIRE(result.value().final_state == pravaha::TaskState::Failed);
}

TEST_CASE (



"CollectAll - downstream C is skipped if any branch failed"
,
"[pravaha][runner][collectall]"
)
 {
    int a_ran = 0;
    int b_ran = 0;
    int c_ran = 0;
    pravaha::Runner<> runner;
    auto a = pravaha::task("A", [&a_ran]() {
        ++a_ran;
        throw std::runtime_error("fail");
    });
    auto b = pravaha::task("B", [&b_ran]() { ++b_ran; });
    auto c = pravaha::task("C", [&c_ran]() { ++c_ran; });
    auto par = pravaha::collect_all(std::move(a) & std::move(b));
    auto expr = std::move(par) | std::move(c);
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(a_ran == 1);
    REQUIRE(b_ran == 1);
    REQUIRE(c_ran == 0);
    REQUIRE(result.value().node_states[2] == pravaha::TaskState::Skipped);
    REQUIRE(result.value().final_state == pravaha::TaskState::Failed);
}

TEST_CASE (



"CollectAll - both failing branches still run"
,
"[pravaha][runner][collectall]"
)
 {
    int a_ran = 0;
    int b_ran = 0;
    pravaha::Runner<> runner;
    auto a = pravaha::task("A", [&a_ran]() {
        ++a_ran;
        throw std::runtime_error("err_A");
    });
    auto b = pravaha::task("B", [&b_ran]() {
        ++b_ran;
        throw std::runtime_error("err_B");
    });
    auto par = pravaha::collect_all(std::move(a) & std::move(b));
    auto result = runner.submit(std::move(par));
    REQUIRE(result.has_value());
    REQUIRE(a_ran == 1);
    REQUIRE(b_ran == 1);
    REQUIRE(result->final_state == pravaha::TaskState::Failed);
}

TEST_CASE (



"CollectAll - normal AllOrNothing behavior remains unchanged"
,
"[pravaha][runner][collectall]"
)
 {
    int b_ran = 0;
    pravaha::Runner<> runner;
    // Default is AllOrNothing - A fails, B should still run (parallel sibling, no dep)
    // But downstream C should be skipped
    auto a = pravaha::task("A", []() { throw std::runtime_error("fail"); });
    auto b = pravaha::task("B", [&b_ran]() { ++b_ran; });
    auto c = pravaha::task("C", []() {
    });
    auto expr = (std::move(a) & std::move(b)) | std::move(c);
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    // B still runs because it's a sibling with no dependency on A
    REQUIRE(b_ran == 1);
    // C is skipped because A (predecessor via edge) failed
    REQUIRE(result.value().node_states[2] == pravaha::TaskState::Skipped);
    REQUIRE(result.value().final_state == pravaha::TaskState::Failed);
}

// ============================================================================
// SECTION 19: JThreadBackend
// ============================================================================

TEST_CASE (



"JThreadBackend - submit and drain executes command"
,
"[pravaha][jthread]"
)
 {
    std::atomic counter{0};
    {
        pravaha::JThreadBackend backend(2);
        backend.submit(pravaha::TaskCommand::make([&counter]() { counter.fetch_add(1); }));
        backend.drain();
    }
    REQUIRE(counter.load() == 1);
}

TEST_CASE (



"JThreadBackend - multiple commands execute"
,
"[pravaha][jthread]"
)
 {
    std::atomic counter{0};
    {
        pravaha::JThreadBackend backend(2);
        for (int i = 0; i < 10; ++i) {
            backend.submit(pravaha::TaskCommand::make([&counter]() { counter.fetch_add(1); }));
        }
        backend.drain();
    }
    REQUIRE(counter.load() == 10);
}

TEST_CASE (



"JThreadBackend - request_stop is safe"
,
"[pravaha][jthread]"
)
 {
    pravaha::JThreadBackend backend(2);
    backend.request_stop();
    REQUIRE(backend.stopped());
}

TEST_CASE (



"JThreadBackend - destructor does not deadlock"
,
"[pravaha][jthread]"
)
 {
    std::atomic counter{0};
    {
        pravaha::JThreadBackend backend(2);
        backend.submit(pravaha::TaskCommand::make([&counter]() { counter.fetch_add(1); }));
        // destructor should join cleanly without deadlock
    }
    // Command may or may not have executed before stop, but no deadlock occurred
    REQUIRE(true);
}

TEST_CASE (



"JThreadBackend - no graph logic in backend"
,
"[pravaha][jthread]"
)
 {
    std::atomic val{0};
    {
        pravaha::JThreadBackend backend(1);
        auto cmd = pravaha::TaskCommand::make([&val]() { val.store(42); });
        backend.submit(std::move(cmd));
        backend.drain();
    }
    REQUIRE(val.load() == 42);
}

TEST_CASE (



"JThreadBackend - bounded queue rejects when full"
,
"[pravaha][jthread]"
)
 {
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool started = false;
    bool release = false;

    pravaha::JThreadBackend backend(1, 1);
    REQUIRE(backend.submit(pravaha::TaskCommand::make([&]() {
        std::unique_lock lock(gate_mutex);
        started = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&]() { return release; });
        })));

    {
        std::unique_lock lock(gate_mutex);
        gate_cv.wait(lock, [&]() { return started; });
    }

    REQUIRE(backend.submit(pravaha::TaskCommand::make([]() {})));
    REQUIRE_FALSE(backend.submit(pravaha::TaskCommand::make([]() {})));

    {
        std::lock_guard lock(gate_mutex);
        release = true;
    }
    gate_cv.notify_all();

    auto drained = std::async(std::launch::async, [&]() {
        backend.drain();
    });
    REQUIRE(drained.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    drained.get();
}

TEST_CASE (



"JThreadBackend - high priority runs first"
,
"[pravaha][jthread][priority]"
)
 {
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool started = false;
    bool release = false;
    std::mutex order_mutex;
    std::vector<int> order;

    pravaha::JThreadBackend backend(1);
    REQUIRE(backend.submit(pravaha::TaskCommand::make([&]() {
        std::unique_lock lock(gate_mutex);
        started = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&]() { return release; });
        })));

    {
        std::unique_lock lock(gate_mutex);
        gate_cv.wait(lock, [&]() { return started; });
    }

    REQUIRE(backend.submit(pravaha::TaskCommand::make([&]() {
        std::lock_guard lock(order_mutex);
        order.push_back(1);
        }, {}, pravaha::TaskPriority::Low)));

    REQUIRE(backend.submit(pravaha::TaskCommand::make([&]() {
        std::lock_guard lock(order_mutex);
        order.push_back(2);
        }, {}, pravaha::TaskPriority::High)));

    {
        std::lock_guard lock(gate_mutex);
        release = true;
    }
    gate_cv.notify_all();
    backend.drain();

    REQUIRE(order.size() == 2);
    REQUIRE(order[0] == 2);
    REQUIRE(order[1] == 1);
}

TEST_CASE (



"JThreadBackend - same priority keeps FIFO"
,
"[pravaha][jthread][priority]"
)
 {
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool started = false;
    bool release = false;
    std::mutex order_mutex;
    std::vector<int> order;

    pravaha::JThreadBackend backend(1);
    REQUIRE(backend.submit(pravaha::TaskCommand::make([&]() {
        std::unique_lock lock(gate_mutex);
        started = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&]() { return release; });
        })));

    {
        std::unique_lock lock(gate_mutex);
        gate_cv.wait(lock, [&]() { return started; });
    }

    REQUIRE(backend.submit(pravaha::TaskCommand::make([&]() {
        std::lock_guard lock(order_mutex);
        order.push_back(10);
        }, {}, pravaha::TaskPriority::High)));

    REQUIRE(backend.submit(pravaha::TaskCommand::make([&]() {
        std::lock_guard lock(order_mutex);
        order.push_back(20);
        }, {}, pravaha::TaskPriority::High)));

    {
        std::lock_guard lock(gate_mutex);
        release = true;
    }
    gate_cv.notify_all();
    backend.drain();

    REQUIRE(order.size() == 2);
    REQUIRE(order[0] == 10);
    REQUIRE(order[1] == 20);
}

TEST_CASE (



"JThreadBackend - cancellation still works with priority queue"
,
"[pravaha][jthread][priority]"
)
 {
    pravaha::JThreadBackend backend(1);
    backend.request_stop();
    REQUIRE(backend.stopped());
    REQUIRE_FALSE(backend.submit(pravaha::TaskCommand::make([]() {}, {}, pravaha::TaskPriority::High)));
    backend.drain();
    REQUIRE(backend.stopped());
}

TEST_CASE (



"JThreadBackend - submit after stop is rejected and drain returns"
,
"[pravaha][jthread][regression]"
)
 {
    std::atomic counter{0};
    pravaha::JThreadBackend backend(2);
    backend.request_stop();
    backend.submit(pravaha::TaskCommand::make([&counter]() { counter.fetch_add(1); }));
    backend.drain();
    REQUIRE(counter.load() == 0);
}

TEST_CASE (



"Runner<JThreadBackend> - stopped backend submit is reported and does not deadlock"
,
"[pravaha][jthread][runner][regression]"
)
 {
    pravaha::JThreadBackend backend(2);
    backend.request_stop();
    pravaha::Runner runner(backend);

    auto a = pravaha::task("A", []() {
    });
    auto result = runner.submit(std::move(a));

    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Failed);
    REQUIRE_FALSE(result->errors.empty());
    const auto err_kind = result->errors.front().kind;
    const bool accepted_kind =
            err_kind == pravaha::ErrorKind::QueueRejected
            || err_kind == pravaha::ErrorKind::TaskFailed;
    REQUIRE(accepted_kind);
}

TEST_CASE (



"Runner<JThreadBackend> - bounded queue rejection does not deadlock"
,
"[pravaha][jthread][runner][regression]"
)
 {
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool started = false;
    bool release = false;

    pravaha::JThreadBackend backend(1, 1);
    REQUIRE(backend.submit(pravaha::TaskCommand::make([&]() {
        std::unique_lock lock(gate_mutex);
        started = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&]() { return release; });
        })));

    {
        std::unique_lock lock(gate_mutex);
        gate_cv.wait(lock, [&]() { return started; });
    }

    REQUIRE(backend.submit(pravaha::TaskCommand::make([]() {})));

    pravaha::Runner runner(backend);
    auto result = runner.submit(pravaha::task("A", []() {
    }));

    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Failed);
    REQUIRE_FALSE(result->errors.empty());
    REQUIRE(result->errors.front().kind == pravaha::ErrorKind::QueueRejected);

    {
        std::lock_guard lock(gate_mutex);
        release = true;
    }
    gate_cv.notify_all();
    backend.drain();
}

TEST_CASE (



"Flow control reject on backend submit rejection fails task and skips downstream"
,
"[pravaha][runner][flow_control][regression]"
)
 {
    struct RejectingBackend {
        bool submit(pravaha::TaskCommand) { return false; }

        void drain() noexcept {
        }

        void request_stop() noexcept {
        }

        [[nodiscard]] bool stopped() const noexcept { return false; }
    };

    RejectingBackend backend;
    pravaha::Runner runner(backend);
    std::atomic downstream_runs{0};

    auto expr =
            pravaha::task("A", []() {
            })
            | pravaha::task("B", [&]() { downstream_runs.fetch_add(1); });

    auto fut = std::async(std::launch::async, [&runner, expr = std::move(expr)]() mutable {
        return runner.submit(std::move(expr));
    });

    REQUIRE(fut.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    auto result = fut.get();
    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Failed);
    REQUIRE(result->node_states.size() == 2);
    REQUIRE(result->node_states[0] == pravaha::TaskState::Failed);
    REQUIRE(result->node_states[1] == pravaha::TaskState::Skipped);
    REQUIRE(downstream_runs.load() == 0);
    REQUIRE(std::any_of(result->errors.begin(), result->errors.end(), [](const pravaha::PravahaError& err) {
        return err.kind == pravaha::ErrorKind::QueueRejected;
        }));
}

TEST_CASE (



"Flow control would-block on backend submit rejection does not deadlock"
,
"[pravaha][runner][flow_control][regression]"
)
 {
    struct RejectingBackend {
        bool submit(pravaha::TaskCommand) { return false; }

        void drain() noexcept {
        }

        void request_stop() noexcept {
        }

        [[nodiscard]] bool stopped() const noexcept { return false; }
    };

    struct WouldBlockPolicy {
        static pravaha::SubmitDecision on_submit_rejected(const pravaha::PravahaError &) noexcept {
            return pravaha::SubmitDecision::WouldBlock;
        }
    };

    RejectingBackend backend;
    pravaha::Runner<RejectingBackend, pravaha::DefaultGraphAlgorithmPolicy, pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy, pravaha::NoObserver, pravaha::NoRetryPolicy, pravaha::NoTimeoutPolicy,
        WouldBlockPolicy> runner(backend);

    auto fut = std::async(std::launch::async, [&runner]() mutable {
        return runner.submit(pravaha::task("A", []() {
        }));
    });

    REQUIRE(fut.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    auto result = fut.get();
    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Failed);
    REQUIRE(std::any_of(result->errors.begin(), result->errors.end(), [](const pravaha::PravahaError& err) {
        return err.kind == pravaha::ErrorKind::InternalError;
        }));
    REQUIRE_FALSE(std::any_of(result->errors.begin(), result->errors.end(), [](const pravaha::PravahaError& err) {
        return err.kind == pravaha::ErrorKind::QueueRejected;
        }));
}

TEST_CASE (



"retry immediate retries once then succeeds"
,
"[pravaha][runner][retry_immediate]"
)
 {
    struct RetryWithinBudget {
        static pravaha::RetryDecision on_failure(const pravaha::PravahaError &, std::size_t attempt_count,
                                                 std::size_t max_retries) noexcept {
            return attempt_count < max_retries
                       ? pravaha::RetryDecision::RetryImmediate
                       : pravaha::RetryDecision::FailFinal;
        }
    };

    std::atomic attempts{0};
    std::atomic downstream{0};

    pravaha::Runner<pravaha::InlineBackend, pravaha::DefaultGraphAlgorithmPolicy, pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy, pravaha::NoObserver, RetryWithinBudget> runner;

    auto expr = pravaha::with_retry<1>(
                    pravaha::task("flaky", [&]() -> pravaha::Outcome<pravaha::Unit> {
                        const auto n = attempts.fetch_add(1);
                        if (n == 0) {
                            return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "fail once"});
                        }
                        return pravaha::Unit{};
                    })
                ) | pravaha::task("downstream", [&]() { downstream.fetch_add(1); });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(attempts.load() == 2);
    REQUIRE(downstream.load() == 1);
}

TEST_CASE (



"retry exhaustion fails when budget is consumed"
,
"[pravaha][runner][retry_immediate]"
)
 {
    struct RetryWithinBudget {
        static pravaha::RetryDecision on_failure(const pravaha::PravahaError &, std::size_t attempt_count,
                                                 std::size_t max_retries) noexcept {
            return attempt_count < max_retries
                       ? pravaha::RetryDecision::RetryImmediate
                       : pravaha::RetryDecision::FailFinal;
        }
    };

    std::atomic attempts{0};

    pravaha::Runner<pravaha::InlineBackend, pravaha::DefaultGraphAlgorithmPolicy, pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy, pravaha::NoObserver, RetryWithinBudget> runner;

    auto expr = pravaha::with_retry<1>(
        pravaha::task("always_fail", [&]() -> pravaha::Outcome<pravaha::Unit> {
            attempts.fetch_add(1);
            return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "still failing"});
        })
    );

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->failed());
    REQUIRE(attempts.load() == 2);
}

TEST_CASE (



"join member retry does not double-count and any_success still succeeds"
,
"[pravaha][runner][retry_immediate][join]"
)
 {
    struct RetryWithinBudget {
        static pravaha::RetryDecision on_failure(const pravaha::PravahaError &, std::size_t attempt_count,
                                                 std::size_t max_retries) noexcept {
            return attempt_count < max_retries
                       ? pravaha::RetryDecision::RetryImmediate
                       : pravaha::RetryDecision::FailFinal;
        }
    };

    std::atomic flaky_attempts{0};
    std::atomic fail_attempts{0};
    std::atomic downstream{0};

    pravaha::JThreadBackend backend(1);
    pravaha::Runner<pravaha::JThreadBackend, pravaha::DefaultGraphAlgorithmPolicy, pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy, pravaha::NoObserver, RetryWithinBudget> runner(backend);

    auto expr = pravaha::any_success_of(
                    pravaha::with_retry<1>(
                        pravaha::task("flaky", [&]() -> pravaha::Outcome<pravaha::Unit> {
                            const auto n = flaky_attempts.fetch_add(1);
                            if (n == 0) {
                                return std::unexpected(pravaha::PravahaError{
                                    pravaha::ErrorKind::TaskFailed, "flaky first fail"
                                });
                            }
                            return pravaha::Unit{};
                        })
                    ),
                    pravaha::task("always_fail", [&]() -> pravaha::Outcome<pravaha::Unit> {
                        fail_attempts.fetch_add(1);
                        return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "other fail"});
                    })
                ) | pravaha::task("downstream", [&]() { downstream.fetch_add(1); });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(flaky_attempts.load() == 2);
    REQUIRE(fail_attempts.load() == 1);
    REQUIRE(downstream.load() == 1);
}

TEST_CASE (



"cooperative timeout cancels expired task before execution and skips downstream"
,
"[pravaha][runner][timeout_runtime]"
)
 {
    auto wait_for_queue_at_least = [](ControlledBackend &backend, std::size_t min_size) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            if (backend.queue_size() >= min_size) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return backend.queue_size() >= min_size;
    };

    ControlledBackend backend;
    pravaha::Runner runner(backend);
    std::atomic a_runs{0};
    std::atomic b_runs{0};

    auto expr = pravaha::with_timeout(std::chrono::milliseconds(1),
                                      pravaha::task("a", [&]() { a_runs.fetch_add(1); }))
                | pravaha::task("b", [&]() { b_runs.fetch_add(1); });

    auto fut = std::async(std::launch::async, [&runner, expr = std::move(expr)]() mutable {
        return runner.submit(std::move(expr));
    });

    REQUIRE(wait_for_queue_at_least(backend, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(backend.run_front());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready
           && std::chrono::steady_clock::now() < deadline) {
        if (!backend.run_front()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    REQUIRE(fut.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    auto result = fut.get();
    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Canceled);
    REQUIRE(result->node_states.size() == 2);
    REQUIRE(result->node_states[0] == pravaha::TaskState::Canceled);
    REQUIRE(result->node_states[1] == pravaha::TaskState::Skipped);
    REQUIRE(a_runs.load() == 0);
    REQUIRE(b_runs.load() == 0);
}

TEST_CASE (



"cooperative timeout lets non expired task run"
,
"[pravaha][runner][timeout_runtime]"
)
 {
    std::atomic runs{0};
    pravaha::Runner<> runner;
    auto expr = pravaha::with_timeout(std::chrono::seconds(1), pravaha::task("a", [&]() { runs.fetch_add(1); }));
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(runs.load() == 1);
}

TEST_CASE (



"custom timeout policy forcing timeout keeps cancellation behavior"
,
"[pravaha][runner][timeout_runtime][policy]"
)
 {
    struct ForceTimeoutPolicy {
        static bool on_timeout(std::chrono::nanoseconds) noexcept {
            return true;
        }
    };

    auto wait_for_queue_at_least = [](ControlledBackend &backend, std::size_t min_size) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            if (backend.queue_size() >= min_size) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return backend.queue_size() >= min_size;
    };

    using ForceTimeoutRunner = pravaha::Runner<
        ControlledBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        pravaha::NoObserver,
        pravaha::NoRetryPolicy,
        ForceTimeoutPolicy>;

    ControlledBackend backend;
    ForceTimeoutRunner runner(backend);
    std::atomic a_runs{0};
    std::atomic b_runs{0};

    auto expr = pravaha::with_timeout(std::chrono::milliseconds(1),
                                      pravaha::task("a", [&]() { a_runs.fetch_add(1); }))
                | pravaha::task("b", [&]() { b_runs.fetch_add(1); });

    auto fut = std::async(std::launch::async, [&runner, expr = std::move(expr)]() mutable {
        return runner.submit(std::move(expr));
    });

    REQUIRE(wait_for_queue_at_least(backend, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(backend.run_front());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready
           && std::chrono::steady_clock::now() < deadline) {
        if (!backend.run_front()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    REQUIRE(fut.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    auto result = fut.get();
    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Canceled);
    REQUIRE(result->node_states.size() == 2);
    REQUIRE(result->node_states[0] == pravaha::TaskState::Canceled);
    REQUIRE(result->node_states[1] == pravaha::TaskState::Skipped);
    REQUIRE(a_runs.load() == 0);
    REQUIRE(b_runs.load() == 0);
}

TEST_CASE (



"custom timeout policy never timing out allows expired task to run"
,
"[pravaha][runner][timeout_runtime][policy]"
)
 {
    struct NeverTimeoutPolicy {
        static bool on_timeout(std::chrono::nanoseconds) noexcept {
            return false;
        }
    };

    auto wait_for_queue_at_least = [](ControlledBackend &backend, std::size_t min_size) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            if (backend.queue_size() >= min_size) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return backend.queue_size() >= min_size;
    };

    using TimeoutDisabledRunner = pravaha::Runner<
        ControlledBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        pravaha::NoObserver,
        pravaha::NoRetryPolicy,
        NeverTimeoutPolicy>;

    ControlledBackend backend;
    TimeoutDisabledRunner runner(backend);
    std::atomic a_runs{0};
    std::atomic b_runs{0};

    auto expr = pravaha::with_timeout(std::chrono::milliseconds(1),
                                      pravaha::task("a", [&]() { a_runs.fetch_add(1); }))
                | pravaha::task("b", [&]() { b_runs.fetch_add(1); });

    auto fut = std::async(std::launch::async, [&runner, expr = std::move(expr)]() mutable {
        return runner.submit(std::move(expr));
    });

    REQUIRE(wait_for_queue_at_least(backend, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(backend.run_front());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready
           && std::chrono::steady_clock::now() < deadline) {
        if (!backend.run_front()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    REQUIRE(fut.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    auto result = fut.get();
    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Succeeded);
    REQUIRE(result->node_states.size() == 2);
    REQUIRE(result->node_states[0] == pravaha::TaskState::Succeeded);
    REQUIRE(result->node_states[1] == pravaha::TaskState::Succeeded);
    REQUIRE(a_runs.load() == 1);
    REQUIRE(b_runs.load() == 1);
}

TEST_CASE (



"NoTimeoutPolicy ignores timeout metadata on expired task"
,
"[pravaha][runner][timeout_runtime][policy]"
)
 {
    auto wait_for_queue_at_least = [](ControlledBackend &backend, std::size_t min_size) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            if (backend.queue_size() >= min_size) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return backend.queue_size() >= min_size;
    };

    using NoTimeoutRunner = pravaha::Runner<
        ControlledBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        pravaha::NoObserver,
        pravaha::NoRetryPolicy,
        pravaha::NoTimeoutPolicy>;

    ControlledBackend backend;
    NoTimeoutRunner runner(backend);
    std::atomic a_runs{0};
    std::atomic b_runs{0};

    auto expr = pravaha::with_timeout(std::chrono::milliseconds(1),
                                      pravaha::task("a", [&]() { a_runs.fetch_add(1); }))
                | pravaha::task("b", [&]() { b_runs.fetch_add(1); });

    auto fut = std::async(std::launch::async, [&runner, expr = std::move(expr)]() mutable {
        return runner.submit(std::move(expr));
    });

    REQUIRE(wait_for_queue_at_least(backend, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(backend.run_front());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready
           && std::chrono::steady_clock::now() < deadline) {
        if (!backend.run_front()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    REQUIRE(fut.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    auto result = fut.get();
    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Succeeded);
    REQUIRE(result->node_states.size() == 2);
    REQUIRE(result->node_states[0] == pravaha::TaskState::Succeeded);
    REQUIRE(result->node_states[1] == pravaha::TaskState::Succeeded);
    REQUIRE(a_runs.load() == 1);
    REQUIRE(b_runs.load() == 1);
}

// ============================================================================
// SECTION 20: Runner with JThreadBackend
// ============================================================================

TEST_CASE (



"Runner<JThreadBackend> - sequence works"
,
"[pravaha][jthread][runner]"
)
 {
    std::atomic<int> order_a{0}, order_b{0};
    std::atomic seq{0};
    pravaha::JThreadBackend backend(2);
    pravaha::Runner runner(backend);
    auto a = pravaha::task("A", [&]() { order_a.store(seq.fetch_add(1)); });
    auto b = pravaha::task("B", [&]() { order_b.store(seq.fetch_add(1)); });
    auto expr = std::move(a) | std::move(b);
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result.value().final_state == pravaha::TaskState::Succeeded);
    REQUIRE(order_a.load() < order_b.load());
}

TEST_CASE (



"Runner<JThreadBackend> - parallel independent tasks run"
,
"[pravaha][jthread][runner]"
)
 {
    std::atomic counter{0};
    pravaha::JThreadBackend backend(4);
    pravaha::Runner runner(backend);
    auto a = pravaha::task("A", [&counter]() { counter.fetch_add(1); });
    auto b = pravaha::task("B", [&counter]() { counter.fetch_add(1); });
    auto expr = std::move(a) & std::move(b);
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(counter.load() == 2);
    REQUIRE(result.value().final_state == pravaha::TaskState::Succeeded);
}

TEST_CASE (



"Runner<JThreadBackend> - (A & B) | C waits for both"
,
"[pravaha][jthread][runner]"
)
 {
    std::atomic<int> a_done{0}, b_done{0}, c_start{0};
    pravaha::JThreadBackend backend(4);
    pravaha::Runner runner(backend);
    auto a = pravaha::task("A", [&]() { a_done.store(1); });
    auto b = pravaha::task("B", [&]() { b_done.store(1); });
    auto c = pravaha::task("C", [&]() { c_start.store(a_done.load() + b_done.load()); });
    auto expr = (std::move(a) & std::move(b)) | std::move(c);
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(c_start.load() == 2);
    REQUIRE(result.value().final_state == pravaha::TaskState::Succeeded);
}

TEST_CASE (



"Runner<JThreadBackend> - failure skips downstream"
,
"[pravaha][jthread][runner]"
)
 {
    std::atomic c_ran{0};
    pravaha::JThreadBackend backend(2);
    pravaha::Runner runner(backend);
    auto a = pravaha::task("A", []() { throw std::runtime_error("fail"); });
    auto b = pravaha::task("B", [&c_ran]() { ++c_ran; });
    auto expr = std::move(a) | std::move(b);
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(c_ran.load() == 0);
    REQUIRE(result.value().final_state == pravaha::TaskState::Failed);
    REQUIRE(result.value().node_states[1] == pravaha::TaskState::Skipped);
}

TEST_CASE (



"Runner<JThreadBackend> - no deadlocks on repeated runs"
,
"[pravaha][jthread][runner]"
)
 {
    pravaha::JThreadBackend backend(2);
    pravaha::Runner runner(backend);
    for (int run = 0; run < 5; ++run) {
        std::atomic counter{0};
        auto a = pravaha::task("A", [&counter]() { counter.fetch_add(1); });
        auto b = pravaha::task("B", [&counter]() { counter.fetch_add(1); });
        auto expr = std::move(a) & std::move(b);
        auto result = runner.submit(std::move(expr));
        REQUIRE(result.has_value());
        REQUIRE(counter.load() == 2);
        REQUIRE(result.value().final_state == pravaha::TaskState::Succeeded);
    }
}

TEST_CASE (



"Runner<JThreadBackend> - repeated runs do not share stale state"
,
"[pravaha][jthread][runner]"
)
 {
    pravaha::JThreadBackend backend(2);
    pravaha::Runner runner(backend);

    // First run: succeed
    {
        auto a = pravaha::task("A", []() {
        });
        auto result = runner.submit(std::move(a));
        REQUIRE(result.has_value());
        REQUIRE(result.value().final_state == pravaha::TaskState::Succeeded);
    }
    // Second run: fail
    {
        auto a = pravaha::task("A", []() { throw std::runtime_error("fail"); });
        auto result = runner.submit(std::move(a));
        REQUIRE(result.has_value());
        REQUIRE(result.value().final_state == pravaha::TaskState::Failed);
    }
    // Third run: succeed again
    {
        auto a = pravaha::task("A", []() {
        });
        auto result = runner.submit(std::move(a));
        REQUIRE(result.has_value());
        REQUIRE(result.value().final_state == pravaha::TaskState::Succeeded);
    }
}

// ============================================================================
// SECTION 20.5: Runner Policy Slots
// ============================================================================

struct CountingGraphPolicy {
    static inline int validate_calls = 0;

    static pravaha::Outcome<pravaha::Unit> validate(const pravaha::TaskIr& ir) {
        ++validate_calls;
        return pravaha::DefaultGraphAlgorithmPolicy::validate(ir);
    }

    static pravaha::Outcome<std::vector<pravaha::TaskId>> topological_order(const pravaha::TaskIr& ir) {
        return pravaha::DefaultGraphAlgorithmPolicy::topological_order(ir);
    }
};

struct CountingNoProgressPolicy {
    static inline int checks = 0;

    template <class SharedSchedulerStateLike>
    static bool handle_no_progress(SharedSchedulerStateLike& sstate) {
        ++checks;
        return pravaha::DefaultNoProgressPolicy::handle_no_progress(sstate);
    }
};

struct CountingReadyPolicy {
    static inline int ready_checks = 0;

    template <class RuntimeStateLike>
    static bool is_ready(const RuntimeStateLike& state, std::size_t index) {
        ++ready_checks;
        return pravaha::DefaultReadyPolicy::is_ready(state, index);
    }
};

struct CompileOnlyObserver {
    static constexpr bool enabled = true;
    static inline int task_calls = 0;
    static inline int join_calls = 0;
    static inline int graph_calls = 0;

    static void on_task_event(const pravaha::TaskEvent&) noexcept { ++task_calls; }
    static void on_join_event(const pravaha::JoinEvent&) noexcept { ++join_calls; }
    static void on_graph_event(const pravaha::GraphEvent&) noexcept { ++graph_calls; }
};

struct DisabledObserver {
    static constexpr bool enabled = false;
    static inline int task_calls = 0;
    static inline int join_calls = 0;
    static inline int graph_calls = 0;

    static void reset() {
        task_calls = 0;
        join_calls = 0;
        graph_calls = 0;
    }

    static void on_task_event(const pravaha::TaskEvent&) noexcept { ++task_calls; }
    static void on_join_event(const pravaha::JoinEvent&) noexcept { ++join_calls; }
    static void on_graph_event(const pravaha::GraphEvent&) noexcept { ++graph_calls; }
};

struct EnabledObserver {
    static constexpr bool enabled = true;
    static inline int task_calls = 0;
    static inline int join_calls = 0;
    static inline int graph_calls = 0;

    static void reset() {
        task_calls = 0;
        join_calls = 0;
        graph_calls = 0;
    }

    static void on_task_event(const pravaha::TaskEvent&) noexcept { ++task_calls; }
    static void on_join_event(const pravaha::JoinEvent&) noexcept { ++join_calls; }
    static void on_graph_event(const pravaha::GraphEvent&) noexcept { ++graph_calls; }
};

struct QueueWaitObserver {
    static constexpr bool enabled = true;
    static inline std::mutex mutex{};
    static inline std::vector<pravaha::TaskEvent> task_events{};

    static void reset() {
        std::lock_guard lock(mutex);
        task_events.clear();
    }

    static void on_task_event(const pravaha::TaskEvent& e) noexcept {
        std::lock_guard lock(mutex);
        task_events.push_back(e);
    }

    static void on_join_event(const pravaha::JoinEvent&) noexcept {}

    static void on_graph_event(const pravaha::GraphEvent&) noexcept {}
};

struct TestObserver {
    static constexpr bool enabled = true;
    static inline int lowered = 0;
    static inline int validated = 0;
    static inline std::vector<pravaha::EventKind> task_events{};
    static inline std::vector<pravaha::TaskEvent> task_event_records{};
    static inline std::vector<pravaha::JoinEvent> join_events{};
    static inline std::vector<pravaha::GraphEvent> graph_events{};

    static void reset() {
        lowered = 0;
        validated = 0;
        task_events.clear();
        task_event_records.clear();
        join_events.clear();
        graph_events.clear();
    }

    static void on_task_event(const pravaha::TaskEvent& e) noexcept {
        task_events.push_back(e.kind);
        task_event_records.push_back(e);
    }

    static void on_join_event(const pravaha::JoinEvent& e) noexcept { join_events.push_back(e); }

    static void on_graph_event(const pravaha::GraphEvent& e) noexcept {
        graph_events.push_back(e);
        if (e.kind == pravaha::EventKind::GraphLowered) ++lowered;
        if (e.kind == pravaha::EventKind::GraphValidated) ++validated;
    }
};

struct TraceObserver {
    static constexpr bool enabled = true;
    static inline std::vector<std::string> trace{};

    static void reset() {
        trace.clear();
    }

    static void on_task_event(const pravaha::TaskEvent& e) noexcept {
        std::string kind;
        switch (e.kind) {
        case pravaha::EventKind::TaskReady:
            kind = "ready";
            break;
        case pravaha::EventKind::TaskScheduled:
            kind = "scheduled";
            break;
        case pravaha::EventKind::TaskStarted:
            kind = "started";
            break;
        case pravaha::EventKind::TaskCompleted:
            kind = "completed";
            break;
        case pravaha::EventKind::TaskFailed:
            kind = "failed";
            break;
        case pravaha::EventKind::TaskSkipped:
            kind = "skipped";
            break;
        case pravaha::EventKind::TaskCanceled:
            kind = "canceled";
            break;
        default:
            return;
        }
        trace.push_back("task:" + kind + ":" + std::string(e.task_name));
    }

    static void on_join_event(const pravaha::JoinEvent& e) noexcept {
        if (e.kind == pravaha::EventKind::JoinResolved) {
            trace.push_back(std::string{"join:resolved:"} + (e.success ? "success" : "failure"));
        }
    }

    static void on_graph_event(const pravaha::GraphEvent& e) noexcept {
        if (e.kind == pravaha::EventKind::GraphLowered) {
            trace.push_back("graph:lowered");
        }
        else if (e.kind == pravaha::EventKind::GraphValidated) {
            trace.push_back("graph:validated");
        }
    }
};

struct BadObserver {
    static constexpr bool enabled = true;

    static void on_task_event(const pravaha::TaskEvent&) noexcept {}

    static void on_join_event(const pravaha::JoinEvent&) noexcept {}
};

static_assert(pravaha::ObserverPolicy<pravaha::NoObserver>);
static_assert(pravaha::ObserverPolicy<TestObserver>);
static_assert(!pravaha::ObserverPolicy<BadObserver>);
static_assert(pravaha::ObserverPolicy<DisabledObserver>);
static_assert(pravaha::ObserverPolicy<EnabledObserver>);

TEST_CASE (



"Runner policy slot - custom GraphAlgorithmPolicy is used"
,
"[pravaha][runner][policy]"
)
 {
    CountingGraphPolicy::validate_calls = 0;
    pravaha::Runner<pravaha::InlineBackend, CountingGraphPolicy> runner;
    auto a = pravaha::task("A", []() {
    });
    auto b = pravaha::task("B", []() {
    });
    auto expr = std::move(a) | std::move(b);
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(CountingGraphPolicy::validate_calls > 0);
}

TEST_CASE (



"Runner policy slot - custom NoProgressPolicy is used"
,
"[pravaha][runner][policy]"
)
 {
    CountingNoProgressPolicy::checks = 0;
    pravaha::Runner<pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        CountingNoProgressPolicy> runner;
    auto a = pravaha::task("A", []() {
    });
    auto result = runner.submit(std::move(a));
    REQUIRE(result.has_value());
    REQUIRE(CountingNoProgressPolicy::checks > 0);
}

TEST_CASE (



"Runner policy slot - custom ReadyPolicy is used"
,
"[pravaha][runner][policy]"
)
 {
    CountingReadyPolicy::ready_checks = 0;
    pravaha::Runner<pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        CountingReadyPolicy> runner;
    auto a = pravaha::task("A", []() {
    });
    auto b = pravaha::task("B", []() {
    });
    auto expr = std::move(a) | std::move(b);
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(CountingReadyPolicy::ready_checks > 0);
}

TEST_CASE (



"Runner policy slot - default Runner compiles and runs"
,
"[pravaha][runner][policy]"
)
 {
    STATIC_REQUIRE(std::is_same_v<typename pravaha::Runner<>::observer_type, pravaha::NoObserver>);
    pravaha::Runner<> runner;
    auto a = pravaha::task("A", []() {
    });
    auto b = pravaha::task("B", []() {
    });
    auto expr = std::move(a) | std::move(b);
    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Succeeded);
}

TEST_CASE (



"Runner policy slot - explicit NoObserver slot compiles"
,
"[pravaha][runner][policy]"
)
 {
    using ExplicitNoObserverRunner = pravaha::Runner<
        pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        pravaha::NoObserver>;

    STATIC_REQUIRE(std::is_same_v<typename ExplicitNoObserverRunner::observer_type, pravaha::NoObserver>);
    ExplicitNoObserverRunner runner;
    auto result = runner.submit(pravaha::task("A", []() {
    }));
    REQUIRE(result.has_value());
}

TEST_CASE (



"Runner policy slot - custom Observer slot compiles"
,
"[pravaha][runner][policy]"
)
 {
    using CustomObserverRunner = pravaha::Runner<
        pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        CompileOnlyObserver>;

    CompileOnlyObserver::task_calls = 0;
    CompileOnlyObserver::join_calls = 0;
    CompileOnlyObserver::graph_calls = 0;

    STATIC_REQUIRE(std::is_same_v<typename CustomObserverRunner::observer_type, CompileOnlyObserver>);
    CustomObserverRunner runner;
    auto result = runner.submit(pravaha::task("A", []() {
    }));
    REQUIRE(result.has_value());
    REQUIRE(CompileOnlyObserver::task_calls == 4);
    REQUIRE(CompileOnlyObserver::join_calls == 0);
    REQUIRE(CompileOnlyObserver::graph_calls == 2);
}

TEST_CASE (



"Runner policy slot - disabled observer emits no events"
,
"[pravaha][runner][policy]"
)
 {
    using DisabledRunner = pravaha::Runner<
        pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        DisabledObserver>;

    DisabledObserver::reset();

    DisabledRunner runner;
    auto result = runner.submit(pravaha::task("A", []() {
    }) | pravaha::task("B", []() {
    }));

    REQUIRE(result.has_value());
    REQUIRE(DisabledObserver::task_calls == 0);
    REQUIRE(DisabledObserver::join_calls == 0);
    REQUIRE(DisabledObserver::graph_calls == 0);
}

TEST_CASE (



"Runner policy slot - enabled observer emits events"
,
"[pravaha][runner][policy]"
)
 {
    using EnabledRunner = pravaha::Runner<
        pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        EnabledObserver>;

    EnabledObserver::reset();

    EnabledRunner runner;
    auto result = runner.submit(pravaha::task("A", []() {
    }) | pravaha::task("B", []() {
    }));

    REQUIRE(result.has_value());
    REQUIRE(EnabledObserver::task_calls > 0);
    REQUIRE(EnabledObserver::graph_calls > 0);
}

TEST_CASE (



"Runner emits GraphLowered and GraphValidated events"
,
"[pravaha][runner][observer]"
)
 {
    using ObservedRunner = pravaha::Runner<
        pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        TestObserver>;

    TestObserver::reset();

    ObservedRunner runner;
    auto result = runner.submit(pravaha::task("a", []() {
    }));

    REQUIRE(result.has_value());
    REQUIRE(TestObserver::lowered == 1);
    REQUIRE(TestObserver::validated == 1);
}

TEST_CASE (



"Runner emits task lifecycle events for successful task"
,
"[pravaha][runner][observer]"
)
 {
    using ObservedRunner = pravaha::Runner<
        pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        TestObserver>;

    TestObserver::reset();

    ObservedRunner runner;
    auto result = runner.submit(pravaha::task("a", []() {
    }));

    REQUIRE(result.has_value());
    REQUIRE(TestObserver::task_events == std::vector<pravaha::EventKind>{
            pravaha::EventKind::TaskReady,
            pravaha::EventKind::TaskScheduled,
            pravaha::EventKind::TaskStarted,
            pravaha::EventKind::TaskCompleted
            });
}

TEST_CASE (



"Runner emits PayloadForwarded when forwarding task output to one-input successor"
,
"[pravaha][runner][observer][payload_forward]"
)
 {
    using ObservedRunner = pravaha::Runner<
        pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        TestObserver>;

    TestObserver::reset();
    ObservedRunner runner;
    auto result = runner.submit(
        pravaha::task("source", []() -> int { return 42; })
        | pravaha::task("sink", [](int) -> pravaha::Unit { return {}; })
    );

    REQUIRE(result.has_value());
    REQUIRE(
        std::count(TestObserver::task_events.begin(), TestObserver::task_events.end(), pravaha::EventKind::
            PayloadForwarded) == 1);
}

TEST_CASE (



"Runner with disabled observer emits no PayloadForwarded events"
,
"[pravaha][runner][observer][payload_forward]"
)
 {
    using DisabledRunner = pravaha::Runner<
        pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        DisabledObserver>;

    DisabledObserver::reset();
    DisabledRunner runner;
    auto result = runner.submit(
        pravaha::task("source", []() -> int { return 7; })
        | pravaha::task("sink", [](int) -> pravaha::Unit { return {}; })
    );

    REQUIRE(result.has_value());
    REQUIRE(DisabledObserver::task_calls == 0);
    REQUIRE(DisabledObserver::join_calls == 0);
    REQUIRE(DisabledObserver::graph_calls == 0);
}

TEST_CASE (



"PayloadForwarded event records source target and payload type hash"
,
"[pravaha][runner][observer][payload_forward]"
)
 {
    using ObservedRunner = pravaha::Runner<
        pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        TestObserver>;

    TestObserver::reset();
    ObservedRunner runner;
    auto result = runner.submit(
        pravaha::task("source", []() -> int { return 9; })
        | pravaha::task("sink", [](int) -> pravaha::Unit { return {}; })
    );

    REQUIRE(result.has_value());
    const auto it = std::find_if(
        TestObserver::task_event_records.begin(),
        TestObserver::task_event_records.end(),
        [](const pravaha::TaskEvent &e) {
            return e.kind == pravaha::EventKind::PayloadForwarded;
        }
    );
    REQUIRE(it != TestObserver::task_event_records.end());
    REQUIRE(it->from_task_id == pravaha::TaskId{0});
    REQUIRE(it->to_task_id == pravaha::TaskId{1});
    REQUIRE(it->payload_type_hash == pravaha::make_type_contract<int>().type_hash);
}

TEST_CASE (



"Runner emits TaskStarted and TaskFailed for failing task"
,
"[pravaha][runner][observer]"
)
 {
    using ObservedRunner = pravaha::Runner<
        pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        TestObserver>;

    TestObserver::reset();

    ObservedRunner runner;
    auto result = runner.submit(pravaha::task("bad", []() -> pravaha::Outcome<pravaha::Unit> {
        return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "bad"});
    }));

    REQUIRE(result.has_value());
    const auto started_it = std::find(TestObserver::task_events.begin(), TestObserver::task_events.end(),
                                      pravaha::EventKind::TaskStarted);
    const auto failed_it = std::find(TestObserver::task_events.begin(), TestObserver::task_events.end(),
                                     pravaha::EventKind::TaskFailed);
    REQUIRE(started_it != TestObserver::task_events.end());
    REQUIRE(failed_it != TestObserver::task_events.end());
    REQUIRE(started_it < failed_it);
}

TEST_CASE (



"Runner emits TaskSkipped for downstream task after failure"
,
"[pravaha][runner][observer]"
)
 {
    using ObservedRunner = pravaha::Runner<
        pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        TestObserver>;

    TestObserver::reset();

    int after_ran = 0;
    ObservedRunner runner;
    auto result = runner.submit(
        pravaha::task("bad", []() -> pravaha::Outcome<pravaha::Unit> {
            return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "bad"});
        })
        |
        pravaha::task("after", [&after_ran]() { ++after_ran; })
    );

    REQUIRE(result.has_value());
    REQUIRE(after_ran == 0);
    REQUIRE(
        std::count(TestObserver::task_events.begin(), TestObserver::task_events.end(), pravaha::EventKind::TaskFailed)
        == 1);
    REQUIRE(
        std::count(TestObserver::task_events.begin(), TestObserver::task_events.end(), pravaha::EventKind::TaskSkipped)
        == 1);
    const auto failed_it = std::find(TestObserver::task_events.begin(), TestObserver::task_events.end(),
                                     pravaha::EventKind::TaskFailed);
    const auto skipped_it = std::find(TestObserver::task_events.begin(), TestObserver::task_events.end(),
                                      pravaha::EventKind::TaskSkipped);
    REQUIRE(failed_it != TestObserver::task_events.end());
    REQUIRE(skipped_it != TestObserver::task_events.end());
    REQUIRE(failed_it < skipped_it);
}

TEST_CASE (



"Runner emits TaskCanceled and TaskSkipped on cancellation"
,
"[pravaha][runner][observer][cancellation]"
)
 {
    using ObservedRunner = pravaha::Runner<
        pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        TestObserver>;

    TestObserver::reset();
    pravaha::CancellationSource source;
    source.request_stop();

    ObservedRunner runner;
    auto result = runner.submit(
        pravaha::task("a", []() {
        })
        | pravaha::task("b", []() {
        })
        | pravaha::task("c", []() {
        }),
        source.token());

    REQUIRE(result.has_value());
    REQUIRE(std::count(result->node_states.begin(), result->node_states.end(), pravaha::TaskState::Canceled) >= 1);
    REQUIRE(std::count(result->node_states.begin(), result->node_states.end(), pravaha::TaskState::Skipped) >= 1);
}

TEST_CASE (



"Runner emits TaskCanceled and TaskSkipped on timeout policy cancellation"
,
"[pravaha][runner][observer][timeout_policy]"
)
 {
    struct ForceTimeoutPolicy {
        static bool on_timeout(std::chrono::nanoseconds) noexcept {
            return true;
        }
    };

    auto wait_for_queue_at_least = [](ControlledBackend &backend, std::size_t min_size) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            if (backend.queue_size() >= min_size) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return backend.queue_size() >= min_size;
    };

    using ObservedRunner = pravaha::Runner<
        ControlledBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        TestObserver,
        pravaha::NoRetryPolicy,
        ForceTimeoutPolicy>;

    TestObserver::reset();
    ControlledBackend backend;
    ObservedRunner runner(backend);
    std::atomic a_runs{0};
    std::atomic b_runs{0};

    auto expr = pravaha::with_timeout(std::chrono::milliseconds(1),
                                      pravaha::task("a", [&]() { a_runs.fetch_add(1); }))
                | pravaha::task("b", [&]() { b_runs.fetch_add(1); });

    auto fut = std::async(std::launch::async, [&runner, expr = std::move(expr)]() mutable {
        return runner.submit(std::move(expr));
    });

    REQUIRE(wait_for_queue_at_least(backend, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(backend.run_front());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready
           && std::chrono::steady_clock::now() < deadline) {
        if (!backend.run_front()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    REQUIRE(fut.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    auto result = fut.get();
    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Canceled);
    REQUIRE(result->node_states[0] == pravaha::TaskState::Canceled);
    REQUIRE(result->node_states[1] == pravaha::TaskState::Skipped);
    REQUIRE(a_runs.load() == 0);
    REQUIRE(b_runs.load() == 0);
    REQUIRE(
        std::count(TestObserver::task_events.begin(), TestObserver::task_events.end(), pravaha::EventKind::TaskCanceled)
        >= 1);
    REQUIRE(
        std::count(TestObserver::task_events.begin(), TestObserver::task_events.end(), pravaha::EventKind::TaskSkipped)
        >= 1);
}

TEST_CASE (



"Runner emits JoinResolved for AllOrNothing success"
,
"[pravaha][runner][observer]"
)
 {
    using ObservedRunner = pravaha::Runner<
        pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        TestObserver>;

    TestObserver::reset();
    ObservedRunner runner;
    auto result = runner.submit(pravaha::task("a", []() {
    }) & pravaha::task("b", []() {
    }));

    REQUIRE(result.has_value());
    REQUIRE(TestObserver::join_events.size() == 1);
    const auto &e = TestObserver::join_events.front();
    REQUIRE(e.kind == pravaha::EventKind::JoinResolved);
    REQUIRE(e.policy.kind == pravaha::JoinPolicyKind::AllOrNothing);
    REQUIRE(e.success);
    REQUIRE(e.expected == 2);
    REQUIRE(e.succeeded == 2);
    REQUIRE(e.failed == 0);
    REQUIRE(e.canceled == 0);
    REQUIRE(e.skipped == 0);
}

TEST_CASE (



"Runner emits JoinResolved for CollectAll failure"
,
"[pravaha][runner][observer]"
)
 {
    using ObservedRunner = pravaha::Runner<
        pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        TestObserver>;

    TestObserver::reset();
    ObservedRunner runner;
    auto result = runner.submit(pravaha::collect_all(
        pravaha::task("ok", []() {
        })
        &
        pravaha::task("fail", []() -> pravaha::Outcome<pravaha::Unit> {
            return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "fail"});
        })
    ));

    REQUIRE(result.has_value());
    REQUIRE(TestObserver::join_events.size() == 1);
    const auto &e = TestObserver::join_events.front();
    REQUIRE(e.kind == pravaha::EventKind::JoinResolved);
    REQUIRE(e.policy.kind == pravaha::JoinPolicyKind::CollectAll);
    REQUIRE_FALSE(e.success);
    REQUIRE(e.expected == 2);
    REQUIRE(e.succeeded == 1);
    REQUIRE(e.failed == 1);
    REQUIRE(e.canceled == 0);
    REQUIRE(e.skipped == 0);
}

TEST_CASE (



"Runner emits one JoinResolved for AnySuccess"
,
"[pravaha][runner][observer]"
)
 {
    using ObservedRunner = pravaha::Runner<
        pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        TestObserver>;

    TestObserver::reset();
    ObservedRunner runner;
    auto result = runner.submit(pravaha::any_success(
        pravaha::task("ok", []() {
        })
        &
        pravaha::task("fail", []() -> pravaha::Outcome<pravaha::Unit> {
            return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "fail"});
        })
    ));

    REQUIRE(result.has_value());
    REQUIRE(TestObserver::join_events.size() == 1);
    const auto &e = TestObserver::join_events.front();
    REQUIRE(e.kind == pravaha::EventKind::JoinResolved);
    REQUIRE(e.policy.kind == pravaha::JoinPolicyKind::AnySuccess);
    REQUIRE(e.success);
}

TEST_CASE (



"Runner emits one JoinResolved for Quorum<1>"
,
"[pravaha][runner][observer]"
)
 {
    using ObservedRunner = pravaha::Runner<
        pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        TestObserver>;

    TestObserver::reset();
    ObservedRunner runner;
    auto result = runner.submit(pravaha::quorum<1>(
        pravaha::task("ok", []() {
        })
        &
        pravaha::task("fail", []() -> pravaha::Outcome<pravaha::Unit> {
            return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "fail"});
        })
    ));

    REQUIRE(result.has_value());
    REQUIRE(TestObserver::join_events.size() == 1);
    const auto &e = TestObserver::join_events.front();
    REQUIRE(e.kind == pravaha::EventKind::JoinResolved);
    REQUIRE(e.policy.kind == pravaha::JoinPolicyKind::Quorum);
    REQUIRE(e.success);
}

TEST_CASE (



"Runner emits non-zero timestamps for task/join/graph events"
,
"[pravaha][runner][observer]"
)
 {
    using ObservedRunner = pravaha::Runner<
        pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        TestObserver>;

    TestObserver::reset();
    ObservedRunner runner;
    auto result = runner.submit(pravaha::any_success(
        pravaha::task("ok", []() {
        })
        &
        pravaha::task("fail", []() -> pravaha::Outcome<pravaha::Unit> {
            return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "fail"});
        })
    ));

    REQUIRE(result.has_value());
    REQUIRE_FALSE(TestObserver::task_event_records.empty());
    REQUIRE_FALSE(TestObserver::join_events.empty());
    REQUIRE_FALSE(TestObserver::graph_events.empty());
    REQUIRE(std::all_of(TestObserver::task_event_records.begin(), TestObserver::task_event_records.end(),
        [](const pravaha::TaskEvent& e) { return e.timestamp_ns > 0; }));
    REQUIRE(std::all_of(TestObserver::join_events.begin(), TestObserver::join_events.end(),
        [](const pravaha::JoinEvent& e) { return e.timestamp_ns > 0; }));
    REQUIRE(std::all_of(TestObserver::graph_events.begin(), TestObserver::graph_events.end(),
        [](const pravaha::GraphEvent& e) { return e.timestamp_ns > 0; }));
}

TEST_CASE (



"TraceObserver captures compact external trace"
,
"[pravaha][runner][observer]"
)
 {
    using TraceRunner = pravaha::Runner<
        pravaha::InlineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        TraceObserver>;

    TraceObserver::reset();

    TraceRunner runner;
    auto result = runner.submit(
        (pravaha::task("a", []() {
        }) & pravaha::task("b", []() {
        }))
        | pravaha::task("c", []() {
        })
    );

    REQUIRE(result.has_value());
    REQUIRE(
        std::find(TraceObserver::trace.begin(), TraceObserver::trace.end(), "graph:lowered") != TraceObserver::trace.end
        ());
    REQUIRE(
        std::find(TraceObserver::trace.begin(), TraceObserver::trace.end(), "graph:validated") != TraceObserver::trace.
        end());
    REQUIRE(
        std::find(TraceObserver::trace.begin(), TraceObserver::trace.end(), "task:scheduled:a") != TraceObserver::trace.
        end());
    REQUIRE(
        std::find(TraceObserver::trace.begin(), TraceObserver::trace.end(), "task:scheduled:b") != TraceObserver::trace.
        end());
    REQUIRE(
        std::find(TraceObserver::trace.begin(), TraceObserver::trace.end(), "task:scheduled:c") != TraceObserver::trace.
        end());
    REQUIRE(
        std::find(TraceObserver::trace.begin(), TraceObserver::trace.end(), "join:resolved:success") != TraceObserver::
        trace.end());
}

TEST_CASE (



"Runner<JThreadBackend> exposes queue wait timing via task timestamps"
,
"[pravaha][jthread][runner][observer]"
)
 {
    using ObservedRunner = pravaha::Runner<
        pravaha::JThreadBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        QueueWaitObserver>;

    QueueWaitObserver::reset();

    pravaha::JThreadBackend backend(2);
    ObservedRunner runner(backend);
    auto result = runner.submit(pravaha::task("a", []() {
    }) | pravaha::task("b", []() {
    }));

    REQUIRE(result.has_value());

    std::vector<std::pair<pravaha::TaskId, std::uint64_t> > scheduled;
    std::vector<std::pair<pravaha::TaskId, std::uint64_t> > started;
    {
        std::lock_guard lock(QueueWaitObserver::mutex);
        for (const auto &e: QueueWaitObserver::task_events) {
            if (e.kind == pravaha::EventKind::TaskScheduled) {
                REQUIRE(e.timestamp_ns > 0);
                scheduled.emplace_back(e.task_id, e.timestamp_ns);
            }
            if (e.kind == pravaha::EventKind::TaskStarted) {
                REQUIRE(e.timestamp_ns > 0);
                started.emplace_back(e.task_id, e.timestamp_ns);
            }
        }
    }

    REQUIRE(scheduled.size() >= 2);
    REQUIRE(started.size() >= 2);

    for (const auto &[task_id, started_ns]: started) {
        const auto it = std::find_if(scheduled.begin(), scheduled.end(), [task_id](const auto &p) {
            return p.first == task_id;
        });
        REQUIRE(it != scheduled.end());
        REQUIRE(started_ns >= it->second);
    }
}

TEST_CASE (



"parallel_for_eager executes immediately"
,
"[pravaha][algorithms][parallel_for][eager]"
)
 {
    std::vector values(7, 0);
    int chunks_called = 0;

    auto result = pravaha::parallel_for_eager("pf", values, 3, [&](std::size_t begin, std::size_t end) {
        ++chunks_called;
        for (std::size_t i = begin; i < end; ++i) {
            values[i] = 1;
        }
    });

    REQUIRE(chunks_called == 3);
    REQUIRE(result.chunk_count == 3);
    REQUIRE(std::all_of(values.begin(), values.end(), [](int v) { return v == 1; }));
}

TEST_CASE (



"parallel_for construction does not execute body"
,
"[pravaha][algorithms][parallel_for][lazy]"
)
 {
    std::vector values(8, 0);
    int calls = 0;

    auto expr = pravaha::parallel_for("pf", values, 3, [&](int) {
        ++calls;
    });

    REQUIRE(calls == 0);
    REQUIRE(expr.frontend.hash != 0);
}

TEST_CASE (



"StaticChunkingPolicy produces expected chunk ranges"
,
"[pravaha][algorithms][chunking]"
)
 {
    const auto ranges = pravaha::StaticChunkingPolicy::chunks(7, 3);
    REQUIRE(ranges.size() == 3);
    REQUIRE(ranges[0] == pravaha::ChunkRange{0, 3});
    REQUIRE(ranges[1] == pravaha::ChunkRange{3, 6});
    REQUIRE(ranges[2] == pravaha::ChunkRange{6, 7});
}

TEST_CASE (



"StaticChunkingPolicy normalizes zero chunk size"
,
"[pravaha][algorithms][chunking]"
)
 {
    const auto ranges = pravaha::StaticChunkingPolicy::chunks(3, 0);
    REQUIRE(ranges.size() == 3);
    REQUIRE(ranges[0] == pravaha::ChunkRange{0, 1});
    REQUIRE(ranges[1] == pravaha::ChunkRange{1, 2});
    REQUIRE(ranges[2] == pravaha::ChunkRange{2, 3});
}

TEST_CASE (



"parallel_for custom ChunkingPolicy changes lowered node count"
,
"[pravaha][algorithms][chunking]"
)
 {
    struct SingleChunkPolicy {
        static std::vector<pravaha::ChunkRange> chunks(std::size_t total, std::size_t) {
            if (total == 0) {
                return {};
            }
            return {{0, total}};
        }
    };

    std::vector values(8, 1);
    auto default_expr = pravaha::parallel_for("pf", values, 3, [](int) {
    });
    auto custom_expr = pravaha::parallel_for<SingleChunkPolicy>("pf", values, 3, [](int) {
    });

    auto default_ir = pravaha::lower_to_ir(std::move(default_expr));
    auto custom_ir = pravaha::lower_to_ir(std::move(custom_expr));

    REQUIRE(default_ir.has_value());
    REQUIRE(custom_ir.has_value());
    REQUIRE(default_ir->nodes.size() == 3);
    REQUIRE(custom_ir->nodes.size() == 1);
}

TEST_CASE (



"parallel_for custom ChunkingPolicy preserves execution result"
,
"[pravaha][algorithms][chunking]"
)
 {
    struct SingleChunkPolicy {
        static std::vector<pravaha::ChunkRange> chunks(std::size_t total, std::size_t) {
            if (total == 0) {
                return {};
            }
            return {{0, total}};
        }
    };

    std::vector values(11, 1);
    std::atomic seen{0};
    auto expr = pravaha::parallel_for<SingleChunkPolicy>("pf", values, 2, [&](int v) {
        if (v == 1) {
            seen.fetch_add(1);
        }
    });

    pravaha::Runner<> runner;
    auto result = runner.submit(std::move(expr));

    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Succeeded);
    REQUIRE(seen.load() == 11);
}

TEST_CASE (



"parallel_transform custom ChunkingPolicy preserves result"
,
"[pravaha][algorithms][chunking]"
)
 {
    struct SingleChunkPolicy {
        static std::vector<pravaha::ChunkRange> chunks(std::size_t total, std::size_t) {
            if (total == 0) {
                return {};
            }
            return {{0, total}};
        }
    };

    std::vector input{2, 4, 6, 8};
    std::vector output(input.size(), 0);
    std::span<const int> in_view(input.data(), input.size());
    std::span out_view(output.data(), output.size());
    auto expr = pravaha::parallel_transform<SingleChunkPolicy>(in_view, out_view, [](int v) { return v + 3; }, 1);

    pravaha::Runner<> runner;
    auto result = runner.submit(std::move(expr));

    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Succeeded);
    REQUIRE(output == std::vector<int>{5, 7, 9, 11});
}

TEST_CASE (



"lazy_parallel_for frontend hash is stable for same shape"
,
"[pravaha][algorithms][parallel_for][lazy]"
)
 {
    std::vector left(16, 0);
    std::vector right(16, 0);

    auto first = pravaha::lazy_parallel_for(left, [](std::size_t, std::size_t) {
    }, 4);
    auto second = pravaha::lazy_parallel_for(right, [](std::size_t, std::size_t) {
    }, 4);

    REQUIRE(first.frontend.hash == second.frontend.hash);
}

TEST_CASE (



"lazy_parallel_for frontend hash changes with chunk size"
,
"[pravaha][algorithms][parallel_for][lazy]"
)
 {
    std::vector values(16, 0);

    auto first = pravaha::lazy_parallel_for(values, [](std::size_t, std::size_t) {
    }, 2);
    auto second = pravaha::lazy_parallel_for(values, [](std::size_t, std::size_t) {
    }, 8);

    REQUIRE(first.frontend.hash != second.frontend.hash);
}

TEST_CASE (



"Runner::submit(parallel_for(...)) executes all items"
,
"[pravaha][algorithms][parallel_for][lazy][runner]"
)
 {
    constexpr std::size_t item_count = 10;
    std::vector values(item_count, 1);
    std::atomic seen{0};

    pravaha::Runner<> runner;
    auto expr = pravaha::parallel_for("pf", values, 3, [&](int v) {
        if (v == 1) {
            seen.fetch_add(1);
        }
    });
    auto result = runner.submit(std::move(expr));

    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Succeeded);
    REQUIRE(seen.load() == static_cast<int>(item_count));
}

TEST_CASE (



"lazy_parallel_for(...) | task runs task after all chunks"
,
"[pravaha][algorithms][parallel_for][lazy][runner]"
)
 {
    constexpr std::size_t item_count = 12;
    std::vector values(item_count, 1);
    std::atomic seen{0};
    int after_ran = 0;

    auto expr = pravaha::lazy_parallel_for(values, [&](int v) {
        if (v == 1) {
            seen.fetch_add(1);
        }
    }, 4) | pravaha::task("after", [&]() {
        if (seen.load() != static_cast<int>(item_count)) {
            throw std::runtime_error("parallel_for incomplete");
        }
        ++after_ran;
    });

    pravaha::Runner<> runner;
    auto result = runner.submit(std::move(expr));

    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Succeeded);
    REQUIRE(after_ran == 1);
}

TEST_CASE (



"task | lazy_parallel_for(...) runs chunks after before"
,
"[pravaha][algorithms][parallel_for][lazy][runner]"
)
 {
    std::vector values(9, 1);
    std::atomic before_done{false};
    std::atomic seen{0};

    auto expr = pravaha::task("before", [&]() {
        before_done.store(true);
    }) | pravaha::lazy_parallel_for(values, [&](int v) {
        if (!before_done.load()) {
            throw std::runtime_error("before not complete");
        }
        if (v == 1) {
            seen.fetch_add(1);
        }
    }, 2);

    pravaha::Runner<> runner;
    auto result = runner.submit(std::move(expr));

    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Succeeded);
    REQUIRE(seen.load() == 9);
}

TEST_CASE (



"lazy_parallel_for failure blocks downstream under AllOrNothing"
,
"[pravaha][algorithms][parallel_for][lazy][runner]"
)
 {
    std::vector values{0, 1, 2, 3, 4, 5};
    int after_ran = 0;

    auto expr = pravaha::lazy_parallel_for(values, [&](int v) {
        if (v == 3) {
            throw std::runtime_error("chunk failure");
        }
    }, 2) | pravaha::task("after", [&]() {
        ++after_ran;
    });

    pravaha::Runner<> runner;
    auto result = runner.submit(std::move(expr));

    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Failed);
    REQUIRE(after_ran == 0);
}

TEST_CASE (



"parallel_transform_eager executes immediately"
,
"[pravaha][algorithms][parallel_transform][eager]"
)
 {
    std::vector values{1, 2, 3, 4};
    int elements_called = 0;

    auto &&out = pravaha::parallel_transform_eager(values, 2, [&](int &v) {
        ++elements_called;
        v *= 10;
    });

    REQUIRE(&out == &values);
    REQUIRE(elements_called == 4);
    REQUIRE(values == std::vector<int>{10, 20, 30, 40});
}

TEST_CASE (



"parallel_transform construction does not execute fn"
,
"[pravaha][algorithms][parallel_transform][lazy]"
)
 {
    std::vector input{1, 2, 3, 4};
    std::vector output(input.size(), 0);
    int calls = 0;

    std::span<const int> in_view(input.data(), input.size());
    std::span out_view(output.data(), output.size());

    auto expr = pravaha::parallel_transform(in_view, out_view, [&](int) {
        ++calls;
        return 0;
    });

    REQUIRE(calls == 0);
    REQUIRE(expr.frontend.hash != 0);
}

TEST_CASE (



"lazy_parallel_transform frontend hash changes with chunk size"
,
"[pravaha][algorithms][parallel_transform][lazy]"
)
 {
    std::vector input(16, 1);
    std::vector output_a(input.size(), 0);
    std::vector output_b(input.size(), 0);

    auto first = pravaha::lazy_parallel_transform(input, output_a, [](int v) { return v; }, 2);
    auto second = pravaha::lazy_parallel_transform(input, output_b, [](int v) { return v; }, 8);

    REQUIRE(first.frontend.hash != second.frontend.hash);
}

TEST_CASE (



"Runner::submit(parallel_reduce(...)) stores unchanged reduction result"
,
"[pravaha][algorithms][parallel_reduce][lazy][runner]"
)
 {
    std::vector values{1, 2, 3, 4, 5, 6, 7};
    auto expr = pravaha::parallel_reduce(
        values,
        0,
        [](int v) { return v; },
        [](int acc, int v) { return acc + v; },
        3
    );
    auto result_handle = expr.result();

    pravaha::Runner<> runner;
    auto result = runner.submit(std::move(expr));

    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Succeeded);
    REQUIRE(result_handle.value != nullptr);
    REQUIRE(result_handle.value->has_value());
    REQUIRE(result_handle.value->value() == 28);
}

TEST_CASE (



"parallel_reduce custom ChunkingPolicy preserves reduction result"
,
"[pravaha][algorithms][chunking]"
)
 {
    struct SingleChunkPolicy {
        static std::vector<pravaha::ChunkRange> chunks(std::size_t total, std::size_t) {
            if (total == 0) {
                return {};
            }
            return {{0, total}};
        }
    };

    std::vector values{1, 2, 3, 4, 5, 6, 7};
    auto default_expr = pravaha::parallel_reduce(
        values,
        0,
        [](int v) { return v; },
        [](int acc, int v) { return acc + v; },
        2
    );
    auto custom_expr = pravaha::parallel_reduce<SingleChunkPolicy>(
        values,
        0,
        [](int v) { return v; },
        [](int acc, int v) { return acc + v; },
        2
    );
    auto default_handle = default_expr.result();
    auto custom_handle = custom_expr.result();

    pravaha::Runner<> runner;
    auto default_result = runner.submit(std::move(default_expr));
    auto custom_result = runner.submit(std::move(custom_expr));

    REQUIRE(default_result.has_value());
    REQUIRE(custom_result.has_value());
    REQUIRE(default_result->final_state == pravaha::TaskState::Succeeded);
    REQUIRE(custom_result->final_state == pravaha::TaskState::Succeeded);
    REQUIRE(default_handle.value->has_value());
    REQUIRE(custom_handle.value->has_value());
    REQUIRE(default_handle.value->value() == 28);
    REQUIRE(custom_handle.value->value() == 28);
}

TEST_CASE (



"parallel_reduce lowering remains acyclic"
,
"[pravaha][algorithms][parallel_reduce][lazy][ir]"
)
 {
    std::vector values{1, 2, 3, 4, 5, 6, 7};
    auto expr = pravaha::parallel_reduce(
        values,
        0,
        [](int v) { return v; },
        [](int acc, int v) { return acc + v; },
        2
    );

    auto ir_result = pravaha::lower_to_ir(std::move(expr));
    REQUIRE(ir_result.has_value());
    REQUIRE(ir_result->node_count() == 8);
    REQUIRE(ir_result->nodes.back().name == "parallel_reduce.final");

    auto validation = pravaha::validate_ir_with_litegraph(ir_result.value());
    REQUIRE(validation.has_value());
}

TEST_CASE (



"parallel_reduce FlatReductionPolicy computes correct result"
,
"[pravaha][algorithms][parallel_reduce][policy]"
)
 {
    std::vector values{1, 2, 3, 4, 5, 6, 7};
    auto expr = pravaha::parallel_reduce<pravaha::StaticChunkingPolicy, pravaha::FlatReductionPolicy>(
        values,
        0,
        [](int v) { return v; },
        [](int acc, int v) { return acc + v; },
        2
    );
    auto handle = expr.result();

    pravaha::Runner<> runner;
    auto result = runner.submit(std::move(expr));

    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Succeeded);
    REQUIRE(handle.value->has_value());
    REQUIRE(handle.value->value() == 28);
}

TEST_CASE (



"parallel_reduce NAry and Flat policies produce same result"
,
"[pravaha][algorithms][parallel_reduce][policy]"
)
 {
    std::vector values{1, 2, 3, 4, 5, 6, 7};
    auto nary_expr = pravaha::parallel_reduce<pravaha::StaticChunkingPolicy, pravaha::NAryTreeReductionPolicy>(
        values,
        0,
        [](int v) { return v; },
        [](int acc, int v) { return acc + v; },
        2
    );
    auto flat_expr = pravaha::parallel_reduce<pravaha::StaticChunkingPolicy, pravaha::FlatReductionPolicy>(
        values,
        0,
        [](int v) { return v; },
        [](int acc, int v) { return acc + v; },
        2
    );
    auto nary_handle = nary_expr.result();
    auto flat_handle = flat_expr.result();

    pravaha::Runner<> runner;
    auto nary_result = runner.submit(std::move(nary_expr));
    auto flat_result = runner.submit(std::move(flat_expr));

    REQUIRE(nary_result.has_value());
    REQUIRE(flat_result.has_value());
    REQUIRE(nary_result->final_state == pravaha::TaskState::Succeeded);
    REQUIRE(flat_result->final_state == pravaha::TaskState::Succeeded);
    REQUIRE(nary_handle.value->has_value());
    REQUIRE(flat_handle.value->has_value());
    REQUIRE(nary_handle.value->value() == flat_handle.value->value());
}

TEST_CASE (



"parallel_reduce NAry and Flat topologies are valid"
,
"[pravaha][algorithms][parallel_reduce][policy][ir]"
)
 {
    std::vector values{1, 2, 3, 4, 5, 6, 7};
    auto nary_expr = pravaha::parallel_reduce<pravaha::StaticChunkingPolicy, pravaha::NAryTreeReductionPolicy>(
        values,
        0,
        [](int v) { return v; },
        [](int acc, int v) { return acc + v; },
        2
    );
    auto flat_expr = pravaha::parallel_reduce<pravaha::StaticChunkingPolicy, pravaha::FlatReductionPolicy>(
        values,
        0,
        [](int v) { return v; },
        [](int acc, int v) { return acc + v; },
        2
    );

    auto nary_ir = pravaha::lower_to_ir(std::move(nary_expr));
    auto flat_ir = pravaha::lower_to_ir(std::move(flat_expr));

    REQUIRE(nary_ir.has_value());
    REQUIRE(flat_ir.has_value());
    REQUIRE(pravaha::validate_ir_with_litegraph(nary_ir.value()).has_value());
    REQUIRE(pravaha::validate_ir_with_litegraph(flat_ir.value()).has_value());
    REQUIRE(flat_ir->node_count() < nary_ir->node_count());
}

TEST_CASE (



"Runner::submit(parallel_transform(...)) fills output correctly"
,
"[pravaha][algorithms][parallel_transform][lazy][runner]"
)
 {
    std::vector input{1, 2, 3, 4, 5};
    std::vector output(input.size(), 0);

    std::span<const int> in_view(input.data(), input.size());
    std::span out_view(output.data(), output.size());

    auto expr = pravaha::parallel_transform(in_view, out_view, [](int v) {
        return v * 10;
    }, 2);

    pravaha::Runner<> runner;
    auto result = runner.submit(std::move(expr));

    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Succeeded);
    REQUIRE(output == std::vector<int>{10, 20, 30, 40, 50});
}

TEST_CASE (



"lazy_parallel_transform(...) | task runs task after transform"
,
"[pravaha][algorithms][parallel_transform][lazy][runner]"
)
 {
    std::vector input{1, 2, 3, 4};
    std::vector output(input.size(), 0);
    int after_ran = 0;

    std::span<const int> in_view(input.data(), input.size());
    std::span out_view(output.data(), output.size());

    auto expr = pravaha::lazy_parallel_transform(in_view, out_view, [](int v) {
        return v + 1;
    }, 2) | pravaha::task("after", [&]() {
        if (output != std::vector{2, 3, 4, 5}) {
            throw std::runtime_error("transform incomplete");
        }
        ++after_ran;
    });

    pravaha::Runner<> runner;
    auto result = runner.submit(std::move(expr));

    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Succeeded);
    REQUIRE(after_ran == 1);
}

TEST_CASE (



"task | lazy_parallel_transform(...) runs transform after before"
,
"[pravaha][algorithms][parallel_transform][lazy][runner]"
)
 {
    std::vector input{3, 4, 5};
    std::vector output(input.size(), 0);
    std::atomic before_done{false};

    std::span<const int> in_view(input.data(), input.size());
    std::span out_view(output.data(), output.size());

    auto expr = pravaha::task("before", [&]() {
        before_done.store(true);
    }) | pravaha::lazy_parallel_transform(in_view, out_view, [&](int v) {
        if (!before_done.load()) {
            throw std::runtime_error("before not complete");
        }
        return v * 2;
    }, 2);

    pravaha::Runner<> runner;
    auto result = runner.submit(std::move(expr));

    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Succeeded);
    REQUIRE(output == std::vector<int>{6, 8, 10});
}

TEST_CASE (



"lazy_parallel_transform failure blocks downstream under AllOrNothing"
,
"[pravaha][algorithms][parallel_transform][lazy][runner]"
)
 {
    std::vector input{1, 2, 3, 4, 5};
    std::vector output(input.size(), 0);
    int after_ran = 0;

    std::span<const int> in_view(input.data(), input.size());
    std::span out_view(output.data(), output.size());

    auto expr = pravaha::lazy_parallel_transform(in_view, out_view, [](int v) {
        if (v == 3) {
            throw std::runtime_error("transform failure");
        }
        return v;
    }, 2) | pravaha::task("after", [&]() {
        ++after_ran;
    });

    pravaha::Runner<> runner;
    auto result = runner.submit(std::move(expr));

    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Failed);
    REQUIRE(after_ran == 0);
}

namespace deterministic_controlled_cancellation {
    struct ControlledBackend {
        std::deque<pravaha::TaskCommand> queue;
        bool stopped{false};

        bool submit(pravaha::TaskCommand cmd) {
            if (stopped) {
                return false;
            }
            queue.push_back(std::move(cmd));
            return true;
        }

        void drain() {
            std::size_t guard = 0;
            while (!stopped && run_one()) {
                ++guard;
                if (guard > 100000) {
                    break;
                }
            }
        }

        void request_stop() noexcept {
            stopped = true;
        }

        bool run_one() {
            if (queue.empty()) {
                return false;
            }
            auto cmd = std::move(queue.front());
            queue.pop_front();
            (void)cmd.run();
            return true;
        }

        std::size_t pending() const noexcept {
            return queue.size();
        }
    };

    using ObservedRunner = pravaha::Runner<
        ControlledBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        TestObserver>;

    bool wait_for_pending_at_least(const ControlledBackend& backend, std::size_t min_size) {
        for (std::size_t i = 0; i < 200000; ++i) {
            if (backend.pending() >= min_size) {
                return true;
            }
            std::this_thread::yield();
        }
        return backend.pending() >= min_size;
    }
}

TEST_CASE (



"Deterministic cancellation sequence before first task"
,
"[pravaha][runner][cancellation][controlled][deterministic]"
)
 {
    deterministic_controlled_cancellation::ControlledBackend backend;
    deterministic_controlled_cancellation::ObservedRunner runner(backend);
    TestObserver::reset();

    std::atomic ran_a{0};
    std::atomic ran_b{0};
    std::atomic ran_c{0};

    auto expr =
            pravaha::task("A", [&]() { ran_a.fetch_add(1); })
            | pravaha::task("B", [&]() { ran_b.fetch_add(1); })
            | pravaha::task("C", [&]() { ran_c.fetch_add(1); });

    auto fut = std::async(std::launch::async, [&]() mutable {
        return runner.submit(std::move(expr));
    });

    REQUIRE(deterministic_controlled_cancellation::wait_for_pending_at_least(backend, 1));
    runner.request_stop();
    while (backend.run_one()) {
    }

    REQUIRE(fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    auto result = fut.get();
    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Canceled);
    REQUIRE((ran_a.load() == 0 || (ran_b.load() == 0 && ran_c.load() == 0)));
    REQUIRE(ran_b.load() == 0);
    REQUIRE(ran_c.load() == 0);
    REQUIRE(std::count(result->node_states.begin(), result->node_states.end(), pravaha::TaskState::Canceled) >= 1);
    REQUIRE(std::count(result->node_states.begin(), result->node_states.end(), pravaha::TaskState::Skipped) >= 1);
}

TEST_CASE (



"Deterministic cancellation sequence after first task"
,
"[pravaha][runner][cancellation][controlled][deterministic]"
)
 {
    deterministic_controlled_cancellation::ControlledBackend backend;
    deterministic_controlled_cancellation::ObservedRunner runner(backend);
    TestObserver::reset();

    std::atomic ran_a{0};
    std::atomic ran_b{0};
    std::atomic ran_c{0};

    auto expr =
            pravaha::task("A", [&]() { ran_a.fetch_add(1); })
            | pravaha::task("B", [&]() { ran_b.fetch_add(1); })
            | pravaha::task("C", [&]() { ran_c.fetch_add(1); });

    auto fut = std::async(std::launch::async, [&]() mutable {
        return runner.submit(std::move(expr));
    });

    REQUIRE(deterministic_controlled_cancellation::wait_for_pending_at_least(backend, 1));
    REQUIRE(backend.run_one());
    runner.request_stop();
    while (backend.run_one()) {
    }

    REQUIRE(fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    auto result = fut.get();
    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Canceled);
    REQUIRE(ran_a.load() == 1);
    REQUIRE(ran_b.load() == 0);
    REQUIRE(ran_c.load() == 0);
    REQUIRE(result->node_states.size() == 3);
    REQUIRE(result->node_states[1] == pravaha::TaskState::Canceled);
    REQUIRE(result->node_states[2] == pravaha::TaskState::Skipped);
}

TEST_CASE (



"Deterministic cancellation parallel before one branch runs"
,
"[pravaha][runner][cancellation][controlled][deterministic]"
)
 {
    deterministic_controlled_cancellation::ControlledBackend backend;
    deterministic_controlled_cancellation::ObservedRunner runner(backend);
    TestObserver::reset();

    std::atomic ran_a{0};
    std::atomic ran_b{0};
    std::atomic ran_c{0};

    auto expr =
            (pravaha::task("A", [&]() { ran_a.fetch_add(1); })
             & pravaha::task("B", [&]() { ran_b.fetch_add(1); }))
            | pravaha::task("C", [&]() { ran_c.fetch_add(1); });

    auto fut = std::async(std::launch::async, [&]() mutable {
        return runner.submit(std::move(expr));
    });

    REQUIRE(deterministic_controlled_cancellation::wait_for_pending_at_least(backend, 2));
    REQUIRE(backend.run_one());
    runner.request_stop();
    while (backend.run_one()) {
    }

    REQUIRE(fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    auto result = fut.get();
    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Canceled);
    REQUIRE(ran_a.load() + ran_b.load() == 1);
    REQUIRE(ran_c.load() == 0);
    REQUIRE(result->node_states.size() == 3);
    REQUIRE(std::count(result->node_states.begin(), result->node_states.end(), pravaha::TaskState::Canceled) == 1);
    REQUIRE(result->node_states[2] == pravaha::TaskState::Skipped);
}

TEST_CASE (



"Deterministic cancellation after AnySuccess resolution"
,
"[pravaha][runner][cancellation][controlled][deterministic]"
)
 {
    deterministic_controlled_cancellation::ControlledBackend backend;
    deterministic_controlled_cancellation::ObservedRunner runner(backend);
    TestObserver::reset();

    std::atomic ran_a{0};
    std::atomic ran_b{0};
    std::atomic ran_c{0};

    auto expr = pravaha::any_success(
                    pravaha::task("A_success", [&]() { ran_a.fetch_add(1); })
                    &
                    pravaha::task("B_slow", [&]() { ran_b.fetch_add(1); })
                ) | pravaha::task("C", [&]() { ran_c.fetch_add(1); });

    auto fut = std::async(std::launch::async, [&]() mutable {
        return runner.submit(std::move(expr));
    });

    REQUIRE(deterministic_controlled_cancellation::wait_for_pending_at_least(backend, 2));
    REQUIRE(backend.run_one());
    REQUIRE(deterministic_controlled_cancellation::wait_for_pending_at_least(backend, 1));
    runner.request_stop();
    if (backend.pending() > 0) {
        REQUIRE(backend.run_one());
    }
    while (backend.run_one()) {
    }

    REQUIRE(fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    auto result = fut.get();
    REQUIRE(result.has_value());
    REQUIRE(!result->failed());
    REQUIRE(ran_a.load() == 1);
    REQUIRE(ran_b.load() == 0);
    REQUIRE(ran_c.load() <= 1);
}

TEST_CASE (



"Deterministic cancellation after Quorum<1> resolution"
,
"[pravaha][runner][cancellation][controlled][deterministic]"
)
 {
    deterministic_controlled_cancellation::ControlledBackend backend;
    deterministic_controlled_cancellation::ObservedRunner runner(backend);
    TestObserver::reset();

    std::atomic ran_a{0};
    std::atomic ran_b{0};
    std::atomic ran_c{0};

    auto expr = pravaha::quorum<1>(
                    pravaha::task("A_success", [&]() { ran_a.fetch_add(1); })
                    &
                    pravaha::task("B_slow", [&]() { ran_b.fetch_add(1); })
                ) | pravaha::task("C", [&]() { ran_c.fetch_add(1); });

    auto fut = std::async(std::launch::async, [&]() mutable {
        return runner.submit(std::move(expr));
    });

    REQUIRE(deterministic_controlled_cancellation::wait_for_pending_at_least(backend, 2));
    REQUIRE(backend.run_one());
    REQUIRE(deterministic_controlled_cancellation::wait_for_pending_at_least(backend, 1));
    runner.request_stop();
    if (backend.pending() > 0) {
        REQUIRE(backend.run_one());
    }
    while (backend.run_one()) {
    }

    REQUIRE(fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    auto result = fut.get();
    REQUIRE(result.has_value());
    REQUIRE(!result->failed());
    REQUIRE(ran_a.load() == 1);
    REQUIRE(ran_b.load() == 0);
    REQUIRE(ran_c.load() <= 1);
}

TEST_CASE (



"Deterministic cancellation for parallel_reduce remains functional"
,
"[pravaha][runner][cancellation][controlled][deterministic][parallel_reduce]"
)
 {
    deterministic_controlled_cancellation::ControlledBackend backend;
    deterministic_controlled_cancellation::ObservedRunner runner(backend);
    TestObserver::reset();

    std::vector values(32, 1);
    std::atomic after_runs{0};

    auto expr = pravaha::parallel_reduce(
                    values,
                    0,
                    [](int v) { return v; },
                    [](int acc, int v) { return acc + v; },
                    1
                ) | pravaha::task("after", [&]() { after_runs.fetch_add(1); });

    auto fut = std::async(std::launch::async, [&]() mutable {
        return runner.submit(std::move(expr));
    });

    REQUIRE(deterministic_controlled_cancellation::wait_for_pending_at_least(backend, 1));
    runner.request_stop();
    while (backend.run_one()) {
    }

    REQUIRE(fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    auto result = fut.get();
    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Canceled);
    REQUIRE(after_runs.load() == 0);
    REQUIRE(std::count(result->node_states.begin(), result->node_states.end(), pravaha::TaskState::Canceled) >= 1);
}
