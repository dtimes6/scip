/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2016 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the ZIB Academic License.         */
/*                                                                           */
/*  You should have received a copy of the ZIB Academic License              */
/*  along with SCIP; see the file COPYING. If not email to scip@zib.de.      */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/**@file   cons_expr_entropy.c
 * @brief  handler for -x*log(x) expressions
 * @author Benjamin Mueller
 * @author Fabian Wegscheider
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#include "scip/cons_expr_entropy.h"
#include "scip/cons_expr_value.h"
#include "scip/cons_expr.h"

#define SCIP_PRIVATE_ROWPREP
#include "scip/cons_quadratic.h"

#include <string.h>

/* fundamental expression handler properties */
#define EXPRHDLR_NAME         "entropy"
#define EXPRHDLR_DESC         "expression handler for -x*log(x)"
#define EXPRHDLR_PRECEDENCE   0
#define EXPRHDLR_HASHKEY      SCIPcalcFibHash(7477.0)

/*
 * Data structures
 */

/*
 * Local methods
 */

/** helper function to separate a given point; needed for proper unittest */
static
SCIP_RETCODE separatePointEntropy(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_CONSHDLR*        conshdlr,           /**< expression constraint handler */
   SCIP_CONSEXPR_EXPR*   expr,               /**< entropy expression */
   SCIP_SOL*             sol,                /**< solution to be separated (NULL for the LP solution) */
   SCIP_Real             mincutviolation,    /**< minimal cut violation to be achieved */
   SCIP_Bool             overestimate,       /**< should the expression be overestimated? */
   SCIP_ROW**            cut                 /**< pointer to store the row */
   )
{
   SCIP_CONSEXPR_EXPR* child;
   SCIP_ROWPREP* rowprep;
   SCIP_VAR* auxvar;
   SCIP_VAR* childvar;
   SCIP_Real refpoint;
   SCIP_Real coef;
   SCIP_Real constant;
   SCIP_Bool success;

   assert(scip != NULL);
   assert(conshdlr != NULL);
   assert(strcmp(SCIPconshdlrGetName(conshdlr), "expr") == 0);
   assert(expr != NULL);
   assert(strcmp(SCIPgetConsExprExprHdlrName(SCIPgetConsExprExprHdlr(expr)), EXPRHDLR_NAME) == 0);
   assert(cut != NULL);

   *cut = NULL;

   /* get linearization variable */
   auxvar = SCIPgetConsExprExprAuxVar(expr);
   assert(auxvar != NULL);

   /* get expression data */
   child = SCIPgetConsExprExprChildren(expr)[0];
   assert(child != NULL);
   childvar = SCIPgetConsExprExprAuxVar(child);
   assert(childvar != NULL);

   refpoint = SCIPgetSolVal(scip, sol, childvar);

   /* reference point is outside the domain of f(x) = x*log(x) */
   if( refpoint < 0.0 )
      return SCIP_OKAY;

   /* use secant for underestimate (locally valid) */
   if( !overestimate )
   {
      SCIP_Real lb;
      SCIP_Real ub;
      SCIP_Real vallb;
      SCIP_Real valub;

      lb = SCIPvarGetLbLocal(childvar);
      ub = SCIPvarGetUbLocal(childvar);

      if( lb < 0.0 || SCIPisInfinity(scip, ub) || SCIPisEQ(scip, lb, ub) )
         return SCIP_OKAY;

      assert(lb >= 0.0 && ub >= 0.0);
      assert(ub - lb != 0.0);

      vallb = (lb == 0.0) ? 0.0 : -lb * log(lb);
      valub = (ub == 0.0) ? 0.0 : -ub * log(ub);

      coef = (valub - vallb) / (ub - lb);
      constant = valub - coef * ub;
      assert(SCIPisEQ(scip, constant, vallb - coef * lb));
   }
   /* use gradient cut for underestimate (globally valid) */
   else
   {
      /* no gradient cut possible if reference point is too close at 0 */
      if( SCIPisZero(scip, refpoint) )
         return SCIP_OKAY;

      /* -x*(1+log(x*)) + x* <= -x*log(x) */
      coef = -(1.0 + log(refpoint));
      constant = refpoint;
   }

   /* give up if the constant or coefficient is too large */
   if( SCIPisInfinity(scip, REALABS(constant)) || SCIPisInfinity(scip, REALABS(coef)) )
      return SCIP_OKAY;

   /* create cut */
   SCIP_CALL( SCIPcreateRowprep(scip, &rowprep, overestimate ? SCIP_SIDETYPE_LEFT : SCIP_SIDETYPE_RIGHT, !overestimate) );
   SCIPaddRowprepConstant(rowprep, constant);
   SCIP_CALL( SCIPensureRowprepSize(scip, rowprep, 2) );
   SCIP_CALL( SCIPaddRowprepTerm(scip, rowprep, auxvar, -1.0) );
   SCIP_CALL( SCIPaddRowprepTerm(scip, rowprep, childvar, coef) );

   /* take care of cut numerics */
   SCIP_CALL( SCIPcleanupRowprep(scip, rowprep, sol, SCIP_CONSEXPR_CUTMAXRANGE, mincutviolation, NULL, &success) );

   if( success )
   {
      (void) SCIPsnprintf(rowprep->name, SCIP_MAXSTRLEN, "entropy_cut");  /* @todo make cutname unique, e.g., add LP number */
      SCIP_CALL( SCIPgetRowprepRowCons(scip, cut, rowprep, conshdlr) );
   }

   SCIPfreeRowprep(scip, &rowprep);

   return SCIP_OKAY;
}

/** helper function for reverseProp() which returns an x* in [xmin,xmax] s.t. the distance -x*log(x) and a given target
 *  value is minimized; the function assumes that -x*log(x) is monotone on [xmin,xmax];
 */
static
SCIP_Real reversePropBinarySearch(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_Real             xmin,               /**< smallest possible x */
   SCIP_Real             xmax,               /**< largest possible x */
   SCIP_Bool             increasing,         /**< -x*log(x) is increasing or decreasing on [xmin,xmax] */
   SCIP_Real             targetval           /**< target value */
   )
{
   SCIP_Real xminval = (xmin == 0.0) ? 0.0 : -xmin * log(xmin);
   SCIP_Real xmaxval = (xmax == 0.0) ? 0.0 : -xmax * log(xmax);
   int i;

   assert(xmin <= xmax);
   assert(increasing ? xminval <= xmaxval : xminval >= xmaxval);

   /* function can not achieve -x*log(x) -> return xmin or xmax */
   if( SCIPisGE(scip, xminval, targetval) && SCIPisGE(scip, xmaxval, targetval) )
      return increasing ? xmin : xmax;
   else if( SCIPisLE(scip, xminval, targetval) && SCIPisLE(scip, xmaxval, targetval) )
      return increasing ? xmax : xmin;

   /* binary search */
   for( i = 0; i < 1000; ++i )
   {
      SCIP_Real x = (xmin + xmax) / 2.0;
      SCIP_Real xval = (x == 0.0) ? 0.0 : -x * log(x);

      /* found the corresponding point -> skip */
      if( SCIPisEQ(scip, xval, targetval) )
         return x;
      else if( SCIPisLT(scip, xval, targetval) )
      {
         if( increasing )
            xmin = x;
         else
            xmax = x;
      }
      else
      {
         if( increasing )
            xmax = x;
         else
            xmin = x;
      }
   }

   return SCIP_INVALID;
}

/** helper function for reverse propagation; needed for proper unittest */
static
SCIP_RETCODE reverseProp(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_INTERVAL         exprinterval,       /**< bounds on the expression */
   SCIP_INTERVAL         childinterval,      /**< bounds on the interval of the child */
   SCIP_INTERVAL*        interval            /**< resulting interval */
   )
{
   SCIP_INTERVAL childentropy;
   SCIP_INTERVAL intersection;
   SCIP_INTERVAL tmp;
   SCIP_Real childinf;
   SCIP_Real childsup;
   SCIP_Real extremum;
   SCIP_Real boundinf;
   SCIP_Real boundsup;

   assert(scip != NULL);
   assert(interval != NULL);

   /* check whether domain is empty, i.e., bounds on -x*log(x) > 1/e */
   if( SCIPisGT(scip, SCIPintervalGetInf(exprinterval), exp(-1.0))
      || SCIPintervalIsEmpty(SCIPinfinity(scip), childinterval) )
   {
      SCIPintervalSetEmpty(interval);
      return SCIP_OKAY;
   }

   /* compute the intersection between entropy([childinf,childsup]) and [expr.inf, expr.sup] */
   SCIPintervalEntropy(SCIP_INTERVAL_INFINITY, &childentropy, childinterval);
   SCIPintervalIntersect(&intersection, childentropy, exprinterval);

   /* intersection empty -> infeasible */
   if( SCIPintervalIsEmpty(SCIP_INTERVAL_INFINITY, intersection) )
   {
      SCIPintervalSetEmpty(interval);
      return SCIP_OKAY;
   }

   /* intersection = childentropy -> nothing can be learned */
   if( SCIPintervalIsSubsetEQ(SCIP_INTERVAL_INFINITY, childentropy, intersection) )
   {
      *interval = childinterval;
      return SCIP_OKAY;
   }

   childinf = MAX(0.0, SCIPintervalGetInf(childinterval)); /*lint !e666*/
   childsup = SCIPintervalGetSup(childinterval);
   extremum = exp(-1.0);
   boundinf = SCIP_INVALID;
   boundsup = SCIP_INVALID;

   /*
    * check whether lower bound of child can be improved
    */
   SCIPintervalSetBounds(&tmp, childinf, childinf);
   SCIPintervalEntropy(SCIP_INTERVAL_INFINITY, &tmp, tmp);

   /* entropy(childinf) < intersection.inf -> consider [childinf, MIN(childsup, extremum)] */
   if( SCIPisLT(scip, SCIPintervalGetSup(tmp), SCIPintervalGetInf(intersection)) )
   {
      boundinf = reversePropBinarySearch(scip, childinf, MIN(extremum, childsup), TRUE,
         SCIPintervalGetInf(intersection));
   }
   /* entropy(childinf) > intersection.sup -> consider [MAX(childinf,extremum), childsup] */
   else if( SCIPisGT(scip, SCIPintervalGetInf(tmp), SCIPintervalGetSup(intersection)) )
   {
      boundinf = reversePropBinarySearch(scip, MAX(childinf,extremum), childsup, FALSE,
         SCIPintervalGetSup(intersection));
   }

   /*
    * check whether upper bound of child can be improved
    */
   SCIPintervalSetBounds(&tmp, childsup, childsup);
   SCIPintervalEntropy(SCIP_INTERVAL_INFINITY, &tmp, tmp);

   /* entropy(childsup) < intersection.inf -> consider [MAX(childinf,extremum), childsup] */
   if( SCIPisLT(scip, SCIPintervalGetSup(tmp), SCIPintervalGetInf(intersection)) )
   {
      boundsup = reversePropBinarySearch(scip, MAX(childinf,extremum), childsup, FALSE,
         SCIPintervalGetInf(intersection));
   }
   /* entropy(childsup) > intersection.sup -> consider [childinf, MIN(childsup,extremum)] */
   else if( SCIPisGT(scip, SCIPintervalGetInf(tmp), SCIPintervalGetSup(intersection)) )
   {
      boundinf = reversePropBinarySearch(scip, childinf, MIN(childsup,extremum), TRUE,
         SCIPintervalGetSup(intersection));
   }

   if( boundinf != SCIP_INVALID ) /*lint !e777*/
   {
      assert(boundinf > childinf);
      childinf = MAX(childinf, boundinf);
   }
   if( boundsup != SCIP_INVALID ) /*lint !e777*/
   {
      assert(boundsup < childsup);
      childsup = boundsup;
   }
   assert(childinf <= childsup); /* infeasible case has been handled already */

   /* set the resulting bounds */
   SCIPintervalSetBounds(interval, childinf, childsup);

   return SCIP_OKAY;
}

/*
 * Callback methods of expression handler
 */

/** expression handler copy callback */
static
SCIP_DECL_CONSEXPR_EXPRCOPYHDLR(copyhdlrEntropy)
{  /*lint --e{715}*/
   SCIP_CALL( SCIPincludeConsExprExprHdlrEntropy(scip, consexprhdlr) );
   *valid = TRUE;

   return SCIP_OKAY;
}

/** simplifies an entropy expression */
static
SCIP_DECL_CONSEXPR_EXPRSIMPLIFY(simplifyEntropy)
{  /*lint --e{715}*/
   SCIP_CONSEXPR_EXPR* child;
   SCIP_CONSHDLR* conshdlr;

   assert(scip != NULL);
   assert(expr != NULL);
   assert(simplifiedexpr != NULL);
   assert(SCIPgetConsExprExprNChildren(expr) == 1);

   conshdlr = SCIPfindConshdlr(scip, "expr");
   assert(conshdlr != NULL);

   child = SCIPgetConsExprExprChildren(expr)[0];
   assert(child != NULL);

   /* check for value expression */
   if( SCIPgetConsExprExprHdlr(child) == SCIPgetConsExprExprHdlrValue(conshdlr) )
   {
      SCIP_Real childvalue = SCIPgetConsExprExprValueValue(child);

      /* TODO how to handle a negative value? */
      assert(childvalue >= 0.0);

      if( childvalue == 0.0 || childvalue == 1.0 )
      {
         SCIP_CALL( SCIPcreateConsExprExprValue(scip, conshdlr, simplifiedexpr, 0.0) );
      }
      else
      {
         SCIP_CALL( SCIPcreateConsExprExprValue(scip, conshdlr, simplifiedexpr, -childvalue * log(childvalue)) );
      }
   }
   else
   {
      *simplifiedexpr = expr;

      /* we have to capture it, since it must simulate a "normal" simplified call in which a new expression is created */
      SCIPcaptureConsExprExpr(*simplifiedexpr);
   }

   return SCIP_OKAY;
}

/** expression data copy callback */
static
SCIP_DECL_CONSEXPR_EXPRCOPYDATA(copydataEntropy)
{  /*lint --e{715}*/
   assert(targetexprdata != NULL);
   assert(sourceexpr != NULL);
   assert(SCIPgetConsExprExprData(sourceexpr) == NULL);

   *targetexprdata = NULL;
   return SCIP_OKAY;
}

/** expression data free callback */
static
SCIP_DECL_CONSEXPR_EXPRFREEDATA(freedataEntropy)
{  /*lint --e{715}*/
   assert(expr != NULL);

   SCIPsetConsExprExprData(expr, NULL);
   return SCIP_OKAY;
}

/** expression print callback */
static
SCIP_DECL_CONSEXPR_EXPRPRINT(printEntropy)
{  /*lint --e{715}*/
   assert(expr != NULL);
   assert(SCIPgetConsExprExprData(expr) == NULL);

   switch( stage )
   {
   case SCIP_CONSEXPREXPRWALK_ENTEREXPR :
   {
      /* print function with opening parenthesis */
      SCIPinfoMessage(scip, file, "entropy(");
      break;
   }

   case SCIP_CONSEXPREXPRWALK_VISITINGCHILD :
   {
      assert(SCIPgetConsExprExprWalkCurrentChild(expr) == 0);
      break;
   }

   case SCIP_CONSEXPREXPRWALK_LEAVEEXPR :
   {
      /* print closing parenthesis */
      SCIPinfoMessage(scip, file, ")");
      break;
   }

   case SCIP_CONSEXPREXPRWALK_VISITEDCHILD :
   default: ;
   }

   return SCIP_OKAY;
}

/** expression parse callback */
static
SCIP_DECL_CONSEXPR_EXPRPARSE(parseEntropy)
{
   SCIP_CONSEXPR_EXPR* childexpr;

   assert(expr != NULL);

   /* parse child expression from remaining string */
   SCIP_CALL( SCIPparseConsExprExpr(scip, consexprhdlr, string, endstring, &childexpr) );
   assert(childexpr != NULL);

   /* create entropy expression */
   SCIP_CALL( SCIPcreateConsExprExprEntropy(scip, consexprhdlr, expr, childexpr) );
   assert(*expr != NULL);

   /* release child expression since it has been captured by the entropy expression */
   SCIP_CALL( SCIPreleaseConsExprExpr(scip, &childexpr) );

   *success = TRUE;

   return SCIP_OKAY;
}


/** expression (point-) evaluation callback */
static
SCIP_DECL_CONSEXPR_EXPREVAL(evalEntropy)
{  /*lint --e{715}*/
   SCIP_Real childvalue;

   assert(expr != NULL);
   assert(SCIPgetConsExprExprData(expr) == NULL);
   assert(SCIPgetConsExprExprNChildren(expr) == 1);
   assert(SCIPgetConsExprExprValue(SCIPgetConsExprExprChildren(expr)[0]) != SCIP_INVALID); /*lint !e777*/

   childvalue = SCIPgetConsExprExprValue(SCIPgetConsExprExprChildren(expr)[0]);

   if( childvalue < 0.0 )
   {
      SCIPdebugMsg(scip, "invalid evaluation of entropy expression\n");
      *val = SCIP_INVALID;
   }
   else if( childvalue == 0.0 || childvalue == 1.0 )
   {
      /* -x*log(x) = 0 iff x in {0,1} */
      *val = 0.0;
   }
   else
   {
      *val = -childvalue * log(childvalue);
   }

   return SCIP_OKAY;
}

/** expression derivative evaluation callback */
static
SCIP_DECL_CONSEXPR_EXPRBWDIFF(bwdiffEntropy)
{  /*lint --e{715}*/
   SCIP_CONSEXPR_EXPR* child;
   SCIP_Real childvalue;

   assert(expr != NULL);
   assert(childidx == 0);
   assert(SCIPgetConsExprExprNChildren(expr) == 1);
   assert(SCIPgetConsExprExprValue(expr) != SCIP_INVALID); /*lint !e777*/

   child = SCIPgetConsExprExprChildren(expr)[0];
   assert(child != NULL);
   assert(strcmp(SCIPgetConsExprExprHdlrName(SCIPgetConsExprExprHdlr(child)), "val") != 0);

   childvalue = SCIPgetConsExprExprValue(child);

   /* derivative is not defined for x = 0 */
   if( childvalue <= 0.0 )
      *val = SCIP_INVALID;
   else
      *val = -1.0 - log(childvalue);

   return SCIP_OKAY;
}

/** expression interval evaluation callback */
static
SCIP_DECL_CONSEXPR_EXPRINTEVAL(intevalEntropy)
{  /*lint --e{715}*/
   SCIP_INTERVAL childinterval;

   assert(expr != NULL);
   assert(SCIPgetConsExprExprData(expr) == NULL);
   assert(SCIPgetConsExprExprNChildren(expr) == 1);

   childinterval = SCIPgetConsExprExprInterval(SCIPgetConsExprExprChildren(expr)[0]);
   assert(!SCIPintervalIsEmpty(SCIPinfinity(scip), childinterval));

   SCIPintervalEntropy(SCIPinfinity(scip), interval, childinterval);

   return SCIP_OKAY;
}

/** expression separation callback */
static
SCIP_DECL_CONSEXPR_EXPRSEPA(sepaEntropy)
{  /*lint --e{715}*/
   SCIP_ROW* cut;
   SCIP_Bool infeasible;

   cut = NULL;
   *ncuts = 0;
   *result = SCIP_DIDNOTFIND;

   SCIP_CALL( separatePointEntropy(scip, conshdlr, expr, sol, mincutviolation, overestimate, &cut) );

   /* failed to compute a nice cut */
   if( cut == NULL )
      return SCIP_OKAY;

   /* add cut */
   SCIP_CALL( SCIPaddRow(scip, cut, FALSE, &infeasible) );
   *result = infeasible ? SCIP_CUTOFF : SCIP_SEPARATED;
   *ncuts += 1;

#ifdef SCIP_DEBUG
   SCIPdebugMsg(scip, "add cut with violation %e\n", violation);
   SCIP_CALL( SCIPprintRow(scip, cut, NULL) );
#endif

   SCIP_CALL( SCIPreleaseRow(scip, &cut) );

   return SCIP_OKAY;
}

/** expression reverse propagation callback */
static
SCIP_DECL_CONSEXPR_REVERSEPROP(reversepropEntropy)
{  /*lint --e{715}*/
   SCIP_INTERVAL newinterval;
   SCIP_INTERVAL exprinterval;
   SCIP_INTERVAL childinterval;

   SCIP_CONSEXPR_EXPR* child;

   assert(scip != NULL);
   assert(expr != NULL);
   assert(SCIPgetConsExprExprNChildren(expr) == 1);
   assert(nreductions != NULL);

   *nreductions = 0;

   child = SCIPgetConsExprExprChildren(expr)[0];
   childinterval = SCIPgetConsExprExprInterval(child);
   exprinterval = SCIPgetConsExprExprInterval(expr);

   /* compute resulting intervals */
   SCIP_CALL( reverseProp(scip, exprinterval, childinterval, &newinterval) );

   /* try to tighten the bounds of the child node */
   SCIP_CALL( SCIPtightenConsExprExprInterval(scip, child, newinterval, force, reversepropqueue, infeasible, nreductions) );

   return SCIP_OKAY;
}

/** entropy hash callback */
static
SCIP_DECL_CONSEXPR_EXPRHASH(hashEntropy)
{  /*lint --e{715}*/
   unsigned int childhash;

   assert(expr != NULL);
   assert(SCIPgetConsExprExprNChildren(expr) == 1);
   assert(expr2key != NULL);
   assert(hashkey != NULL);

   *hashkey = EXPRHDLR_HASHKEY;

   assert(SCIPhashmapExists(expr2key, (void*)SCIPgetConsExprExprChildren(expr)[0]));
   childhash = (unsigned int)(size_t)SCIPhashmapGetImage(expr2key, SCIPgetConsExprExprChildren(expr)[0]);

   *hashkey ^= childhash;

   return SCIP_OKAY;
}

/** expression curvature detection callback */
static
SCIP_DECL_CONSEXPR_EXPRCURVATURE(curvatureEntropy)
{  /*lint --e{715}*/
   SCIP_CONSEXPR_EXPR* child;

   assert(scip != NULL);
   assert(expr != NULL);
   assert(curvature != NULL);
   assert(SCIPgetConsExprExprNChildren(expr) == 1);

   child = SCIPgetConsExprExprChildren(expr)[0];
   assert(child != NULL);

   /* expression is concave if child is concave */
   if( (int)(SCIPgetConsExprExprCurvature(child) & SCIP_EXPRCURV_CONCAVE) != 0 )
      *curvature = SCIP_EXPRCURV_CONCAVE;
   else
      *curvature = SCIP_EXPRCURV_UNKNOWN;

   return SCIP_OKAY;
}

/** expression monotonicity detection callback */
static
SCIP_DECL_CONSEXPR_EXPRMONOTONICITY(monotonicityEntropy)
{  /*lint --e{715}*/
   SCIP_CONSEXPR_EXPR* child;
   SCIP_Real childinf;
   SCIP_Real childsup;
   SCIP_Real brpoint = exp(-1.0);

   assert(scip != NULL);
   assert(expr != NULL);
   assert(result != NULL);
   assert(childidx == 0);

   child = SCIPgetConsExprExprChildren(expr)[0];
   assert(child != NULL);

   childinf = SCIPintervalGetInf(SCIPgetConsExprExprInterval(child));
   childsup = SCIPintervalGetSup(SCIPgetConsExprExprInterval(child));

   if( childsup <= brpoint )
      *result = SCIP_MONOTONE_INC;
   else if( childinf >= brpoint )
      *result = SCIP_MONOTONE_DEC;
   else
      *result = SCIP_MONOTONE_UNKNOWN;

   return SCIP_OKAY;
}

/** expression integrality detection callback */
static
SCIP_DECL_CONSEXPR_EXPRINTEGRALITY(integralityEntropy)
{  /*lint --e{715}*/

   assert(scip != NULL);
   assert(expr != NULL);
   assert(isintegral != NULL);

   /* TODO it is possible to check for the special case that the child is integral and its bounds are [0,1]; in
    * this case the entropy expression can only achieve 0 and is thus integral
    */
   *isintegral = FALSE;

   return SCIP_OKAY;
}

/** creates the handler for x*log(x) expressions and includes it into the expression constraint handler */
SCIP_RETCODE SCIPincludeConsExprExprHdlrEntropy(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_CONSHDLR*        consexprhdlr        /**< expression constraint handler */
   )
{
   SCIP_CONSEXPR_EXPRHDLRDATA* exprhdlrdata;
   SCIP_CONSEXPR_EXPRHDLR* exprhdlr;

   /* create expression handler data */
   exprhdlrdata = NULL;

   /* include expression handler */
   SCIP_CALL( SCIPincludeConsExprExprHdlrBasic(scip, consexprhdlr, &exprhdlr, EXPRHDLR_NAME, EXPRHDLR_DESC,
         EXPRHDLR_PRECEDENCE, evalEntropy, exprhdlrdata) );
   assert(exprhdlr != NULL);

   SCIP_CALL( SCIPsetConsExprExprHdlrCopyFreeHdlr(scip, consexprhdlr, exprhdlr, copyhdlrEntropy, NULL) );
   SCIP_CALL( SCIPsetConsExprExprHdlrCopyFreeData(scip, consexprhdlr, exprhdlr, copydataEntropy, freedataEntropy) );
   SCIP_CALL( SCIPsetConsExprExprHdlrSimplify(scip, consexprhdlr, exprhdlr, simplifyEntropy) );
   SCIP_CALL( SCIPsetConsExprExprHdlrPrint(scip, consexprhdlr, exprhdlr, printEntropy) );
   SCIP_CALL( SCIPsetConsExprExprHdlrParse(scip, consexprhdlr, exprhdlr, parseEntropy) );
   SCIP_CALL( SCIPsetConsExprExprHdlrIntEval(scip, consexprhdlr, exprhdlr, intevalEntropy) );
   SCIP_CALL( SCIPsetConsExprExprHdlrSepa(scip, consexprhdlr, exprhdlr, sepaEntropy) );
   SCIP_CALL( SCIPsetConsExprExprHdlrReverseProp(scip, consexprhdlr, exprhdlr, reversepropEntropy) );
   SCIP_CALL( SCIPsetConsExprExprHdlrHash(scip, consexprhdlr, exprhdlr, hashEntropy) );
   SCIP_CALL( SCIPsetConsExprExprHdlrBwdiff(scip, consexprhdlr, exprhdlr, bwdiffEntropy) );
   SCIP_CALL( SCIPsetConsExprExprHdlrCurvature(scip, consexprhdlr, exprhdlr, curvatureEntropy) );
   SCIP_CALL( SCIPsetConsExprExprHdlrMonotonicity(scip, consexprhdlr, exprhdlr, monotonicityEntropy) );
   SCIP_CALL( SCIPsetConsExprExprHdlrIntegrality(scip, consexprhdlr, exprhdlr, integralityEntropy) );

   return SCIP_OKAY;
}

/** creates an x*log(x) expression */
SCIP_RETCODE SCIPcreateConsExprExprEntropy(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_CONSHDLR*        consexprhdlr,       /**< expression constraint handler */
   SCIP_CONSEXPR_EXPR**  expr,               /**< pointer where to store expression */
   SCIP_CONSEXPR_EXPR*   child               /**< child expression */
   )
{
   SCIP_CONSEXPR_EXPRHDLR* exprhdlr;
   SCIP_CONSEXPR_EXPRDATA* exprdata;

   assert(consexprhdlr != NULL);
   assert(expr != NULL);
   assert(child != NULL);

   exprhdlr = SCIPfindConsExprExprHdlr(consexprhdlr, EXPRHDLR_NAME);
   assert(exprhdlr != NULL);

   /* create expression data */
   exprdata = NULL;

   /* create expression */
   SCIP_CALL( SCIPcreateConsExprExpr(scip, expr, exprhdlr, exprdata, 1, &child) );

   return SCIP_OKAY;
}
