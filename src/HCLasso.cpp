/*
 * [Anew, Loss_new] = mexHCLasso(G,W,A,lambda)
 *
 * ---Input---
 *
 * G (k,k) full double matrix
 * W (k,d) full double matrix
 * A (k,d) full double matrix
 *
 * ---Output---
 *
 * Anew       full (k,d) matrix
 * Loss_new   double number
 *
 * ---Algorithm---
 *
 * For each clm of W, denoted by w, solve
 *
 * min     a' * G * a - 2 * w' * a + lambda * norm(a,1),
 * sb.to.  1>= a_i >= 0
 *
 * i.e. a is in the hypercube of R^k.
 *
 * where the starting value(the estimate of the minimizer)
 * is stored in the corresponding clms of A(Anew).
 *
 * The implementation is based on SPG.
 *
 * Parallel Computing is supported by OpenMP.
 *
 * BLAS routines are embedded in for operations on matrices & vectors.
 *
 * ---Default parameters---
 *
 * convergence accuracy: 1e-10
 * suffcient descent criterion in line search: 1e-3
 * memory size in non-mono. descent checking: 10
 * max #threads : 1
 * dynamic allocating threads: yes
 *
 */


#include <stdio.h>
#include <cstddef>
#include "dynblas.h"
#include <omp.h>
#include <limits>
#include <Rcpp.h>
using namespace std;
using namespace Rcpp;

#define MEM_OLD_VALUES 10
#define MAX_NUM_THREADS 1
#define OPT_TOL   1e-10
#define SUFF_DESC 1e-3
#define DYNAMIC_THREAD 1

/*
 * compute the absolute value of x
 * write this function explicitly to avoid compiler issue
 */
inline double dabs(double x){
    if ( x < 0 )
        x = -x;
    return x;
}

/* compute the constant Hess and Beta */
inline void SetInput(double* G, double* w, double lambda, double* Hess, double* beta, ptrdiff_t k){
    
    // beta = 2 * w - lambda:
    // don't know BLAS function for subtracting a scalar for every
    // component of a vecotr
    for( int i = 0; i < k ; i++){
        beta[i] = 2 * w[i] - lambda;
    }
    
    // Hess = 2 * G :
    // Hess = G;
    // Hess = 1 * G + Hess;
    ptrdiff_t ione = 1;
    double one = 1.0;
    ptrdiff_t ks = (ptrdiff_t) (k * k);
    dcopy(&ks, G, &ione, Hess, &ione);
    daxpy(&ks, &one, G, &ione, Hess, &ione);
    
    //int ik = (int) k;
    //for(int i = 0; i < ik; i++ ){
    //	mexPrintf("%f\n",beta[i]);
    //}
    
    //for(int i = 0; i < ik; i++ ){
    //	for (int j = 0; j < ik; j++ ){
    //		mexPrintf("%f\t", Hess[i + j * k]);
    //	}
    //	mexPrintf("%\n");
    //}
}

/* compute the gradient at x */
inline void GetGrad(double* grad, double* Hess, double* beta, double* x, ptrdiff_t k){
    // grad = 2 * G * x - beta = Hess * x - beta;
    
    ptrdiff_t ione = 1;
    char* chn = (char*)"N";
    double one  = 1.0;
    double zero = 0.0;
    double mone = -1.0;
    
    // step 1: grad =  Hess * x ;
    dgemv(chn, &k, &k, &one, Hess, &k, x, &ione, &zero, grad, &ione);
    
    // step 2: grad =  -1 * beta + grad;
    daxpy(&k, &mone, beta, &ione, grad, &ione);
    
    //int ik = (int) k;
    //for(int i = 0; i < ik; i++ ){
    //	mexPrintf("%f\n",grad[i]);
    //}
    
}

/* compute the objective value at x */
inline double ObjValue(double* G, double* beta, double* x, double* tmp, ptrdiff_t k){
    // x' * G * x - beta' * x = x' * (G * x - beta)
    ptrdiff_t ione = 1;
    char* chn = (char*)"N";
    double one  = 1.0;
    double zero = 0.0;
    double mone = -1.0;
    
    // step 1: tmp =  G * x ;
    dgemv(chn, &k, &k, &one, G, &k, x, &ione, &zero, tmp, &ione);
    
    // step 2: tmp =  -1 * beta + tmp;
    daxpy(&k, &mone, beta, &ione, tmp, &ione);
    
    // step 3: f = tmp' * x;
    double f = (double)ddot(&k, x, &ione, tmp, &ione);
    
    return f;
}

/* compute the BB parameter */
inline double GetAlpha(double* x, double* x_old, double* g, double* g_old, double* tmp, double* tmp1, ptrdiff_t k){
    
    // alpha = (x - x_old)' * (x - x_old)  /  [ (x - x_old)' * (g - g_old)];
    
    ptrdiff_t ione = 1;
    //double one  = 1.0;
    //double zero = 0.0;
    double mone = -1.0;
    
    // tmp = x - x_old
    // step 1: copy x to tmp
    dcopy(&k, x, &ione, tmp, &ione);
    // step 2: tmp =  -1 * x_old + tmp;
    daxpy(&k, &mone, x_old, &ione, tmp, &ione);
    
    // tmp1 = g - g_old
    // step 1: copy g to tmp1
    dcopy(&k, g, &ione, tmp1, &ione);
    // step 2: tmp1 =  -1 * g_old + tmp1;
    daxpy(&k, &mone, g_old, &ione, tmp1, &ione);
    
    // numerator =  tmp' * tmp
    double numerator = (double)ddot(&k, tmp, &ione, tmp, &ione);
    
    // denominator = tmp' * tmp1;
    double denominator = (double)ddot(&k, tmp, &ione, tmp1, &ione);
    
    double alpha = numerator / denominator;
    
    return alpha;
}

/* project x to the hypercube -> output f */
inline void ProjHyperCube(double* f, double* x, int m) {
    for(int i = 0; i< m; i++){
        if (x[i] < 0)
            f[i] = 0;
        else if (x[i] > 1)
            f[i] = 1;
        else
            f[i] = x[i];
    }
}

/* compute the projected step */
inline void GetProjStep(double* d, double* x, double* g, double alpha, ptrdiff_t k) {
    // d = Proj(x - alpha * g) - x;
    
    ptrdiff_t ione = 1;
    double mone = -1.0;
    
    //Step 1:  d = x;
    dcopy(&k, x, &ione, d, &ione);
    
    //Step 2:  d = -alpha*g + d;
    double malpha = -alpha;
    daxpy(&k, &malpha, g, &ione, d, &ione);
    
    //Step 3: d = ProjHyperCube(d)
    ProjHyperCube(d, d, (int)k);
    
    //step 4: d = d - x;
    daxpy(&k, &mone, x, &ione, d, &ione);
    
    //int ik = (int) k;
    //for(int i = 0; i < ik; i++ ){
    //	mexPrintf("%f\n",d[i]);
    //}
}

/* compute the directional derivative */
inline double GetDirectDerivative(double* g, double* d, ptrdiff_t k){
    ptrdiff_t ione = 1;
    double sum = (double)ddot(&k, g, &ione, d, &ione);
    return sum;
}

/* compute norm(x,1) for a vector x */
inline double NormOne(double* x, ptrdiff_t k){
    ptrdiff_t ione = 1;
    double sum = dasum(&k, x, &ione);
    return sum;
}

/***  Solve Lasso Problem on Hypercube by SPG ***/
void HCLasso(double* G, double* w, double* a0, double lambda, ptrdiff_t k,
        double* Hess, double* beta,
        double* x, double* x_old, double* g, double* g_old, double* d,
        double* old_fvals, double* tmp, double* tmp1,
        double* ahat, double* fhat){
    /********
     *
     * solve:  min_a   a'* G * a - 2 * w'* a + lambda * norm(a,1);
     *         sb.to.  a >= 0
     *
     * ---Input---
     *
     * G,w    - quandratic form
     * a0     - starting value
     * lambda - regression parameter
     * k      - length of a0
     *
     * ---Temporary Variables---
     *
     * Hess  - Hessian = 2 * G, constant
     * beta  - (2 * w - lambda), constant
     * x     - current solution
     * x_old - previous solution
     * g     - current gradient  = Hess * x - beta
     * g_old - previous gradient  = Hess * x - beta
     * d     - descent direction
     *
     * old_fvals - for non-monotonically descent
     * tmp,tmp1  - for BLAS
     *
     * ---Output---
     *
     * ahat - minimizer
     * fhat - objective value at ahat
     *
     *******/
    
    ptrdiff_t ione = 1;
    double one = 1.0;
    double mone = -1.0;
    double zero = 0.0;
    char* chn = (char*)"N";
    
    /*** Initialiation ***/
    
    // set parameter
    double optTol = OPT_TOL;
    double suffDec = SUFF_DESC;
    double f;    // objective value at current solution
    double fmin; // minimum. objective value in the sequence generated by SPG
    // memory for non-monotone line search
    for(int i = 0; i < MEM_OLD_VALUES; i++) {
        old_fvals[i] = -std::numeric_limits<double>::max();
    }
    
    // set Hessian and beta
    SetInput(G, w, lambda, Hess, beta, k);
    
    // get starting point a0, gradient & fval
    dcopy(&k, a0, &ione, x, &ione);
    GetGrad(g, Hess, beta, x, k);
    f = ObjValue(G, beta, x, tmp, k);
    fmin = f;
    
    // copy to estimate
    dcopy(&k, x, &ione, ahat, &ione);
    fhat[0] = fmin;
    
    /*** SPG Loop ***/
    
    double alpha; // BB parameter
    double gtd; // Directional Derivative
    double t; //stepsize;
    double f_ref; // reference function value in non-monotone linear search
    double Linear, Quad; // ingredient to compute new function value;
    double factor; // for linear search, factor to reduce stepsize
    double Norm1_dx; // ||dx||_1, for linear search and as stopping criterion
    double linear, quad, red_f, f_tmp, norm1_dx; //temporary variable in linear search
    
    int iter = 0;
    int itermax = 500;
    while (1){
        
        //** Compute Step Direction
        if (iter == 0)
            alpha = 1;
        else{
            alpha = GetAlpha(x, x_old, g, g_old, tmp, tmp1, k);
            if (alpha <= 1e-10 || alpha > 1e10) {
                alpha = 1;
            }
        }
        
        //** Compute the projected step
        GetProjStep(d, x, g, alpha, k);
        
        
        //** Check that Progress can be made along the direction
        gtd = GetDirectDerivative(g, d, k);
        
        if (gtd > -optTol){
            //mexPrintf("Directional Derivative below optTol\n%f", gtd);
            break;
        }
        
        //** Backtracking Line Search
        // Select Initial Guess to step length
        if (iter == 0){
            t = 1/NormOne(g, k);
            t =  (t > 1) ? 1 : t;
        }
        else{
            t = 1;
        }
        
        // Get the reference function value for non-monotone condition:
        // __update the old_values memorized
        if (iter < MEM_OLD_VALUES)
            old_fvals[iter] = f;
        else{
            for(int i = 0; i < MEM_OLD_VALUES-1; i++){
                old_fvals[i] = old_fvals[i+1];
            }
            old_fvals[MEM_OLD_VALUES-1] = f;
        }
        
        // __find f_ref = max(old_fvals);
        f_ref = old_fvals[0];
        for(int i = 1; i < MEM_OLD_VALUES; i++){
            if (f_ref < old_fvals[i])
                f_ref = old_fvals[i];
        }
        
        // ingredients for computing (f_new - f) based on stepsize t:
        // __dx = t * d; Linear = g' * dx; Quad = dx' * Hess * dx;
        // __equivalently, Linear = t * g' * d = t * gtd; Quad = t^2 * d' * Hess * d;
        Linear = t * gtd;
        dgemv(chn, &k, &k, &one, Hess, &k, d, &ione, &zero, tmp, &ione);
        Quad = (double)ddot(&k, d, &ione, tmp, &ione);
        Quad = Quad * t * t;
        
        // __|dx||_1
        Norm1_dx = t * NormOne(d, k);
        //mexPrintf("%f\n\n", Norm1_dx);
        
        // stepsize selection
        factor = 1;
        norm1_dx = Norm1_dx * factor;
        while (1) {
            
            //__compute (f_new - f)
            linear = Linear * factor;
            quad = Quad * factor * factor;
            red_f = 0.5 * quad + linear;
            f_tmp = f + red_f;
            
            if (f_tmp < f_ref + suffDec * linear) {
                //__get sufficient descent
                t = t * factor;
                norm1_dx = Norm1_dx * factor;
                break;
            }
            else {
                //__Evaluate New Stepsize
                //__t = t * 0.5; -> t0 fixed; factor = factor * 0.5; t = t0 * factor;
                factor = factor * 0.5;
            }
            
            //__Check whether step has become too small
            if (Norm1_dx * factor < optTol || t == 0) {
                //    mexPrintf("Line Search failed\n");
                t = 0;
                norm1_dx = 0;
                red_f = 0;
                break;
            }
            
        }
        
        //** Take Step
        
        /*
         * x_old = x;
         * x = x + t * d;
         *
         * first copy x to x_old
         * then x = t * d + x;
         *
         */
        dcopy(&k, x, &ione, x_old, &ione);
        daxpy(&k, &t, d, &ione, x, &ione);
        
        /*
         * g_old = g;
         * g = compute grad(x);
         *
         * first copy g to g_old
         * then compute new gradient
         *
         */
        dcopy(&k, g, &ione, g_old, &ione);
        GetGrad(g, Hess, beta, x, k);
        
        // new objective value and iteration index
        f = f + red_f;
        iter = iter + 1;
        
        //** keep track of the minimum value attained
        if ( f < fmin ){
            fmin = f; // update
            // copy to the estimate
            dcopy(&k, x, &ione, ahat, &ione);
            fhat[0] = fmin;
        }
        
        //** Check 1st order optimality condition
        // tmp = ProjHyperCube(x-g)-x;
        dcopy(&k, x, &ione, tmp, &ione);
        daxpy(&k, &mone, g, &ione, tmp, &ione);
        ProjHyperCube(tmp, tmp, (int)k);
        daxpy(&k, &mone, x, &ione, tmp, &ione);
        
        if ( NormOne(tmp, k) < optTol ){
            // mexPrintf("First-Order Optimality Conditions Below optTol\n");
            break;
        }
        
        if (norm1_dx < optTol ) {
            //  mexPrintf("***********************norm_1: \t %f",norm1_dx);
            break;
        }
        
        if ( dabs(red_f) < optTol ) {
            //   mexPrintf("***************red_f: \t %f \t optTol: \t %.10f ",dabs(red_f),optTol);
            break;
        }
        
        if( iter > itermax ) {
            //   mexPrintf("***********update T SPG: Reach iteration limits.");
            break;
        }
    }
}


/*** Parallel Computing ***/
void spawn_threads(double* G, double* W, double* A, double lambda, ptrdiff_t k, int d, double* Anew, double* Loss_new) {
    
    /* temporary variables */
    double* Hess       = (double*)malloc(MAX_NUM_THREADS * k * k * sizeof(double));
    double* beta       = (double*)malloc(MAX_NUM_THREADS * k * sizeof(double));
    double* x          = (double*)malloc(MAX_NUM_THREADS * k * sizeof(double));
    double* x_old      = (double*)malloc(MAX_NUM_THREADS * k * sizeof(double));
    double* g          = (double*)malloc(MAX_NUM_THREADS * k * sizeof(double));
    double* g_old      = (double*)malloc(MAX_NUM_THREADS * k * sizeof(double));
    double* dsct       = (double*)malloc(MAX_NUM_THREADS * k * sizeof(double));
    double* old_fvals  = (double*)malloc(MAX_NUM_THREADS * MEM_OLD_VALUES * sizeof(double));
    double* tmp        = (double*)malloc(MAX_NUM_THREADS * k * sizeof(double));
    double* tmp1       = (double*)malloc(MAX_NUM_THREADS * k * sizeof(double));
    double* fhat       = (double*) malloc(MAX_NUM_THREADS * sizeof(double));
    
    if ( Hess == NULL || beta == NULL || x == NULL || x_old == NULL || g == NULL || g_old == NULL ||
            dsct == NULL || old_fvals == NULL || tmp == NULL || tmp1 == NULL || fhat == NULL ) {
        //mexErrMsgTxt("Out of memory.");

    }
    // construct independet subproblems
    omp_set_num_threads(MAX_NUM_THREADS);
    omp_set_dynamic(DYNAMIC_THREAD);
    int j = 0;
    double loss = 0.0;
    //#pragma omp parallel for private(j), reduction(+: loss)
    for(j = 0; j < d; j++){
        int id = omp_get_thread_num();
        
        //int id = 0;
        HCLasso(G, W + j * k, A + j * k, lambda, k,
                Hess + id * (k * k), beta + id * k,
                x + id * k, x_old + id * k, g + id * k, g_old + id * k, dsct + id * k,
                old_fvals + id * k, tmp + id * k, tmp1 + id * k,
                Anew + j * k, fhat + id);
        loss = loss + *(fhat+id);
    }
    
    Loss_new[0] = loss;
    
    free(fhat);
    free(tmp1);
    free(tmp);
    free(old_fvals);
    free(dsct);
    free(g_old);
    free(g);
    free(x_old);
    free(x);
    free(beta);
    free(Hess);
    
}



//[[Rcpp::export]]
List RHLasso(NumericMatrix  Ginp, NumericMatrix  Winp, NumericMatrix  Ainp, NumericVector l)
{

	Rcpp::NumericMatrix Gi(clone(Ginp));
	Rcpp::NumericMatrix Wi(clone(Winp));
	Rcpp::NumericMatrix Ai(clone(Ainp));

	double* Gptr =  Gi.begin();
	double* Wptr =  Wi.begin();
	double* Aptr =  Ai.begin();

	double lambda = Rcpp::as<double>(l);

	ptrdiff_t k = (ptrdiff_t) Wi.nrow();
	int d = Wi.ncol();

    /* create the output data */
    double* Anew = NULL;
    double* Loss_new = NULL;
    //double* iters = NULL;

    Rcpp::NumericMatrix newA((int)k,d);
    Anew = newA.begin();


    Rcpp::NumericMatrix newLoss(1,1);
    Loss_new = newLoss.begin();

    //printf("Starting threads\n");
    /* parallel computing */
    spawn_threads(Gptr, Wptr, Aptr, lambda, k, d, Anew, Loss_new);

    //printf("%f\n", newLoss[0]);
    //printf("%f\n", NumIters[0]);
    //printf("%f\n", newA[0]);

	List result = List::create(
			Named("A") = wrap(newA),
			Named("Loss") = wrap(newLoss)
			);

	return(result);
}
