#include "catch_amalgamated.hpp"
#include <manas/brain.hpp>

TEST_CASE (
"BrainId operations"
,
"[manas][brain]"
)
 {
    manas::BrainId id1{.value = 12345};
    manas::BrainId id2{.value = 12345};
    manas::BrainId id3{.value = 67890};

    REQUIRE(id1 == id2);
    REQUIRE(id1 != id3);
}

TEST_CASE (
"NeuronIndex operations"
,
"[manas][brain]"
)
 {
    manas::NeuronIndex idx1{.value = 42};
    manas::NeuronIndex idx2{.value = 42};
    manas::NeuronIndex idx3{.value = 99};
    
    REQUIRE(idx1 == idx2);
    REQUIRE(idx1 != idx3);
}