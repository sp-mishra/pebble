#include <catch_amalgamated.hpp>
#include <petika/petika.hpp>
#include <petika/adapters/pravaha.hpp>
#include <filesystem>
#include <vector>
#include <string>
#include <atomic>

using namespace petika;
using namespace petika::adapters::pravaha;

TEST_CASE("Petika Pravaha Adapter: Async Commit & Parallel Range Scan", "[petika][adapter][pravaha]") {
    const std::filesystem::path test_dir = "./test_petika_pravaha_adapter_db";
    std::filesystem::remove_all(test_dir);

    PetikaOptions opts{
        .db_dir = test_dir,
        .segment_size = 1024 * 1024,
        .sync_on_write = false
    };

    PravahaAsyncStore<StringSkipStore> async_store(opts);

    // 1. Basic put and get
    auto r1 = async_store.put("k1", "v1");
    REQUIRE(r1.has_value());
    auto g1 = async_store.get("k1");
    REQUIRE(g1.has_value());
    REQUIRE(*g1 == "v1");

    // 2. Asynchronous batch commit
    using Mutation = Transaction<StringSkipStore>::Mutation;
    std::vector<Mutation> batch;
    for (int i = 0; i < 50; ++i) {
        batch.push_back({
            .op = EntryOp::Put,
            .key = "batch_k_" + std::to_string(i),
            .value = "batch_v_" + std::to_string(i)
        });
    }

    auto fut = async_store.commit_async(std::move(batch));
    auto commit_res = fut.get();
    REQUIRE(commit_res.has_value());

    // 3. Parallel range iteration
    std::atomic<int> visited_count{0};
    async_store.parallel_for_each([&](const auto& entry) {
        visited_count.fetch_add(1, std::memory_order_relaxed);
    });

    REQUIRE(visited_count.load() == 51); // 1 manual + 50 batch

    std::filesystem::remove_all(test_dir);
}
