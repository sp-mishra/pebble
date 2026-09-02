#pragma once
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>
#include "activation.hpp"
#include "genome.hpp"

namespace manas {
    struct BinaryEncoding {};

    template <typename EncodingPolicy = BinaryEncoding>
    struct GenomeSerializer {
        // Magic: "MNGB" (Manas Genome Binary), Version: 1
        static constexpr uint32_t kMagic = 0x42474E4D;
        static constexpr uint32_t kVersion = 1;

        static std::vector<uint8_t> serialize(const BrainGenome& genome) {
            std::vector<uint8_t> buffer;

            auto write_val = [&buffer]<typename T>(const T& val) {
                const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&val);
                buffer.insert(buffer.end(), ptr, ptr + sizeof(T));
            };

            // Header
            write_val(kMagic);
            write_val(kVersion);
            write_val(static_cast<uint32_t>(genome.topology_type));
            write_val(genome.generation);
            write_val(genome.parent_id.value);

            // Layer count
            const uint32_t num_layers = static_cast<uint32_t>(genome.layer_weights.size());
            write_val(num_layers);

            // Layers data: weights, biases, activations
            for (size_t l = 0; l < num_layers; ++l) {
                const auto& w = genome.layer_weights[l];
                const auto& b = genome.layer_biases[l];

                // Weight shape
                const auto& w_shape = w.shape();
                write_val(static_cast<uint32_t>(w_shape.size()));
                for (size_t d : w_shape) {
                    write_val(static_cast<uint64_t>(d));
                }
                // Weight data
                const uint32_t w_elems = static_cast<uint32_t>(w.size());
                write_val(w_elems);
                const uint8_t* w_ptr = reinterpret_cast<const uint8_t*>(w.data());
                buffer.insert(buffer.end(), w_ptr, w_ptr + w_elems * sizeof(float));

                // Bias shape
                const auto& b_shape = b.shape();
                write_val(static_cast<uint32_t>(b_shape.size()));
                for (size_t d : b_shape) {
                    write_val(static_cast<uint64_t>(d));
                }
                // Bias data
                const uint32_t b_elems = static_cast<uint32_t>(b.size());
                write_val(b_elems);
                const uint8_t* b_ptr = reinterpret_cast<const uint8_t*>(b.data());
                buffer.insert(buffer.end(), b_ptr, b_ptr + b_elems * sizeof(float));

                // Activation
                ActivationType act = (l < genome.layer_activations.size())
                                         ? genome.layer_activations[l]
                                         : ActivationType::Identity;
                write_val(static_cast<uint32_t>(act));
            }

            return buffer;
        }

        static BrainGenome deserialize(std::span<const uint8_t> data) {
            if (data.size() < sizeof(uint32_t) * 5 + sizeof(uint64_t) * 2) {
                throw std::runtime_error("GenomeSerializer: payload too small");
            }

            size_t offset = 0;
            auto read_val = [&data, &offset]<typename T>() -> T {
                if (offset + sizeof(T) > data.size()) {
                    throw std::runtime_error("GenomeSerializer: unexpected end of payload");
                }
                T val;
                std::memcpy(&val, data.data() + offset, sizeof(T));
                offset += sizeof(T);
                return val;
            };

            const uint32_t magic = read_val.template operator()<uint32_t>();
            if (magic != kMagic) {
                throw std::runtime_error("GenomeSerializer: invalid magic header");
            }
            const uint32_t version = read_val.template operator()<uint32_t>();
            if (version != kVersion) {
                throw std::runtime_error("GenomeSerializer: unsupported version");
            }

            BrainGenome genome;
            genome.topology_type = static_cast<TopologyType>(read_val.template operator()<uint32_t>());
            genome.generation = read_val.template operator()<uint64_t>();
            genome.parent_id = BrainId{.value = read_val.template operator()<uint64_t>()};

            const uint32_t num_layers = read_val.template operator()<uint32_t>();
            for (uint32_t l = 0; l < num_layers; ++l) {
                // Read weights shape & data
                const uint32_t w_rank = read_val.template operator()<uint32_t>();
                ts::TensorShape w_shape;
                for (uint32_t r = 0; r < w_rank; ++r) {
                    w_shape.push_back(static_cast<size_t>(read_val.template operator()<uint64_t>()));
                }
                const uint32_t w_elems = read_val.template operator()<uint32_t>();
                std::vector<float> w_data(w_elems);
                for (uint32_t i = 0; i < w_elems; ++i) {
                    w_data[i] = read_val.template operator()<float>();
                }
                genome.layer_weights.emplace_back(w_shape, w_data);

                // Read biases shape & data
                const uint32_t b_rank = read_val.template operator()<uint32_t>();
                ts::TensorShape b_shape;
                for (uint32_t r = 0; r < b_rank; ++r) {
                    b_shape.push_back(static_cast<size_t>(read_val.template operator()<uint64_t>()));
                }
                const uint32_t b_elems = read_val.template operator()<uint32_t>();
                std::vector<float> b_data(b_elems);
                for (uint32_t i = 0; i < b_elems; ++i) {
                    b_data[i] = read_val.template operator()<float>();
                }
                genome.layer_biases.emplace_back(b_shape, b_data);

                // Read activation
                const auto act = static_cast<ActivationType>(read_val.template operator()<uint32_t>());
                genome.layer_activations.push_back(act);
            }

            return genome;
        }
    };
} // namespace manas