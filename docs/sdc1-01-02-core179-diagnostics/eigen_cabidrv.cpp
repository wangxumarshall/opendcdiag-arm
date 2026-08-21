// eigen_cabidrv.cpp — C-callable wrapper around Eigen's SimplicialCholesky
// numeric factorize + solve, compiled WITHOUT libstdc++ dependency.
//
// Constraint (per goal): the reproducer must not depend on any software library
// except libc. Eigen is header-only (no linked library) but its templates use
// operator new/delete and __throw_length_error, which live in libstdc++. To
// eliminate the libstdc++ dynamic dependency entirely, we:
//   1. compile with -fno-exceptions -fno-rtti (no exception runtime)
//   2. provide our OWN operator new/delete (-> malloc/free) and a stub
//      __throw_length_error (-> abort) so NO libstdc++ symbols are needed.
// The final binary, linked with -nostdlib++ against only libc, contains the
// exact Eigen machine code (triggering instruction stream) but links ONLY libc.
//
// The exported C ABI:
//   extern "C" void* escnew_solver();          // allocate SimplicialCholesky (placement)
//   extern "C" void  escfree_solver(void*);    // free
//   extern "C" int   escanalyze(void* s, const int* Ap, const int* Ai, const double* Ax, int N, int nnz);
//   extern "C" int   escfactorize(void* s, const int* Ap, const int* Ai, const double* Ax, int N, int nnz);
//   extern "C" int   escsolve(void* s, const double* b, double* x, int N);
//
#include <Eigen/Sparse>
#include <Eigen/Dense>
#include <vector>
#include <cstdlib>
#include <cstdio>

// ---- provide our own operator new/delete -> malloc/free (no libstdc++) ----
// Declared with C++ linkage so they satisfy _Znwm/_ZdlPvm.
void* operator new(size_t n) { return malloc(n ? n : 1); }
void operator delete(void* p, size_t) noexcept { free(p); }
void* operator new[](size_t n) { return malloc(n ? n : 1); }
void operator delete[](void* p, size_t) noexcept { free(p); }

// ---- stub for __throw_length_error (only reached on impossible alloc path) ----
// The mangled name is _ZSt20__throw_length_errorPKc
extern "C" {
void _ZSt20__throw_length_errorPKc(const char* msg) {
    (void)msg;
    std::fprintf(stderr, "length_error stub reached\n");
    std::abort();
}
}

// ---- type aliases (at namespace scope) ----
using SparseM = Eigen::SparseMatrix<double, Eigen::ColMajor, int>;
using SC = Eigen::SimplicialCholesky<SparseM>;

// ---- C ABI wrappers ----
extern "C" {

void* escnew_solver() {
    // SimplicialCholesky< SparseM, Eigen::LDLT > via placement new on malloc
    void* mem = malloc(sizeof(SC));
    if (!mem) return nullptr;
    return new (mem) SC();   // placement new
}

void escfree_solver(void* s) {
    if (!s) return;
    ((SC*)s)->~SC();
    free(s);
}

// build A from CSC triplets then analyzePattern
int escanalyze(void* s, const int* Ap, const int* Ai, const double* Ax, int N, int nnz) {
    SC* sc = (SC*)s;
    SparseM A(N, N);
    std::vector<Eigen::Triplet<double>> trips;
    trips.reserve(nnz);
    for (int j = 0; j < N; j++) {
        for (int p = Ap[j]; p < Ap[j+1]; p++) {
            int i = Ai[p];
            trips.push_back({i, j, Ax[p]});
        }
    }
    A.setFromTriplets(trips.begin(), trips.end());
    sc->analyzePattern(A);
    return 0;
}

int escfactorize(void* s, const int* Ap, const int* Ai, const double* Ax, int N, int nnz) {
    SC* sc = (SC*)s;
    SparseM A(N, N);
    std::vector<Eigen::Triplet<double>> trips;
    trips.reserve(nnz);
    for (int j = 0; j < N; j++) {
        for (int p = Ap[j]; p < Ap[j+1]; p++) {
            int i = Ai[p];
            trips.push_back({i, j, Ax[p]});
        }
    }
    A.setFromTriplets(trips.begin(), trips.end());
    sc->factorize(A);
    return (sc->info() == Eigen::Success) ? 0 : 1;
}

int escsolve(void* s, const double* b, double* x, int N) {
    SC* sc = (SC*)s;
    Eigen::Map<const Eigen::VectorXd> bv(b, N);
    Eigen::Map<Eigen::VectorXd> xv(x, N);
    xv = sc->solve(bv);
    return 0;
}

} // extern "C"
