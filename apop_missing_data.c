/** \file apop_missing_data.c
 
  Some missing data handlers.

  (c) 2007, Ben Klemens. Licensed under the GNU GPL v 2.
*/

#include <apop.h>

/** If there is an NaN anywhere in the row of data (including the matrix
    and the vector) then delete the row from the data set.

    The function returns a new data set with the NaNs removed, so
    the original data set is left unmolested. You may want to \c
    apop_data_free the original immediately after this function.

    If every row has an NaN, then this returns NULL; you may want to
    check for this after the function returns.

    \param d    The data, with NaNs
    \return     A (potentially shorter) copy of the data set, without NaNs.
*/
apop_data * apop_data_listwise_delete(apop_data *d){
  int i, j, min = 0, max = 0, height=0, has_vector=0, has_matrix=0, to_rm;
    //get to know the input.
    if (d->matrix){
        height      = d->matrix->size1;
        max         = d->matrix->size2;
        has_matrix  ++;
    } 
    if (d->vector){
        height      = height ?  height : d->vector->size;
        min         = -1;
        has_vector  ++;
    } 
    if (!has_matrix && !has_vector) {
        fprintf(stderr, "You sent to apop_data_listwise_delete a data set with void matrix and vector. Confused, it is returning NULL.\n");
        return NULL;
        }
    //find out where the NaNs are
  gsl_vector *marked = gsl_vector_calloc(height);
    for (i=0; i< d->matrix->size1; i++)
        for (j=min; j <max; j++)
            if (gsl_isnan(apop_data_get(d, i, j))){
                    gsl_vector_set(marked, i, 1);
                    break;
            }
    to_rm   = apop_sum(marked);
    //copy the good data.
    if (to_rm  == height)
        return NULL;
  apop_data *out = apop_data_alloc(0,height-to_rm, has_matrix ? max : -1);
    out->names  = apop_name_copy(d->names);                           ///You loser!!! Fix this. And add text!!!!
    if (has_vector && has_matrix)
        out->vector = gsl_vector_alloc(height - to_rm);
    j   = 0;
    for (i=0; i< height; i++){
        if (!gsl_vector_get(marked, i)){
            if (has_vector)
                gsl_vector_set(out->vector, j, gsl_vector_get(d->vector, i));
            if (has_matrix){
                APOP_ROW(d, i, v);
                gsl_matrix_set_row(out->matrix, j, v);
            if (d->names->rownames && d->names->rownamect > i)
                apop_name_add(out->names, d->names->rownames[i], 'r');
            }
            j++;
        }
    }
    gsl_vector_free(marked);
    return out;
}




//ML imputation

static apop_model apop_ml_imputation_model;

typedef struct {
size_t      *row, *col;
int         ct;
apop_data   *meanvar;
} apop_ml_imputation_struct;

static void addin(apop_ml_imputation_struct *m, size_t i, size_t j){
    m->row  = realloc(m->row, ++(m->ct) * sizeof(size_t));
    m->col  = realloc(m->col, m->ct * sizeof(size_t));
    m->row[m->ct-1]    = i;
    m->col[m->ct-1]    = j;
}

static void  find_missing(apop_data *d, apop_ml_imputation_struct *mask){
  int i, j, min = 0, max = 0;
    //get to know the input.
    if (d->matrix)
        max = d->matrix->size2;
    if (d->vector)
        min = -1;
    mask->row   = 
    mask->col   = NULL;
    mask->ct    = 0;
    //find out where the NaNs are
    for (i=0; i< d->matrix->size1; i++)
        for (j=min; j <max; j++)
            if (gsl_isnan(apop_data_get(d, i, j)))
                addin(mask, i, j);
}


//The model to send to the optimization

static void unpack(const apop_data *v, apop_data *x, apop_ml_imputation_struct * m){
  int                       i;
    for (i=0; i< m->ct; i++){
        apop_data_set(x, m->row[i], m->col[i], gsl_vector_get(v->vector,i));
    }
}

static double ll(const apop_data *d, apop_data *x, apop_params * ep){
  apop_ml_imputation_struct *m  = ep->model_params;
    unpack(d, x, m);
    return apop_multivariate_normal.log_likelihood(x, m->meanvar, NULL);
}


static apop_model apop_ml_imputation_model= {"Impute missing data via maximum likelihood", 0,0,0, NULL, NULL, ll, NULL, NULL};




/**
    Impute the most likely data points to replace NaNs in the data, and
    insert them into the given data. That is, the data set is modified
    in place.


\param  d       The data set. It comes in with NaNs and leaves entirely filled in.
\param  meanvar An \c apop_data set where the vector is the mean of each column and the matrix is the covariance matrix
\param  parameters  The most likely data points are naturally found via MLE. These are the parameters sent to the MLE.

*/
apop_params * apop_ml_imputation(apop_data *d,  apop_data* meanvar, apop_params * parameters){
  apop_ml_imputation_struct mask;
  apop_model *mc    = apop_model_copy(apop_ml_imputation_model);
  apop_mle_params *mlp   = NULL;
  apop_params   *p;
    find_missing(d, &mask);
    mc->vbase           = mask.ct;
    mask.meanvar        = meanvar;
    if (!parameters || strcmp(parameters->method_name, "MLE")){
        mlp     = apop_mle_params_alloc(d, *mc, parameters);
        p = parameters          = mlp->ep;
    } else{
        p       = apop_params_alloc(d, *mc, parameters->method_params,parameters->model_params);
        mlp     = apop_mle_params_alloc(d, *mc, parameters);
    } 
    mlp->method             = 5;
    parameters->model_params= &mask;
    mlp->step_size          = 2;
    mlp->tolerance          = 0.2;
//    parameters->starting_pt     = calloc(mask.ct, sizeof(double));
    return apop_maximum_likelihood(d, *mc, p);
    //We're done. The last step of the MLE filled the data with the best estimate.
}
