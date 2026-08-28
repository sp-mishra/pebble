#pragma once
// ============================================================================
// prakriti/celestial/sector_cache_manager.hpp — Two-Tier Kosha RAM + Glaze/Petika Cache
// ============================================================================
// Zero-virtual, header-only C++23 sector paging & persistence engine.
// ============================================================================

#include "sector_types.hpp"
#include "sector_generator.hpp"
#include "sector_multipole.hpp"
#include "petika/async_persistence_worker.hpp"
#include "containers/cache/kosha.hpp"
#include "glaze/glaze.hpp"
#include <unordered_map>
#include <vector>
#include <string>
#include <optional>
#include <filesystem>
#include <fstream>

namespace prakriti::celestial {

class SectorCacheManager {
public:
    explicit SectorCacheManager(std::size_t max_ram_sectors = 128)
        : ram_cache_(max_ram_sectors), async_persister_() {}

    // Retrieves sector from RAM cache, persistent Glaze disk store, or procedurally nucleates
    [[nodiscard]] SectorData get_or_generate_sector(SectorKey key, std::uint64_t cosmic_seed = 13371337ULL) {
        const std::uint64_t hid = hash_sector_key(key);

        // 1. Check Tier-1 Kosha RAM LRU Cache
        if (auto cached = ram_cache_.get(hid)) {
            return *cached;
        }

        // 2. Check Tier-2 Glaze Persistent Disk Cache (if exists on disk)
        std::string filename = get_sector_filepath(key);
        if (std::filesystem::exists(filename)) {
            std::string buffer;
            if (std::ifstream in(filename, std::ios::in | std::ios::binary); in.is_open()) {
                in.seekg(0, std::ios::end);
                buffer.resize(static_cast<std::size_t>(in.tellg()));
                in.seekg(0, std::ios::beg);
                in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));

                SectorData loaded_sector;
                if (!glz::read_json(loaded_sector, buffer)) {
                    (void)ram_cache_.put(hid, loaded_sector);
                    discovered_sectors_[hid] = loaded_sector;
                    return loaded_sector;
                }
            }
        }

        // 3. Procedural Discovery: Nucleate new celestial sector
        SectorData new_sector = generate_procedural_sector(key, cosmic_seed);
        (void)ram_cache_.put(hid, new_sector);
        discovered_sectors_[hid] = new_sector;
        return new_sector;
    }

    // Puts active evolved sector into Kosha RAM cache, registers dormant macro-node, and enqueues async persistence
    void freeze_sector(const SectorData& sector) {
        const std::uint64_t hid = hash_sector_key(sector.key);
        (void)ram_cache_.put(hid, sector);
        discovered_sectors_[hid] = sector;

        // Register as a dormant macro node with total mass & barycenter center of mass
        if (sector.total_mass > 0.0f) {
            dormant_macro_nodes_[hid] = SectorMacroNode{
                .key = sector.key,
                .total_mass = sector.total_mass,
                .bx = sector.barycenter_x,
                .by = sector.barycenter_y,
                .q = sector.quadrupole
            };
        } else {
            dormant_macro_nodes_.erase(hid);
        }

        // Non-blocking async persistence via lock-free SPSC ring buffer worker (< 1 µs)
        async_persister_.enqueue(sector, get_sector_filepath(sector.key));
    }

    // Wakes up a sector when entering active simulation window (unregisters from dormant macro nodes)
    void mark_sector_active(SectorKey key) {
        const std::uint64_t hid = hash_sector_key(key);
        dormant_macro_nodes_.erase(hid);
    }

    // Returns all active dormant macro nodes for collective gravitational pull
    [[nodiscard]] const std::unordered_map<std::uint64_t, SectorMacroNode>& dormant_macro_nodes() const noexcept {
        return dormant_macro_nodes_;
    }

    // Returns all known/discovered sector barycenters
    [[nodiscard]] const std::unordered_map<std::uint64_t, SectorData>& discovered_sectors() const noexcept {
        return discovered_sectors_;
    }

    [[nodiscard]] std::size_t cached_count() const noexcept {
        return discovered_sectors_.size();
    }

private:
    [[nodiscard]] static std::string get_sector_filepath(SectorKey k) {
        return "./pebble_universe_data/sectors/sec_" + std::to_string(k.x) + "_" + std::to_string(k.y) + ".json";
    }

    kosha::LRUCache<std::uint64_t, SectorData> ram_cache_;
    std::unordered_map<std::uint64_t, SectorData> discovered_sectors_;
    std::unordered_map<std::uint64_t, SectorMacroNode> dormant_macro_nodes_;
    petika::AsyncPersistenceWorker<SectorData> async_persister_;
};

} // namespace prakriti::celestial
