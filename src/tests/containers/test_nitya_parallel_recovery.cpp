#include <catch_amalgamated.hpp>
#include <nitya/nitya.hpp>
#include <nitya/adapters/pravaha.hpp>
#include <pravaha/pravaha.hpp>
#include <filesystem>
#include <vector>
#include <string>

using namespace nitya;

TEST_CASE("Nitya: Multi-Segment Parallel Recovery", "[nitya][recovery][pravaha]") {
    const std::filesystem::path test_dir = "./test_nitya_par_recovery_db";
    std::filesystem::remove_all(test_dir);

    wal_options opts{
        .wal_dir = test_dir,
        .segment_size = 512, // Force fast segment rollover
        .auto_rotate = true
    };

    std::vector<std::string> written_payloads;

    {
        wal log(opts);
        for (int i = 0; i < 20; ++i) {
            std::string payload = "record_payload_segment_data_" + std::to_string(i);
            written_payloads.push_back(payload);
            auto lsn = log.append(std::span{reinterpret_cast<const std::byte*>(payload.data()), payload.size()});
            REQUIRE(lsn.has_value());
        }
        auto sync_res = log.sync();
        REQUIRE(sync_res.has_value());
    }

    // Reopen and run parallel recovery
    {
        wal log(opts);
        pravaha::Runner<pravaha::JThreadBackend> runner;
        std::vector<std::string> recovered_payloads;
        std::mutex mtx;

        auto recovered = nitya::pravaha_adapter::recover_async(log, runner, [&](const wal_record& rec) {
            std::string s(reinterpret_cast<const char*>(rec.payload.data()), rec.payload.size());
            std::lock_guard lock(mtx);
            recovered_payloads.push_back(s);
        });

        REQUIRE(recovered.has_value());
        REQUIRE(recovered->error == LogError::Success);
        REQUIRE(recovered->records_recovered == 20);
        REQUIRE(recovered_payloads.size() == 20);

        for (std::size_t i = 0; i < written_payloads.size(); ++i) {
            REQUIRE(recovered_payloads[i] == written_payloads[i]);
        }
    }

    std::filesystem::remove_all(test_dir);
}
