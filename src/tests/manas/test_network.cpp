#include "catch_amalgamated.hpp"
#include <manas/network.hpp>
#include <manas/genome.hpp>
#include <manas/activation.hpp>
#include <manas/evolution.hpp>
#include <manas/selection.hpp>
#include <manas/mutation.hpp>
#include <manas/crossover.hpp>
#include <manas/serialization.hpp>
#include <manas/archive.hpp>

using namespace manas;
using Catch::Approx;

TEST_CASE (
"Reactive topology evaluation"
)
 {
    BrainGenome genome;
    genome.topology_type = TopologyType::Reactive;

    genome.layer_weights.push_back(ts::tensor<float>({2, 2}, {1.0f, 0.0f, 0.0f, 1.0f}));
    genome.layer_biases.push_back(ts::tensor<float>({2}, {0.1f, -0.1f}));

    Network network(genome);
    ts::tensor<float> input({2}, {1.0f, 0.5f});
    auto output = network(input);

    REQUIRE(output.shape().size() == 1);
    REQUIRE(output.shape()[0] == 2);
    REQUIRE(output[0] == Approx(1.1f));
    REQUIRE(output[1] == Approx(0.4f));
}

TEST_CASE (
"FeedForward topology evaluation"
)
 {
    BrainGenome genome;
    genome.topology_type = TopologyType::FeedForward;

    genome.layer_weights.push_back(ts::tensor<float>({3, 2}, {0.5f, -0.5f, 0.2f, 0.8f, -0.3f, 0.7f}));
    genome.layer_biases.push_back(ts::tensor<float>({3}, {0.1f, -0.1f, 0.2f}));

    genome.layer_weights.push_back(ts::tensor<float>({2, 3}, {0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f}));
    genome.layer_biases.push_back(ts::tensor<float>({2}, {-0.2f, 0.3f}));

    Network network(genome);
    ts::tensor<float> input({2}, {1.0f, 0.5f});
    auto output = network(input);

    REQUIRE(output.shape().size() == 1);
    REQUIRE(output.shape()[0] == 2);
    // Layer1 = W1*input + b1 = [0.35, 0.5, 0.25]
    // Layer2 = W2*Layer1 + b2 = [-0.19, 0.26]
    REQUIRE(output[0] == Approx(-0.19f).epsilon(0.001f));
    REQUIRE(output[1] == Approx(0.26f).epsilon(0.001f));
}

TEST_CASE (
"Mismatched layer counts throws"
)
 {
    BrainGenome genome;
    genome.layer_weights.push_back(ts::tensor<float>({2, 2}, {1.0f, 0.0f, 0.0f, 1.0f}));

    Network network(genome);
    ts::tensor<float> input({2}, {1.0f, 0.5f});
    REQUIRE_THROWS_AS(network(input), std::invalid_argument);
}

TEST_CASE (
"Network with ReLU and Sigmoid activations"
)
 {
    BrainGenome genome;
    genome.topology_type = TopologyType::FeedForward;

    // Layer 1: output = ReLU(W*x + b)
    // W = [[1, -1]], b = [-0.2] => input=[1.0, 0.5] => (1*1 - 1*0.5) - 0.2 = 0.3 => ReLU(0.3) = 0.3
    genome.layer_weights.push_back(ts::tensor<float>({1, 2}, {1.0f, -1.0f}));
    genome.layer_biases.push_back(ts::tensor<float>({1}, {-0.2f}));
    genome.layer_activations.push_back(ActivationType::ReLU);

    Network network(genome);
    ts::tensor<float> input({2}, {1.0f, 0.5f});
    auto output = network(input);

    REQUIRE(output.size() == 1);
    REQUIRE(output[0] == Approx(0.3f));
}

TEST_CASE (
"EvolutionaryProcess runs generations and tracks best genome"
)
 {
    EvolutionaryProcess process(
        TournamentSelection{.tournament_size = 2, .num_parents = 2},
        GaussianJitterMutation{.rate = 0.5f, .sigma = 0.1f},
        UniformCrossover{.bias = 0.5f}
    );

    for (int i = 0; i < 6; ++i) {
        BrainGenome g;
        g.layer_weights.push_back(ts::tensor<float>({1, 1}, {static_cast<float>(i)}));
        g.layer_biases.push_back(ts::tensor<float>({1}, {0.0f}));
        process.add_genome(g);
    }

    auto fitness_fn = [](const BrainGenome& g) -> FitnessMetrics {
        float val = g.layer_weights[0].data()[0];
        // Target: val = 10.0f, higher score when closer to 10
        return FitnessMetrics{ .score = -std::abs(10.0f - val), .complexity = 1 };
    };

    REQUIRE_NOTHROW(process.run_generation(fitness_fn, 2));
    REQUIRE(process.population().size() == 6);
}

TEST_CASE (
"GenomeSerializer round-trips correctly"
)
 {
    BrainGenome original;
    original.topology_type = TopologyType::FeedForward;
    original.generation = 42;
    original.parent_id = BrainId{.value = 0xCAFE};
    original.layer_weights.push_back(ts::tensor<float>({2, 2}, {1.0f, 2.0f, 3.0f, 4.0f}));
    original.layer_biases.push_back(ts::tensor<float>({2}, {0.5f, -0.5f}));
    original.layer_activations.push_back(ActivationType::Tanh);

    auto bytes = GenomeSerializer<BinaryEncoding>::serialize(original);
    REQUIRE(!bytes.empty());

    BrainGenome restored = GenomeSerializer<BinaryEncoding>::deserialize(bytes);
    REQUIRE(restored.topology_type == TopologyType::FeedForward);
    REQUIRE(restored.generation == 42);
    REQUIRE(restored.parent_id == BrainId{.value = 0xCAFE});
    REQUIRE(restored.layer_weights.size() == 1);
    REQUIRE(restored.layer_weights[0].data()[0] == Approx(1.0f));
    REQUIRE(restored.layer_weights[0].data()[3] == Approx(4.0f));
    REQUIRE(restored.layer_biases[0].data()[0] == Approx(0.5f));
    REQUIRE(restored.layer_activations[0] == ActivationType::Tanh);
}