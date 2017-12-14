/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2017 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the ZIB Academic License.         */
/*                                                                           */
/*  You should have received a copy of the ZIB Academic License              */
/*  along with SCIP; see the file COPYING. If not email to scip@zib.de.      */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/**@file   cons_expr_nlhdlr_quadratic.c
 * @brief  nonlinear handler to handle quadratic constraints
 * @author Felipe Serrano
 *
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#include <string.h>

#define SCIP_PRIVATE_ROWPREP

#include "scip/cons_quadratic.h" /* for SCIP_ROWPREP, SCIP_QUADVARTERM, etc */
#include "scip/cons_expr_nlhdlr_quadratic.h"
#include "scip/cons_expr_pow.h"
#include "scip/cons_expr_sum.h"
#include "scip/cons_expr_var.h"
#include "scip/cons_expr_product.h"

#include "nlpi/nlpi_ipopt.h" /* for LAPACK */

/* fundamental nonlinear handler properties */
#define NLHDLR_NAME         "quadratic"
#define NLHDLR_DESC         "handler for quadratic expressions"
#define NLHDLR_PRIORITY     100

/*
 * Data structures
 */

/** nonlinear handler data */
struct SCIP_ConsExpr_NlhdlrData
{
   SCIP_Bool             initialized;        /**< whether handler has been initialized and not yet de-initialized */
};

/** nonlinear handler expression data */
struct SCIP_ConsExpr_NlhdlrExprData
{
   int                   nlinvars;           /**< number of linear variables */
   int                   linvarssize;        /**< size of linvars and lincoefs arrays */
   SCIP_VAR**            linvars;            /**< linear variables */
   SCIP_Real*            lincoefs;           /**< coefficients of linear variables */

   int                   nquadvars;          /**< number of variables in quadratic terms */
   int                   quadvarssize;       /**< size of quadvarterms array */
   SCIP_QUADVARTERM*     quadvarterms;       /**< array with quadratic variable terms */

   int                   nbilinterms;        /**< number of bilinear terms */
   int                   bilintermssize;     /**< size of bilinterms array */
   SCIP_BILINTERM*       bilinterms;         /**< bilinear terms array */

   SCIP_EXPRCURV         curvature;          /**< curvature of the quadratic representation of the expression */
};

/*
 * static methods
 */


/** frees nlhdlrexprdata structure */
static
void freeNlhdlrExprData(SCIP* scip, SCIP_CONSEXPR_NLHDLREXPRDATA* nlhdlrexprdata)
{
   int i;

   SCIPfreeBlockMemoryArray(scip, &(nlhdlrexprdata->linvars), nlhdlrexprdata->linvarssize);
   SCIPfreeBlockMemoryArray(scip, &(nlhdlrexprdata->lincoefs), nlhdlrexprdata->linvarssize);
   SCIPfreeBlockMemoryArray(scip, &(nlhdlrexprdata->bilinterms), nlhdlrexprdata->bilintermssize);

   for( i = 0; i < nlhdlrexprdata->nquadvars; ++i )
   {
      SCIPfreeBlockMemoryArray(scip, &(nlhdlrexprdata->quadvarterms[i].adjbilin),
            nlhdlrexprdata->quadvarterms[i].nadjbilin);
   }
   SCIPfreeBlockMemoryArray(scip, &(nlhdlrexprdata->quadvarterms), nlhdlrexprdata->quadvarssize);
}

/** ensures, that linear vars and coefs arrays can store at least num entries */
static
SCIP_RETCODE exprdataEnsureLinearVarsSize(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_CONSEXPR_NLHDLREXPRDATA* exprdata,   /**< nlhdlr expression data */
   int                   num                 /**< minimum number of entries to store */
   )
{
   assert(scip != NULL);
   assert(exprdata != NULL);
   assert(exprdata->nlinvars <= exprdata->linvarssize);

   if( num > exprdata->linvarssize )
   {
      int newsize;

      newsize = SCIPcalcMemGrowSize(scip, num);
      SCIP_CALL( SCIPreallocBlockMemoryArray(scip, &exprdata->linvars,  exprdata->linvarssize, newsize) );
      SCIP_CALL( SCIPreallocBlockMemoryArray(scip, &exprdata->lincoefs, exprdata->linvarssize, newsize) );
      exprdata->linvarssize = newsize;
   }
   assert(num <= exprdata->linvarssize);

   return SCIP_OKAY;
}

/** ensures, that quadratic variable terms array can store at least num entries */
static
SCIP_RETCODE exprdataEnsureQuadVarTermsSize(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_CONSEXPR_NLHDLREXPRDATA* exprdata,   /**< nlhdlr expression data */
   int                   num                 /**< minimum number of entries to store */
   )
{
   assert(scip != NULL);
   assert(exprdata != NULL);
   assert(exprdata->nquadvars <= exprdata->quadvarssize);

   if( num > exprdata->quadvarssize )
   {
      int newsize;

      newsize = SCIPcalcMemGrowSize(scip, num);
      SCIP_CALL( SCIPreallocBlockMemoryArray(scip, &exprdata->quadvarterms, exprdata->quadvarssize, newsize) );
      exprdata->quadvarssize = newsize;
   }
   assert(num <= exprdata->quadvarssize);

   return SCIP_OKAY;
}

/** ensures, that adjacency array can store at least num entries */
static
SCIP_RETCODE exprdataEnsureAdjBilinSize(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_QUADVARTERM*     quadvarterm,        /**< quadratic variable term */
   int                   num                 /**< minimum number of entries to store */
   )
{
   assert(scip != NULL);
   assert(quadvarterm != NULL);
   assert(quadvarterm->nadjbilin <= quadvarterm->adjbilinsize);

   if( num > quadvarterm->adjbilinsize )
   {
      int newsize;

      newsize = SCIPcalcMemGrowSize(scip, num);
      SCIP_CALL( SCIPreallocBlockMemoryArray(scip, &quadvarterm->adjbilin, quadvarterm->adjbilinsize, newsize) );
      quadvarterm->adjbilinsize = newsize;
   }
   assert(num <= quadvarterm->adjbilinsize);

   return SCIP_OKAY;
}

/** ensures, that bilinear term arrays can store at least num entries */
static
SCIP_RETCODE exprdataEnsureBilinSize(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_CONSEXPR_NLHDLREXPRDATA* exprdata,   /**< nlhdlr expression data */
   int                   num                 /**< minimum number of entries to store */
   )
{
   assert(scip != NULL);
   assert(exprdata != NULL);
   assert(exprdata->nbilinterms <= exprdata->bilintermssize);

   if( num > exprdata->bilintermssize )
   {
      int newsize;

      newsize = SCIPcalcMemGrowSize(scip, num);
      SCIP_CALL( SCIPreallocBlockMemoryArray(scip, &exprdata->bilinterms, exprdata->bilintermssize, newsize) );
      exprdata->bilintermssize = newsize;
   }
   assert(num <= exprdata->bilintermssize);

   return SCIP_OKAY;
}

static
SCIP_RETCODE addVarToQuadterms(SCIP* scip, SCIP_VAR* var, SCIP_Real sqrcoef, int bilintermidx, SCIP_CONSEXPR_NLHDLREXPRDATA* exprdata, SCIP_HASHMAP* varidx)
{
   SCIP_QUADVARTERM* quadterm;

   /* if var is in varidx --> it could be because it is linear or quadratic; if linear, remove it from varidx and
    * add a quadterm with square coef equal 0: because of the ordering, we cannot see var nor var^2 anymore;
    * if quadratic, just ignore it.
    * if it is not in varidx --> add a quadterm linear and square coef equal 0 and add it to varidx
    */
   if( SCIPhashmapExists(varidx, var) ) /* var has been seen before */
   {
      int idx;

      idx = (int)(size_t)SCIPhashmapGetImage(varidx, var);

      if( idx >= 0 ) /* var has been seen before, but only as a linear variable -> new quadterm */
      {
         assert(var == exprdata->linvars[idx]);

         SCIP_CALL( exprdataEnsureQuadVarTermsSize(scip, exprdata, exprdata->nquadvars + 1) );

         quadterm = &exprdata->quadvarterms[exprdata->nquadvars];

         quadterm->var = var;
         quadterm->sqrcoef = sqrcoef;
         quadterm->lincoef = exprdata->lincoefs[idx];
         quadterm->nadjbilin = 0;
         quadterm->adjbilinsize = 0;
         quadterm->adjbilin = NULL;

         /* var is no longer linear var --> remove it from exprdata->linvars */
         exprdata->nlinvars--;
         if( idx < exprdata->nlinvars )
         {
            exprdata->linvars[idx] = exprdata->linvars[exprdata->nlinvars];
            exprdata->lincoefs[idx] = exprdata->lincoefs[exprdata->nlinvars];
            SCIP_CALL( SCIPhashmapSetImage(varidx, (void *)exprdata->linvars[idx], (void *)(size_t)idx) );
         }

         /* update index of var */
         SCIP_CALL( SCIPhashmapSetImage(varidx, (void *)var, (void *)(size_t)(-exprdata->nquadvars - 1)) );

         /* update number of quadvars */
         exprdata->nquadvars++;
      }
      else /* var has been seen before in a quadratic expression (var^2 or var * other var) */
      {
         quadterm = &exprdata->quadvarterms[-idx-1];

         if( bilintermidx >= 0 ) /* quadratic variable seen again in a bilinear term; store which */
         {
            SCIP_CALL( exprdataEnsureAdjBilinSize(scip, quadterm, quadterm->nadjbilin + 1) );

            quadterm->adjbilin[quadterm->nadjbilin] = bilintermidx;
            quadterm->nadjbilin++;
         }
         else /* first time seing quadratic variable in var^2 from */
         {
            assert(bilintermidx == -1);
            assert(quadterm->var == var);
            assert(quadterm->sqrcoef == 0.0);

            quadterm->sqrcoef = sqrcoef;
         }
      }
   }
   else /* first time seeing var; it appears in a quadratic expression (var^2 or var * other var) -> new quadterm */
   {
      SCIP_CALL( exprdataEnsureQuadVarTermsSize(scip, exprdata, exprdata->nquadvars + 1) );

      quadterm = &exprdata->quadvarterms[exprdata->nquadvars];

      quadterm->var = var;
      quadterm->lincoef = 0.0;
      quadterm->nadjbilin = 0;
      quadterm->adjbilinsize = 0;
      quadterm->adjbilin = NULL;

      if( bilintermidx >= 0 ) /* seen from a bilinear term; store which */
      {
         SCIP_CALL( exprdataEnsureAdjBilinSize(scip, quadterm, 1) );

         quadterm->adjbilin[quadterm->nadjbilin] = bilintermidx;
         quadterm->nadjbilin++;
         quadterm->sqrcoef = 0.0;
      }
      else /* seen from a quadratic term */
      {
         quadterm->sqrcoef = sqrcoef;
      }

      /* add var to varidx */
      SCIP_CALL( SCIPhashmapInsert(varidx, (void *)var, (void *)(size_t)(-exprdata->nquadvars - 1)) );

      /* update number of quadvars */
      exprdata->nquadvars++;
   }
   return SCIP_OKAY;
}

/*
 * Callback methods of nonlinear handler
 */

/** callback to free data of handler */
static
SCIP_DECL_CONSEXPR_NLHDLRFREEHDLRDATA(freeHdlrDataQuadratic)
{
   return SCIP_OKAY;
}

/** callback to free expression specific data */
static
SCIP_DECL_CONSEXPR_NLHDLRFREEEXPRDATA(freeExprDataQuadratic)
{
   freeNlhdlrExprData(scip, *nlhdlrexprdata);
   SCIPfreeBlockMemory(scip, nlhdlrexprdata);

   return SCIP_OKAY;
}

/** callback to be called in initialization */
static
SCIP_DECL_CONSEXPR_NLHDLRINIT(initHdlrQuadratic)
{
   return SCIP_OKAY;
}

/** callback to be called in deinitialization */
static
SCIP_DECL_CONSEXPR_NLHDLREXIT(exitHldrQuadratic)
{
   return SCIP_OKAY;
}


static
SCIP_Bool isTwoVarsProduct(SCIP* scip, SCIP_CONSHDLR* conshdlr, SCIP_CONSEXPR_EXPR* expr, SCIP_VAR** var1, SCIP_VAR** var2)
{
   assert(var1 != NULL && var2 != NULL);

   if( SCIPgetConsExprExprHdlr(expr) != SCIPgetConsExprExprHdlrProduct(conshdlr) )
      return FALSE;

   if( SCIPgetConsExprExprNChildren(expr) != 2 )
      return FALSE;

   SCIP_CALL( SCIPcreateConsExprExprAuxVar(scip, conshdlr, SCIPgetConsExprExprChildren(expr)[0], var1) );
   assert(*var1 != NULL);

   SCIP_CALL( SCIPcreateConsExprExprAuxVar(scip, conshdlr, SCIPgetConsExprExprChildren(expr)[1], var2) );
   assert(*var2 != NULL);

   return TRUE;
}

static
SCIP_Bool isVarSquare(SCIP* scip, SCIP_CONSHDLR* conshdlr, SCIP_CONSEXPR_EXPR* expr, SCIP_VAR** var)
{
   if( strcmp("pow", SCIPgetConsExprExprHdlrName(SCIPgetConsExprExprHdlr(expr))) != 0 )
      return FALSE;

   assert(SCIPgetConsExprExprNChildren(expr) == 1);

   if( SCIPgetConsExprExprPowExponent(expr) != 2.0 )
      return FALSE;

   SCIP_CALL( SCIPcreateConsExprExprAuxVar(scip, conshdlr, SCIPgetConsExprExprChildren(expr)[0], var) );
   assert(*var != NULL);

   return TRUE;
}

static
SCIP_RETCODE checkProperQuadratic(SCIP_CONSEXPR_EXPR* expr, SCIP_HASHMAP* seenexpr, SCIP_Bool* properquadratic)
{

   if( SCIPhashmapExists(seenexpr, (void *)expr) )
      *properquadratic = TRUE;
   else
   {
      SCIP_CALL( SCIPhashmapInsert(seenexpr, (void *)expr, NULL) );
   }

   return SCIP_OKAY;
}

/* TODO: make more simple test; like diagonal entries don't change sign, etc */
static
SCIP_RETCODE checkCurvature(
   SCIP* scip,
   SCIP_CONSEXPR_NLHDLREXPRDATA* exprdata,
   SCIP_CONSEXPR_EXPRENFO_METHOD* provided
   )
{
   SCIP_HASHMAP* var2matrix;
   double* matrix;
   double* alleigval;
   int nvars;
   int nn;
   int n;
   int i;

   n  = exprdata->nquadvars;
   nn = n * n;

   /* do not check curvature if nn is too large */
   if( nn < 0 || (unsigned) (int) nn > UINT_MAX / sizeof(SCIP_Real) )
   {
      SCIPverbMessage(scip, SCIP_VERBLEVEL_FULL, NULL, "nlhdr_quadratic - number of quadratic variables is too large (%d) to check the curvature; will not handle this expression\n", n);

      return SCIP_OKAY;
   }

   SCIP_CALL( SCIPallocBufferArray(scip, &alleigval, n) );
   SCIP_CALL( SCIPallocBufferArray(scip, &matrix, nn) );
   BMSclearMemoryArray(matrix, nn);

   SCIP_CALL( SCIPhashmapCreate(&var2matrix, SCIPblkmem(scip), n) );

   /* fill matrix's diagonal */
   nvars = 0;
   for( i = 0; i < n; ++i )
   {
      SCIP_QUADVARTERM quadterm;

      quadterm = exprdata->quadvarterms[i];

      assert(!SCIPhashmapExists(var2matrix, (void*)quadterm.var));

      //printf("quadvarterm = %s^2 * %g\n", SCIPvarGetName(quadterm.var), quadterm.sqrcoef);
      if( quadterm.sqrcoef == 0.0 )
      {
         SCIPdebugMsg(scip, "var <%s> appears in bilinear term but is not squared --> indefinite quadratic\n", SCIPvarGetName(quadterm.var));
         goto CLEANUP;
      }

      matrix[nvars * n + nvars] = quadterm.sqrcoef;

      /* remember row of variable in matrix */
      SCIP_CALL( SCIPhashmapInsert(var2matrix, (void *)quadterm.var, (void *)(size_t)nvars) );
      nvars++;
   }

   /* fill matrix's upper-diagonal */
   for( i = 0; i < exprdata->nbilinterms; ++i )
   {
      SCIP_BILINTERM bilinterm;
      int col;
      int row;

      bilinterm = exprdata->bilinterms[i];

      assert(SCIPhashmapExists(var2matrix, (void*)bilinterm.var1));
      assert(SCIPhashmapExists(var2matrix, (void*)bilinterm.var2));
      row = (int)(size_t)SCIPhashmapGetImage(var2matrix, bilinterm.var1);
      col = (int)(size_t)SCIPhashmapGetImage(var2matrix, bilinterm.var2);

      assert(row != col);

      if( row < col )
         matrix[row * n + col] = bilinterm.coef / 2.0;
      else
         matrix[col * n + row] = bilinterm.coef / 2.0;
   }

   /* compute eigenvalues */
   if( LapackDsyev(FALSE, n, matrix, alleigval) != SCIP_OKAY )
   {
      SCIPwarningMessage(scip, "Failed to compute eigenvalues of quadratic coefficient matrix --> don't know curvature\n");
      goto CLEANUP;
   }

   /* check convexity */
   if( !SCIPisNegative(scip, alleigval[0]) )
   {
      *provided = SCIP_CONSEXPR_EXPRENFO_SEPAUNDER;
      exprdata->curvature = SCIP_EXPRCURV_CONVEX;
   }
   else if( !SCIPisPositive(scip, alleigval[n-1]) )
   {
      *provided = SCIP_CONSEXPR_EXPRENFO_SEPAOVER;
      exprdata->curvature = SCIP_EXPRCURV_CONCAVE;
   }

CLEANUP:
   SCIPhashmapFree(&var2matrix);
   SCIPfreeBufferArray(scip, &matrix);
   SCIPfreeBufferArray(scip, &alleigval);

   return SCIP_OKAY;
}

/** callback to detect structure in expression tree */
/*
 * A term is quadratic if:
 * - It is a product expression of two expressions
 * - It is power expression of an expression with exponent 2.0
 *
 * An proper quadratic expression (i.e the only quadratic expressions that can be handled by this nlhdlr) is a sum
 * expression such that there is at least one (aux) variable that appears in at least two quadratic terms.
 *
 * @note:
 * - the expression needs to be simplified (in particular, it is assumed to be sorted)
 * - common subexpressions are also assumed to have been identified.
 *
 * Sorted implies that:
 *  - expr < expr^2: bases are the same, but exponent 1 < 2
 *  - expr < expr * other_expr: u*v < w holds if and only if v < w (OR8), but here w = u < v, since expr comes before
 *  other_expr in the product
 *  - expr < other_expr * expr: u*v < w holds if and only if v < w (OR8), but here v = w
 *
 * It also implies that
 *  - expr^2 < expr * other_expr
 *  - other_expr * expr < expr^2
 *
 * It also implies that x^-2 < x^-1, but since, so far, we do not interpret x^-2 as (x^-1)^2, it is not a problem.
 */
/* TODO: capture variables? */
static
SCIP_DECL_CONSEXPR_NLHDLRDETECT(detectHdlrQuadratic)
{
   SCIP_CONSEXPR_NLHDLREXPRDATA* exprdata;
   SCIP_HASHMAP*  varidx;
   SCIP_HASHMAP*  seenexpr;
   SCIP_VAR* var1 = NULL;
   SCIP_VAR* var2 = NULL;
   SCIP_Bool properquadratic;
   int c;

   assert(scip != NULL);
   assert(nlhdlr != NULL);
   assert(expr != NULL);
   assert(provided != NULL);
   assert(nlhdlrexprdata != NULL);

   *provided = SCIP_CONSEXPR_EXPRENFO_NONE;
   printf("Detecting expression\n");
   SCIP_CALL( SCIPdismantleConsExprExpr(scip, expr) );

   /* if it is not a sum of at least two terms, it cannot be a proper quadratic expressions */
   if( SCIPgetConsExprExprHdlr(expr) != SCIPgetConsExprExprHdlrSum(conshdlr) || SCIPgetConsExprExprNChildren(expr) < 2 )
      return SCIP_OKAY;

   /* check if expression is a proper quadratic expression */
   properquadratic = FALSE;
   SCIP_CALL( SCIPhashmapCreate(&seenexpr, SCIPblkmem(scip), 2*SCIPgetConsExprExprNChildren(expr)) );
   for( c = 0; c < SCIPgetConsExprExprNChildren(expr); ++c )
   {
      SCIP_CONSEXPR_EXPR* child;
      SCIP_Real coef;

      child = SCIPgetConsExprExprChildren(expr)[c];
      coef = SCIPgetConsExprExprSumCoefs(expr)[c];

      assert(child != NULL);
      assert(! SCIPisZero(scip, coef));

      if( strcmp("pow", SCIPgetConsExprExprHdlrName(SCIPgetConsExprExprHdlr(child))) == 0 &&
            SCIPgetConsExprExprPowExponent(child) == 2.0 ) /* quadratic term */
      {
         SCIP_CALL( checkProperQuadratic(SCIPgetConsExprExprChildren(child)[0], seenexpr, &properquadratic) );
      }
      else if( SCIPgetConsExprExprHdlr(child) == SCIPgetConsExprExprHdlrProduct(conshdlr) &&
            SCIPgetConsExprExprNChildren(child) == 2 ) /* bilinear term */
      {
         SCIP_CALL( checkProperQuadratic(SCIPgetConsExprExprChildren(child)[0], seenexpr, &properquadratic) );
         SCIP_CALL( checkProperQuadratic(SCIPgetConsExprExprChildren(child)[1], seenexpr, &properquadratic) );
      }
      else
      {
         SCIP_CALL( checkProperQuadratic(child, seenexpr, &properquadratic) );
      }

      if( properquadratic )
         break;
   }
   SCIPhashmapFree(&seenexpr);

   if( ! properquadratic )
      return SCIP_OKAY;

   /* varidx maps vars to indices; if index > 0, it is its index in the linvars array, otherwise -index-1 is its
    * index in the quadterms array
    */
   SCIP_CALL( SCIPhashmapCreate(&varidx, SCIPblkmem(scip), SCIPgetConsExprExprNChildren(expr)) );

   /* sets everything to 0; exprdata->nquadvars, etc */
   SCIP_CALL( SCIPallocClearBlockMemory(scip, nlhdlrexprdata) );
   exprdata = *nlhdlrexprdata;

   /* for every term of the expr */
   for( c = 0; c < SCIPgetConsExprExprNChildren(expr); ++c )
   {
      SCIP_CONSEXPR_EXPR* child;
      SCIP_Real coef;

      child = SCIPgetConsExprExprChildren(expr)[c];
      coef = SCIPgetConsExprExprSumCoefs(expr)[c];

      assert(child != NULL);
      assert(! SCIPisZero(scip, coef));

      var1 = NULL;
      var2 = NULL;
      if( isVarSquare(scip, conshdlr, child, &var1) ) /* quadratic term */
      {
         assert(var1 != NULL);
         SCIP_CALL( addVarToQuadterms(scip, var1, coef, -1, exprdata, varidx) );
      }
      else if( isTwoVarsProduct(scip, conshdlr, child, &var1, &var2) ) /* bilinear term */
      {
         SCIP_BILINTERM* bilinterm;
         assert(SCIPgetConsExprExprProductCoef(child) == 1.0);
         assert(var1 != NULL && var2 != NULL);

         SCIP_CALL( exprdataEnsureBilinSize(scip, exprdata, exprdata->nbilinterms + 1) );

         bilinterm = &exprdata->bilinterms[exprdata->nbilinterms];

         bilinterm->coef = coef;
         bilinterm->var1 = var1;
         bilinterm->var2 = var2;

         /* variables involved in a bilinear term that are not in a quadterm, need to be added to a quadterm and removed
          * from exprdata->linvars */
         SCIP_CALL( addVarToQuadterms(scip, var1, 0.0, exprdata->nbilinterms, exprdata, varidx) );
         SCIP_CALL( addVarToQuadterms(scip, var2, 0.0, exprdata->nbilinterms, exprdata, varidx) );

         exprdata->nbilinterms++;
      }
      else
      {
         /* not a product of exprs nor square of an expr --> create aux var */
         var1 = SCIPgetConsExprExprLinearizationVar(child);
         assert(var1 != NULL);

         SCIP_CALL( exprdataEnsureLinearVarsSize(scip, exprdata, exprdata->nlinvars + 1) );

         /* store its index in linvars in case we see this var again later in a bilinear or quadratic term */
         SCIP_CALL( SCIPhashmapInsert(varidx, var1, (void *)(size_t)exprdata->nlinvars) );
         exprdata->linvars[exprdata->nlinvars] = var1;
         exprdata->lincoefs[exprdata->nlinvars] = coef;
         exprdata->nlinvars++;
      }
   }
   SCIPhashmapFree(&varidx);

   /* check curvature of quadratic function stored in exprdata */
   SCIP_CALL( checkCurvature(scip, exprdata, provided) );

   /* if we can't handle this expression, free data */
   if( *provided == SCIP_CONSEXPR_EXPRENFO_NONE )
   {
      SCIP_CALL( freeExprDataQuadratic(scip, nlhdlr, nlhdlrexprdata) );
   }

   return SCIP_OKAY;
}

/** nonlinear handler separation callback */
static
SCIP_DECL_CONSEXPR_NLHDLRSEPA(sepaHdlrQuadratic)
{
   SCIP_ROWPREP* rowprep;
   SCIP_Real constant;
   SCIP_Real coef;
   SCIP_Real coef2;
   SCIP_VAR* var;
   SCIP_Bool success;
   int j;
   int k;

   assert(scip != NULL);
   assert(expr != NULL);
   assert(conshdlr != NULL);
   assert(SCIPgetConsExprExprHdlr(expr) == SCIPgetConsExprExprHdlrSum(conshdlr));
   assert(result != NULL);
   assert(nlhdlrexprdata != NULL);
   assert(nlhdlrexprdata->curvature == SCIP_EXPRCURV_CONVEX || nlhdlrexprdata->curvature == SCIP_EXPRCURV_CONCAVE);

   *ncuts = 0;
   *result = SCIP_DIDNOTFIND; /* TODO: is this ok? */

   SCIP_CALL( SCIPcreateRowprep(scip, &rowprep, SCIP_SIDETYPE_RIGHT, FALSE) );

   /* check violation side; one must use expression stored in nlhdlrexprdata!
    * TODO: maybe have a flag to know whether the expression is quadratic in the original variables
    */
   {
      SCIP_Real activity;
      SCIP_Real side;
      SCIP_VAR* auxvar;
      int i;

      activity = SCIPgetConsExprExprSumConstant(expr); /* TODO: is this okay or should the constant be stored at the moment of creation? */
      for( i = 0; i < nlhdlrexprdata->nlinvars; ++i ) /* linear vars */
         activity += nlhdlrexprdata->lincoefs[i] * SCIPgetSolVal(scip, sol, nlhdlrexprdata->linvars[i]);
      for( i = 0; i < nlhdlrexprdata->nquadvars; ++i ) /* quadratic terms */
      {
         SCIP_QUADVARTERM quadterm;
         SCIP_Real solval;

         quadterm = nlhdlrexprdata->quadvarterms[i];
         solval = SCIPgetSolVal(scip, sol, quadterm.var);
         activity += (quadterm.lincoef + quadterm.sqrcoef * solval) * solval;
      }
      for( i = 0; i < nlhdlrexprdata->nbilinterms; ++i ) /* bilinear terms */
      {
         SCIP_BILINTERM bilinterm;

         bilinterm = nlhdlrexprdata->bilinterms[i];
         activity += bilinterm.coef * SCIPgetSolVal(scip, sol, bilinterm.var1) * SCIPgetSolVal(scip, sol, bilinterm.var2);
      }

      auxvar = SCIPgetConsExprExprLinearizationVar(expr);
      assert(auxvar != NULL);

      side = SCIPgetSolVal(scip, sol, auxvar);

      SCIPdebugMsg(scip, "Activity = %g (act of expr is %g), side = %g, curvature %s\n", activity,
            SCIPgetConsExprExprValue(expr), side, nlhdlrexprdata->curvature == SCIP_EXPRCURV_CONVEX ? "convex" :
            "concave");
      //SCIP_CALL( SCIPprintConsExprExpr(scip, expr, NULL) );

      if( activity > side && nlhdlrexprdata->curvature == SCIP_EXPRCURV_CONVEX )
         rowprep->sidetype = SCIP_SIDETYPE_RIGHT;
      else if( activity < side && nlhdlrexprdata->curvature == SCIP_EXPRCURV_CONCAVE )
         rowprep->sidetype = SCIP_SIDETYPE_LEFT;
      else
         goto CLEANUP;
   }

   /* compute cut */
   success = TRUE;
   SCIP_BILINTERM* bilinterm;

   /*
    * do first-order Taylor for each term
    */

   /* constat */
   SCIPaddRowprepConstant(rowprep, SCIPgetConsExprExprSumConstant(expr));

   /* purely linear variables */
   SCIP_CALL( SCIPaddRowprepTerms(scip, rowprep, nlhdlrexprdata->nlinvars, nlhdlrexprdata->linvars,
            nlhdlrexprdata->lincoefs) );

   /* quadratic variables */
   for( j = 0; j < nlhdlrexprdata->nquadvars && success; ++j )
   {
      var = nlhdlrexprdata->quadvarterms[j].var;

      /* initialize coefficients to linear coefficients of quadratic variables */
      SCIP_CALL( SCIPaddRowprepTerm(scip, rowprep, var, nlhdlrexprdata->quadvarterms[j].lincoef) );

      /* add linearization of square term */
      coef = 0.0;
      constant = 0.0;
      SCIPaddSquareLinearization(scip, nlhdlrexprdata->quadvarterms[j].sqrcoef, SCIPgetSolVal(scip, sol, var),
         nlhdlrexprdata->quadvarterms[j].nadjbilin == 0 && SCIPvarGetType(var) < SCIP_VARTYPE_CONTINUOUS, &coef, &constant, &success);

      SCIP_CALL( SCIPaddRowprepTerm(scip, rowprep, var, coef) );
      SCIPaddRowprepConstant(rowprep, constant);

      /* add linearization of bilinear terms that have var as first variable */
      for( k = 0; k < nlhdlrexprdata->quadvarterms[j].nadjbilin && success; ++k )
      {
         bilinterm = &nlhdlrexprdata->bilinterms[nlhdlrexprdata->quadvarterms[j].adjbilin[k]];
         if( bilinterm->var1 != var )
            continue;
         assert(bilinterm->var2 != var);

         coef = 0.0;
         coef2 = 0.0;
         constant = 0.0;
         SCIPaddBilinLinearization(scip, bilinterm->coef, SCIPgetSolVal(scip, sol, var), SCIPgetSolVal(scip, sol,
                  bilinterm->var2), &coef, &coef2, &constant, &success);
         SCIP_CALL( SCIPaddRowprepTerm(scip, rowprep, var, coef) );
         SCIP_CALL( SCIPaddRowprepTerm(scip, rowprep, bilinterm->var2, coef2) );
         SCIPaddRowprepConstant(rowprep, constant);
      }
   }
   if( !success )
      goto CLEANUP;

   /* add auxiliary variable */
   SCIP_CALL( SCIPaddRowprepTerm(scip, rowprep, SCIPgetConsExprExprLinearizationVar(expr), -1.0) );

   SCIPaddRowprepSide(rowprep, 0.0);

   /* check build cut and check violation */
   {
      SCIP_Real coefrange;
      SCIP_Real viol;
      SCIP_ROW* row;
      SCIP_Bool infeasible;

      viol = SCIPgetRowprepViolation(scip, rowprep, sol);

      if( viol <= 0 )
         goto CLEANUP;

      SCIPmergeRowprepTerms(scip, rowprep);

      /* improve coefficients */
      SCIP_CALL( SCIPcleanupRowprep(scip, rowprep, sol, 1e7, minviolation, &coefrange, &viol) );
      success = coefrange <= 1e7; /* magic number should maybe be given in as argument? */

      if( !success || viol < minviolation )
         goto CLEANUP;

      SCIP_CALL( SCIPgetRowprepRowCons(scip, &row, rowprep, conshdlr) );

      SCIP_CALL( SCIPaddRow(scip, row, TRUE, &infeasible) );
      (*ncuts)++;

      if( infeasible )
         *result = SCIP_CUTOFF;
      else
         *result = SCIP_SEPARATED;
   }

CLEANUP:
   SCIPfreeRowprep(scip, &rowprep);

   return SCIP_OKAY;
}

/** nonlinear handler copy callback
 *
 * the method includes the nonlinear handler into a expression constraint handler
 *
 * This method is usually called when doing a copy of an expression constraint handler.
 */
static
SCIP_DECL_CONSEXPR_NLHDLRCOPYHDLR(copyHdlrQuadratic)
{
   SCIP_CONSEXPR_NLHDLR* targetnlhdlr;
   SCIP_CONSEXPR_NLHDLRDATA* nlhdlrdata;

   assert(targetscip != NULL);
   assert(targetconsexprhdlr != NULL);
   assert(sourceconsexprhdlr != NULL);
   assert(sourcenlhdlr != NULL);
   assert(strcmp(SCIPgetConsExprNlhdlrName(sourcenlhdlr), "testhdlr") == 0);

   SCIP_CALL( SCIPallocClearMemory(targetscip, &nlhdlrdata) );

   SCIP_CALL( SCIPincludeConsExprNlhdlrBasic(targetscip, targetconsexprhdlr, &targetnlhdlr,
            SCIPgetConsExprNlhdlrName(sourcenlhdlr), SCIPgetConsExprNlhdlrDesc(sourcenlhdlr),
            SCIPgetConsExprNlhdlrPriority(sourcenlhdlr), detectHdlrQuadratic, nlhdlrdata) );

   SCIPsetConsExprNlhdlrFreeHdlrData(targetscip, targetnlhdlr, freeHdlrDataQuadratic);
   SCIPsetConsExprNlhdlrFreeExprData(targetscip, targetnlhdlr, freeExprDataQuadratic);
   SCIPsetConsExprNlhdlrCopyHdlr(targetscip, targetnlhdlr, copyHdlrQuadratic);
   SCIPsetConsExprNlhdlrInitExit(targetscip, targetnlhdlr, initHdlrQuadratic, exitHldrQuadratic);
   SCIPsetConsExprNlhdlrSepa(targetscip, targetnlhdlr, NULL, sepaHdlrQuadratic, NULL); // TODO: init exit sepa callbacks

   return SCIP_OKAY;
}

/** includes quadratic nonlinear handler to consexpr */
SCIP_RETCODE SCIPincludeConsExprNlhdlrQuadratic(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_CONSHDLR*        consexprhdlr        /**< expression constraint handler */
   )
{
   SCIP_CONSEXPR_NLHDLR* nlhdlr;
   SCIP_CONSEXPR_NLHDLRDATA* nlhdlrdata;

   assert(scip != NULL);
   assert(consexprhdlr != NULL);

   /* SCIP_CALL( SCIPallocClearMemory(scip, &nlhdlrdata) ); */
   nlhdlrdata = NULL;

   SCIP_CALL( SCIPincludeConsExprNlhdlrBasic(scip, consexprhdlr, &nlhdlr, NLHDLR_NAME, NLHDLR_DESC, NLHDLR_PRIORITY,
            detectHdlrQuadratic, nlhdlrdata) );

   SCIPsetConsExprNlhdlrFreeExprData(scip, nlhdlr, freeExprDataQuadratic);
   SCIPsetConsExprNlhdlrSepa(scip, nlhdlr, NULL, sepaHdlrQuadratic, NULL);



   /* TODO: create and store expression specific data here */

   return SCIP_OKAY;
}
