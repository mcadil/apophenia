/** \file apop_mle.c	The MLE functions. Call them with an \ref apop_model.

This file includes a number of distributions and models whose parameters
one would estimate using maximum likelihood techniques.

Each typically includes four functions: the likelihood function, the 
derivative of the likelihood function, a function that calls both of them,
and a user-usable function which takes in data and a blank vector, fills 
the vector with the most likely parameters, and returns the likelihood
of those parameters.

At the bottom are the maximum likelihood procedures themselves. There
are two: the no-derivative version and the with-derivative version.
Use the with-derivative version wherever possible---in fact, it is at
the moment entirely unused, but is just here for future use.

Copyright (c) 2006 by Ben Klemens. Licensed under the GNU GPL v2.
*/
#include "likelihoods.h"
#include <assert.h>
#include <gsl/gsl_deriv.h>

//in apop_regress.c:
void apop_estimate_parameter_t_tests (apop_params *est);


/** \page trace_path Plotting the path of an ML estimation.

If \c apop_opts.mle_trace_path has a name of positive length, then every time
the MLE evaluates the function, then the value will be output to a table
in the database with the given name. You can then plot this table to
get an idea of the path the estimation routine used to arrive at its MLE.

First, set this variable and run the MLE. The begin/commit wrapper
speeds things up a touch, but this will clearly be slower than without
taking notes:

\code
    strcpy(apop_opts.mle_trace_path, "path");
    apop_query("begin;");
    e   = apop_zipf.estimate(...);
    apop_query("commit;");
\endcode


Then, plot using a function like the below. Notice that you will want
\c splot for 3-d variables and \c plot for 2-d. The change in width
and pointsize is to remind the eye that the lines connecting the points
only indicate the path the maximizer went along, not actual values of
the function.

\code
static void plotme(char *outfile){
FILE            *f;
gsl_matrix      *traced_path;
    f       = fopen(outfile, "w");  //overwrites. Use "a" to append.
    fprintf(f,"splot '-' with linespoints linewidth 0.5 pointsize 2\n");
    traced_path = apop_query_to_matrix("select * from %s", apop_opts.mle_trace_path);
    fclose(f);
    apop_matrix_print(traced_path, "\t", outfile);
}
\endcode

Finally, call gnuplot:
\code 
gnuplot -persist < plotme
(or)
gnuplot plotme -
\endcode

Below is a sample of the sort of output one would get:<br>
\image latex "search.gif" "An ML search, tracing out the surface of the function" width=\textwidth
\image html "search.gif" "An ML search, tracing out the surface of the function" 

\ingroup mle
*/

/** If you already have an \c apop_params struct, but want to set the \c method_params
 element to the default MLE parameters, use this. Returns the \c
 apop_mle_params pointer, but the argument now has the method_params element set, so you can ignore the returned pointer if you prefer.

 \param parent  A pointer to an allocated \c apop_params struct.
 \return A pointer to a set-up \c apop_mle_params struct. The parent's \c method_params element points to this struct as well.

 */
apop_mle_params *apop_mle_params_set_default(apop_params *parent){
  apop_mle_params *setme =   calloc(1,sizeof(apop_mle_params));
    setme->starting_pt      = 0;
    setme->tolerance        = 0;
    setme->resolution       = 0;
    setme->method           = 1;
    setme->verbose          = 0;
    setme->step_size        = 1;
    setme->want_cov         = 1;
//siman:
    //siman also uses step_size  = 1.;  
    setme->n_tries          = 200; 
    setme->iters_fixed_T    = 200; 
    setme->k                = 1.0;
    setme->t_initial        = 50;  
    setme->mu_t             = 1.002; 
    setme->t_min            = 5.0e-1;
    setme->rng              = NULL;
    setme->ep               = parent;
    parent->method_params   = setme;
    strcpy(setme->ep->method_name, "MLE");
    return setme;
}


/** Neatly allocate an \ref apop_mle_params structure. Sets a
few defaults, so you can change just one or two values and everything
else will be predictable.
When you finally call your MLE, use the \c ep element of this 

\ingroup mle
 */
apop_mle_params *apop_mle_params_alloc(apop_data * data, apop_model model, apop_params* model_params){
  apop_mle_params *setme = apop_mle_params_set_default(apop_params_alloc(data, model, NULL, model_params));
    return setme;
}


		///////////////////////
		//MLE support functions
		///////////////////////

typedef	void 	(*apop_df_with_void)(const gsl_vector *beta, void *d, gsl_vector *gradient);
typedef	void 	(*apop_fdf_with_void)( const gsl_vector *beta, void *d, double *f, gsl_vector *df);


//Including numerical differentiation and a couple of functions to
//negate the likelihood fns without bothering the user.


typedef struct {
	gsl_vector	*beta;
	apop_data	*d;
	int		    dimension;
} grad_params;

typedef struct {
    apop_model  *model;
    apop_data   *data;
    apop_fn_with_params   *f;
    apop_df_with_void   *df;
    apop_params *params;
    grad_params *gp;
    gsl_vector  *beta;
    int         use_constraint;
    double      **gradient_list;
    double      **gradientp;
    size_t      *gsize;
    size_t      *gpsize;
}   infostruct;

static apop_params * apop_annealing(infostruct*);                         //below.
static int dnegshell (const gsl_vector * betain, void * , gsl_vector * g); //below.

static double one_d(double b, void *in){
  infostruct    *i   =in;
  int           vsize           = (i->params->parameters->vector? i->params->parameters->vector->size:0),
                msize1          = (i->params->parameters->matrix? i->params->parameters->matrix->size1:0),
                msize2          = (i->params->parameters->matrix? i->params->parameters->matrix->size2:0);
    gsl_vector_set(i->gp->beta, i->gp->dimension, b);
    apop_data   *p  = apop_data_unpack(i->gp->beta, vsize, msize1, msize2);
	double out= (*(i->f))(p, i->gp->d, i->params);
    apop_data_free(p);
    return out;
}


#include "apop_findzeros.c"


static void apop_internal_numerical_gradient(apop_fn_with_params ll, const gsl_vector *beta, infostruct* info, gsl_vector *out){
  int		    j;
  gsl_function	F;
  double		result, err;
  grad_params 	gp;
  infostruct    i;
    memcpy(&i, info, sizeof(i));
    i.f         = &ll;
	gp.beta		= gsl_vector_alloc(beta->size);
	gp.d		= info->data;
    i.gp        = &gp;
	F.function	= one_d;
	F.params	= &i;
	for (j=0; j< beta->size; j++){
		gp.dimension	= j;
		gsl_vector_memcpy(gp.beta, beta);
		gsl_deriv_central(&F, gsl_vector_get(beta,j), 1e-5, &result, &err);
		gsl_vector_set(out, j, result);
	}
}

/**The GSL provides one-dimensional numerical differentiation; here's
 the multidimensional extension.
 
 \code
 gradient = apop_numerical_gradient(beta, data, your_model, params);
 \endcode

 \ingroup linear_algebra
 \todo This fn has a hard-coded differential (1e-5).
 */
gsl_vector * apop_numerical_gradient(gsl_vector *beta, apop_data *data, apop_model m, apop_params *eps){
  infostruct    i;
  apop_fn_with_params ll  = m.log_likelihood ? m.log_likelihood : m.p;
  gsl_vector        *out= gsl_vector_alloc(beta->size);
    i.model = &m;
    i.data  = data;
    i.params    = eps;
    i.beta      = beta;
    apop_internal_numerical_gradient(ll, beta, &i, out);
    return out;
}

/* They always tell you to just negate your likelihood function to turn
a minimization routine into a maximization routine---and this is the
sort of annoying little detail that Apophenia is intended to take care
of for you. The next few functions do the negation, so you have one
less sign that you have to remember. 

These fns also take care of checking constraints, if any.

*/


static void insert_path_into_db(const gsl_vector *beta, double out){
    if (beta->size == 1){
        if(!apop_table_exists(apop_opts.mle_trace_path, 0))
            apop_query("create table  %s (beta0, ll);", apop_opts.mle_trace_path);
        apop_query("insert into %s values (%g, %g);", apop_opts.mle_trace_path, gsl_vector_get(beta,0), out);
    } else {
        if(!apop_table_exists(apop_opts.mle_trace_path, 0))
            apop_query("create table  %s (beta0, beta1, ll);", apop_opts.mle_trace_path);
        apop_query("insert into %s values (%g, %g, %g);", apop_opts.mle_trace_path, gsl_vector_get(beta,0), gsl_vector_get(beta,1), out);
    }

}

static double negshell (const gsl_vector * betain, void * in){
  infostruct    *i              = in;
  apop_data 	*returned_beta	= NULL,
                *p              = i->params->parameters;
  double		penalty         = 0,
                out             = 0; 
  static double	base_for_penalty= 0;
  int           vsize           = (p->vector? p->vector->size:0),
                msize1          = (p->matrix? p->matrix->size1:0),
                msize2          = (p->matrix? p->matrix->size2:0);
  double 	(*f)(const apop_data *, apop_data *, apop_params *);
  apop_data     *beta           = apop_data_unpack(betain, vsize, msize1, msize2);
    f   = i->model->log_likelihood? i->model->log_likelihood : i->model->p;
    if (!f)
        apop_error(0, 's', "The model you sent to the MLE function has neither log_likelihood element nor p element.\n");
	if (i->use_constraint && i->model->constraint){
        returned_beta	= apop_data_copy(beta);
		penalty	= i->model->constraint(beta, returned_beta, i->params);
		if (penalty > 0){
			base_for_penalty	= f(returned_beta, i->data, i->params);
			out = -base_for_penalty + penalty;
		}
	}
    if (!penalty){
	    out = - f(beta, i->data, i->params);
    }
    if (strlen(apop_opts.mle_trace_path))
        insert_path_into_db(betain,-out);
	if (returned_beta) apop_data_free(returned_beta);
    apop_data_free(beta);
    return out;
}


/* The derivative-calculating routine.
If the constraint binds
    then: take the numerical derivative of negshell, which will be the
    numerical derivative of the penalty.
    else: just find dlog_likelihood. If the model doesn't have a
    dlog likelihood or the user asked to ignore it, then the main
    maximum likelihood fn replaced model.score with
    apop_numerical_gradient anyway.
Finally, reverse the sign, since the GSL is trying to minimize instead of maximize.
*/
//static void dnegshell (const gsl_vector * beta, apop_data * d, gsl_vector * g){
static int dnegshell (const gsl_vector * betain, void * in, gsl_vector * g){
  infostruct    *i              = in;
  int           vsize       =(i->params->parameters->vector ? i->params->parameters->vector->size :0),
                msize1      =(i->params->parameters->matrix ? i->params->parameters->matrix->size1 :0),
                msize2      =(i->params->parameters->matrix ? i->params->parameters->matrix->size2:0);
  apop_data     *returned_beta  = apop_data_alloc(vsize, msize1, msize2);
  apop_data     *beta           = apop_data_unpack(betain, vsize, msize1, msize2);
  const gsl_vector    *usemep;
  apop_data     *useme          = beta;
    usemep  = betain;
	if (i->model->constraint && i->model->constraint(beta, returned_beta, i->params)){
		usemep  = apop_data_pack(returned_beta);
        useme   = returned_beta;
    }
    if (i->model->score)
        i->model->score(useme, i->data, g, i->params);
    else{
        apop_fn_with_params ll  = i->model->log_likelihood ? i->model->log_likelihood : i->model->p;
        apop_internal_numerical_gradient(ll, usemep, i, g);
        }
    gsl_vector_scale(g, -1);
	apop_data_free(returned_beta);
    apop_data_free(beta);
    return GSL_SUCCESS;
}

//By the way, the awkward pointer-to-pointer for the gradient list and
//plist is because the simulated annealing algorithm breaks if
//i->gradient_list or i->gradientp itself changes.
static void record_gradient_info(double f, gsl_vector *g, infostruct *i){
    if (!gsl_finite(f)) return;
    *i->gradient_list    = realloc(*i->gradient_list, sizeof(double)*(g->size +*i->gsize));
    if (g->stride ==1)
        memcpy(*i->gradient_list+*i->gsize, g->data, sizeof(*g->data)*g->size);
    else {
        int j;
        for (j=0; j< g->size; j++)
            (*i->gradient_list)[*i->gsize+j] = gsl_vector_get(g, j);
    }
    (*i->gsize)                += g->size;
    (*i->gradientp)            = realloc(*i->gradientp, sizeof(double)*(*i->gpsize + 1));
    (*i->gradientp)[*i->gpsize] = i->model->log_likelihood? -f : -log(f);
    (*i->gpsize)               ++;
}

//static void fdf_shell(const gsl_vector *beta, apop_data *d, double *f, gsl_vector *df){
static void fdf_shell(gsl_vector *beta, infostruct *i, double *f, gsl_vector *df){
	//if (negshell_model.fdf==NULL){
		*f	= negshell(beta, i);
		dnegshell(beta, i, df);
	/*} else	{
		negshell_model.fdf(beta, d, f, df);
		(*f) 	*= -1;
		gsl_vector_scale(df, -1);
	}*/
}



			//////////////////////////////////////////
			//The max likelihood functions themselves. 
			//Mostly straight out of the GSL manual.
			//////////////////////////////////////////



/** Calculate the Hessian.

  This is a synonym for \ref apop_numerical_second_derivative, q.v.
gsl_matrix * apop_numerical_hessian(apop_model dist, gsl_vector *beta, apop_data * d, void *params){
	return apop_numerical_second_derivative(dist, beta, d, params);
}
*/

/** Feeling lazy? Rather than doing actual pencil-and-paper math to find
your variance-covariance matrix, just use the negative inverse of the Hessian.

\param dist	The model
\param est	The estimate, with the parameters already calculated. The var/covar matrix will be placed in est->covariance.
\param data	The data
\ingroup basic_stats
void apop_numerical_covariance_matrix(apop_model dist, apop_params *est, apop_data *data){
//int		i;
apop_fn_with_params tmp;
gsl_matrix	    *hessian;
	tmp			= apop_fn_for_derivative;
	hessian			= apop_numerical_second_derivative(dist, est->parameters->vector, data);
	gsl_matrix_scale(hessian, -1);
	apop_det_and_inv(hessian, &(est->covariance->matrix), 0, 1);
	gsl_matrix_free(hessian);

	if (est->ep.uses.confidence == 0)
		return;
	//else:
        apop_estimate_parameter_t_tests(est);
	apop_fn_for_derivative 	= tmp;
}
*/
void apop_numerical_covariance_matrix(apop_model dist, apop_params *est, apop_data *data){
    //As you can see, this is a placeholder.
    return;
}

void cov_cleanup(infostruct *i){
    free(*i->gradient_list);
    free(*i->gradientp);
    free(i->gsize);
    free(i->gpsize);
    free(i->gradient_list);
    free(i->gradientp);
}

void produce_covariance_matrix(apop_params * est, infostruct *i){
  apop_mle_params *p = i->params->method_params;
  if (!p->want_cov){
      cov_cleanup(i);
      return;
  }
  int        m, j, k, betasize  = *i->gsize/ *i->gpsize;
  double     proportion[*i->gpsize];
  gsl_matrix *preinv    = gsl_matrix_calloc(betasize, betasize);
    memset(proportion, 1, sizeof(double)* *i->gpsize);
    //If p_2 = k p_1, then ln(k) = ln(p_2) - ln(p_1).
    //So p_1+p_2 ... = 1 + exp(ln p_2-ln p1) + exp(ln p_3- ln p1)+...
    //and the weight we place on item i is p_i/p_1+p_2+... = 1/(1+exp()+exp()...
    for (j=0; j<  *i->gpsize; j++)
        for (k=0; k<  *i->gpsize; k++)
            if (j!=k)
                proportion[j]   += exp((*i->gradientp)[k] - (*i->gradientp)[j]);
    for (j=0; j< *i->gpsize; j++)
        printf("%i: %f\n", j, proportion[j]);
    //inv (n E(score) dot E(score)) = info matrix.
    for (m=0; m<  *i->gpsize; m++)
        if (gsl_finite(proportion[m]))
         for (j=0; j< betasize; j++)
          for (k=0; k< betasize; k++)
            apop_matrix_increment(preinv, j,k, (*i->gradient_list)[m*betasize +k] * (*i->gradient_list)[m*betasize +j]/ proportion[m]);
    gsl_matrix *inv;
    if (est->data && est->data->matrix)
        gsl_matrix_scale(preinv, est->data->matrix->size1);
    apop_det_and_inv(preinv, &inv, 0, 1);
    est->covariance = apop_matrix_to_data(inv);
    if (est->parameters->names->colnames){
        apop_name_stack(est->covariance->names, est->parameters->names, 'c');
        apop_name_cross_stack(est->covariance->names, est->parameters->names, 'r', 'c');
    }
    gsl_matrix_free(preinv);
    cov_cleanup(i);
}


/* The maximum likelihood calculations, given a derivative of the log likelihood.

If no derivative exists, will calculate a numerical gradient.

\param data	the data matrix
\param	dist	the \ref apop_model object: waring, probit, zipf, &amp;c.
\param	starting_pt	an array of doubles suggesting a starting point. If NULL, use a vector whose elements are all 0.1 (zero has too many pathological cases).
\param step_size	the initial step size.
\param tolerance	the precision the minimizer uses. Only vaguely related to the precision of the actual var.
\param verbose		Y'know.
\return	an \ref apop_params with the parameter estimates, &c. If returned_estimate->status == 0, then optimum parameters were found; if status != 0, then there were problems.

  \todo readd names */
static apop_params *	apop_maximum_likelihood_w_d(apop_data * data,
			apop_model dist, infostruct *i){
  gsl_multimin_function_fdf minme;
  gsl_multimin_fdfminimizer *s;
  gsl_vector 			    *x;
  apop_params		        *est    = i->params;
  int                       vsize   =(est->parameters->vector ? est->parameters->vector->size :0),
                            msize1  =(est->parameters->matrix ? est->parameters->matrix->size1 :0),
                            msize2  =(est->parameters->matrix ? est->parameters->matrix->size2:0);
  int				        iter 	= 0, 
				            status  = 0,
				            betasize= vsize+ msize1*msize2;
  apop_mle_params           *mp     = est->method_params;
    if (mp->method == 2)
	    s	= gsl_multimin_fdfminimizer_alloc(gsl_multimin_fdfminimizer_vector_bfgs, betasize);
    else if (mp->method == 3)
	    s	= gsl_multimin_fdfminimizer_alloc(gsl_multimin_fdfminimizer_conjugate_pr, betasize);
    else
	    s	= gsl_multimin_fdfminimizer_alloc(gsl_multimin_fdfminimizer_conjugate_fr, betasize);
	if (mp->starting_pt==NULL){
		x	= gsl_vector_alloc(betasize);
  		gsl_vector_set_all (x,  0.1);
	}
	else 	x   = apop_array_to_vector(mp->starting_pt, betasize);
	minme.f		= negshell;
	minme.df	= (apop_df_with_void) dnegshell;
	minme.fdf	= (apop_fdf_with_void) fdf_shell;
	minme.n		= betasize;
	minme.params	= i;
	gsl_multimin_fdfminimizer_set (s, &minme, x, 
             mp->step_size ? mp->step_size : 0.05, 
             mp->tolerance ? mp->tolerance : 1e-3);
      	do { 	iter++;
		status 	= gsl_multimin_fdfminimizer_iterate(s);
        record_gradient_info(gsl_multimin_fdfminimizer_minimum(s), gsl_multimin_fdfminimizer_gradient(s), i);
		if (status) 	break; 
		status = gsl_multimin_test_gradient(s->gradient, 
                                             mp->tolerance ? mp->tolerance : 1e-3);
        	if (mp->verbose){
	        	printf ("%5i %.5f  f()=%10.5f gradient=%.3f\n", iter, gsl_vector_get (s->x, 0),  s->f, gsl_vector_get(s->gradient,0));
		}
        	if (status == GSL_SUCCESS){
			est->status	= 0;
		   	if(mp->verbose)	printf ("Minimum found.\n");
		}
       	 }
	while (status == GSL_CONTINUE && iter < MAX_ITERATIONS_w_d);
	if(iter==MAX_ITERATIONS_w_d) {
		est->status	= 1;
		if (mp->verbose) printf("No min!!\n");
	}
	//Clean up, copy results to output estimate.
    est->parameters = apop_data_unpack(s->x, vsize, msize1, msize2);
	gsl_multimin_fdfminimizer_free(s);
	if (mp->starting_pt==NULL) 
		gsl_vector_free(x);
	est->log_likelihood	= dist.log_likelihood ? 
        dist.log_likelihood(est->parameters, data, i->params):
        log(dist.p(est->parameters, data, i->params));
    produce_covariance_matrix(est, i);
    apop_estimate_parameter_t_tests (est);
	return est;
}

static apop_params *	apop_maximum_likelihood_no_d(apop_data * data, 
			apop_model dist, infostruct * i){
    //, double *starting_pt, double step_size, double tolerance, int verbose){
  apop_data                 *p          = i->params->parameters;
  int                       vsize       =(p->vector ? p->vector->size :0),
                            msize1      =(p->matrix ? p->matrix->size1 :0),
                            msize2      =(p->matrix ? p->matrix->size2:0);
  int			            status,
			                iter 		= 0,
			                betasize	=   vsize + msize1*msize2; 
  size_t 			        j;
  gsl_multimin_function 	minme;
  gsl_multimin_fminimizer   *s;
  gsl_vector 		        *x, *ss;
  double			        size;
  apop_params		        *est    = i->params;
  apop_mle_params           *mp     = est->method_params;
	s	= gsl_multimin_fminimizer_alloc(gsl_multimin_fminimizer_nmsimplex, betasize);
	ss	= gsl_vector_alloc(betasize);
	x	= gsl_vector_alloc(betasize);
	//est	= apop_params_alloc(data->size1, betasize, NULL, actual_uses);
	est->status	= 1;	//assume failure until we score a success.
	if (mp->starting_pt==NULL)
  		gsl_vector_set_all (x,  0);
	else
		x   = apop_array_to_vector(mp->starting_pt, betasize);
  	gsl_vector_set_all (ss,  mp->step_size);
	minme.f		    = negshell;
	minme.n		    = betasize;
	minme.params	= i;
	gsl_multimin_fminimizer_set (s, &minme, x,  ss);
      	do { 	iter++;
		status 	= gsl_multimin_fminimizer_iterate(s);
		if (status) 	break; 
		size	= gsl_multimin_fminimizer_size(s);
	   	status 	= gsl_multimin_test_size (size, mp->tolerance); 
		if(mp->verbose){
			printf ("%5d ", iter);
			for (j = 0; j < betasize; j++) {
				printf ("%8.3e ", gsl_vector_get (s->x, j)); } 
			printf ("f()=%7.3f size=%.3f\n", s->fval, size);
       			if (status == GSL_SUCCESS) {
                printf ("Optimum found at:\n");
                printf ("%5d ", iter);
                for (j = 0; j < betasize; j++) {
                    printf ("%8.3e ", gsl_vector_get (s->x, j)); } 
                printf ("f()=%7.3f size=%.3f\n", s->fval, size);
			}
		}
      	} while (status == GSL_CONTINUE && iter < MAX_ITERATIONS);
	if (iter == MAX_ITERATIONS && mp->verbose)
		apop_error(1, 'c', "Optimization reached maximum number of iterations.");
    if (status == GSL_SUCCESS) 
        est->status	= 0;
    est->parameters = apop_data_unpack(s->x, vsize, msize1, msize2);
	gsl_multimin_fminimizer_free(s);
	est->log_likelihood	= dist.log_likelihood ?
        dist.log_likelihood(est->parameters, data, i->params):
        dist.p(est->parameters, data, i->params);
	if (mp->want_cov) 
		apop_numerical_covariance_matrix(dist, est, data);
	return est;
}


/** The maximum likelihood calculations

\param data	the data matrix
\param	dist	the \ref apop_model object: waring, probit, zipf, &amp;c.
\param params	an \ref apop_params structure, probably allocated using \c apop_mle_params_alloc (not quite: see that page), featuring:<br>
starting_pt:	an array of doubles suggesting a starting point. If NULL, use zero.<br>
step_size:	the initial step size.<br>
tolerance:	the precision the minimizer uses. Only vaguely related to the precision of the actual var.<br>
verbose:	Y'know.<br>
method:		The sum of a method and a gradient-handling rule.
\li 0: Nelder-Mead simplex (gradient handling rule is irrelevant)
\li 1: conjugate gradient (Fletcher-Reeves) (default)
\li 2: conjugate gradient (BFGS: Broyden-Fletcher-Goldfarb-Shanno)
\li 3: conjugate gradient (Polak-Ribiere)
\li 5: \ref simanneal "simulated annealing"
\li 10: Find a root of the derivative via Newton's method
\li 11: Find a root of the derivative via the Broyden Algorithm
\li 12: Find a root of the derivative via the Hybrid method
\li 13: Find a root of the derivative via the Hybrid method; no internal scaling
\return	an \ref apop_params with the parameter estimates, &c. If returned_estimate->status == 0, then optimum parameters were found; if status != 0, then there were problems.

 \ingroup mle */
apop_params *	apop_maximum_likelihood(apop_data * data, apop_model dist, apop_params *params){
  infostruct    info    = {&dist, data, NULL, NULL, params, NULL, NULL, 1, NULL, NULL, 0,0};
    info.gradientp      = malloc(sizeof(double*)); *info.gradientp = NULL;
    info.gradient_list  = malloc(sizeof(double*)); *info.gradient_list = NULL;
    info.gsize          = malloc(sizeof(size_t)); *info.gsize = 0;
    info.gpsize         = malloc(sizeof(size_t)); *info.gpsize = 0;
  apop_mle_params   *mp;
    if (!params){
        mp  = apop_mle_params_alloc(data, dist, NULL);
        info.params = mp->ep;
    } else if (strcmp(params->method_name, "MLE")){
        mp  = apop_mle_params_set_default(params);
        params->method_params   = mp;
    } else
        mp  = params->method_params;
	if (mp->method == 5)
        return apop_annealing(&info);  //below.
    else if (mp->method==0)
		return apop_maximum_likelihood_no_d(data, dist, &info);
    else if (mp->method >= 10 && mp->method <= 13)
        return  find_roots (data, dist, params);
	//else:
	return apop_maximum_likelihood_w_d(data, dist, &info);
}


/** This function goes row by row through <tt>m</tt> and calculates the
likelihood of the given row, putting the result in <tt>v</tt>. 
You can use this to find the variance of the estimator if other means fail.

\param m 	A GSL matrix, exactly like those used for probit, Waring, Gamma, &c MLEs.

\param v	A vector that will hold the likelihood of each row of m. Declare but do not allocate.

\param dist	An \ref apop_model object whose log likelihood function you'd like to use.

\param fn_beta		The parameters at which you will evaluate the likelihood. If <tt>e</tt> is an \ref
			apop_estimate, then one could use <tt>e->parameters->vector</tt>.

This functions is used in the sample code in the \ref mle section.

void apop_make_likelihood_vector(gsl_matrix *m, gsl_vector **v, 
				apop_model dist, gsl_vector* fn_beta){
gsl_matrix      mm;
int             i;
	*v	= gsl_vector_alloc(m->size1);
        for(i=0; i< m->size1; i++){
                mm      = gsl_matrix_submatrix(m,i,0, 1,m->size2).matrix;      //get a single row
                gsl_vector_set(*v, i, dist.log_likelihood(fn_beta, apop_matrix_to_data(&mm), model_params));
        }
}
\ingroup mle */

/** Input an earlier estimate, and then I will re-start the MLE search
 where the last one ended. You can specify greater precision or a new
 search method.

The prior estimation parameters are copied over.  If the estimate
converged to an OK value, then use the converged value; else use the
starting point from the last estimate.

Only one estimate is returned, either the one you sent in or a new
one. The loser (which may be the one you sent in) is freed. That is,
there is no memory leak when you do
\code
est = apop_estimate_restart(est, 200, 1e-2);
\endcode

 \param e   An \ref apop_params that is the output from a prior MLE estimation.
 \param new_method  If -1, use the prior method; otherwise, a new method to try.
 \param scale       Something like 1e-2. The step size and tolerance
                    will both be mutliplied by this amount. Of course, if this is 1, nothing changes.

\return         At the end of this procedure, we'll have two \ref
    apop_params structs: the one you sent in, and the one produced using the
    new method/scale. If the new estimate includes any NaNs/Infs, then
    the old estimate is returned (even if the old estimate included
    NaNs/Infs). Otherwise, the estimate with the largest log likelihood
    is returned.

\ingroup mle
\todo The tolerance for testing boundaries are hard coded (1e4). Will need to either add another input term or a global var.
*/ 
apop_params * apop_estimate_restart (apop_params *e, int  new_method, int scale){
  double      *start_pt2;
  apop_params *copy  = apop_params_clone(e, sizeof(apop_mle_params), 0, 0);
  apop_mle_params *old_params   = e->method_params;
  apop_mle_params *new_params   = copy->method_params; 

            //copy off the old params; modify the starting pt, method, and scale
    if (apop_vector_bounded(e->parameters->vector, 1e4)){
        apop_vector_to_array(e->parameters->vector, &start_pt2);
	    new_params->starting_pt	= start_pt2;
    }
    else
	    new_params->starting_pt	= old_params->starting_pt;
    new_params->tolerance   = old_params->tolerance * scale;
    new_params->step_size   = old_params->step_size * scale;
    new_params->method	    = new_method;
	copy                    = e->model->estimate(e->data, copy);
            //Now check whether the new output is better than the old
printf("orig: 1st: %g, ll %g\n", e->parameters->vector->data[0],e->log_likelihood );
printf("copy: 1st: %g, ll %g\n", copy->parameters->vector->data[0],copy->log_likelihood );
    if (apop_vector_bounded(copy->parameters->vector, 1e4) && copy->log_likelihood > e->log_likelihood){
        //apop_params_free(e);
        return copy;
    } //else:
    //free(new_params);
    //apop_params_free(copy);
    return e;
}


//////////////////////////
// Simulated Annealing.

/** \page simanneal Notes on simulated annealing

Simulated annealing is a controlled random walk.
As with the other methods, the system tries a new point, and if it
is better, switches. Initially, the system is allowed to make large
jumps, and then with each iteration, the jumps get smaller, eventually
converging. Also, there is some decreasing probability that if the new
point is {\em less} likely, it will still be chosen. Simulated annealing
is best for situations where there may be multiple local optima. Early
in the random walk, the system can readily jump from one to another;
later it will fine-tune its way toward the optimum. The number of points
tested is basically not dependent on the function: if you give it a
4,000 step program, that is basically how many steps it will take.
If you know your function is globally convex (as are most standard
probability functions), then this method is overkill.

The GSL's simulated annealing system doesn't actually do very much. It
basically provides a for loop that calls a half-dozen functions that we
the users get to write. So, the file \ref apop_mle.c handles all of this
for you. The likelihood function is taken from the model, the metric
is the Manhattan metric, the copy/destroy functions are just the usual
vector-handling fns., et cetera. The reader who wants further control
is welcome to override these functions.

Verbosity: if ep->verbose==1, show likelihood,  temp, &c. in a table;
if ep->verbose>1, show that plus the vector of params.

 \ingroup mle
 */

static void an_record(infostruct *i, double energy){
    gsl_vector *grad = gsl_vector_alloc(i->beta->size);
    dnegshell(i->beta, i, grad);
    record_gradient_info(energy, grad, i);
    gsl_vector_free(grad);
}

static double annealing_energy(void *in) {
  infostruct *i      = in;
  double energy      = negshell(i->beta, i);
  apop_mle_params *p = i->params->method_params;
    if (p->want_cov)
        an_record(i, energy);
    return energy;
}

/** We use the Manhattan metric to correspond to the annealing_step fn below.
 */
static double annealing_distance(void *xin, void *yin) {
  return apop_vector_grid_distance(((infostruct*)xin)->beta, ((infostruct*)yin)->beta);
}


/** The algorithm: 
    --randomly pick dimension
    --shift by some amount of remaining step size
    --repeat for all dims
This will give a move \f$\leq\f$ step_size on the Manhattan metric.
*/
static void annealing_step(const gsl_rng * r, void *in, double step_size){
  infostruct  *i          = in;
  int         dims_used[i->beta->size];
  int         dims_left, dim, sign;
  double      step_left, amt;
  int           vsize       =(i->params->parameters->vector ? i->params->parameters->vector->size :0),
                msize1      =(i->params->parameters->matrix ? i->params->parameters->matrix->size1 :0),
                msize2      =(i->params->parameters->matrix ? i->params->parameters->matrix->size2:0);
  apop_data     *testme     = NULL,
                *dummy      = apop_data_alloc(vsize, msize1, msize2);
    memset(dims_used, 0, i->beta->size * sizeof(int));
    dims_left   = i->beta->size;
    step_left   = step_size;
    while (dims_left){
        do {
            dim = gsl_rng_uniform(r)* i->beta->size;
        } while (dims_used[dim]);
        dims_used[dim]  ++;
        dims_left       --;
        sign    = (gsl_rng_uniform(r) > 0.5) ? 1 : -1;
        amt     = gsl_rng_uniform(r);
        apop_vector_increment(i->beta, dim, amt * step_left * sign); 
        step_left   *= amt;
    }
    testme      = apop_data_unpack(i->beta, vsize, msize1, msize2);
    if (i->model->constraint && i->model->constraint(testme, dummy, i->params)){
        gsl_vector *cv  = apop_data_pack(dummy);
        gsl_vector_memcpy(i->beta, cv);
        gsl_vector_free(cv);
    }
    apop_data_free(dummy);
    apop_data_free(testme);
}

static void annealing_print2(void *xp) { return; }

static void annealing_print(void *xp) {
    apop_vector_show(((infostruct*)xp)->beta);
}

static void annealing_memcpy(void *xp, void *yp){
  infostruct    *yi = yp;
  infostruct    *xi = xp;
    if (yi->beta && yi->beta !=xi->beta) 
        gsl_vector_free(yi->beta);
    memcpy(yp, xp, sizeof(infostruct));
    yi->beta = gsl_vector_alloc(((infostruct*)xp)->beta->size);
    gsl_vector_memcpy(yi->beta, ((infostruct*)xp)->beta);
}

static void *annealing_copy(void *xp){
    infostruct *out = malloc(sizeof(infostruct));
    memcpy(out, xp, sizeof(infostruct));
    out->beta        = gsl_vector_alloc(((infostruct*)xp)->beta->size);
    gsl_vector_memcpy(out->beta, ((infostruct*)xp)->beta);
    return out;
}

static void annealing_free(void *xp){
    gsl_vector_free(((infostruct*)xp)->beta);
    free(xp);
}

apop_params * apop_annealing(infostruct *i){
  apop_data     *data   = i->data;
  apop_params   *ep     = i->params;
                        //iters_fixed_T, step_size, k, t_initial, damping factor, t_min
  gsl_siman_params_t    simparams;
  apop_mle_params       *mp = NULL;
  gsl_vector    *beta;
  int           vsize       =(i->params->parameters->vector ? i->params->parameters->vector->size :0),
                msize1      =(i->params->parameters->matrix ? i->params->parameters->matrix->size1 :0),
                msize2      =(i->params->parameters->matrix ? i->params->parameters->matrix->size2:0);
  int           paramct = vsize + msize1*msize2;
    //parameter setup
    if (ep && !strcmp(ep->method_name, "MLE")){
        mp  = ep->method_params;
        simparams.n_tries       = mp->n_tries;
        simparams.iters_fixed_T = mp->iters_fixed_T;
        simparams.step_size     = mp->step_size;
        simparams.k             = mp->k;
        simparams.t_initial     = mp->t_initial;
        simparams.mu_t          = mp->mu_t;
        simparams.t_min         = mp->t_min;
    } else{
        simparams.n_tries =200; //The number of points to try for each step. 
        simparams.iters_fixed_T = 200;  //The number of iterations at each temperature. 
        simparams.step_size  = 1.;  //The maximum step size in the random walk. 
        simparams.k          = 1.0, //cooling schedule data
        simparams.t_initial   = 50,   
        simparams.mu_t        = 1.002, 
        simparams.t_min       = 5.0e-1;
    }

    static const gsl_rng   * r    = NULL;
    if (mp && mp->rng) 
        r    =  mp->rng;
    if (!r)
        r =  gsl_rng_alloc(gsl_rng_env_setup()) ; 

    if (mp && mp->starting_pt)
        beta = apop_array_to_vector(mp->starting_pt, paramct);
    else{
        beta  = gsl_vector_alloc(paramct);
        gsl_vector_set_all(beta, 1);
    }
	i->beta             = beta;
    i->use_constraint   = 0; //negshell doesn't check it; annealing_step does.

    gsl_siman_print_t printing_fn   = NULL;
    if (mp && mp->verbose>1)
        printing_fn = annealing_print;
    else if (mp && mp->verbose)
        printing_fn = annealing_print2;

    gsl_siman_solve(r,        //   const gsl_rng * r
          i,                //   void * x0_p
          annealing_energy, //   gsl_siman_Efunc_t Ef
          annealing_step,   //   gsl_siman_step_t take_step
          annealing_distance, // gsl_siman_metric_t distance
          printing_fn,      //gsl_siman_print_t print_position
          annealing_memcpy, //   gsl_siman_copy_t copyfunc
          annealing_copy,   //   gsl_siman_copy_construct_t copy_constructor
          annealing_free,   //   gsl_siman_destroy_t destructor
         paramct,    //   size_t element_size
         simparams);           //   gsl_siman_params_t params

    i->params->parameters   = apop_data_unpack(i->beta, vsize, msize1, msize2); //just in case of mis-linking
    //Clean up, copy results to output estimate.
	i->params->log_likelihood	= i->model->log_likelihood ? 
        i->model->log_likelihood(i->params->parameters, data, i->params):
        log(i->model->p(i->params->parameters, data, i->params));
    produce_covariance_matrix(i->params, i);
    apop_estimate_parameter_t_tests(i->params);
    return i->params;
}
