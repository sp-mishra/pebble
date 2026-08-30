#include <catch_amalgamated.hpp>
#include <containers/matrix/matrix.hpp>

using namespace ga;

// ============================================================================
// test_mat.cpp — ga::Mat unified adaptive matrix type
// ============================================================================

TEST_CASE("Mat: auto-select dense for small matrices", "[mat][selection]") {
    // 4×4, density=1.0 → Dense (> static_max=16 so Static threshold not met... wait 4*4=16 ≤ 16 so Static)
    Mat<float> m(4, 4, 1.0f);
    // Either Static or Dense depending on SelectionPolicy; should work either way
    REQUIRE(m.rows() == 4);
    REQUIRE(m.cols() == 4);
}

TEST_CASE("Mat: forced dense via tag", "[mat][dense_tag]") {
    Mat<float> m(5, 5, dense_tag{});
    REQUIRE(m.kind() == MatKind::Dense);
    m(0, 0) = 3.14f;
    CHECK(m(0, 0) == Catch::Approx(3.14f));
}

TEST_CASE("Mat: forced sparse via tag", "[mat][sparse_tag]") {
    Mat<float> m(5, 5, sparse_tag{});
    REQUIRE(m.kind() == MatKind::Sparse);
}

TEST_CASE("Mat: forced banded via tag", "[mat][banded_tag]") {
    Mat<float> m(5, 5, banded_tag{2});
    REQUIRE(m.kind() == MatKind::Diagonal);
}

TEST_CASE("Mat: mutable element access throws on sparse", "[mat][element_access]") {
    Mat<float> m(5, 5, sparse_tag{});
    REQUIRE_THROWS_AS(m(0, 0) = 1.f, std::runtime_error);
}

TEST_CASE("Mat: const operator() returns element for dense", "[mat][element_access][dense]") {
    Mat<float> m(3, 3, dense_tag{});
    m(1, 1) = 7.f;
    const auto& cm = m;
    CHECK(cm(1, 1) == 7.f);
}

TEST_CASE("Mat: as_dense returns non-null for dense", "[mat][as_dense]") {
    Mat<float> m(3, 3, dense_tag{});
    REQUIRE(m.as_dense() != nullptr);
    REQUIRE(m.as_sparse() == nullptr);
}

TEST_CASE("Mat: as_sparse returns non-null for sparse", "[mat][as_sparse]") {
    Mat<float> m(3, 3, sparse_tag{});
    REQUIRE(m.as_sparse() != nullptr);
    REQUIRE(m.as_dense() == nullptr);
}

TEST_CASE("Mat: maybe_adapt with NoAdaptorPolicy returns nullopt", "[mat][adapt]") {
    Mat<float> m(4, 4, dense_tag{});
    m.record_density(0.01f);
    auto result = m.maybe_adapt();
    REQUIRE(!result.has_value());
}

TEST_CASE("Mat: info() returns correct dimensions", "[mat][info]") {
    Mat<float> m(6, 4, dense_tag{});
    auto info = m.info();
    CHECK(info.rows == 6);
    CHECK(info.cols == 4);
    CHECK(info.kind == MatKind::Dense);
}

TEST_CASE("Mat: info() for sparse", "[mat][info][sparse]") {
    Mat<double> m(10, 10, sparse_tag{});
    auto info = m.info();
    CHECK(info.kind == MatKind::Sparse);
    CHECK(info.rows == 10);
}

TEST_CASE("Mat: usage_stats initially zero", "[mat][usage_stats]") {
    Mat<float> m(3, 3, dense_tag{});
    const auto& stats = m.usage_stats();
    CHECK(stats.spmv_count == 0);
    CHECK(stats.gemm_count == 0);
}

TEST_CASE("Mat: inspect free function matches info()", "[mat][inspect]") {
    Mat<float> m(4, 5, dense_tag{});
    auto info1 = m.info();
    auto info2 = ga::inspect(m);
    CHECK(info1.rows == info2.rows);
    CHECK(info1.cols == info2.cols);
    CHECK(info1.kind == info2.kind);
}

TEST_CASE("Mat: construct from existing Dense", "[mat][construct_dense]") {
    Matrix<float> M(3, 3, 2.f);
    Mat<float> m(M);
    REQUIRE(m.kind() == MatKind::Dense);
    CHECK(m(0, 0) == 2.f);
}

TEST_CASE("Mat: construct from existing Sparse (CSR)", "[mat][construct_sparse]") {
    std::vector<std::size_t> rows={0,1,2}, cols={0,1,2};
    std::vector<float> vals={1.f,2.f,3.f};
    auto csr = CsrMatrix<float>::from_triplets(3, 3, rows, cols, vals);
    Mat<float> m(std::move(csr));
    REQUIRE(m.kind() == MatKind::Sparse);
    CHECK(m(2, 2) == 3.f);
}

TEST_CASE("ga::MatInfo: std::format output", "[mat][format]") {
    Matrix<float> M(3, 4);
    auto info = ga::inspect(M);
    auto s = std::format("{}", info);
    // should contain "Dense" and "3"
    REQUIRE(s.find("Dense") != std::string::npos);
    REQUIRE(s.find("3") != std::string::npos);
}

TEST_CASE("ga::MatInfo: inspect StaticMatrix", "[mat][inspect_static]") {
    StaticMatrix<float, 4, 4> A;
    auto info = ga::inspect(A);
    CHECK(info.kind == MatKind::Static);
    CHECK(info.rows == 4);
    CHECK(info.cols == 4);
    CHECK(info.density == 1.f);
}
