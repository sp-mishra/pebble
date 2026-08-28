#pragma once
#include "brain.hpp"
#include "genome.hpp"
#include "serialization.hpp"
#include <petika/petika.hpp>
#include <optional>
#include <string>

namespace manas {

// Petika-backed Genome Archive supporting crash-durable, versioned genome persistence
template<typename PetikaStore = petika::StringSkipStore>
class GenomeArchive {
public:
    explicit GenomeArchive(petika::PetikaOptions opts = {})
        : store_{std::move(opts)} {}

    // Save a genome under its BrainId (and optional generation tag)
    petika::Result<void> save(const BrainGenome& genome) {
        std::string key = make_key(genome.parent_id, genome.generation);
        std::vector<uint8_t> bytes = GenomeSerializer<BinaryEncoding>::serialize(genome);
        std::string val(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return store_.put(std::move(key), std::move(val));
    }

    // Save with explicit custom string key / ID
    petika::Result<void> save_as(const std::string& key, const BrainGenome& genome) {
        std::vector<uint8_t> bytes = GenomeSerializer<BinaryEncoding>::serialize(genome);
        std::string val(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return store_.put(key, std::move(val));
    }

    // Load a genome by key
    petika::Result<BrainGenome> load(const std::string& key) {
        auto res = store_.get(key);
        if (!res) return std::unexpected(res.error());
        
        std::span<const uint8_t> span(reinterpret_cast<const uint8_t*>(res->data()), res->size());
        try {
            return GenomeSerializer<BinaryEncoding>::deserialize(span);
        } catch (const std::exception&) {
            return std::unexpected(petika::StorageError::CorruptedRecord);
        }
    }

    // Load by BrainId and generation
    petika::Result<BrainGenome> load(BrainId id, uint64_t generation = 0) {
        return load(make_key(id, generation));
    }

    PetikaStore& store() noexcept { return store_; }
    const PetikaStore& store() const noexcept { return store_; }

private:
    static std::string make_key(BrainId id, uint64_t generation) {
        return "genome:" + std::to_string(id.value) + ":gen:" + std::to_string(generation);
    }

    PetikaStore store_;
};

} // namespace manas