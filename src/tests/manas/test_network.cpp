#include "catch_amalgamated.hpp"
#include <manas/network.hpp>
#include <manas/genome.hpp>

using namespace manas;
using Catch::Approx;

TEST_CASE("Reactive topology evaluation") {
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

TEST_CASE("FeedForward topology evaluation") {
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

TEST_CASE("Mismatched layer counts throws") {
    BrainGenome genome;
    genome.layer_weights.push_back(ts::tensor<float>({2, 2}, {1.0f, 0.0f, 0.0f, 1.0f}));
    
    Network network(genome);
    ts::tensor<float> input({2}, {1.0f, 0.5f});
    REQUIRE_THROWS_AS(network(input), std::invalid_argument);
}