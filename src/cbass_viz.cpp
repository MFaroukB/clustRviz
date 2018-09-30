#include "clustRviz.h"

// TODO - Consolidate CBASS and CBASS-VIZ
// Most of the internal logic is the same (modulo back-tracking vs fixed step size)

// [[Rcpp::export]]
Rcpp::List CBASS_VIZcpp(const Eigen::MatrixXd& X,
                        const Eigen::MatrixXd& D_row,
                        const Eigen::MatrixXd& D_col,
                        double lambda_init, // TODO: Change to gamma_init
                        const Eigen::VectorXd& weights_col,
                        const Eigen::VectorXd& weights_row,
                        double rho         = 1,
                        int max_iter       = 10000,
                        int burn_in        = 50,
                        int ti             = 15,
                        double t_switch    = 1.01,
                        int keep           = 10,
                        bool l1            = false){

  Eigen::Index n = X.rows();
  Eigen::Index p = X.cols();

  // Typically, our weights are "sparse" (i.e., mostly zeros) because we
  // drop small weights to achieve performance.
  Eigen::Index num_row_edges = D_row.rows();
  Eigen::Index num_col_edges = D_col.cols();

  /// Set-up storage for CBASS iterates

  // In order to pre-allocate storage arrays, we need to estimate the number of
  // steps with fusions we will encounter. Dendrograms are the common case and
  // we are bi-clustering, so we expect O(n + p) fusions. It's a bit
  // cheaper to drop observations than to extend the internal buffers of our
  // storage objects, so we use 1.5(n + p) for now
  Eigen::Index buffer_size = 1.5 * (n + p);

  // Primal variable
  Eigen::MatrixXd U = X;
  Eigen::MatrixXd UPath(n * p, buffer_size); // Storage (for values to return to R)
  // We cannot directly copy matrices into columns of our return object since
  // a matrix isn't a vector, so we map the same storage into a vector which we
  // can then insert into the storage matrix to be returned to R
  Eigen::Map<Eigen::VectorXd> u_vec = Eigen::Map<Eigen::VectorXd>(U.data(), n * p);

  // 'Split' variable for row ADMM
  Eigen::MatrixXd V_row = D_row * X;
  Eigen::MatrixXd V_rowPath(p * num_row_edges, buffer_size); // Storage (for values to return to R)
  Eigen::Map<Eigen::VectorXd> v_row_vec = Eigen::Map<Eigen::VectorXd>(V_row.data(), p * num_row_edges);

  // (Scaled) dual variable for row ADMM
  Eigen::MatrixXd Z_row = V_row;

  // 'Split' variable for column ADMM
  Eigen::MatrixXd V_col = D_col.transpose() * X.transpose();
  Eigen::MatrixXd V_colPath(n * num_col_edges, buffer_size); // Storage (for values to return to R)
  Eigen::Map<Eigen::VectorXd> v_col_vec = Eigen::Map<Eigen::VectorXd>(V_col.data(), n * num_col_edges);

  // (Scaled) dual variable for row ADMM
  Eigen::MatrixXd Z_col = V_col;

  U.transposeInPlace(); // See note on transpositions below
  UPath.col(0) = u_vec;
  U.transposeInPlace();
  V_row.transposeInPlace();
  V_rowPath.col(0) = v_row_vec;
  V_row.transposeInPlace();
  V_col.transposeInPlace();
  V_colPath.col(0) = v_col_vec;
  V_col.transposeInPlace();

  // Regularization level
  double gamma     = lambda_init;          // Working copy
  double gamma_old = lambda_init;
  Eigen::VectorXd gamma_path(buffer_size); // Storage (to be returned to R)
  gamma_path(0) = lambda_init;

  // Fusions
  Eigen::MatrixXi v_row_zeros_path(num_row_edges, buffer_size); // Storage (to be returned to R)
  v_row_zeros_path.col(0).setZero();
  Eigen::MatrixXi v_col_zeros_path(num_col_edges, buffer_size); // Storage (to be returned to R)
  v_col_zeros_path.col(0).setZero();

  /// END Preallocations

  // PreCompute chol(I_n + rho D_row^T D_row) and chol(I_p + rho D_col D_col^T) for easy inversions in the ADMM primal update steps
  Eigen::MatrixXd IDTD_row = rho * D_row.transpose() * D_row + Eigen::MatrixXd::Identity(n, n);
  Eigen::LLT<Eigen::MatrixXd> row_primal_solver; row_primal_solver.compute(IDTD_row);

  Eigen::MatrixXd IDDT_col = rho * D_col * D_col.transpose()  + Eigen::MatrixXd::Identity(p, p);
  Eigen::LLT<Eigen::MatrixXd> col_primal_solver; col_primal_solver.compute(IDDT_col);

  // The DLPA (on which CBASS is based) adds two auxiliary variables -- P & Q --
  // with the same dimensions as the optimization variable, initialized to zero
  Eigen::MatrixXd P = Eigen::MatrixXd::Zero(n, p);
  Eigen::MatrixXd Q = Eigen::MatrixXd::Zero(n, p);

  // Book-keeping variables
  // Number of iterations stored, total iteration count, number of column fusions, number of row fusions
  Eigen::Index path_iter  = 1; // path_iter is next column to put something in,
  Eigen::Index iter       = 0; // so we start at 1 since we put data in column 0 above
  Eigen::Index nzeros_old_row = 0;
  Eigen::Index nzeros_new_row = 0;
  Eigen::Index nzeros_old_col = 0;
  Eigen::Index nzeros_new_col = 0;

  Eigen::ArrayXi v_row_zeros_old(num_row_edges); // 0/1 arrays indicating edge fusion
  Eigen::ArrayXi v_row_zeros_new(num_row_edges);
  Eigen::ArrayXi v_col_zeros_old(num_col_edges);
  Eigen::ArrayXi v_col_zeros_new(num_col_edges);

  // We begin CBASS-VIZ by taking relatively large step sizes (t = 1.1)
  // but once we get to an "interesting" part of the path, we switch to
  // smaller step sizes (as determined by t_switch)
  double t = 1.1;

  while( ((nzeros_new_row < num_row_edges) | (nzeros_new_col < num_col_edges)) & (iter < max_iter) ){
    ClustRVizLogger::info("Beginning iteration k = ") << iter + 1;
    ClustRVizLogger::debug("gamma = ") << gamma;

    nzeros_old_row = nzeros_new_row; // Update fusion counts
    nzeros_old_col = nzeros_new_col;
    v_row_zeros_old = v_row_zeros_new;
    v_col_zeros_old = v_col_zeros_new;

    // Keep previous U, P, Q, V, and Z variables to use in back-tracking
    Eigen::MatrixXd U_old     = U;
    Eigen::MatrixXd P_old     = P;
    Eigen::MatrixXd Q_old     = Q;
    Eigen::MatrixXd V_row_old = V_row;
    Eigen::MatrixXd Z_row_old = Z_row;
    Eigen::MatrixXd V_col_old = V_col;
    Eigen::MatrixXd Z_col_old = Z_col;

    // VIZ book-keeping
    bool rep_iter = true;
    Eigen::Index try_iter = 0;

    double gamma_upper = gamma;
    double gamma_lower = gamma_old;

    // This is the core CBASS-VIZ Logic:
    //
    // We take CBASS-type (one iteration of each ADMM step) steps, but instead of
    // proceeding with a fixed step-size update, we include a back-tracking step
    // (described in more detail below)
    //
    //
    while(rep_iter){
      /// Row-fusion iterations
      // Primal Update
      Eigen::MatrixXd T = row_primal_solver.solve(U_old + P_old + rho * D_row.transpose() * (V_row_old - Z_row_old));
      Eigen::MatrixXd DT = D_row * T;
      ClustRVizLogger::debug("T = ") << T;

      // Copy Update
      Eigen::MatrixXd DTZ = DT + Z_row_old;
      MatrixProx(DTZ, V_row, gamma / rho, weights_row, l1);
      ClustRVizLogger::debug("V_row = ") << V_row;

      // Dual Update
      Z_row = Z_row_old + DT - V_row;
      ClustRVizLogger::debug("Z_row = ") << Z_row;
      /// END Row-fusion iterations

      P = P_old + U_old - T;
      Eigen::MatrixXd TQT = T + Q_old; TQT.transposeInPlace();

      /// Column-fusion iterations
      // Primal Update
      Eigen::MatrixXd S = col_primal_solver.solve(TQT + rho * D_col * (V_col_old - Z_col_old));
      Eigen::MatrixXd DTS = D_col.transpose() * S;
      ClustRVizLogger::debug("S = ") << S;

      // Copy Update
      Eigen::MatrixXd DTSZ = DTS + Z_col_old;
      MatrixProx(DTSZ, V_col, gamma / rho, weights_col, l1);
      ClustRVizLogger::debug("V_col = ") << V_col;

      // Dual Update
      Z_col = Z_col_old + DTS - V_col;
      ClustRVizLogger::debug("Z_col = ") << Z_col;
      /// END Column-fusion iterations

      U = S.transpose();
      Q = Q_old + T - U;

      // Identify row fusions (rows of V_row which have gone to zero)
      Eigen::VectorXd v_row_norms = V_row.rowwise().squaredNorm();

      for(Eigen::Index i = 0; i < num_row_edges; i++){
        v_row_zeros_new(i) = v_row_norms(i) == 0;
      }
      nzeros_new_row = v_row_zeros_new.sum();

      // Identify column fusions (rows of V_col which have gone to zero)
      // Remember, V_col and Z_col are internal to the "transposed prox" sub-problem
      // so everything is reversed of what we'd expect
      Eigen::VectorXd v_col_norms = V_col.rowwise().squaredNorm();

      for(Eigen::Index i = 0; i < num_col_edges; i++){
        v_col_zeros_new(i) = v_col_norms(i) == 0;
      }
      nzeros_new_col = v_col_zeros_new.sum();

      ClustRVizLogger::debug("Number of row fusions identified ") << nzeros_new_row;
      ClustRVizLogger::debug("Number of column fusions identified ") << nzeros_new_col;

      /// END CBASS steps

      try_iter++; // Increment internal iteration count (used to check stopping below)

      // Safety check - only so many iterations of the inner loop before we move on
      if(try_iter > ti){
        break;
      }

      if( (nzeros_new_row == nzeros_old_row) & (nzeros_new_col == nzeros_old_col) & (try_iter == 1) ){
        // If the sparsity pattern (number of fusions) hasn't changed, we have
        // no need to back-track (we didn't miss any fusions) so we can go immediately
        // to the next iteration.
        rep_iter = false;
        ClustRVizLogger::debug("No fusions identified -- continuing to next step.");
      } else if((nzeros_new_row > nzeros_old_row + 1) | (nzeros_new_col > nzeros_old_col + 1) ){
        // If we see two (or more) new fusions, we need to back-track and figure
        // out which one occured first
        // (NB: we don't need a 'cross' ordering of row and global fusions, so we
        //  use a separate check for each, instead of a check on the sum)
        v_col_zeros_new = v_col_zeros_old;
        v_row_zeros_new = v_row_zeros_old;
        if(try_iter == 1){
          gamma = 0.5 * (gamma_lower + gamma_upper);
        } else{
          gamma_upper = gamma;
          gamma = 0.5 * (gamma_lower + gamma_upper);
        }
        ClustRVizLogger::debug("Too many fusions -- backtracking.");
      } else if( (nzeros_new_row == nzeros_old_row) & (nzeros_new_col == nzeros_old_col) ){
        // If we don't observe any new fusions, we take another iteration without
        v_col_zeros_new = v_col_zeros_old;
        v_row_zeros_new = v_row_zeros_old;
        gamma_lower = gamma;
        gamma = 0.5 * (gamma_lower + gamma_upper);
        ClustRVizLogger::debug("Fusion not isolated -- moving forward.");
      } else{
        // If we see exactly one new fusion, we have a good step size and exit
        // the inner back-tracking loop
        rep_iter = false;
        ClustRVizLogger::debug("Good iteration - continuing to next step.");
      }

    }

    // If we have gotten to the "lots of fusions" part of the solution space, start
    // taking smaller step sizes.
    if( (nzeros_new_col > 0) | (nzeros_new_row >0) ){
      t = t_switch;
    }

    // If we have seen a fusion or are otherwise interested in keeping this iteration,
    // add values to our storage buffers
    if( (nzeros_new_row != nzeros_old_row) | (nzeros_new_col != nzeros_old_col) | (iter % keep == 0) ){

      // Before we can store values, we need to make sure we have enough buffer space
      if(path_iter >= buffer_size){
        ClustRVizLogger::info("Resizing storage from ") << buffer_size << " to " << 2 * buffer_size << " iterations.";
        buffer_size *= 2; // Double our buffer sizes
        UPath.conservativeResize(UPath.rows(), buffer_size);
        V_rowPath.conservativeResize(V_rowPath.rows(), buffer_size);
        V_colPath.conservativeResize(V_colPath.rows(), buffer_size);
        gamma_path.conservativeResize(buffer_size);
        v_row_zeros_path.conservativeResize(v_row_zeros_path.rows(), buffer_size);
        v_col_zeros_path.conservativeResize(v_col_zeros_path.rows(), buffer_size);
      }

      // Store values
      //
      // FIXME -- The post-processing code assumes output in the form of
      //          Chi, Allen, Baraniuk (Biometrics '17) which more or less is equivalent
      //          to vec(U^T) and vec(V^T) instead of what we're using internally
      //          here.
      //
      //          It should be re-written, but until it is, we can achieve the
      //          same result by transposing and un-transposing the data internally
      //          before copying it to our storage buffers
      //
      //          Obviously, this burns some cycles so it would be better to avoid this
      U.transposeInPlace(); V_row.transposeInPlace(); V_col.transposeInPlace();
      UPath.col(path_iter)            = u_vec;
      V_rowPath.col(path_iter)        = v_row_vec;
      V_colPath.col(path_iter)        = v_col_vec;
      gamma_path(path_iter)           = gamma;
      v_row_zeros_path.col(path_iter) = v_row_zeros_new;
      v_col_zeros_path.col(path_iter) = v_col_zeros_new;
      U.transposeInPlace(); V_row.transposeInPlace(); V_col.transposeInPlace();

      path_iter++;

      // Update gamma old as the basis for future back-tracking
      gamma_old = gamma;
    }

    iter++;
    if(iter >= burn_in){
      gamma *= t;
    }

    if((iter % CLUSTRVIZ_CHECK_USER_INTERRUPT_RATE) == 0){
      Rcpp::checkUserInterrupt();
    }
  }

  if(iter >= max_iter){
    ClustRVizLogger::warning("CBASS-VIZ ended early -- `max_iter` reached");
  }

  // Now that we are done, we can "drop" unused buffer space before returning to R
  //
  // See explanatory comment in carp_viz.cpp
  UPath.conservativeResize(UPath.rows(), path_iter);
  V_rowPath.conservativeResize(V_rowPath.rows(), path_iter);
  V_colPath.conservativeResize(V_colPath.rows(), path_iter);
  gamma_path.conservativeResize(path_iter);
  v_row_zeros_path.conservativeResize(v_row_zeros_path.rows(), path_iter);
  v_col_zeros_path.conservativeResize(v_col_zeros_path.rows(), path_iter);

  // Wrap up our results and pass them to R
  return Rcpp::List::create(Rcpp::Named("u.path")          = UPath,
                            Rcpp::Named("v.row.path")      = V_rowPath,
                            Rcpp::Named("v.col.path")      = V_colPath,
                            Rcpp::Named("v.row.zero.inds") = v_row_zeros_path,
                            Rcpp::Named("v.col.zero.inds") = v_col_zeros_path,
                            Rcpp::Named("lambda.path")     = gamma_path); // TODO - Change lambda -> gamma in R code
}
