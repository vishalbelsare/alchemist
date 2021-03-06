#ifndef SKYLARK_FACTORIZED_CG_HPP
#define SKYLARK_FACTORIZED_CG_HPP

/***
 * Factorized and Regularized CG Solver
 *
 * Solves argmin 1/(2n)*||A X - Y||_F^2 + lambda/2 ||X||_F^2
 * when A, Y, lambda are passed in (n is the height of A), by using CG
 * to get the solution to the equivalent system 
 * (A^T A + lambda*n I) X = A^T Y
 *
 * modified version of libskylark/algorithms/Krylov/CG.hpp
 ***/

#include "base/base.hpp"
#include "utility/elem_extender.hpp"
#include "utility/typer.hpp"
#include "utility/external/print.hpp"
#include "utility/timer.hpp"
#include "algorithms/Krylov/internal.hpp"
#include "algorithms/Krylov/precond.hpp"
#include "spdlog/spdlog.h"

namespace skylark { namespace algorithms {

/**
 * CG method.
 *
 * X should be allocated, and we use it as initial value.
 */
template<typename MatrixType, typename RhsType, typename SolType>
int factorizedCG(const MatrixType& A, const RhsType& Y, SolType& X, double
    lambda, const std::shared_ptr<spdlog::logger> log, krylov_iter_params_t params =
    krylov_iter_params_t(), const outplace_precond_t<RhsType, SolType>& M =
    outplace_id_precond_t<RhsType, SolType>()) {

#   if SKYLARK_HAVE_PROFILER
    boost::mpi::communicator comm = utility::get_communicator(A);
#   endif

    log->info("Arrived in CG solver");

    SKYLARK_TIMER_INITIALIZE(CG_SYMM_PROFILE);
    SKYLARK_TIMER_INITIALIZE(CG_PRECOND_APPLY_PROFILE);

    int ret;

    typedef typename utility::typer_t<MatrixType>::value_type value_type;
    typedef typename utility::typer_t<MatrixType>::index_type index_type;

    typedef MatrixType matrix_type;
    typedef RhsType rhs_type;
    typedef SolType sol_type;

    typedef utility::print_t<rhs_type> rhs_print_t;
    typedef utility::print_t<sol_type> sol_print_t;

    typedef utility::elem_extender_t<
        typename internal::scalar_cont_typer_t<rhs_type>::type >
        scalar_cont_type;

    bool log_lev1 = params.am_i_printing && params.log_level >= 1;
    bool log_lev2 = params.am_i_printing && params.log_level >= 2;

    /** Throughout, we will use n, d, k to denote the problem dimensions */
    index_type n = base::Height(A);
    index_type d = base::Width(A); 
    index_type k = base::Width(Y);
    rhs_type B(X);
    rhs_type hermIntermed(Y);
    base::Gemm(El::TRANSPOSE, El::NORMAL, value_type(1.0), A, Y, value_type(0.0), B);

    /** Set the parameter values accordingly */
    const value_type eps = 32*std::numeric_limits<value_type>::epsilon();
    if (params.tolerance<eps) params.tolerance=eps;
    else if (params.tolerance>=1.0) params.tolerance=(1-eps);
    else {} /* nothing */

    sol_type P(X);
    rhs_type R(B), Q(B);
    bool isprecond = !(M.is_id() && std::is_same<sol_type, rhs_type>::value);
    sol_type &Z =  !isprecond ? R : *(new sol_type(X));

    SKYLARK_TIMER_RESTART(CG_SYMM_PROFILE);
    base::Gemm(El::NORMAL, El::NORMAL, value_type(1.0), A, X, value_type(0.0), hermIntermed);
    //log->info("Gemm : {} by {} times {} by {} stored in {} by {}", A.Height(), A.Width(), X.Height(), X.Width(), hermIntermed.Height(), hermIntermed.Width());
    base::Gemm(El::TRANSPOSE, El::NORMAL, value_type(-1.0), A, hermIntermed, value_type(1.0), R);
    //log->info("Gemm : {} by {} times {} by {} stored in {} by {}", A.Width(), A.Height(), hermIntermed.Height(), hermIntermed.Width(), R.Height(), R.Width());
    base::Axpy(-value_type(lambda*n), X, R);
    //log->info("Axpy: {} by {} added to {} by {}", X.Height(), X.Width(), R.Height(), R.Width());
    SKYLARK_TIMER_ACCUMULATE(CG_SYMM_PROFILE);

    scalar_cont_type
        nrmb(internal::scalar_cont_typer_t<rhs_type>::build_compatible(k, 1, B));
    base::ColumnNrm2(B, nrmb);
    double total_nrmb = 0.0;
    for(index_type i = 0; i < k; i++) {
        total_nrmb += nrmb[i] * nrmb[i];
        log->info("nrmb[{}] = {}", i, nrmb[i]);
    }
    total_nrmb = sqrt(total_nrmb);
    scalar_cont_type ressqr(nrmb), rho(nrmb), rho0(nrmb), rhotmp(nrmb),
        alpha(nrmb), malpha(nrmb), beta(nrmb);
    base::ColumnDot(R, R, ressqr);

    for (index_type itn=0; itn<params.iter_lim; ++itn) {
        if (isprecond) {
            SKYLARK_TIMER_RESTART(CG_PRECOND_APPLY_PROFILE);
            M.apply(R, Z);
            SKYLARK_TIMER_ACCUMULATE(CG_PRECOND_APPLY_PROFILE);

            base::ColumnDot(R, Z, rho);
        } else {
            base::Copy(R, Z);
            rho = ressqr;
        }

        if (itn == 0)
            El::Zero(beta);
        else
            for(index_type i = 0; i < k; i++) 
                beta[i] = rho[i] / rho0[i];

        El::DiagonalScale(El::RIGHT, El::NORMAL, beta, P);
        base::Axpy(value_type(1.0), Z, P);

        // Compute Q = (A^TA + lambda*n I) P
        SKYLARK_TIMER_RESTART(CG_SYMM_PROFILE);
        base::Gemm(El::NORMAL, El::NORMAL, value_type(1.0), A, P, value_type(0.0), hermIntermed);
        //log->info("Gemm : {} by {} times {} by {} stored in {} by {}", A.Height(), A.Width(), P.Height(), P.Width(), hermIntermed.Height(), hermIntermed.Width());
        base::Gemm(El::TRANSPOSE, El::NORMAL, value_type(1.0), A, hermIntermed, value_type(0.0), Q);
        //log->info("Gemm : {} by {} times {} by {} stored in {} by {}", A.Height(), A.Width(), hermIntermed.Height(), hermIntermed.Width(), Q.Height(), Q.Width());
        base::Axpy(value_type(lambda*n), P, Q);
        //log->info("Axpy: {} by {} added to {} by {}", P.Height(), P.Width(), Q.Height(), Q.Width());
        SKYLARK_TIMER_ACCUMULATE(CG_SYMM_PROFILE);

        base::ColumnDot(P, Q, rhotmp);
        for(index_type i = 0; i < k; i++) {
            alpha[i] = rho[i] / rhotmp[i];
            malpha[i] = -alpha[i];
        }

        base::Axpy(alpha, P, X);
        base::Axpy(malpha, Q, R);

        rho0 = rho;

        base::ColumnDot(R, R, ressqr);

        int convg = 0;
        for(index_type i = 0; i < k; i++) {
            if (sqrt(ressqr[i]) < (params.tolerance*nrmb[i]))
                convg++;
        }

        if (log_lev2 && (itn % params.res_print == 0 || convg == k)) {
            double total_ressqr = 0.0;
            for(index_type i = 0; i < k; i++)
                total_ressqr += ressqr[i];
            double relres = sqrt(total_ressqr) / total_nrmb;
            log->info("CG: Iteration {}, Relres = {:.2e}, {} rhs converged", itn, relres, convg);
        }

        if(convg == k) {
            if (log_lev1)
              log->info("CG: Convergence!");
            ret = -1;
            goto cleanup;
        }
    }

    ret = -6;
    if (log_lev1)
      log->info("CG: No convergence within iteration limit.");

 cleanup:
    if (isprecond)
        delete &Z;

    SKYLARK_TIMER_PRINT(CG_SYMM_PROFILE, comm);
    SKYLARK_TIMER_PRINT(CG_PRECOND_APPLY_PROFILE, comm);

    return ret;
}

} } /** namespace skylark::algorithms */

#endif // SKYLARK_FACTORIZED_CG_HPP
