#include <catch_amalgamated.hpp>
#include <containers/matrix/sparse.hpp>
#include <containers/matrix/dense.hpp>
#include <cmath>

using namespace ga;

TEST_CASE("CsrMatrix: construction from triplets", "[sparse][csr]") {
    // 3×3 tridiagonal
    std::vector<std::size_t> rows = {0,0,1,1,1,2,2};
    std::vector<std::size_t> cols = {0,1,0,1,2,1,2};
    std::vector<float>       vals = {2.f,-1.f,-1.f,2.f,-1.f,-1.f,2.f};
    auto A = CsrMatrix<float>::from_triplets(3, 3, rows, cols, vals);
    REQUIRE(A.nrows == 3);
    REQUIRE(A.ncols == 3);
    REQUIRE(A.nnz() == 7);
    CHECK(A.get(0,0) == 2.f);
    CHECK(A.get(0,1) == -1.f);
    CHECK(A.get(1,0) == -1.f);
    CHECK(A.get(1,1) == 2.f);
    CHECK(A.get(2,2) == 2.f);
    CHECK(A.get(0,2) == 0.f);  // structural zero
}

TEST_CASE("CsrMatrix: row_ptr sorted correctly", "[sparse][csr]") {
    std::vector<std::size_t> rows = {0,0,1,2};
    std::vector<std::size_t> cols = {2,0,1,2};
    std::vector<float>       vals = {1.f,3.f,5.f,7.f};
    auto A = CsrMatrix<float>::from_triplets(3, 3, rows, cols, vals);
    REQUIRE(A.row_ptr.size() == 4);
    CHECK(A.row_ptr[0] == 0);
    CHECK(A.row_ptr[1] == 2);
    CHECK(A.row_ptr[2] == 3);
    CHECK(A.row_ptr[3] == 4);
}

TEST_CASE("CsrMatrix: spmv correctness", "[sparse][spmv]") {
    // A = [[2,-1,0],[-1,2,-1],[0,-1,2]], x = [1,2,3]
    // Ax = [2-2, -1+4-3, -2+6] = [0, 0, 4]
    std::vector<std::size_t> rows = {0,0,1,1,1,2,2};
    std::vector<std::size_t> cols = {0,1,0,1,2,1,2};
    std::vector<float>       vals = {2.f,-1.f,-1.f,2.f,-1.f,-1.f,2.f};
    auto A = CsrMatrix<float>::from_triplets(3, 3, rows, cols, vals);
    Vector<float> x(3); x[0]=1.f; x[1]=2.f; x[2]=3.f;
    auto y = spmv(A, x);
    REQUIRE(y.size() == 3);
    CHECK(y[0] == Catch::Approx(0.f).margin(1e-6f));
    CHECK(y[1] == Catch::Approx(0.f).margin(1e-6f));
    CHECK(y[2] == Catch::Approx(4.f).margin(1e-6f));
}

TEST_CASE("CooMatrix: push and to_csr", "[sparse][coo]") {
    CooMatrix<double> coo(4, 4);
    coo.push(0, 0, 1.0);
    coo.push(1, 1, 2.0);
    coo.push(2, 2, 3.0);
    coo.push(3, 3, 4.0);
    coo.push(0, 3, 5.0);
    REQUIRE(coo.nnz() == 5);
    auto csr = coo.to_csr();
    CHECK(csr.get(0, 0) == 1.0);
    CHECK(csr.get(1, 1) == 2.0);
    CHECK(csr.get(3, 3) == 4.0);
    CHECK(csr.get(0, 3) == 5.0);
    CHECK(csr.get(0, 1) == 0.0);
}

TEST_CASE("DiaMatrix: construction and spmv", "[sparse][dia]") {
    // Tridiagonal 4×4, offsets: -1, 0, 1
    std::vector<std::ptrdiff_t> offsets = {-1, 0, 1};
    DiaMatrix<float> D(4, 4, offsets);
    // main diagonal (index 1, offset=0): 2,2,2,2
    for (std::size_t i=0;i<4;++i) D.set_diag(1, i, 2.f);
    // sub-diag (index 0, offset=-1): rows 1,2,3 → col 0,1,2
    for (std::size_t i=1;i<4;++i) D.set_diag(0, i, -1.f);
    // super-diag (index 2, offset=+1): rows 0,1,2 → col 1,2,3
    for (std::size_t i=0;i<3;++i) D.set_diag(2, i, -1.f);

    Vector<float> x(4); for (std::size_t i=0;i<4;++i) x[i]=1.f;
    auto y = spmv(D, x);
    // Row 0: 2*1 - 1*1 = 1
    // Row 1: -1*1 + 2*1 - 1*1 = 0
    // Row 2: -1*1 + 2*1 - 1*1 = 0
    // Row 3: -1*1 + 2*1 = 1
    REQUIRE(y.size() == 4);
    CHECK(y[0] == Catch::Approx(1.f).margin(1e-6f));
    CHECK(y[1] == Catch::Approx(0.f).margin(1e-6f));
    CHECK(y[2] == Catch::Approx(0.f).margin(1e-6f));
    CHECK(y[3] == Catch::Approx(1.f).margin(1e-6f));
}

TEST_CASE("DiaMatrix: to_csr", "[sparse][dia][csr]") {
    std::vector<std::ptrdiff_t> offsets = {0};
    DiaMatrix<float> D(3, 3, offsets);
    D.set_diag(0, 0, 1.f);
    D.set_diag(0, 1, 2.f);
    D.set_diag(0, 2, 3.f);
    auto csr = D.to_csr();
    REQUIRE(csr.nrows == 3);
    CHECK(csr.get(0,0) == 1.f);
    CHECK(csr.get(1,1) == 2.f);
    CHECK(csr.get(2,2) == 3.f);
    CHECK(csr.get(0,1) == 0.f);
}

TEST_CASE("dense_to_csr round-trip", "[sparse][dense_to_csr]") {
    Matrix<float> M(3, 3, 0.f);
    M(0,0)=1.f; M(0,2)=2.f;
    M(1,1)=3.f;
    M(2,0)=4.f; M(2,2)=5.f;
    auto csr = dense_to_csr(M, 0.f);
    CHECK(csr.nnz() == 5);
    CHECK(csr.get(0,0) == 1.f);
    CHECK(csr.get(0,1) == 0.f);
    CHECK(csr.get(0,2) == 2.f);
    CHECK(csr.get(1,1) == 3.f);
    CHECK(csr.get(2,0) == 4.f);
    CHECK(csr.get(2,2) == 5.f);
}

TEST_CASE("amd_order: produces valid permutation", "[sparse][amd]") {
    std::vector<std::size_t> rows = {0,0,0,1,1,1,2,2,2,3,3,3};
    std::vector<std::size_t> cols = {0,1,3,0,1,2,1,2,3,0,2,3};
    std::vector<float>       vals(12, 1.f);
    vals[0]=vals[4]=vals[8]=vals[11]=4.f;
    vals[1]=vals[2]=vals[3]=vals[5]=vals[6]=vals[7]=vals[9]=vals[10]=-1.f;
    auto A = CsrMatrix<float>::from_triplets(4, 4, rows, cols, vals);
    auto perm = amd_order(A);
    REQUIRE(perm.size() == 4);
    // must be a valid permutation [0..3]
    std::vector<bool> seen(4, false);
    for (auto p : perm) {
        REQUIRE(p < 4);
        CHECK(!seen[p]);
        seen[p] = true;
    }
}

TEST_CASE("apply_permutation: reorders rows and cols", "[sparse][permutation]") {
    std::vector<std::size_t> rows = {0,1,2};
    std::vector<std::size_t> cols = {0,1,2};
    std::vector<float>       vals = {1.f,2.f,3.f};
    auto A = CsrMatrix<float>::from_triplets(3, 3, rows, cols, vals);
    std::vector<std::size_t> perm = {2, 0, 1};  // row/col reorder
    auto B = apply_permutation(A, perm);
    // B[0,0] = A[perm[0], perm[0]] = A[2,2] = 3
    CHECK(B.get(0, 0) == 3.f);
    CHECK(B.get(1, 1) == 1.f);
    CHECK(B.get(2, 2) == 2.f);
}

TEST_CASE("CsrMatrix: from_triplets coalesces duplicate entries", "[sparse][csr]") {
    std::vector<std::size_t> rows = {0, 0, 0, 1};
    std::vector<std::size_t> cols = {1, 1, 2, 1};
    std::vector<float> vals = {2.f, -0.5f, 3.f, 4.f};
    auto A = CsrMatrix<float>::from_triplets(2, 3, rows, cols, vals);

    REQUIRE(A.nnz() == 3);
    CHECK(A.get(0, 1) == Catch::Approx(1.5f));
    CHECK(A.get(0, 2) == Catch::Approx(3.f));
    CHECK(A.get(1, 1) == Catch::Approx(4.f));
}

TEST_CASE("CsrMatrix: from_triplets validates dimensions and indices", "[sparse][csr]") {
    {
        std::vector<std::size_t> rows = {0, 1};
        std::vector<std::size_t> cols = {0};
        std::vector<float> vals = {1.f, 2.f};
        CHECK_THROWS_AS(CsrMatrix<float>::from_triplets(2, 2, rows, cols, vals), std::invalid_argument);
    }
    {
        std::vector<std::size_t> rows = {0, 2};
        std::vector<std::size_t> cols = {0, 1};
        std::vector<float> vals = {1.f, 2.f};
        CHECK_THROWS_AS(CsrMatrix<float>::from_triplets(2, 2, rows, cols, vals), std::out_of_range);
    }
}

