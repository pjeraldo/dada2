/*
Pval.cpp contains the functions related to calculating the abundance and singleton pvals in DADA.
*/

#include <Rcpp.h>
#include "dada.h"

// [[Rcpp::interfaces(r, cpp)]]

// Calculate abundance pval for given reads and expected number of reads
double calc_pA(int reads, double E_reads) {
  double norm, pval=1.;
  
  // Calculate norm (since conditioning on sequence being present).
  norm = (1.0 - exp(-E_reads));
  if(norm < TAIL_APPROX_CUTOFF) {
    norm = E_reads - 0.5*E_reads*E_reads; 
    // Assumption: TAIL_APPROX_CUTOFF is small enough to terminate taylor expansion at 2nd order
  }
  
  // Calculate pval from poisson cdf.
  if(IMPLEMENTATION == 'R') {
    Rcpp::IntegerVector n_repeats(1);
    n_repeats(0) = reads-1;
    Rcpp::NumericVector res = Rcpp::ppois(n_repeats, E_reads, false);  // lower.tail = false
    pval = Rcpp::as<double>(res);
  }          
/*      else { // C implementation
    double gslval = 1 - gsl_cdf_poisson_P(reads-1, mu);
    double minval = (pval < gslval) ? pval : gslval;
    if( fabs(pval-gslval)/minval > 1e-5 && fabs(pval-gslval) > 1e-25 ) {
      Rcpp::Rcout << "Pval disagreement (gsl/R) for mu=" << mu << " and n=" << reads-1 << ": " << gslval << ", " << pval << "\n";
    }
  }  // THIS MUST BE CHANGED. DOES NOT LIMIT APPROPRIATELY!!!! */
  
  pval = pval/norm;
  return pval;
}

// Find abundance pval from a Fam in a Bi
double get_pA(Fam *fam, Bi *bi) {
  double E_reads, norm, pval = 1.;
  
  if(fam->reads < 1) {
    printf("Warning: No or negative reads (%i) in fam.\n", fam->reads);
    pval=1.;
  } 
  else if(fam->reads == 1) {   // Singleton. No abundance pval.
    pval=1.;
  } 
  else if(!(fam->sub)) { // Outside kmer threshhold
    pval=0.;
  } 
  else if(fam->sub->nsubs == 0) { // Cluster center
    pval=1.;
  }
  else if(fam->lambda == 0) { // Zero expected reads of this fam
    pval = 0.;
  } else { // Calculate abundance pval.
    // E_reads is the expected number of reads for this fam
    E_reads = fam->lambda * bi->reads;
    pval = calc_pA(fam->reads, E_reads);
  }
  return pval;
}

// Find singleton pval for a Fam in a Bi, also have to pass in B for lambda/cdf lookup
double get_pS(Fam *fam, Bi *bi, B *b) {
  size_t ifirst, imid, ilast;
  double pval;

  // Calculate singleton pval (pS) from cluster lookup
  if(fam->lambda >= b->lams[0]) {  // fam->lambda bigger than all lambdas in lookup
    pval = 1.0;
  }
  else if(fam->lambda <= b->lams[b->nlam-1]) { // fam->lam smaller than all lams in lookup
    pval = (1.0 - b->cdf[b->nlam-1]);
  }
  else { // Find lam in the lookup and assign pS
    ifirst = 0;
    ilast = b->nlam-1;
    while((ilast-ifirst) > 1) {
      imid = (ifirst+ilast)/2;
      if(b->lams[imid] > fam->lambda) {
        ifirst = imid;
      } else {
        ilast = imid;
      }
    }
    pval = (1.0 - b->cdf[ifirst]);
  }
  return pval;
}

void getCDF(std::vector<double>& ps, std::vector<double>& cdf, double err[4][4], int nnt[4], int maxD) {
  int i, j, k, d;
  size_t index;
  
  // Copy err "matrix" into relative errs vector
  k=0;
  double errs[NERRS];
  for(i=0;i<4;i++) {
    for(j=0;j<4;j++) {
      if(i!=j) { errs[k++] = err[i][j]; }
    }
  }

  std::vector<Prob> probs;

  // Get prob of error for each nt, and the self-trans prob
  double pa = errs[0]+errs[1]+errs[2];
  double pc = errs[3]+errs[4]+errs[5];
  double pg = errs[6]+errs[7]+errs[8];
  double pt = errs[9]+errs[10]+errs[11];
  double self = pow((1.-pa), nnt[0]) * pow((1.-pc), nnt[1]) * pow((1.-pg), nnt[2]) * pow((1.-pt), nnt[3]);

  // make errors relative to the non-err prob
  for(i=0;i<3;i++) { errs[i] = errs[i]/(1.0 - pa); }
  for(i=3;i<6;i++) { errs[i] = errs[i]/(1.0 - pc); }
  for(i=6;i<9;i++) { errs[i] = errs[i]/(1.0 - pg); }
  for(i=9;i<12;i++) { errs[i] = errs[i]/(1.0 - pt); }
  
  // Declare variables needed to iterate though all d-aways
  int nerr[NERRS];
  double nopen[4];
  int first, store;
  double p, n;

  for(d=0;d<=maxD;d++) {
    // init partition
    for(i=0;i<NERRS;i++) { nerr[i] = 0; }
    nerr[NERRS-1] = d;
    first = NERRS-1;
    if(VERBOSE) {
      Rcpp::Rcout << "---- D = " << d << " ----\n";
    }

    while(1) {
      // Calc and store p/n/pval contribution
      p = self;
      n = 1;
      for(i=0;i<4;i++) { nopen[i] = nnt[i]; }
      
      for(i=0;i<NERRS;i++) {
        if(nerr[i]>0) {
          for(j=0;j<nerr[i];j++) {
            p = p*errs[i];
            n = n*nopen[i/3]/(j+1.0);
            nopen[i/3]--; // one fewer of that base available for future errors
            // going below zero no prob, since the times zero makes everything zero
          }
        }
      }
      probs.push_back(Prob(p,n));

      if(nerr[0] >= d) { break; } // Should break when all d in first
      
      // Advance to next partition
      if(first > 0) {
        nerr[first]--;
        first--;
        nerr[first]++;
      } else {
        store = nerr[0];
        nerr[0] = 0;
        for(first=1;first<NERRS;first++) {
          if(nerr[first]>0) { break; }
          if(first==(NERRS-1)) {  // Error checking
            printf("Error: Partition iteration failed in getCDF!\n");
            return;
          }
        }
        nerr[first]--;
        first--;
        nerr[first] += (store+1);
      }
    } // while(1)

  } // for(d=0;d<maxD;d++)

  // Sort probs in descending order and create pval vector
  std::sort(probs.rbegin(), probs.rend());   // note: reverse iterators -> decreasing sort
  // Note that the 15-17 digit significand precision of doubles sets a limit on the precision of the 1-cdf tail.

  ps.resize(0);
  cdf.resize(0);
  double cum = 0;
  for(index=0;index < probs.size();index++) {
    cum += (probs[index].first * probs[index].second);
    ps.push_back(probs[index].first);
    cdf.push_back(cum);
  }

  // Find largest p beyond maxD (= max_err**(maxD+1))
  double max_err = 0.0;
  for(i=0;i<12;i++) { if(errs[i] > max_err) max_err = errs[i]; }
  double min_p = pow(max_err, maxD+1);
  
  for(index=0;index < ps.size();index++) {
    if(ps[index] <= min_p) { break; }
  }
  ps.resize(index);
  cdf.resize(index);
}

// [[Rcpp::export]]
Rcpp::DataFrame getSingletonCDF(Rcpp::NumericMatrix err, std::vector<int> nnt, int maxD) {
  int i, j;
  // Copy err into a C style array
  if(err.nrow() != 4 || err.ncol() != 4) {
    Rcpp::Rcout << "C: Error matrix malformed:" << err.nrow() << ", " << err.ncol() << "\n";
    return R_NilValue;
  }

  double c_err[4][4];
  for(i=0;i<4;i++) {
    for(j=0;j<4;j++) {
      c_err[i][j] = err(i,j);
    }
  }
  
  int c_nnt[4];
  for(i=0;i<4;i++) { c_nnt[i] = nnt[i]; }

  std::vector<double> ps;
  std::vector<double> cdf;
  getCDF(ps, cdf, c_err, c_nnt, maxD);
  
  return Rcpp::DataFrame::create(Rcpp::_["p"]=ps, Rcpp::_["cdf"]=cdf);
}

/*
 compute_lambda:
 
 parameters:
 char *seq, the reference sequence away from which a lambda will be computed
 Sub *sub, a substitution struct
 double self, the rate of self-production of the consensus sequence
 double t[4][4], a 4x4 matrix of content-independent error probabilities
 
 returns:
 the double lambda.
 */
double compute_lambda(Sub *sub, double self, double t[4][4]) {
  int i;
  int nti0, nti1;
  double lambda;
  
  if(!sub) { // NULL Sub, outside Kmer threshold
    return 0.0;
  }
//  printf("CL-Key(%i): %s\n", sub->nsubs, ntstr(sub->key));
  lambda = self;
  for(i=0; i < sub->nsubs; i++) {
    nti0 = (int)sub->nt0[i] - 1;
    nti1 = (int)sub->nt1[i] - 1;
//    printf("%c->%c: %.4e/%.4e = %.4e\n", ntstr(sub->nt0)[i], ntstr(sub->nt1)[i], t[nti0][nti1], t[nti0][nti0], t[nti0][nti1]/t[nti0][nti0]);
    
    if(nti0 < 0 || nti0 > 6) { printf("%i!", nti0); }
    if(nti1 < 0 || nti1 > 6) { printf("%i!", nti1); }
    
//    printf("%i:%.2e, ", i, lambda);
    lambda = lambda * t[nti0][nti1] / t[nti0][nti0];
  }

  if(lambda < 0 || lambda > 1) { printf("ERROR: OVERUNDERFLOW OF LAMBDA: %.4e\n", lambda); }

  if(lambda==0.0) { // UNDERFLOW TO ZERO
    printf("COMPUTE_LAMBDA: ZEROFLOW OF LAMBDA (%i): %.4e\n", sub->nsubs, lambda);
  }
  
  return lambda;
}

/* get_self:
 Gets the self-transition probabilty for a sequence under a transition matrix.
 */
double get_self(char *seq, double err[4][4]) {
  int i, nti;
  double self = 1.;
  for(i=0;i<strlen(seq);i++) {
    nti = ((int) seq[i]) - 1;
    if(nti==0 || nti==1 || nti==2 || nti==3) { // A,C,G or T. Not N or -.
      self = self * err[nti][nti];
    }
  }
  if(self==0.0) { // UNDERFLOW TO ZERO
    printf("Warning: get_self underflowed to zero.\n");
  }
  
  return self;
}

/* WORK IN PROGRESS
Rcpp::NumericVector sampleProbs(std::vector<double> errs, std::vector<int> nnt, int32_t nsam) {
  // binom draw of #A errors, #C errors, #G errors and #T errors
  int n, e;
  double self, p;
  Rcpp::RNGScope scope;
  
  // Get prob of error for each nt, and the self-trans prob
  double pa = errs[0]+errs[1]+errs[2];
  double pc = errs[3]+errs[4]+errs[5];
  double pg = errs[6]+errs[7]+errs[8];
  double pt = errs[9]+errs[10]+errs[11];
  double cp_ac = errs[0]/pa;  // conditional prob of A->C given error at an A
  double cp_acg = (errs[0]+errs[1])/pa;  // conditional prob of A->C or A->G given error at an A
  double cp_ca = errs[3]/pc;
  double cp_cag = (errs[3]+errs[4])/pc;
  double cp_ga = errs[6]/pg;
  double cp_gac = (errs[6]+errs[7])/pg;
  double cp_ta = errs[9]/pt;
  double cp_tac = (errs[9]+errs[10])/pt;
  self = pow((1.-pa), nnt[0]) * pow((1.-pc), nnt[1]) * pow((1.-pg), nnt[2]) * pow((1.-pt), nnt[3]);
  
  // sample the number of errors from each nucleotide
  Rcpp::NumericVector n_Aerr = Rcpp::rbinom(nsam, nnt[0], pa);
  Rcpp::NumericVector n_Cerr = Rcpp::rbinom(nsam, nnt[1], pc);
  Rcpp::NumericVector n_Gerr = Rcpp::rbinom(nsam, nnt[2], pg);
  Rcpp::NumericVector n_Terr = Rcpp::rbinom(nsam, nnt[3], pt);
  
  // DOUBLE CHECK THAT THESE ARE UNAMBIGUOUSLY COMPARED TO INTEGERS CORRECTLY
  // IE 2 < (RV=2.) WORKS CORRECTLY EVERY TIME

  // sample a uniform number for each error
  size_t sumErr = 0;
  for(n=0;n<nsam;n++) { sumErr = sumErr + n_Aerr[n] + n_Cerr[n] + n_Gerr[n] + n_Terr[n]; }
  Rcpp::NumericVector rvs = Rcpp::runif(sumErr);

  size_t ii = 0;
  for(n=0;n<nsam;n++) {
    p = self;
    for(e=0;e<n_Aerr[n];e++) {
      if(rvs[ii]<cp_ac) {
        p *= errs[0];  // !!!!!PROBLEM, NOT PROPERLY DIVIDING BY THE NON-ERROR PROB!!
      } else if (rvs[ii] < cp_acg) {
        p *= errs[1];
      } else if (rvs[ii] <= 1.) {
        p *= errs[2];
      } else {
        printf("BAD UNIFORM RANDOM NUMBER!\n");
      }
      ii++;
    }
    /// REPEAT REPEAT REPAAT
  }
}
*/




