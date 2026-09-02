#pragma once

// akruti/layout.hpp — Header-only 2D layout engine for Akruti
// - Authoring: NAryTree payload (LayoutNode)
// - Execution: Baked flat Structure-of-Arrays (SoA) execution buffers
// - High-performance C++23 zero-overhead design: zero virtual functions, zero macros
// - Subsystems: Flexbox main/cross sizing, padding/margins, aspect ratio lock,
//   constraint DAG graph, spatial hashing, phase-aware dirty tracking, text measurement.

#include <cstdint>
#include <cstddef>
#include <ranges>
#include <vector>
#include <span>
#include <deque>
#include <limits>
#include <algorithm>
#include <cassert>
#include <optional>
#include <unordered_set>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <mutex>
#include <fstream>
#include <cmath>
#include <string>
#include <cstdio>

#include "akruti/math.hpp"
#include "akruti/simd.hpp"
#include "containers/tree/NAryTree.hpp"
#include "containers/graph/LiteGraph.hpp"
#include "containers/dynamic/SmallVector.hpp"
#include "mem/smriti.hpp"
#include "mem/arena.hpp"
#include "pravaha/pravaha.hpp"

namespace akruti::layout {
    using Vec2 = akruti::Vec;
    using Bounds2D = akruti::AABB<float>;

    // Basic 2D layout types
    struct Size2D {
        float w = 0.0f;
        float h = 0.0f;

        friend constexpr bool operator==(const Size2D&, const Size2D&) = default;
    };

    struct Rect2D {
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;

        friend constexpr bool operator==(const Rect2D&, const Rect2D&) = default;
    };

    struct Edges {
        float l = 0.0f;
        float t = 0.0f;
        float r = 0.0f;
        float b = 0.0f;

        friend constexpr bool operator==(const Edges&, const Edges&) = default;
    };

    // Performance measurement utilities with granular work counters
    struct PerformanceStats {
        std::chrono::nanoseconds measure_time{0};
        std::chrono::nanoseconds place_time{0};
        std::chrono::nanoseconds clip_time{0};
        std::chrono::nanoseconds constraints_time{0};
        std::chrono::nanoseconds total_time{0};

        std::size_t node_count = 0;
        std::size_t dirty_node_count = 0;

        std::size_t nodes_measured = 0;
        std::size_t nodes_placed = 0;
        std::size_t constraints_applied = 0;
        std::size_t clips_computed = 0;
        std::size_t text_measure_calls = 0;
        std::size_t spatial_hash_updates = 0;

        void reset() {
            measure_time = std::chrono::nanoseconds{0};
            place_time = std::chrono::nanoseconds{0};
            clip_time = std::chrono::nanoseconds{0};
            constraints_time = std::chrono::nanoseconds{0};
            total_time = std::chrono::nanoseconds{0};
            node_count = 0;
            dirty_node_count = 0;
            nodes_measured = 0;
            nodes_placed = 0;
            constraints_applied = 0;
            clips_computed = 0;
            text_measure_calls = 0;
            spatial_hash_updates = 0;
        }
    };

    // Spatial hash for fast 2D spatial hit testing
    class SpatialHash {
    public:
        struct CellKey {
            int32_t x, y;

            bool operator==(const CellKey& other) const {
                return x == other.x && y == other.y;
            }
        };

        struct CellKeyHash {
            std::size_t operator()(const CellKey& k) const {
                return std::hash<int32_t>()(k.x) ^ (std::hash<int32_t>()(k.y) << 1);
            }
        };

    private:
        float cell_size_;
        std::unordered_map<CellKey, std::vector<std::uint32_t>, CellKeyHash> grid_;

    public:
        explicit SpatialHash(float cell_size = 100.0f) : cell_size_(cell_size) {}

        void clear() { grid_.clear(); }

        void insert(std::uint32_t node_id, const Rect2D& rect) {
            const CellKey min_cell = get_cell(rect.x, rect.y);
            const CellKey max_cell = get_cell(rect.x + rect.w, rect.y + rect.h);

            for (int32_t cy = min_cell.y; cy <= max_cell.y; ++cy) {
                for (int32_t cx = min_cell.x; cx <= max_cell.x; ++cx) {
                    grid_[CellKey{cx, cy}].push_back(node_id);
                }
            }
        }

        [[nodiscard]] std::vector<std::uint32_t> query(float x, float y) const {
            const CellKey cell = get_cell(x, y);
            auto it = grid_.find(cell);
            return (it != grid_.end()) ? it->second : std::vector<std::uint32_t>{};
        }

        [[nodiscard]] containers::dynamic::SmallVector<std::uint32_t, 64 * sizeof(std::uint32_t)> query_rect(const Rect2D& rect) const {
            containers::dynamic::SmallVector<std::uint32_t, 64 * sizeof(std::uint32_t)> result;
            const CellKey min_cell = get_cell(rect.x, rect.y);
            const CellKey max_cell = get_cell(rect.x + rect.w, rect.y + rect.h);

            for (int32_t cy = min_cell.y; cy <= max_cell.y; ++cy) {
                for (int32_t cx = min_cell.x; cx <= max_cell.x; ++cx) {
                    auto it = grid_.find(CellKey{cx, cy});
                    if (it != grid_.end()) {
                        result.insert(result.end(), it->second.begin(), it->second.end());
                    }
                }
            }

            std::sort(result.begin(), result.end());
            result.erase(std::unique(result.begin(), result.end()), result.end());
            return result;
        }

    private:
        [[nodiscard]] CellKey get_cell(float x, float y) const {
            return CellKey{
                static_cast<int32_t>(std::floor(x / cell_size_)),
                static_cast<int32_t>(std::floor(y / cell_size_))
            };
        }
    };

    enum class Axis : std::uint8_t { Row = 0, Column = 1 };

    enum class Align : std::uint8_t { Start = 0, Center = 1, End = 2, Stretch = 3 };

    enum class Justify : std::uint8_t { Start = 0, Center = 1, End = 2, SpaceBetween = 3, SpaceAround = 4 };

    enum class Overflow : std::uint8_t {
        Visible = 0, // Children overflow parent bounds (default)
        Clip = 1, // Children clipped to parent bounds
        Scroll = 2 // Children clipped and scrollable
    };

    // Text measurement callback interface
    struct TextMeasure {
        Size2D (*measure)(
            const char* text,
            float max_width,
            void* user_data
        );
        void* user_data;
    };

    // Concept for any object able to measure a text run. Lets callers pass a typed
    // metrics object instead of a C function pointer; adapt via make_text_measure.
    template <class T>
    concept ITextMetrics = requires(const T& t, const char* s, float mw) {
        { t.measure(s, mw) } -> std::convertible_to<Size2D>;
    };

    // Build a TextMeasure trampoline capturing &metrics as user_data. The metrics object
    // must outlive the returned TextMeasure. Zero allocation.
    template <ITextMetrics T>
    TextMeasure make_text_measure(T& metrics) noexcept {
        return TextMeasure{
            +[](const char* text, float max_width, void* ud) -> Size2D {
                return static_cast<T*>(ud)->measure(text, max_width);
            },
            static_cast<void*>(&metrics)
        };
    }

    struct SizeSpec {
        // 0..2 preserve original numeric values & behavior (byte-identical when new
        // units are unused). Fr/Content/Aspect are additive.
        enum class Kind : std::uint8_t {
            Auto = 0,
            Px = 1,
            Percent = 2,
            Fr = 3, // fractional free-space share; value = weight
            Content = 4, // intrinsic content clamped to [value, aux0]
            Aspect = 5 // derive this axis from the resolved other axis; value = ratio
        };

        Kind kind = Kind::Auto;
        float value = 0.0f;
        // Auxiliary parameters used only by additive units (default 0 keeps size lean
        // for the common Auto/Px/Percent path). Content: aux0 = max (0 => +inf).
        float aux0 = 0.0f;
        float aux1 = 0.0f;

        static SizeSpec Auto() noexcept { return SizeSpec{Kind::Auto, 0.0f}; }
        static SizeSpec Px(const float v) noexcept { return SizeSpec{Kind::Px, v}; }
        static SizeSpec Percent(const float v) noexcept { return SizeSpec{Kind::Percent, v}; }

        // value = fractional weight (like flex_grow); translated to flex_grow at bake.
        static SizeSpec Fr(const float weight) noexcept { return SizeSpec{Kind::Fr, weight}; }
        // Intrinsic content size clamped to [min_px, max_px]; max_px <= 0 => unbounded.
        static SizeSpec Content(const float min_px = 0.0f, const float max_px = 0.0f) noexcept {
            return SizeSpec{Kind::Content, min_px, max_px, 0.0f};
        }

        // Derive this axis from the resolved cross axis using ratio = this / other.
        static SizeSpec Aspect(const float ratio) noexcept { return SizeSpec{Kind::Aspect, ratio}; }

        [[nodiscard]] bool is_fr() const noexcept { return kind == Kind::Fr; }
        [[nodiscard]] bool is_content() const noexcept { return kind == Kind::Content; }
        [[nodiscard]] bool is_aspect() const noexcept { return kind == Kind::Aspect; }

        friend constexpr bool operator==(const SizeSpec&, const SizeSpec&) = default;
    };

    // Optional min/pref/max triple for a single axis. Each field is a full SizeSpec so
    // callers can mix units (e.g. min=Px(120), pref=Fr(1), max=Percent(50)). Kept out of
    // SizeSpec itself to keep the SoA columns lean; lives on LayoutStyle as an opt-in.
    struct SizeSpecClamp {
        SizeSpec min = SizeSpec::Auto();
        SizeSpec pref = SizeSpec::Auto();
        SizeSpec max = SizeSpec::Auto();

        [[nodiscard]] bool active() const noexcept {
            return min.kind != SizeSpec::Kind::Auto || pref.kind != SizeSpec::Kind::Auto ||
                max.kind != SizeSpec::Kind::Auto;
        }

        friend constexpr bool operator==(const SizeSpecClamp&, const SizeSpecClamp&) = default;
    };

    struct Constraint {
        enum class Kind : std::uint8_t {
            AnchorLeft = 0,
            AnchorRight = 1,
            AnchorTop = 2,
            AnchorBottom = 3,
            MatchParentWidth = 4,
            MatchParentHeight = 5,
            CenterX = 6,
            CenterY = 7
        };

        Kind kind = Kind::AnchorLeft;
        std::uint32_t target = 0; // node ID receiving constraint
        std::uint32_t source = 0; // node ID constraint refers to (or parent/invalid)
        float offset = 0.0f;

        friend constexpr bool operator==(const Constraint&, const Constraint&) = default;
    };

    struct LayoutStyle {
        Axis axis = Axis::Column;
        Align align_items = Align::Start;
        Justify justify_content = Justify::Start;

        SizeSpec width = SizeSpec::Auto();
        SizeSpec height = SizeSpec::Auto();

        // Min/Max constraints
        SizeSpec min_width = SizeSpec::Auto();
        SizeSpec min_height = SizeSpec::Auto();
        SizeSpec max_width = SizeSpec::Auto();
        SizeSpec max_height = SizeSpec::Auto();

        // Optional min/pref/max triples. When active(), they override the scalar
        // min/max above for that axis and drive the resolve in place_pass. Inactive
        // by default => existing min/max behavior is untouched.
        SizeSpecClamp width_clamp{};
        SizeSpecClamp height_clamp{};

        // Aspect ratio locking
        float aspect_ratio = 0.0f; // width / height
        bool aspect_lock = false;

        // Flex properties
        float flex_grow = 0.0f;
        float flex_shrink = 1.0f;
        SizeSpec flex_basis = SizeSpec::Auto();

        Edges padding{};
        Edges margin{};

        // Constraint properties
        std::uint8_t constraint_mask = 0;
        float anchor_left = 0.0f;
        float anchor_right = 0.0f;
        float anchor_top = 0.0f;
        float anchor_bottom = 0.0f;
        bool match_parent_width = false;
        bool match_parent_height = false;
        bool center_x = false;
        bool center_y = false;

        // Virtualization properties
        bool virtualization_enabled = false;
        Vec2 scroll_offset{0.0f, 0.0f};
        Size2D viewport_size{0.0f, 0.0f};

        // Overflow handling
        Overflow overflow_x = Overflow::Visible;
        Overflow overflow_y = Overflow::Visible;
        float scroll_offset_x = 0.0f;
        float scroll_offset_y = 0.0f;

        // Text measurement
        const char* text = nullptr;
        std::uint32_t text_id = 0;

        friend constexpr bool operator==(const LayoutStyle&, const LayoutStyle&) = default;
    };

    // Payload for NAryTree
    struct LayoutNode {
        LayoutStyle style{};
        std::uint64_t user_tag = 0;

        friend constexpr bool operator==(const LayoutNode&, const LayoutNode&) = default;
    };

    using LayoutTree = ::NAryTree<LayoutNode>;

    // Parent-relative edge for docking helpers.
    enum class Side : std::uint8_t { Left = 0, Right = 1, Top = 2, Bottom = 3, Fill = 4 };

    // Pin a node to a parent edge at `inset` px (parent-relative). Fill stretches to all
    // four edges. Builds on the anchor_* constraints applied in constraints_pass — no
    // sibling DAG required. Returns the mutated style for chaining.
    inline LayoutStyle& dock(LayoutStyle& st, const Side side, const float inset = 0.0f) noexcept {
        switch (side) {
        case Side::Left: st.anchor_left = inset;
            break;
        case Side::Right: st.anchor_right = inset;
            break;
        case Side::Top: st.anchor_top = inset;
            break;
        case Side::Bottom: st.anchor_bottom = inset;
            break;
        case Side::Fill:
            st.anchor_left = inset;
            st.anchor_right = inset;
            st.anchor_top = inset;
            st.anchor_bottom = inset;
            break;
        }
        return st;
    }

    // Give a child a proportional share of the parent's main axis. Two children with
    // split(1)/split(2) divide free space 1:2. Implemented via the Fr unit (translated to
    // flex_grow at bake), so no solver change is needed.
    inline LayoutStyle& split_share(LayoutStyle& st, const float weight, const Axis parent_axis) noexcept {
        if (parent_axis == Axis::Row) st.width = SizeSpec::Fr(weight);
        else st.height = SizeSpec::Fr(weight);
        return st;
    }

    inline constexpr std::uint32_t kInvalid = std::numeric_limits<std::uint32_t>::max();

    static float clamp_min(const float v, const float lo) noexcept { return (v < lo) ? lo : v; }
    static float clamp_max(const float v, const float hi) noexcept { return (v > hi) ? hi : v; }

    static float rect_right(const Rect2D& r) noexcept { return r.x + r.w; }
    static float rect_bottom(const Rect2D& r) noexcept { return r.y + r.h; }

    static Bounds2D rect_to_bounds(const Rect2D& r) noexcept {
        return Bounds2D(Vec2{r.x, r.y}, Vec2{r.x + r.w, r.y + r.h});
    }

    static Rect2D bounds_to_rect(const Bounds2D& b) noexcept {
        Rect2D r;
        r.x = b.lo[0];
        r.y = b.lo[1];
        r.w = b.hi[0] - b.lo[0];
        r.h = b.hi[1] - b.lo[1];
        return r;
    }

    static Bounds2D intersect_bounds(const Bounds2D& a, const Bounds2D& b) noexcept {
        float lo_x = std::max(a.lo[0], b.lo[0]);
        float lo_y = std::max(a.lo[1], b.lo[1]);
        float hi_x = std::min(a.hi[0], b.hi[0]);
        float hi_y = std::min(a.hi[1], b.hi[1]);
        if (hi_x < lo_x) { hi_x = lo_x; }
        if (hi_y < lo_y) { hi_y = lo_y; }
        return Bounds2D(Vec2{lo_x, lo_y}, Vec2{hi_x, hi_y});
    }

    static Rect2D inset_rect(const Rect2D& r, const Edges& e) noexcept {
        Rect2D o = r;
        o.x += e.l;
        o.y += e.t;
        o.w = std::max(0.0f, o.w - (e.l + e.r));
        o.h = std::max(0.0f, o.h - (e.t + e.b));
        return o;
    }

    static float main_size(const Rect2D& r, const Axis axis) noexcept {
        return (axis == Axis::Row) ? r.w : r.h;
    }

    static float cross_size(const Rect2D& r, const Axis axis) noexcept {
        return (axis == Axis::Row) ? r.h : r.w;
    }

    static float main_size(const Size2D& s, const Axis axis) noexcept {
        return (axis == Axis::Row) ? s.w : s.h;
    }

    static float cross_size(const Size2D& s, const Axis axis) noexcept {
        return (axis == Axis::Row) ? s.h : s.w;
    }

    static void set_main_size(Size2D& s, const Axis axis, const float v) noexcept {
        if (axis == Axis::Row) s.w = v;
        else s.h = v;
    }

    static void set_cross_size(Size2D& s, const Axis axis, const float v) noexcept {
        if (axis == Axis::Row) s.h = v;
        else s.w = v;
    }

    static void set_main_pos(Rect2D& r, const Axis axis, const float v) noexcept {
        if (axis == Axis::Row) r.x = v;
        else r.y = v;
    }

    static void set_cross_pos(Rect2D& r, const Axis axis, const float v) noexcept {
        if (axis == Axis::Row) r.y = v;
        else r.x = v;
    }

    static void set_main_len(Rect2D& r, const Axis axis, const float v) noexcept {
        if (axis == Axis::Row) r.w = v;
        else r.h = v;
    }

    static void set_cross_len(Rect2D& r, const Axis axis, const float v) noexcept {
        if (axis == Axis::Row) r.h = v;
        else r.w = v;
    }

    static float main_margin_before(const Edges& m, const Axis axis) noexcept {
        return (axis == Axis::Row) ? m.l : m.t;
    }

    static float main_margin_after(const Edges& m, const Axis axis) noexcept {
        return (axis == Axis::Row) ? m.r : m.b;
    }

    static inline float cross_margin_before(const Edges& m, const Axis axis) noexcept {
        return (axis == Axis::Row) ? m.t : m.l;
    }

    static inline float cross_margin_after(const Edges& m, const Axis axis) noexcept {
        return (axis == Axis::Row) ? m.b : m.r;
    }

    static float resolve_spec_px_or_inf(const SizeSpec& s, const bool is_max) noexcept {
        if (s.kind == SizeSpec::Kind::Px) return std::max(0.0f, s.value);
        return is_max ? std::numeric_limits<float>::infinity() : 0.0f;
    }

    static float clamp_range(const float v, const float lo, const float hi) noexcept {
        float r = v;
        if (r < lo) r = lo;
        if (r > hi) r = hi;
        return r;
    }

    static Size2D apply_min_max_content(Size2D content, const LayoutStyle& st) noexcept {
        const float min_w = resolve_spec_px_or_inf(st.min_width, false);
        const float max_w = resolve_spec_px_or_inf(st.max_width, true);
        const float min_h = resolve_spec_px_or_inf(st.min_height, false);
        const float max_h = resolve_spec_px_or_inf(st.max_height, true);
        content.w = clamp_range(content.w, min_w, max_w);
        content.h = clamp_range(content.h, min_h, max_h);
        return content;
    }

    static Size2D apply_aspect_content(Size2D content, const LayoutStyle& st) noexcept {
        if (!st.aspect_lock) return content;
        if (!(st.aspect_ratio > 0.0f)) return content;

        const bool width_fixed = (st.width.kind == SizeSpec::Kind::Px);
        const bool height_fixed = (st.height.kind == SizeSpec::Kind::Px);

        if (width_fixed && !height_fixed) {
            const float w = std::max(0.0f, st.width.value);
            content.h = (st.aspect_ratio > 0.0f) ? (w / st.aspect_ratio) : content.h;
        }
        else if (height_fixed && !width_fixed) {
            const float h = std::max(0.0f, st.height.value);
            content.w = (st.aspect_ratio > 0.0f) ? (h * st.aspect_ratio) : content.w;
        }
        return content;
    }

    // Resolve one SizeSpec arm against a known parent extent, returning a px value.
    // Px/Percent resolve; other kinds return the passed fallback.
    static float resolve_arm_px(const SizeSpec& s, const float parent_extent,
                                const float fallback) noexcept {
        switch (s.kind) {
        case SizeSpec::Kind::Px: return std::max(0.0f, s.value);
        case SizeSpec::Kind::Percent: return std::max(0.0f, parent_extent * s.value * 0.01f);
        default: return fallback;
        }
    }

    // Clamp a resolved dimension using a min/pref/max triple, resolving each arm against
    // the parent extent. pref (when set) replaces the value; min/max then bound it.
    static float resolve_clamp_rel(float dim, const SizeSpecClamp& c,
                                   const float parent_extent) noexcept {
        if (!c.active()) return dim;
        if (c.pref.kind != SizeSpec::Kind::Auto) {
            dim = resolve_arm_px(c.pref, parent_extent, dim);
        }
        if (c.min.kind != SizeSpec::Kind::Auto) {
            dim = std::max(dim, resolve_arm_px(c.min, parent_extent, 0.0f));
        }
        if (c.max.kind != SizeSpec::Kind::Auto) {
            const float hi = resolve_arm_px(c.max, parent_extent, std::numeric_limits<float>::infinity());
            dim = std::min(dim, hi);
        }
        return std::max(0.0f, dim);
    }

    struct HierarchyBufferView {
        std::vector<std::uint32_t>* parent = nullptr;
        std::vector<std::uint32_t>* first_child = nullptr;
        std::vector<std::uint32_t>* child_count = nullptr;
        std::vector<std::uint32_t>* child_offset = nullptr;
        std::vector<std::uint32_t>* children_idx = nullptr;
        std::vector<std::uint32_t>* postorder = nullptr;
        std::vector<std::uint32_t>* subtree_begin = nullptr;
        std::vector<std::uint32_t>* subtree_end = nullptr;
    };

    struct InputBufferView {
        std::vector<Axis>* axis = nullptr;
        std::vector<Align>* align_items = nullptr;
        std::vector<Justify>* justify_content = nullptr;
        std::vector<SizeSpec>* width = nullptr;
        std::vector<SizeSpec>* height = nullptr;
        std::vector<Edges>* padding = nullptr;
        std::vector<Edges>* margin = nullptr;
        std::vector<std::uint64_t>* user_tag = nullptr;
    };

    struct GeomBufferView {
        std::vector<Rect2D>* rect = nullptr;
        std::vector<Bounds2D>* clip = nullptr;
        std::vector<Size2D>* measured = nullptr;
    };

    struct StateBufferView {
        std::vector<std::uint8_t>* dirty = nullptr;
    };

    struct LayoutSnapshot {
        std::vector<Rect2D> rect;
        std::vector<Size2D> measured;
        std::vector<Bounds2D> clip;
        std::vector<std::uint8_t> dirty;

        std::chrono::system_clock::time_point timestamp;
        std::string label;
        std::size_t node_count = 0;

        void clear() {
            rect.clear();
            measured.clear();
            clip.clear();
            dirty.clear();
            node_count = 0;
            label.clear();
        }

        [[nodiscard]] bool empty() const {
            return node_count == 0;
        }
    };

    class Engine {
    public:
        PerformanceStats perf_stats;
        bool enable_perf_tracking = false;

        SpatialHash spatial_hash;
        bool enable_spatial_hash = false;

        bool enable_structural_hashing = false;

        bool enable_snapshots = false;
        size_t max_snapshots = 10;

        bool enable_constraints_graph_ = false;
        litegraph::Graph<std::monostate, std::monostate> constraints_graph;

        bool enable_virtualization = false;

        bool enable_parallel_layout = false;
        size_t parallel_threshold = 100;
        size_t parallel_batch_size = 50;

        // Baked topology
        std::vector<std::uint32_t> parent;
        std::vector<std::uint32_t> first_child;
        std::vector<std::uint32_t> child_count;
        std::vector<std::uint32_t> subtree_begin;
        std::vector<std::uint32_t> subtree_end;

        // Baked style
        std::vector<Axis> axis;
        std::vector<Align> align_items;
        std::vector<Justify> justify_content;

        std::vector<SizeSpec> width;
        std::vector<SizeSpec> height;

        std::vector<SizeSpec> min_width;
        std::vector<SizeSpec> min_height;
        std::vector<SizeSpec> max_width;
        std::vector<SizeSpec> max_height;
        std::vector<float> aspect_ratio;
        std::vector<char> aspect_lock;

        std::vector<float> flex_grow;
        std::vector<float> flex_shrink;
        std::vector<SizeSpec> flex_basis;

        std::vector<SizeSpecClamp> width_clamp;
        std::vector<SizeSpecClamp> height_clamp;

        std::vector<Overflow> overflow_x;
        std::vector<Overflow> overflow_y;
        std::vector<float> scroll_offset_x;
        std::vector<float> scroll_offset_y;

        std::vector<const char*> text;
        std::vector<std::uint32_t> text_id;

        TextMeasure text_measure_callback{nullptr, nullptr};
        bool enable_text_measure_debug = false;

        struct TextMeasureKey {
            std::uint32_t text_id;
            float max_width;

            bool operator==(const TextMeasureKey& other) const {
                return text_id == other.text_id && max_width == other.max_width;
            }
        };

        struct TextMeasureKeyHash {
            std::size_t operator()(const TextMeasureKey& k) const {
                return std::hash<std::uint32_t>()(k.text_id) ^
                    (std::hash<float>()(k.max_width) << 1);
            }
        };

        std::unordered_map<TextMeasureKey, Size2D, TextMeasureKeyHash> text_measure_cache_;
        bool enable_text_measure_cache = false;

        std::vector<std::uint8_t> constraint_mask;
        std::vector<float> anchor_left;
        std::vector<float> anchor_right;
        std::vector<float> anchor_top;
        std::vector<float> anchor_bottom;
        std::vector<char> match_parent_width;
        std::vector<char> match_parent_height;
        std::vector<char> center_x;
        std::vector<char> center_y;

        std::vector<Edges> padding;
        std::vector<Edges> margin;

        std::vector<char> has_constraint;

        std::vector<std::uint64_t> user_tag;

        // Outputs
        std::vector<Size2D> measured;
        std::vector<Rect2D> rect;
        std::vector<Bounds2D> clip;

        enum DirtyFlags : std::uint8_t {
            DIRTY_NONE = 0,
            DIRTY_MEASURE = 1 << 0,
            DIRTY_GEOMETRY = 1 << 1,
            DIRTY_CONSTRAINT = 1 << 2,
            DIRTY_CLIP = 1 << 3
        };

        static constexpr std::uint8_t DIRTY_GEOM = DIRTY_GEOMETRY;

        std::vector<std::uint8_t> dirty;

        HierarchyBufferView hierarchy;
        InputBufferView input;
        GeomBufferView geom;
        StateBufferView state;

        std::vector<Constraint> constraints;

        using SubtreeHash = std::uint64_t;
        std::unordered_map<SubtreeHash, std::uint32_t> subtree_hash_table_;
        std::vector<SubtreeHash> node_hashes_;
        std::vector<std::uint32_t> reference_count_;

        std::deque<LayoutSnapshot> snapshot_history_;

        std::unordered_set<std::uint32_t> scheduled_updates_;
        bool enable_incremental_scheduling = false;
        bool auto_solve = true;

        void mark_dirty(const std::uint32_t node, const std::uint8_t mask) noexcept {
            if (node >= size()) return;

            std::uint8_t propagated_mask = mask;
            if (mask & DIRTY_MEASURE) {
                propagated_mask |= (DIRTY_GEOMETRY | DIRTY_CONSTRAINT | DIRTY_CLIP);
            }
            else if (mask & DIRTY_GEOMETRY) {
                propagated_mask |= (DIRTY_CONSTRAINT | DIRTY_CLIP);
            }
            else if (mask & DIRTY_CONSTRAINT) {
                propagated_mask |= DIRTY_CLIP;
            }

            std::uint32_t curr = node;
            while (curr != kInvalid) {
                dirty[curr] |= propagated_mask;
                curr = parent[curr];
            }

            if (enable_incremental_scheduling) {
                scheduled_updates_.insert(node);
            }
        }

        void mark_dirty(const std::uint32_t node) noexcept {
            mark_dirty(node, DIRTY_GEOMETRY);
        }

        [[nodiscard]] std::size_t size() const noexcept { return parent.size(); }

        void clear() {
            parent.clear();
            first_child.clear();
            child_count.clear();
            subtree_begin.clear();
            subtree_end.clear();

            axis.clear();
            align_items.clear();
            justify_content.clear();

            width.clear();
            height.clear();

            min_width.clear();
            min_height.clear();
            max_width.clear();
            max_height.clear();
            aspect_ratio.clear();
            aspect_lock.clear();

            flex_grow.clear();
            flex_shrink.clear();
            flex_basis.clear();

            width_clamp.clear();
            height_clamp.clear();

            overflow_x.clear();
            overflow_y.clear();
            scroll_offset_x.clear();
            scroll_offset_y.clear();

            text.clear();
            text_id.clear();
            text_measure_cache_.clear();

            constraint_mask.clear();
            anchor_left.clear();
            anchor_right.clear();
            anchor_top.clear();
            anchor_bottom.clear();
            match_parent_width.clear();
            match_parent_height.clear();
            center_x.clear();
            center_y.clear();

            padding.clear();
            margin.clear();
            has_constraint.clear();

            user_tag.clear();

            measured.clear();
            rect.clear();
            clip.clear();
            dirty.clear();

            constraints.clear();
            spatial_hash.clear();
            subtree_hash_table_.clear();
            node_hashes_.clear();
            reference_count_.clear();
            snapshot_history_.clear();
            scheduled_updates_.clear();

            hierarchy = HierarchyBufferView{};
            input = InputBufferView{};
            geom = GeomBufferView{};
            state = StateBufferView{};
        }

        void set_style(const std::uint32_t node, const LayoutStyle& st) {
            if (node >= size()) return;
            axis[node] = st.axis;
            align_items[node] = st.align_items;
            justify_content[node] = st.justify_content;
            width[node] = st.width;
            height[node] = st.height;

            min_width[node] = st.min_width;
            min_height[node] = st.min_height;
            max_width[node] = st.max_width;
            max_height[node] = st.max_height;
            aspect_ratio[node] = st.aspect_ratio;
            aspect_lock[node] = st.aspect_lock ? 1 : 0;

            flex_grow[node] = st.flex_grow;
            flex_shrink[node] = st.flex_shrink;
            flex_basis[node] = st.flex_basis;

            width_clamp[node] = st.width_clamp;
            height_clamp[node] = st.height_clamp;

            // --- Additive SizeSpec unit translation (zero solver edit for Fr) ---
            // Fr(width/height) reuses the existing free-space flex split: translate to
            // flex_grow + zero basis. An explicit Fr on the main axis overrides the
            // style's own flex_grow for that node (documented precedence). The axis of
            // the *parent* decides which of width/height is the main axis, but a node is
            // laid out inside its parent's axis, so we translate whichever spec is Fr and
            // let place_pass pick it up via flex_grow (cross-axis Fr degrades to grow too,
            // which is harmless because free-space split only runs along the main axis).
            {
                const SizeSpec ws = st.width;
                const SizeSpec hs = st.height;
                if (ws.is_fr() || hs.is_fr()) {
                    const float w = ws.is_fr() ? ws.value : 0.0f;
                    const float h = hs.is_fr() ? hs.value : 0.0f;
                    flex_grow[node] = (w > 0.0f) ? w : ((h > 0.0f) ? h : st.flex_grow);
                    flex_basis[node] = SizeSpec::Px(0.0f);
                }
                // Aspect(ratio) on one axis => lock aspect and let the aspect pass derive
                // the cross dimension from the resolved main dimension.
                if (ws.is_aspect() && ws.value > 0.0f) {
                    aspect_ratio[node] = ws.value;
                    aspect_lock[node] = 1;
                }
                else if (hs.is_aspect() && hs.value > 0.0f) {
                    // Aspect(r) on height: r = w/h, so h = w/r. Store as w/h directly.
                    aspect_ratio[node] = hs.value;
                    aspect_lock[node] = 1;
                }
            }

            overflow_x[node] = st.overflow_x;
            overflow_y[node] = st.overflow_y;
            scroll_offset_x[node] = st.scroll_offset_x;
            scroll_offset_y[node] = st.scroll_offset_y;

            text[node] = st.text;
            text_id[node] = st.text_id;

            constraint_mask[node] = st.constraint_mask;
            anchor_left[node] = st.anchor_left;
            anchor_right[node] = st.anchor_right;
            anchor_top[node] = st.anchor_top;
            anchor_bottom[node] = st.anchor_bottom;
            match_parent_width[node] = st.match_parent_width ? 1 : 0;
            match_parent_height[node] = st.match_parent_height ? 1 : 0;
            center_x[node] = st.center_x ? 1 : 0;
            center_y[node] = st.center_y ? 1 : 0;

            padding[node] = st.padding;
            margin[node] = st.margin;

            const bool c_on = (st.constraint_mask != 0) || st.match_parent_width || st.match_parent_height ||
                st.center_x || st.center_y || (st.anchor_left != 0.0f) || (st.anchor_right != 0.0f) ||
                (st.anchor_top != 0.0f) || (st.anchor_bottom != 0.0f);
            has_constraint[node] = c_on ? 1 : 0;

            mark_dirty(node, DIRTY_GEOMETRY);
        }

        void bake(const LayoutTree& tree) {
            clear();
            const std::size_t count = tree.size();
            if (count == 0) return;

            reserve_and_resize(count);

            std::uint32_t next_index = 0;
            auto bake_recursive = [&](auto& self, std::uint32_t parent_idx,
                                      const typename LayoutTree::TreeNode* n) -> std::uint32_t {
                const std::uint32_t curr = next_index++;
                parent[curr] = parent_idx;
                user_tag[curr] = n->data.user_tag;
                set_style(curr, n->data.style);

                const auto& children = n->children;
                child_count[curr] = static_cast<std::uint32_t>(children.size());
                if (!children.empty()) {
                    first_child[curr] = next_index;
                    for (const auto& child : children) {
                        self(self, curr, child.get());
                    }
                }
                else {
                    first_child[curr] = kInvalid;
                }
                return curr;
            };

            const auto* root = tree.get_root();
            if (root) {
                bake_recursive(bake_recursive, kInvalid, root);
            }

            compute_subtree_ranges();
            build_buffer_views();

            if (enable_constraints_graph_) {
                rebuild_constraints_graph();
            }

            for (std::size_t i = 0; i < count; ++i) {
                dirty[i] = DIRTY_MEASURE | DIRTY_GEOMETRY | DIRTY_CONSTRAINT | DIRTY_CLIP;
            }
        }

        void solve(const Bounds2D& viewport) {
            auto start_time = std::chrono::high_resolution_clock::now();
            if (enable_perf_tracking) perf_stats.reset();

            if (size() == 0) return;

            Rect2D root_r = bounds_to_rect(viewport);

            // Phase 1: Measure pass (bottom-up)
            auto m_start = std::chrono::high_resolution_clock::now();
            measure_pass();
            if (enable_perf_tracking) perf_stats.measure_time = std::chrono::high_resolution_clock::now() - m_start;

            // Phase 2: Place pass (top-down)
            auto p_start = std::chrono::high_resolution_clock::now();
            place_pass(root_r);
            if (enable_perf_tracking) perf_stats.place_time = std::chrono::high_resolution_clock::now() - p_start;

            // Phase 3: Constraints pass
            auto c_start = std::chrono::high_resolution_clock::now();
            constraints_pass();
            if (enable_perf_tracking) perf_stats.constraints_time = std::chrono::high_resolution_clock::now() - c_start;

            // Phase 4: Clip pass (top-down)
            auto cl_start = std::chrono::high_resolution_clock::now();
            clip_pass(viewport);
            if (enable_perf_tracking) perf_stats.clip_time = std::chrono::high_resolution_clock::now() - cl_start;

            if (enable_spatial_hash) {
                update_spatial_hash();
            }

            if (enable_snapshots) {
                take_snapshot("solve");
            }

            // Clear dirty flags
            for (std::size_t i = 0; i < size(); ++i) {
                dirty[i] = DIRTY_NONE;
            }
            scheduled_updates_.clear();

            if (enable_perf_tracking) {
                perf_stats.total_time = std::chrono::high_resolution_clock::now() - start_time;
                perf_stats.node_count = size();
            }
        }

        void solve_incremental(const Bounds2D& viewport) {
            if (size() == 0) return;

            // Check if any node is dirty
            bool has_any_dirty = false;
            for (std::size_t i = 0; i < size(); ++i) {
                if (dirty[i] != DIRTY_NONE) {
                    has_any_dirty = true;
                    break;
                }
            }

            if (!has_any_dirty) {
                // If nothing is dirty, layout is stable — no work needed!
                return;
            }

            auto start_time = std::chrono::high_resolution_clock::now();
            if (enable_perf_tracking) perf_stats.reset();

            Rect2D root_r = bounds_to_rect(viewport);

            // Phase 1: Incremental Measure Pass (only measure dirty nodes bottom-up)
            auto m_start = std::chrono::high_resolution_clock::now();
            measure_pass_incremental();
            if (enable_perf_tracking) perf_stats.measure_time = std::chrono::high_resolution_clock::now() - m_start;

            // Phase 2: Place pass (top-down, only updating paths through dirty nodes)
            auto p_start = std::chrono::high_resolution_clock::now();
            place_pass(root_r);
            if (enable_perf_tracking) perf_stats.place_time = std::chrono::high_resolution_clock::now() - p_start;

            // Phase 3: Constraints pass
            auto c_start = std::chrono::high_resolution_clock::now();
            constraints_pass();
            if (enable_perf_tracking) perf_stats.constraints_time = std::chrono::high_resolution_clock::now() - c_start;

            // Phase 4: Clip pass
            auto cl_start = std::chrono::high_resolution_clock::now();
            clip_pass(viewport);
            if (enable_perf_tracking) perf_stats.clip_time = std::chrono::high_resolution_clock::now() - cl_start;

            if (enable_spatial_hash) {
                update_spatial_hash();
            }

            if (enable_snapshots) {
                take_snapshot("solve_incremental");
            }

            for (std::size_t i = 0; i < size(); ++i) {
                dirty[i] = DIRTY_NONE;
            }
            scheduled_updates_.clear();

            if (enable_perf_tracking) {
                perf_stats.total_time = std::chrono::high_resolution_clock::now() - start_time;
                perf_stats.node_count = size();
            }
        }

        void take_snapshot(const std::string& label = "") {
            if (!enable_snapshots) return;

            LayoutSnapshot snapshot;
            snapshot.rect = rect;
            snapshot.measured = measured;
            snapshot.clip = clip;
            snapshot.dirty = dirty;
            snapshot.timestamp = std::chrono::system_clock::now();
            snapshot.label = label;
            snapshot.node_count = size();

            snapshot_history_.push_back(std::move(snapshot));

            while (snapshot_history_.size() > max_snapshots) {
                snapshot_history_.pop_front();
            }
        }

        bool restore_snapshot(size_t index) {
            if (index >= snapshot_history_.size()) return false;

            const auto& snapshot = snapshot_history_[index];
            if (snapshot.node_count != size()) return false;

            rect = snapshot.rect;
            measured = snapshot.measured;
            clip = snapshot.clip;
            dirty = snapshot.dirty;

            return true;
        }

        [[nodiscard]] std::optional<std::uint32_t> hit_test(float x, float y) const {
            if (enable_spatial_hash) {
                auto candidates = spatial_hash.query(x, y);
                for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
                    const std::uint32_t i = *it;
                    const Rect2D& r = rect[i];
                    if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h) {
                        const Bounds2D& c = clip[i];
                        if (x >= c.lo[0] && x <= c.hi[0] && y >= c.lo[1] && y <= c.hi[1]) {
                            return i;
                        }
                    }
                }
                return std::nullopt;
            }

            for (std::int64_t i = static_cast<std::int64_t>(size()) - 1; i >= 0; --i) {
                const Rect2D& r = rect[static_cast<std::size_t>(i)];
                if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h) {
                    const Bounds2D& c = clip[static_cast<std::size_t>(i)];
                    if (x >= c.lo[0] && x <= c.hi[0] && y >= c.lo[1] && y <= c.hi[1]) {
                        return static_cast<std::uint32_t>(i);
                    }
                }
            }
            return std::nullopt;
        }

        // Fill `out` with the hit node then its ancestor chain (leaf -> root), writing at
        // most out.size() entries. Returns the number written. Zero heap: walks the baked
        // parent[] array. `hit_test` itself is unchanged.
        std::size_t hit_test_chain(float x, float y, std::span<std::uint32_t> out) const {
            const auto hit = hit_test(x, y);
            if (!hit) return 0;
            std::size_t n = 0;
            std::uint32_t curr = *hit;
            while (curr != kInvalid && n < out.size()) {
                out[n++] = curr;
                curr = parent[curr];
            }
            return n;
        }

        // Invoke fn(node_index) for every leaf (child_count == 0) in index order, which is
        // the baked pre-order (natural tab order). Zero heap.
        template <class Fn>
        void for_each_leaf(Fn&& fn) const {
            const std::size_t count = size();
            for (std::size_t u = 0; u < count; ++u) {
                if (child_count[u] == 0) {
                    fn(static_cast<std::uint32_t>(u));
                }
            }
        }

    private:
        void reserve_and_resize(std::size_t count) {
            parent.resize(count, kInvalid);
            first_child.resize(count, kInvalid);
            child_count.resize(count, 0);
            subtree_begin.resize(count, 0);
            subtree_end.resize(count, 0);

            axis.resize(count, Axis::Column);
            align_items.resize(count, Align::Start);
            justify_content.resize(count, Justify::Start);

            width.resize(count, SizeSpec::Auto());
            height.resize(count, SizeSpec::Auto());

            min_width.resize(count, SizeSpec::Auto());
            min_height.resize(count, SizeSpec::Auto());
            max_width.resize(count, SizeSpec::Auto());
            max_height.resize(count, SizeSpec::Auto());
            aspect_ratio.resize(count, 0.0f);
            aspect_lock.resize(count, 0);

            flex_grow.resize(count, 0.0f);
            flex_shrink.resize(count, 1.0f);
            flex_basis.resize(count, SizeSpec::Auto());

            width_clamp.resize(count, SizeSpecClamp{});
            height_clamp.resize(count, SizeSpecClamp{});

            overflow_x.resize(count, Overflow::Visible);
            overflow_y.resize(count, Overflow::Visible);
            scroll_offset_x.resize(count, 0.0f);
            scroll_offset_y.resize(count, 0.0f);

            text.resize(count, nullptr);
            text_id.resize(count, 0);

            constraint_mask.resize(count, 0);
            anchor_left.resize(count, 0.0f);
            anchor_right.resize(count, 0.0f);
            anchor_top.resize(count, 0.0f);
            anchor_bottom.resize(count, 0.0f);
            match_parent_width.resize(count, 0);
            match_parent_height.resize(count, 0);
            center_x.resize(count, 0);
            center_y.resize(count, 0);

            padding.resize(count, Edges{});
            margin.resize(count, Edges{});
            has_constraint.resize(count, 0);

            user_tag.resize(count, 0);

            measured.resize(count, Size2D{});
            rect.resize(count, Rect2D{});
            clip.resize(count, Bounds2D{});
            dirty.resize(count, DIRTY_NONE);
        }

        void compute_subtree_ranges() {
            const std::size_t count = size();
            for (std::size_t i = 0; i < count; ++i) {
                subtree_begin[i] = static_cast<std::uint32_t>(i);
                subtree_end[i] = static_cast<std::uint32_t>(i + 1);
            }
            for (std::int64_t i = static_cast<std::int64_t>(count) - 1; i >= 0; --i) {
                const std::uint32_t p = parent[static_cast<std::size_t>(i)];
                if (p != kInvalid) {
                    subtree_end[p] = std::max(subtree_end[p], subtree_end[static_cast<std::size_t>(i)]);
                }
            }
        }

        void build_buffer_views() {
            hierarchy.parent = &parent;
            hierarchy.first_child = &first_child;
            hierarchy.child_count = &child_count;
            hierarchy.subtree_begin = &subtree_begin;
            hierarchy.subtree_end = &subtree_end;

            input.axis = &axis;
            input.align_items = &align_items;
            input.justify_content = &justify_content;
            input.width = &width;
            input.height = &height;
            input.padding = &padding;
            input.margin = &margin;
            input.user_tag = &user_tag;

            geom.rect = &rect;
            geom.clip = &clip;
            geom.measured = &measured;

            state.dirty = &dirty;
        }

        void measure_node(std::uint32_t u) {
            if (enable_perf_tracking) perf_stats.nodes_measured++;

            Size2D content{0.0f, 0.0f};

            if (text[u] != nullptr && text_measure_callback.measure != nullptr) {
                const float max_w = (width[u].kind == SizeSpec::Kind::Px) ? width[u].value : 0.0f;
                content = text_measure_callback.measure(text[u], max_w, text_measure_callback.user_data);
                if (enable_perf_tracking) perf_stats.text_measure_calls++;
            }

            if (child_count[u] > 0) {
                const Axis ax = axis[u];
                float main_sum = 0.0f;
                float cross_max = 0.0f;

                const std::uint32_t fc = first_child[u];
                const std::uint32_t cc = child_count[u];
                for (std::uint32_t c = 0; c < cc; ++c) {
                    const std::uint32_t ch = fc + c;
                    const Size2D ch_m = measured[ch];
                    const Edges& ch_mg = margin[ch];

                    const float ch_main = main_size(ch_m, ax) + main_margin_before(ch_mg, ax) + main_margin_after(
                        ch_mg, ax);
                    const float ch_cross = cross_size(ch_m, ax) + cross_margin_before(ch_mg, ax) + cross_margin_after(
                        ch_mg, ax);

                    main_sum += ch_main;
                    cross_max = std::max(cross_max, ch_cross);
                }

                Size2D children_size{};
                set_main_size(children_size, ax, main_sum);
                set_cross_size(children_size, ax, cross_max);

                content.w = std::max(content.w, children_size.w);
                content.h = std::max(content.h, children_size.h);
            }

            LayoutStyle st;
            st.width = width[u];
            st.height = height[u];
            st.min_width = min_width[u];
            st.min_height = min_height[u];
            st.max_width = max_width[u];
            st.max_height = max_height[u];
            st.aspect_ratio = aspect_ratio[u];
            st.aspect_lock = (aspect_lock[u] != 0);

            Size2D final_size = content;
            const Edges& pad = padding[u];
            final_size.w += pad.l + pad.r;
            final_size.h += pad.t + pad.b;

            if (st.width.kind == SizeSpec::Kind::Px) final_size.w = st.width.value;
            if (st.height.kind == SizeSpec::Kind::Px) final_size.h = st.height.value;

            // Content unit: keep the intrinsic (content-derived) size but clamp it to the
            // spec's [value, aux0] window. aux0 <= 0 means unbounded above.
            if (st.width.is_content()) {
                final_size.w = clamp_content_unit(final_size.w, st.width);
            }
            if (st.height.is_content()) {
                final_size.h = clamp_content_unit(final_size.h, st.height);
            }

            final_size = apply_min_max_content(final_size, st);
            // SizeSpecClamp min/pref/max windows (px-resolvable arms only at measure time;
            // Percent/Fr arms resolve later in place_pass against the parent).
            final_size.w = apply_clamp_px(final_size.w, width_clamp[u]);
            final_size.h = apply_clamp_px(final_size.h, height_clamp[u]);
            final_size = apply_aspect_content(final_size, st);

            measured[u] = final_size;
        }

        static float clamp_content_unit(const float intrinsic, const SizeSpec& s) noexcept {
            float v = intrinsic;
            const float lo = std::max(0.0f, s.value);
            if (v < lo) v = lo;
            if (s.aux0 > 0.0f && v > s.aux0) v = s.aux0;
            return v;
        }

        // Apply only the px-resolvable arms of a clamp at measure time. Non-px arms
        // (Percent/Fr) are left for place_pass where the parent extent is known.
        static float apply_clamp_px(float v, const SizeSpecClamp& c) noexcept {
            if (!c.active()) return v;
            if (c.pref.kind == SizeSpec::Kind::Px) v = c.pref.value;
            if (c.min.kind == SizeSpec::Kind::Px) v = std::max(v, c.min.value);
            if (c.max.kind == SizeSpec::Kind::Px) v = std::min(v, c.max.value);
            return std::max(0.0f, v);
        }

        void measure_pass() {
            const std::size_t count = size();
            for (std::int64_t i = static_cast<std::int64_t>(count) - 1; i >= 0; --i) {
                measure_node(static_cast<std::uint32_t>(i));
            }
        }

        void measure_pass_incremental() {
            const std::size_t count = size();
            for (std::int64_t i = static_cast<std::int64_t>(count) - 1; i >= 0; --i) {
                const std::uint32_t u = static_cast<std::uint32_t>(i);
                if (dirty[u] & (DIRTY_MEASURE | DIRTY_GEOMETRY)) {
                    measure_node(u);
                }
            }
        }

        void place_pass(const Rect2D& root_r) {
            const std::size_t count = size();
            rect[0] = root_r;
            // Honor root's explicit Px extents and aspect ratio (no parent to derive from).
            if (count > 0) {
                if (width[0].kind == SizeSpec::Kind::Px) rect[0].w = width[0].value;
                if (height[0].kind == SizeSpec::Kind::Px) rect[0].h = height[0].value;
                if (aspect_lock[0] != 0 && aspect_ratio[0] > 0.0f) {
                    if (height[0].is_aspect()) rect[0].h = rect[0].w / aspect_ratio[0];
                    else if (width[0].is_aspect()) rect[0].w = rect[0].h * aspect_ratio[0];
                }
            }
            if (enable_perf_tracking) perf_stats.nodes_placed++;

            for (std::size_t u = 0; u < count; ++u) {
                const std::uint32_t cc = child_count[u];
                if (cc == 0) continue;

                const Rect2D parent_rect = rect[u];
                const Edges parent_pad = padding[u];
                const Rect2D content_rect = inset_rect(parent_rect, parent_pad);

                const Axis ax = axis[u];
                const Align al = align_items[u];
                const Justify jf = justify_content[u];

                const std::uint32_t fc = first_child[u];

                float total_flex_grow = 0.0f;
                float total_flex_shrink = 0.0f;
                float total_main_size = 0.0f;

                for (std::uint32_t c = 0; c < cc; ++c) {
                    const std::uint32_t ch = fc + c;
                    total_flex_grow += flex_grow[ch];
                    total_flex_shrink += flex_shrink[ch];

                    float item_main = main_size(measured[ch], ax);
                    if (flex_basis[ch].kind == SizeSpec::Kind::Px) {
                        item_main = flex_basis[ch].value;
                    }
                    item_main += main_margin_before(margin[ch], ax) + main_margin_after(margin[ch], ax);
                    total_main_size += item_main;
                }

                const float content_main = main_size(content_rect, ax);
                const float free_space = content_main - total_main_size;

                float main_offset = (ax == Axis::Row) ? content_rect.x : content_rect.y;

                if (free_space > 0.0f && total_flex_grow == 0.0f) {
                    if (jf == Justify::Center) main_offset += free_space * 0.5f;
                    else if (jf == Justify::End) main_offset += free_space;
                    else if (jf == Justify::SpaceBetween && cc > 1) main_offset += 0.0f;
                }

                const float space_between_gap = (free_space > 0.0f && jf == Justify::SpaceBetween && cc > 1)
                                                    ? (free_space / (cc - 1))
                                                    : 0.0f;

                for (std::uint32_t c = 0; c < cc; ++c) {
                    const std::uint32_t ch = fc + c;
                    if (enable_perf_tracking) perf_stats.nodes_placed++;

                    Rect2D ch_r{};
                    const Edges ch_mg = margin[ch];

                    float ch_main = main_size(measured[ch], ax);
                    float ch_cross = cross_size(measured[ch], ax);

                    if (flex_basis[ch].kind == SizeSpec::Kind::Px) {
                        ch_main = flex_basis[ch].value;
                    }

                    if (free_space > 0.0f && total_flex_grow > 0.0f) {
                        ch_main += free_space * (flex_grow[ch] / total_flex_grow);
                    }
                    else if (free_space < 0.0f && total_flex_shrink > 0.0f) {
                        ch_main += free_space * (flex_shrink[ch] / total_flex_shrink);
                        ch_main = std::max(0.0f, ch_main);
                    }

                    if (al == Align::Stretch) {
                        const float parent_cross = cross_size(content_rect, ax);
                        ch_cross = std::max(
                            ch_cross, parent_cross - (cross_margin_before(ch_mg, ax) + cross_margin_after(ch_mg, ax)));
                    }

                    // Resolve parent-relative clamp arms (Percent/Fr) now that the parent
                    // content extent is known. px arms were already applied at measure.
                    {
                        const float par_main = content_main;
                        const float par_cross = cross_size(content_rect, ax);
                        const SizeSpecClamp& wc = width_clamp[ch];
                        const SizeSpecClamp& hc = height_clamp[ch];
                        const bool w_is_main = (ax == Axis::Row);
                        if (wc.active()) {
                            const float par = w_is_main ? par_main : par_cross;
                            float& dim = w_is_main ? ch_main : ch_cross;
                            dim = resolve_clamp_rel(dim, wc, par);
                        }
                        if (hc.active()) {
                            const float par = w_is_main ? par_cross : par_main;
                            float& dim = w_is_main ? ch_cross : ch_main;
                            dim = resolve_clamp_rel(dim, hc, par);
                        }
                    }

                    // Aspect unit: derive the cross dimension from the resolved main.
                    if (aspect_lock[ch] != 0 && aspect_ratio[ch] > 0.0f) {
                        // aspect_ratio is width/height. Derive cross from main honoring axis.
                        if (ax == Axis::Row) {
                            // main = width, cross = height
                            ch_cross = ch_main / aspect_ratio[ch];
                        }
                        else {
                            // main = height, cross = width
                            ch_cross = ch_main * aspect_ratio[ch];
                        }
                    }

                    set_main_len(ch_r, ax, ch_main);
                    set_cross_len(ch_r, ax, ch_cross);

                    main_offset += main_margin_before(ch_mg, ax);
                    set_main_pos(ch_r, ax, main_offset);
                    main_offset += ch_main + main_margin_after(ch_mg, ax) + space_between_gap;

                    const float parent_cross_pos = (ax == Axis::Row) ? content_rect.y : content_rect.x;
                    const float parent_cross_len = cross_size(content_rect, ax);

                    float cross_pos = parent_cross_pos + cross_margin_before(ch_mg, ax);
                    if (al == Align::Center) {
                        cross_pos = parent_cross_pos + (parent_cross_len - ch_cross) * 0.5f;
                    }
                    else if (al == Align::End) {
                        cross_pos = parent_cross_pos + parent_cross_len - ch_cross - cross_margin_after(ch_mg, ax);
                    }

                    set_cross_pos(ch_r, ax, cross_pos);
                    rect[ch] = ch_r;
                }
            }
        }

        void constraints_pass() {
            const std::size_t count = size();
            for (std::size_t u = 0; u < count; ++u) {
                if (!has_constraint[u]) continue;

                if (enable_perf_tracking) perf_stats.constraints_applied++;
                const std::uint32_t p = parent[u];

                if (match_parent_width[u] && p != kInvalid) {
                    rect[u].x = rect[p].x + padding[p].l + margin[u].l;
                    rect[u].w = std::max(0.0f, rect[p].w - (padding[p].l + padding[p].r + margin[u].l + margin[u].r));
                }
                if (match_parent_height[u] && p != kInvalid) {
                    rect[u].y = rect[p].y + padding[p].t + margin[u].t;
                    rect[u].h = std::max(0.0f, rect[p].h - (padding[p].t + padding[p].b + margin[u].t + margin[u].b));
                }

                if (center_x[u] && p != kInvalid) {
                    rect[u].x = rect[p].x + (rect[p].w - rect[u].w) * 0.5f;
                }
                if (center_y[u] && p != kInvalid) {
                    rect[u].y = rect[p].y + (rect[p].h - rect[u].h) * 0.5f;
                }

                if (anchor_left[u] != 0.0f && p != kInvalid) {
                    rect[u].x = rect[p].x + anchor_left[u];
                }
                if (anchor_top[u] != 0.0f && p != kInvalid) {
                    rect[u].y = rect[p].y + anchor_top[u];
                }

                // Parent-relative right/bottom anchors: pin the node's right/bottom edge
                // at the given inset from the parent's right/bottom edge (offset measured
                // inward). Symmetric to left/top; applied after them so an L+R pair
                // stretches width, and a lone R/B pins that edge.
                if (anchor_right[u] != 0.0f && p != kInvalid) {
                    const float parent_right = rect[p].x + rect[p].w;
                    if (anchor_left[u] != 0.0f) {
                        // Both edges pinned => derive width from the span.
                        rect[u].w = std::max(0.0f, (parent_right - anchor_right[u]) - rect[u].x);
                    }
                    else {
                        rect[u].x = parent_right - anchor_right[u] - rect[u].w;
                    }
                }
                if (anchor_bottom[u] != 0.0f && p != kInvalid) {
                    const float parent_bottom = rect[p].y + rect[p].h;
                    if (anchor_top[u] != 0.0f) {
                        rect[u].h = std::max(0.0f, (parent_bottom - anchor_bottom[u]) - rect[u].y);
                    }
                    else {
                        rect[u].y = parent_bottom - anchor_bottom[u] - rect[u].h;
                    }
                }
            }
        }

        void clip_pass(const Bounds2D& viewport) {
            const std::size_t count = size();
            clip[0] = viewport;

            for (std::size_t u = 0; u < count; ++u) {
                if (enable_perf_tracking) perf_stats.clips_computed++;

                const Bounds2D parent_clip = (u == 0) ? viewport : clip[parent[u]];
                Bounds2D my_clip = parent_clip;

                if (overflow_x[u] == Overflow::Clip || overflow_y[u] == Overflow::Clip ||
                    overflow_x[u] == Overflow::Scroll || overflow_y[u] == Overflow::Scroll) {
                    Bounds2D self_bounds = rect_to_bounds(rect[u]);
                    my_clip = intersect_bounds(parent_clip, self_bounds);
                }

                clip[u] = my_clip;
            }
        }

        void update_spatial_hash() {
            spatial_hash.clear();
            const std::size_t count = size();
            for (std::size_t i = 0; i < count; ++i) {
                spatial_hash.insert(static_cast<std::uint32_t>(i), rect[i]);
                if (enable_perf_tracking) perf_stats.spatial_hash_updates++;
            }
        }

        void rebuild_constraints_graph() {
            constraints_graph.clear();
            const std::size_t count = size();
            for (std::size_t i = 0; i < count; ++i) {
                constraints_graph.add_node(std::monostate{});
            }
            for (const auto& c : constraints) {
                if (c.target < count && c.source < count && c.source != kInvalid) {
                    constraints_graph.add_edge(litegraph::NodeId{c.source}, litegraph::NodeId{c.target},
                                               std::monostate{});
                }
            }
        }
    };

    // Debug renderer interface for UI inspection
    class DebugRenderer {
    public:
        virtual ~DebugRenderer() = default;
        virtual void draw_rect(const Rect2D& r, float red, float green, float blue, float alpha) = 0;
        virtual void draw_rect_outline(const Rect2D& r, float red, float green, float blue, float alpha,
                                       float thickness) = 0;
        virtual void draw_line(float x1, float y1, float x2, float y2, float red, float green, float blue, float alpha,
                               float thickness) = 0;
        virtual void draw_text(float x, float y, const char* text, float red, float green, float blue, float alpha,
                               float size) = 0;
    };

    struct DebugOverlayConfig {
        struct Color {
            float r, g, b, a;
        };

        Color rect_outline{0.0f, 0.8f, 1.0f, 0.8f};
        Color clip_outline{1.0f, 0.2f, 0.2f, 0.8f};
        Color dirty_highlight{1.0f, 0.9f, 0.0f, 0.9f};
        Color constraint_line{0.2f, 1.0f, 0.3f, 0.7f};
        Color text_color{1.0f, 1.0f, 1.0f, 0.9f};
        Color flex_grow_color{0.3f, 0.8f, 1.0f, 0.6f};
        Color flex_shrink_color{1.0f, 0.5f, 0.2f, 0.6f};

        float rect_outline_width = 1.0f;
        float clip_outline_width = 1.0f;
        float constraint_line_width = 1.5f;
        float text_size = 12.0f;

        bool show_rect_outlines = true;
        bool show_clip_bounds = false;
        bool show_dirty_nodes = true;
        bool show_constraint_edges = true;
        bool show_node_ids = true;
        bool show_rect_bounds = false;
        bool show_flex_factors = false;
        bool show_text_info = true;
    };

    class DebugOverlay {
    public:
        DebugOverlayConfig config;
        bool enabled = true;

        void draw(const Engine& engine, DebugRenderer& renderer) const {
            if (!enabled || engine.size() == 0) return;

            if (config.show_rect_outlines) draw_rect_outlines(engine, renderer);
            if (config.show_clip_bounds) draw_clip_bounds(engine, renderer);
            if (config.show_dirty_nodes) draw_dirty_nodes(engine, renderer);
            if (config.show_constraint_edges) draw_constraint_edges(engine, renderer);
            if (config.show_text_info) draw_text_info(engine, renderer);
        }

    private:
        void draw_rect_outlines(const Engine& engine, DebugRenderer& renderer) const {
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(engine.rect.size()); ++i) {
                const Rect2D& r = engine.rect[i];
                if (r.w <= 0 || r.h <= 0) continue;
                renderer.draw_rect_outline(r,
                                           config.rect_outline.r, config.rect_outline.g,
                                           config.rect_outline.b, config.rect_outline.a,
                                           config.rect_outline_width);
            }
        }

        void draw_clip_bounds(const Engine& engine, DebugRenderer& renderer) const {
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(engine.clip.size()); ++i) {
                const Bounds2D& b = engine.clip[i];
                Rect2D r = bounds_to_rect(b);
                if (r.w <= 0 || r.h <= 0) continue;
                renderer.draw_rect_outline(r,
                                           config.clip_outline.r, config.clip_outline.g,
                                           config.clip_outline.b, config.clip_outline.a,
                                           config.clip_outline_width);
            }
        }

        void draw_dirty_nodes(const Engine& engine, DebugRenderer& renderer) const {
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(engine.rect.size()); ++i) {
                if (i >= engine.dirty.size()) continue;
                const std::uint8_t dirty_flags = engine.dirty[i];
                if (dirty_flags == Engine::DIRTY_NONE) continue;
                const Rect2D& r = engine.rect[i];
                if (r.w <= 0 || r.h <= 0) continue;
                renderer.draw_rect_outline(r,
                                           config.dirty_highlight.r, config.dirty_highlight.g,
                                           config.dirty_highlight.b, config.dirty_highlight.a,
                                           config.rect_outline_width * 2.0f);
            }
        }

        void draw_constraint_edges(const Engine& engine, DebugRenderer& renderer) const {
            for (const auto& c : engine.constraints) {
                if (c.target >= engine.rect.size() || c.source >= engine.rect.size() || c.source == kInvalid) continue;
                const Rect2D& target_rect = engine.rect[c.target];
                const Rect2D& source_rect = engine.rect[c.source];

                const float sx = source_rect.x + source_rect.w * 0.5f;
                const float sy = source_rect.y + source_rect.h * 0.5f;
                const float tx = target_rect.x + target_rect.w * 0.5f;
                const float ty = target_rect.y + target_rect.h * 0.5f;

                renderer.draw_line(sx, sy, tx, ty,
                                   config.constraint_line.r, config.constraint_line.g,
                                   config.constraint_line.b, config.constraint_line.a,
                                   config.constraint_line_width);
            }
        }

        void draw_text_info(const Engine& engine, DebugRenderer& renderer) const {
            char buffer[256];
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(engine.rect.size()); ++i) {
                const Rect2D& r = engine.rect[i];
                if (r.w <= 0 || r.h <= 0) continue;
                if (config.show_node_ids) {
                    std::snprintf(buffer, sizeof(buffer), "#%u", i);
                    renderer.draw_text(r.x + 2.0f, r.y + 2.0f,
                                       buffer,
                                       config.text_color.r, config.text_color.g,
                                       config.text_color.b, config.text_color.a,
                                       config.text_size);
                }
            }
        }
    };

    inline void debug_draw_layout(const Engine& engine, DebugRenderer& renderer,
                                  const DebugOverlayConfig& config = DebugOverlayConfig{}) {
        DebugOverlay overlay;
        overlay.config = config;
        overlay.enabled = true;
        overlay.draw(engine, renderer);
    }
} // namespace akruti::layout

namespace akriti::layout {
    using namespace akruti::layout;
}
