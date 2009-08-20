#ifndef APOP_LINEAR_ALGEBRA_H
#define APOP_LINEAR_ALGEBRA_H

#include "variadic.h"
#include <gsl/gsl_blas.h>
#include <gsl/gsl_sf_log.h>
#include <gsl/gsl_sf_exp.h>
#include <gsl/gsl_matrix.h>
#include <gsl/gsl_linalg.h>
#include "types.h"

#ifdef	__cplusplus
extern "C" {
#endif

double apop_det_and_inv(const gsl_matrix *in, gsl_matrix **out, int calc_det, int calc_inv);
APOP_VAR_DECLARE apop_data * apop_dot(const apop_data *d1, const apop_data *d2, char form1, char form2);
APOP_VAR_DECLARE int         apop_vector_bounded(const gsl_vector *in, long double max);
APOP_VAR_DECLARE void apop_vector_increment(gsl_vector * v, int i, double amt);
APOP_VAR_DECLARE void apop_matrix_increment(gsl_matrix * m, int i, int j, double amt);
gsl_matrix * apop_matrix_inverse(const gsl_matrix *in) ;
double      apop_matrix_determinant(const gsl_matrix *in) ;
//apop_data*  apop_sv_decomposition(gsl_matrix *data, int dimensions_we_want);
APOP_VAR_DECLARE apop_data *  apop_matrix_pca(gsl_matrix *data, int dimensions_we_want);
APOP_VAR_DECLARE gsl_vector * apop_vector_stack(gsl_vector *v1, gsl_vector * v2, char inplace);
APOP_VAR_DECLARE gsl_matrix * apop_matrix_stack(gsl_matrix *m1, gsl_matrix * m2, char posn, char inplace);
gsl_matrix * apop_matrix_rm_columns(gsl_matrix *in, int *drop);

void apop_vector_log(gsl_vector *v);
void apop_vector_log10(gsl_vector *v);
void apop_vector_exp(gsl_vector *v);

#ifdef	__cplusplus
}
#endif
#endif
