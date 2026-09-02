#pragma once
// prakriti/compute/backends.hpp — convenience aggregator for all compute backends.
// Include this to bring all three backend types into scope: ScalarBackend, HighwayBackend,
// and PravahaBackend (the last guarded on PRAKRITI_ENABLE_PRAVAHA + pravaha/pravaha.hpp).
#include "scalar_backend.hpp"
#include "highway_backend.hpp"
#include "pravaha_backend.hpp"
