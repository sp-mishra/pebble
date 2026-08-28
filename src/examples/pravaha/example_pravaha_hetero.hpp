#pragma once
#ifndef SRC_EXAMPLES_EXAMPLE_PRAVAHA_HETERO_HPP
#define SRC_EXAMPLES_EXAMPLE_PRAVAHA_HETERO_HPP

// ============================================================================
// Pravaha Heterogeneous Execution EDSL — usage examples
//
// Demonstrates the full hetero pipeline: build a Lithe expr, bind an optional
// execution_context override, run through hetero_executor, verify results.
//
// Sub-examples:
//   1. SIMD small buffer  — y = x*x + x, N=512 floats  → host SIMD path
//   2. SIMD multi-op      — y = (-x) - (x/x) = -x-1    → neg/div/sub
//   3. Context override   — big buffer forced to SIMD via preferred domain
//   4. GPU big buffer     — N=256K floats, macOS only   → Metal GPU path
// ============================================================================

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "test/example_registry.hpp"
#include "utils/log.hpp"
#include "pravaha/backends/metal_gpu.hpp"

// ─── Sub-example helpers ────────────────────────────────────────────────────

namespace pravaha_hetero_ex {
    // y = x*x + x over N floats via SIMD (N < 256KB threshold).
    static testfw::Result ex1_simd_small_buffer() {
        using namespace pravaha;
        using namespace pravaha::compute;

        constexpr std::size_t N = 512;
        std::vector<float> src(N), dst(N, 0.0f);
        for (std::size_t i = 0; i < N; ++i) src[i] = static_cast<float>(i);

        auto x = lithe::make_node<lithe::call_tag>();
        auto sq = lithe::make_node<lithe::mul_tag>(x, x);
        auto expr = lithe::make_node<lithe::add_tag>(sq, x); // x*x + x

        buffer_descriptor d;
        d.shape = {N};
        d.element_type = data_element_type::f32;

        hetero::hetero_executor exec;
        hetero::execution_context ctx;
        auto r = exec.execute(expr,
                              make_view(dst.data(), d),
                              make_const_view(src.data(), d),
                              ctx);
        if (!r) return testfw::fail("ex1: SIMD execute failed");

        for (std::size_t i = 0; i < N; ++i) {
            float xi = static_cast<float>(i);
            float expected = xi * xi + xi;
            if (dst[i] < expected - 1e-4f || dst[i] > expected + 1e-4f)
                return testfw::fail("ex1: result mismatch");
        }
        lg::info("pravaha ex1 (SIMD small buffer): y=x*x+x, N={}, dst[1]={:.4f} dst[10]={:.4f} (expect 2, 110)",
                 N, dst[1], dst[10]);
        return {};
    }

    // y = (-x) - (x/x) == -x - 1, verifies neg/div/sub SIMD ops.
    static testfw::Result ex2_simd_multi_op() {
        using namespace pravaha;
        using namespace pravaha::compute;

        constexpr std::size_t N = 257; // non-power-of-two exercises scalar tail
        std::vector<float> src(N), dst(N, 0.0f);
        for (std::size_t i = 0; i < N; ++i) src[i] = static_cast<float>(i + 1);

        auto x = lithe::make_node<lithe::call_tag>();
        auto ng = lithe::make_node<lithe::neg_tag>(x);
        auto dv2 = lithe::make_node<lithe::div_tag>(x, x); // x/x == 1
        auto e = lithe::make_node<lithe::sub_tag>(ng, dv2); // -x - 1

        buffer_descriptor d;
        d.shape = {N};
        d.element_type = data_element_type::f32;

        hetero::hetero_executor exec;
        hetero::execution_context ctx;
        auto r = exec.execute(e,
                              make_view(dst.data(), d),
                              make_const_view(src.data(), d),
                              ctx);
        if (!r) return testfw::fail("ex2: SIMD execute failed");

        for (std::size_t i = 0; i < N; ++i) {
            float expected = -src[i] - 1.0f;
            if (dst[i] < expected - 1e-4f || dst[i] > expected + 1e-4f)
                return testfw::fail("ex2: neg/sub/div result mismatch");
        }
        lg::info("pravaha ex2 (SIMD multi-op): y=(-x)-(x/x), N={}, dst[0]={:.4f} dst[N-1]={:.4f} (expect -2, -258)",
                 N, dst[0], dst[N - 1]);
        return {};
    }

    // Big buffer with execution_context forcing SIMD regardless of size.
    // Tests that the overlay preferred domain overrides routing_policy.
    static testfw::Result ex3_context_override_force_simd() {
        using namespace pravaha;
        using namespace pravaha::compute;

        constexpr std::size_t N = 1 << 18; // 256K floats = 1MB → would go GPU by default
        std::vector<float> src(N), dst(N, 0.0f);
        for (std::size_t i = 0; i < N; ++i) src[i] = static_cast<float>(i) * 0.001f;

        auto x = lithe::make_node<lithe::call_tag>();
        auto expr = lithe::make_node<lithe::mul_tag>(x, x); // x*x

        buffer_descriptor d;
        d.shape = {N};
        d.element_type = data_element_type::f32;
        d.is_unified = true;

        // Force SIMD via execution_context preferred domain override.
        hetero::execution_context ctx;
        hetero::node_metadata md;
        md.preferred = hetero::compute_domain::host_simd;
        ctx.bind(hetero::structural_hash(expr), md);

        hetero::hetero_executor exec;
        auto r = exec.execute(expr,
                              make_view(dst.data(), d),
                              make_const_view(src.data(), d),
                              ctx);
        if (!r) return testfw::fail("ex3: forced-SIMD execute failed");

        for (std::size_t i : {std::size_t{0}, std::size_t{1}, N / 2, N - 1}) {
            float expected = src[i] * src[i];
            if (dst[i] < expected - 1e-5f || dst[i] > expected + 1e-5f)
                return testfw::fail("ex3: result mismatch under SIMD override");
        }
        lg::info(
            "pravaha ex3 (context override→SIMD): y=x*x, N={}, dst[1]={:.6f} dst[N/2]={:.6f} (expect 1e-6, ~{:.6f})",
            N, dst[1], dst[N / 2], src[N / 2] * src[N / 2]);
        return {};
    }

#if defined(__APPLE__) && defined(HAS_METAL_CPP)
    // y = x*x + x over 256K floats → auto-routed to Metal GPU on macOS.
    static testfw::Result ex4_gpu_big_buffer() {
        using namespace pravaha;
        using namespace pravaha::compute;

        if (!backends::metal::metal_gpu_backend::instance().available()) {
            lg::info("pravaha ex4 (Metal GPU): no device available, skipped");
            return {};
        }

        constexpr std::size_t N = 1 << 18;
        std::vector<float> src(N), dst(N, 0.0f);
        for (std::size_t i = 0; i < N; ++i) src[i] = static_cast<float>(i) * 0.001f;

        auto x = lithe::make_node<lithe::call_tag>();
        auto sq = lithe::make_node<lithe::mul_tag>(x, x);
        auto expr = lithe::make_node<lithe::add_tag>(sq, x);

        buffer_descriptor d;
        d.shape = {N};
        d.element_type = data_element_type::f32;
        d.is_unified = true;

        hetero::hetero_executor exec;
        hetero::execution_context ctx;
        auto r = exec.execute(expr,
                              make_view(dst.data(), d),
                              make_const_view(src.data(), d),
                              ctx);
        if (!r) return testfw::fail("ex4: GPU execute failed");

        float xN = src[N - 1];
        float expected = xN * xN + xN;
        if (dst[N - 1] < expected - 1e-3f || dst[N - 1] > expected + 1e-3f)
            return testfw::fail("ex4: GPU result spot-check failed");

        lg::info("pravaha ex4 (Metal GPU big buffer): y=x*x+x, N={}, dst[0]={:.6f} dst[N-1]={:.4f} (expect 0, ~{:.4f})",
                 N, dst[0], dst[N - 1], expected);
        return {};
    }
#endif
} // namespace pravaha_hetero_ex

// ============================================================================
// Registry entry
// ============================================================================

struct PravahaHeteroExample {
    static constexpr std::string_view name() { return "pravaha_hetero"; }

    static constexpr std::string_view description() {
        return "Pravaha heterogeneous execution: SIMD small/multi-op, "
            "context override, Metal GPU big buffer (macOS)";
    }

    static constexpr std::array<std::string_view, 3> tag_data{"pravaha", "hetero", "simd"};
    static constexpr std::span<const std::string_view> tags() { return tag_data; }

    static testfw::Result run() {
        if (auto r = pravaha_hetero_ex::ex1_simd_small_buffer(); !r) return r;
        if (auto r = pravaha_hetero_ex::ex2_simd_multi_op(); !r) return r;
        if (auto r = pravaha_hetero_ex::ex3_context_override_force_simd(); !r) return r;
#if defined(__APPLE__) && defined(HAS_METAL_CPP)
        if (auto r = pravaha_hetero_ex::ex4_gpu_big_buffer(); !r) return r;
#endif
        return {};
    }
};

#endif // SRC_EXAMPLES_EXAMPLE_PRAVAHA_HETERO_HPP
