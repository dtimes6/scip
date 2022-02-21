/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2022 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the ZIB Academic License.         */
/*                                                                           */
/*  You should have received a copy of the ZIB Academic License              */
/*  along with SCIP; see the file COPYING. If not visit scipopt.org.         */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#define SCIP_DEBUG
/**@file   compute_symmetry_nauty.c
 * @brief  interface for symmetry computations to nauty/traces
 * @author Marc Pfetsch
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#include "compute_symmetry.h"

/* the following determines whether nauty or traces is used: */
#define NAUTY

/* include nauty/traces */
/* turn off warning (just for including nauty/traces) */
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Wpedantic"

#ifdef NAUTY
#include "nauty/nauty.h"
#include "nauty/nausparse.h"
#else
#include "nauty/traces.h"
#endif

#pragma GCC diagnostic warning "-Wunused-variable"
#pragma GCC diagnostic warning "-Wredundant-decls"
#pragma GCC diagnostic warning "-Wpedantic"

#include "scip/expr_var.h"
#include "scip/expr_sum.h"
#include "scip/expr_pow.h"
#include "scip/expr.h"
#include "scip/cons_nonlinear.h"
#include "scip/cons_linear.h"
#include "scip/scip_mem.h"


/** struct for nauty callback */
struct NAUTY_Data
{
   SCIP*                 scip;               /**< SCIP pointer */
   int                   npermvars;          /**< number of variables for permutations */
   int                   nperms;             /**< number of permutations */
   int**                 perms;              /**< permutation generators as (nperms x npermvars) matrix */
   int                   nmaxperms;          /**< maximal number of permutations */
   int                   maxgenerators;      /**< maximal number of generators constructed (= 0 if unlimited) */
};

/* static data for nauty callback */
static struct NAUTY_Data data_;


/* ------------------- map for operator types ------------------- */

/** gets the key of the given element */
static
SCIP_DECL_HASHGETKEY(SYMhashGetKeyOptype)
{  /*lint --e{715}*/
   return elem;
}

/** returns TRUE iff both keys are equal
 *
 *  Compare the types of two operators according to their name, level and, in case of power, exponent.
 */
static
SCIP_DECL_HASHKEYEQ(SYMhashKeyEQOptype)
{
   SYM_OPTYPE* k1;
   SYM_OPTYPE* k2;

   k1 = (SYM_OPTYPE*) key1;
   k2 = (SYM_OPTYPE*) key2;

   /* first check operator name */
   if ( SCIPexprGetHdlr(k1->expr) != SCIPexprGetHdlr(k2->expr) )
      return FALSE;

   /* for pow expressions, also check exponent (TODO should that happen for signpow as well?) */
   if ( SCIPisExprPower((SCIP*)userptr, k1->expr )
      && SCIPgetExponentExprPow(k1->expr) != SCIPgetExponentExprPow(k2->expr) )  /*lint !e777*/
      return FALSE;

   /* if still undecided, take level */
   if ( k1->level != k2->level )
      return FALSE;

   return TRUE;
}

/** returns the hash value of the key */
static
SCIP_DECL_HASHKEYVAL(SYMhashKeyValOptype)
{  /*lint --e{715}*/
   SYM_OPTYPE* k;
   SCIP_Real exponent;

   k = (SYM_OPTYPE*) key;

   if ( SCIPisExprPower((SCIP*)userptr, k->expr) )
      exponent = SCIPgetExponentExprPow(k->expr);
   else
      exponent = 1.0;

   return SCIPhashTwo(SCIPrealHashCode(exponent), k->level),
      (uint64_t) SCIPexprhdlrGetName(SCIPexprGetHdlr(k->expr));
}

/* ------------------- map for constant types ------------------- */

/** gets the key of the given element */
static
SCIP_DECL_HASHGETKEY(SYMhashGetKeyConsttype)
{  /*lint --e{715}*/
   return elem;
}

/** returns TRUE iff both keys are equal
 *
 *  Compare two constants according to their values.
 */
static
SCIP_DECL_HASHKEYEQ(SYMhashKeyEQConsttype)
{
   SYM_CONSTTYPE* k1;
   SYM_CONSTTYPE* k2;

   k1 = (SYM_CONSTTYPE*) key1;
   k2 = (SYM_CONSTTYPE*) key2;

   return (SCIP_Bool)(k1->value == k2->value);  /*lint !e777*/
}

/** returns the hash value of the key */
static
SCIP_DECL_HASHKEYVAL(SYMhashKeyValConsttype)
{  /*lint --e{715}*/
   SYM_CONSTTYPE* k;

   k = (SYM_CONSTTYPE*) key;

   return SCIPrealHashCode(k->value);
}

/* ------------------- map for constraint side types ------------------- */

/** gets the key of the given element */
static
SCIP_DECL_HASHGETKEY(SYMhashGetKeyRhstype)
{  /*lint --e{715}*/
   return elem;
}

/** returns TRUE iff both keys are equal
 *
 *  Compare two constraint sides according to lhs and rhs.
 */
static
SCIP_DECL_HASHKEYEQ(SYMhashKeyEQRhstype)
{
   SYM_RHSTYPE* k1;
   SYM_RHSTYPE* k2;

   k1 = (SYM_RHSTYPE*) key1;
   k2 = (SYM_RHSTYPE*) key2;

   if ( k1->lhs != k2->lhs )  /*lint !e777*/
      return FALSE;

   return (SCIP_Bool)(k1->rhs == k2->rhs);  /*lint !e777*/
}

/** returns the hash value of the key */
static
SCIP_DECL_HASHKEYVAL(SYMhashKeyValRhstype)
{  /*lint --e{715}*/
   SYM_RHSTYPE* k;

   k = (SYM_RHSTYPE*) key;

   return SCIPhashTwo(SCIPrealHashCode(k->lhs), SCIPrealHashCode(k->rhs));
}


/* ------------------- hook functions ------------------- */

#ifdef NAUTY

/** callback function for nauty */  /*lint -e{715}*/
static
void nautyhook(
   int                   unknown1,
   int*                  p,                  /**< Generator that nauty found */
   int*                  unknown2,
   int                   unknown3,
   int                   unknown4,
   int                   n                   /**< Number of nodes in the graph */
   )
{  /* lint --e{715} */
   SCIP_Bool isidentity = TRUE;
   int* pp;
   int j;

   assert( p != NULL );

   /* make sure we do not generate more that maxgenerators many permutations, if the limit in nauty/traces is not available */
   if ( data_.maxgenerators != 0 && data_.nperms >= data_.maxgenerators )
      return;

   /* check for identity */
   for (j = 0; j < data_.npermvars && isidentity; ++j)
   {
      /* convert index of variable-level 0-nodes to variable indices */
      if ( p[j] != j )
         isidentity = FALSE;
   }

   /* ignore trivial generators, i.e. generators that only permute the constraints */
   if ( isidentity )
      return;

   /* check whether we should allocate space for perms */
   if ( data_.nmaxperms <= 0 )
   {
      if ( data_.maxgenerators == 0 )
         data_.nmaxperms = 100;   /* seems to cover many cases */
      else
         data_.nmaxperms = data_.maxgenerators;

      if ( SCIPallocBlockMemoryArray(data_.scip, &data_.perms, data_.nmaxperms) != SCIP_OKAY )
         return;
   }
   else if ( data_.nperms >= data_.nmaxperms )    /* check whether we need to resize */
   {
      int newsize;

      newsize = SCIPcalcMemGrowSize(data_.scip, data_.nperms + 1);
      assert( newsize >= data_.nperms );
      assert( data_.maxgenerators == 0 );

      if ( SCIPreallocBlockMemoryArray(data_.scip, &data_.perms, data_.nmaxperms, newsize) != SCIP_OKAY )
         return;

      data_.nmaxperms = newsize;
   }

   if ( SCIPduplicateBlockMemoryArray(data_.scip, &pp, p, n) != SCIP_OKAY )
      return;
   data_.perms[data_.nperms++] = pp;
}

#else

/** callback function for traces */
static
void traceshook(
   int                   count,              /**< number of generator */
   int*                  p,                  /**< generator that traces found */
   int                   n                   /**< number of nodes in the graph */
   )
{
   SCIP_Bool isidentity = TRUE;
   int* pp;
   int j;

   assert( p != NULL );

   /* make sure we do not generate more that maxgenerators many permutations, if the limit in nauty/traces is not available */
   if ( data_.maxgenerators != 0 && data_.nperms >= data_.maxgenerators )
      return;

   /* check for identity */
   for (j = 0; j < data_.npermvars && isidentity; ++j)
   {
      /* convert index of variable-level 0-nodes to variable indices */
      if ( p[j] != j )
         isidentity = FALSE;
   }

   /* ignore trivial generators, i.e. generators that only permute the constraints */
   if ( isidentity )
      return;

   /* check whether we should allocate space for perms */
   if ( data_.nmaxperms <= 0 )
   {
      if ( data_.maxgenerators == 0 )
         data_.nmaxperms = 100;   /* seems to cover many cases */
      else
         data_.nmaxperms = data_.maxgenerators;

      if ( SCIPallocBlockMemoryArray(data_.scip, &data_.perms, data_.nmaxperms) != SCIP_OKAY )
         return;
   }
   else if ( data_.nperms >= data_.nmaxperms )    /* check whether we need to resize */
   {
      int newsize;

      newsize = SCIPcalcMemGrowSize(data_.scip, data_.nperms + 1);
      assert( newsize >= data_.nperms );
      assert( data_.maxgenerators == 0 );

      if ( SCIPreallocBlockMemoryArray(data_.scip, &data_.perms, data_.nmaxperms, newsize) != SCIP_OKAY )
         return;

      data_.nmaxperms = newsize;
   }

   if ( SCIPduplicateBlockMemoryArray(data_.scip, &pp, p, n) != SCIP_OKAY )
      return;
   data_.perms[data_.nperms++] = pp;
}

#endif


/* ------------------- other functions ------------------- */

/** determine number of nodes and edges */
static
SCIP_RETCODE determineGraphSize(
   SCIP*                 scip,               /**< SCIP instance */
   SYM_MATRIXDATA*       matrixdata,         /**< data for MIP matrix */
   int*                  nnodes,             /**< pointer to store number of nodes in graph */
   int*                  nedges,             /**< pointer to store number of edges in graph */
   int*                  ninternodes,        /**< pointer to store number of internal nodes in graph */
   int**                 degrees,            /**< pointer to store the degrees of the nodes */
   int*                  maxdegrees,         /**< pointer to store the maximal size of the degree array */
   SCIP_Bool*            success             /**< pointer to store whether the construction was successful */
   )
{
   SCIP_Bool groupByConstraints;
   int* internodes = NULL;
   int nmaxinternodes;
   int oldcolor = -1;
#ifndef NDEBUG
   SCIP_Real oldcoef = SCIP_INVALID;
#endif
   int firstcolornodenumber = -1;
   int j;

   assert( scip != NULL );
   assert( matrixdata != NULL );
   assert( nnodes != NULL );
   assert( nedges != NULL );
   assert( ninternodes != NULL );
   assert( degrees != NULL );
   assert( maxdegrees != NULL );
   assert( success != NULL );

   *nedges = 0;
   *ninternodes = 0;
   *success = TRUE;

   /* count nodes for variables */
   *nnodes = matrixdata->npermvars;

   /* add nodes for rhs of constraints */
   *nnodes += matrixdata->nrhscoef;

   /* allocate memory for degrees */
   *degrees = NULL;
   *maxdegrees = 0;
   SCIP_CALL( SCIPensureBlockMemoryArray(scip, degrees, maxdegrees, *nnodes + 100) );
   for (j = 0; j < *nnodes; ++j)
      (*degrees)[j] = 0;

   /* Grouping of nodes depends on the number of nodes in the bipartite graph class.
    * If there are more variables than constraints, we group by constraints.
    * That is, given several variable nodes which are incident to one constraint node by the same color,
    * we join these variable nodes to the constraint node by only one intermediate node.
    */
   if ( matrixdata->nrhscoef < matrixdata->npermvars )
      groupByConstraints = TRUE;
   else
      groupByConstraints = FALSE;

   /* "colored" edges based on all matrix coefficients - loop through ordered matrix coefficients */
   if ( groupByConstraints )
      nmaxinternodes = matrixdata->nrhscoef;
   else
      nmaxinternodes = matrixdata->npermvars;

   SCIP_CALL( SCIPallocBufferArray(scip, &internodes, nmaxinternodes) ); /*lint !e530*/
   for (j = 0; j < nmaxinternodes; ++j)
      internodes[j] = -1;

   for (j = 0; j < matrixdata->nmatcoef; ++j)
   {
      int varrhsidx;
      int rhsnode;
      int varnode;
      int color;
      int idx;

      idx = matrixdata->matidx[j];
      assert( 0 <= idx && idx < matrixdata->nmatcoef );

      /* find color corresponding to matrix coefficient */
      color = matrixdata->matcoefcolors[idx];
      assert( 0 <= color && color < matrixdata->nuniquemat );

      assert( 0 <= matrixdata->matrhsidx[idx] && matrixdata->matrhsidx[idx] < matrixdata->nrhscoef );
      assert( 0 <= matrixdata->matvaridx[idx] && matrixdata->matvaridx[idx] < matrixdata->npermvars );

      rhsnode = matrixdata->npermvars + matrixdata->matrhsidx[idx];
      varnode = matrixdata->matvaridx[idx];
      assert( matrixdata->npermvars <= rhsnode && rhsnode < matrixdata->npermvars + matrixdata->nrhscoef );
      assert( rhsnode < *nnodes );
      assert( varnode < *nnodes );

      if ( matrixdata->nuniquemat == 1 )
      {
         ++(*degrees)[varnode];
         ++(*degrees)[rhsnode];
         ++(*nedges);
         /* we do not need intermediate nodes if we have only one coefficient class */
      }
      else
      {
         int internode;

         /* if new group of coefficients has been reached */
         if ( color != oldcolor )
         {
            assert( ! SCIPisEQ(scip, oldcoef, matrixdata->matcoef[idx]) );
            oldcolor = color;
            firstcolornodenumber = *nnodes;
#ifndef NDEBUG
            oldcoef = matrixdata->matcoef[idx];
#endif
         }
         else
            assert( SCIPisEQ(scip, oldcoef, matrixdata->matcoef[idx]) );

         if ( groupByConstraints )
            varrhsidx = matrixdata->matrhsidx[idx];
         else
            varrhsidx = matrixdata->matvaridx[idx];
         assert( 0 <= varrhsidx && varrhsidx < nmaxinternodes );

         if ( internodes[varrhsidx] < firstcolornodenumber )
         {
            internodes[varrhsidx] = (*nnodes)++;
            ++(*ninternodes);

            /* ensure memory for degrees */
            SCIP_CALL( SCIPensureBlockMemoryArray(scip, degrees, maxdegrees, *nnodes) );
            (*degrees)[internodes[varrhsidx]] = 0;
         }
         internode = internodes[varrhsidx];
         assert( internode >= matrixdata->npermvars + matrixdata->nrhscoef );
         assert( internode >= firstcolornodenumber );

         /* determine whether graph would be too large for bliss (can only handle int) */
         if ( *nnodes >= INT_MAX/2 )
         {
            *success = FALSE;
            break;
         }

         ++(*degrees)[varnode];
         ++(*degrees)[internode];
         ++(*degrees)[rhsnode];
         ++(*degrees)[internode];
         ++(*nedges);
         ++(*nedges);
      }
   }
   SCIPfreeBufferArray(scip, &internodes);

   SCIPdebugMsg(scip, "#nodes for variables: %d\n", matrixdata->npermvars);
   SCIPdebugMsg(scip, "#nodes for rhs: %d\n", matrixdata->nrhscoef);
   SCIPdebugMsg(scip, "#intermediate nodes: %d\n", *ninternodes);

   return SCIP_OKAY;
}


/** Construct linear part of colored graph for symmetry computations
 *
 *  Construct graph:
 *  - Each variable gets a different node.
 *  - Each constraint gets a different node.
 *  - Each matrix coefficient gets a different node that is connected to the two nodes
 *    corresponding to the respective constraint and variable.
 *
 *  Each different variable, rhs, matrix coefficient gets a different color that is attached to the corresponding entries.
 *
 *  @pre This method assumes that the nodes corresponding to permutation variables are already in the graph and that
 *  their node number is equal to their index.
 */
static
SCIP_RETCODE fillGraphByLinearConss(
   SCIP*                 scip,               /**< SCIP instance */
   sparsegraph*          SG,                 /**< Graph to be constructed */
   SYM_MATRIXDATA*       matrixdata,         /**< data for MIP matrix */
   int                   nnodes,             /**< number of nodes in graph */
   int                   nedges,             /**< number of edges in graph */
   int*                  degrees,            /**< array with the degrees of the nodes */
   int*                  colors              /**< array with colors of nodes on output */
   )
{
   SCIP_Bool groupByConstraints;
   int* internodes = NULL;
   int* pos = NULL;
   int nmaxinternodes;
   int oldcolor = -1;
   int cnt;
#ifndef NDEBUG
   SCIP_Real oldcoef = SCIP_INVALID;
#endif
   int firstcolornodenumber = -1;
   int nusedcolors;
   int n = 0;
   int m = 0;
   int j;
   int i;

   assert( scip != NULL );
   assert( matrixdata != NULL );
   assert( degrees != NULL );
   assert( colors != NULL );

   SCIPdebugMsg(scip, "Filling graph with colored coefficient nodes for linear part.\n");

   /* fill in array with colors for variables */
   for (j = 0; j < matrixdata->npermvars; ++j)
   {
      assert( 0 <= matrixdata->permvarcolors[j] && matrixdata->permvarcolors[j] < matrixdata->nuniquevars );
      colors[n++] = matrixdata->permvarcolors[j];
   }
   nusedcolors = matrixdata->nuniquevars;

   /* fill in array with colors for rhs */
   for (i = 0; i < matrixdata->nrhscoef; ++i)
   {
      assert( 0 <= matrixdata->rhscoefcolors[i] && matrixdata->rhscoefcolors[i] < matrixdata->nuniquerhs );
      colors[n++] = nusedcolors + matrixdata->rhscoefcolors[i];
   }
   nusedcolors += matrixdata->nuniquerhs;

   SCIP_CALL( SCIPallocBufferArray(scip, &pos, nnodes) );

   /* fill in positions in graph */
   cnt = 0;
   for (i = 0; i < nnodes; ++i)
   {
      SG->d[i] = degrees[i];   /* degree of node i */
      SG->v[i] = (size_t) cnt;          /* position of edges for node i */
      pos[i] = cnt;            /* also store position */
      cnt += degrees[i];
   }

   /* Grouping of nodes depends on the number of nodes in the bipartite graph class.  If there are more variables than
    * constraints, we group by constraints.  That is, given several variable nodes which are incident to one constraint
    * node by the same color, we join these variable nodes to the constraint node by only one intermediate node.
    */
   if ( matrixdata->nrhscoef < matrixdata->npermvars )
      groupByConstraints = TRUE;
   else
      groupByConstraints = FALSE;

   /* "colored" edges based on all matrix coefficients - loop through ordered matrix coefficients */
   if ( groupByConstraints )
      nmaxinternodes = matrixdata->nrhscoef;
   else
      nmaxinternodes = matrixdata->npermvars;

   SCIP_CALL( SCIPallocBufferArray(scip, &internodes, nmaxinternodes) ); /*lint !e530*/
   for (j = 0; j < nmaxinternodes; ++j)
      internodes[j] = -1;

   /* We pass through the matrix coeficients, grouped by color, i.e., different coefficients. If the coeffients appear
    * in the same row or column, it suffices to only generate a single node (depending on groupByConstraints). We store
    * this node in the array internodes. In order to avoid reinitialization, we store the node number with increasing
    * numbers for each color. The smallest number for the current color is stored in firstcolornodenumber. */
   for (j = 0; j < matrixdata->nmatcoef; ++j)
   {
      int idx;
      int color;
      int rhsnode;
      int varnode;
      int varrhsidx;

      idx = matrixdata->matidx[j];
      assert( 0 <= idx && idx < matrixdata->nmatcoef );

      /* find color corresponding to matrix coefficient */
      color = matrixdata->matcoefcolors[idx];
      assert( 0 <= color && color < matrixdata->nuniquemat );

      assert( 0 <= matrixdata->matrhsidx[idx] && matrixdata->matrhsidx[idx] < matrixdata->nrhscoef );
      assert( 0 <= matrixdata->matvaridx[idx] && matrixdata->matvaridx[idx] < matrixdata->npermvars );

      rhsnode = matrixdata->npermvars + matrixdata->matrhsidx[idx];
      varnode = matrixdata->matvaridx[idx];
      assert( matrixdata->npermvars <= rhsnode && rhsnode < matrixdata->npermvars + matrixdata->nrhscoef );
      assert( rhsnode < nnodes );
      assert( varnode < nnodes );

      /* if we have only one color, we do not need intermediate nodes */
      if ( matrixdata->nuniquemat == 1 )
      {
         SG->e[pos[varnode]++] = rhsnode;
         SG->e[pos[rhsnode]++] = varnode;
         assert( varnode == nnodes - 1 || pos[varnode] <= (int) SG->v[varnode+1] );
         assert( rhsnode == nnodes - 1 || pos[rhsnode] <= (int) SG->v[rhsnode+1] );
         ++m;
      }
      else
      {
         int internode;

         /* if new group of coefficients has been reached */
         if ( color != oldcolor )
         {
            assert( ! SCIPisEQ(scip, oldcoef, matrixdata->matcoef[idx]) );
            oldcolor = color;
            firstcolornodenumber = n;
#ifndef NDEBUG
            oldcoef = matrixdata->matcoef[idx];
#endif
         }
         else
            assert( SCIPisEQ(scip, oldcoef, matrixdata->matcoef[idx]) );

         if ( groupByConstraints )
            varrhsidx = matrixdata->matrhsidx[idx];
         else
            varrhsidx = matrixdata->matvaridx[idx];
         assert( 0 <= varrhsidx && varrhsidx < nmaxinternodes );

         if ( internodes[varrhsidx] < firstcolornodenumber )
         {
            colors[n] = nusedcolors + color;
            internodes[varrhsidx] = n++;
         }
         internode = internodes[varrhsidx];
         assert( internode >= matrixdata->npermvars + matrixdata->nrhscoef );
         assert( internode >= firstcolornodenumber );
         assert( internode < nnodes );

         SG->e[pos[varnode]++] = internode;
         SG->e[pos[internode]++] = varnode;

         SG->e[pos[rhsnode]++] = internode;
         SG->e[pos[internode]++] = rhsnode;
         assert( varnode == nnodes - 1 || pos[varnode] <= (int) SG->v[varnode+1] );
         assert( internode == nnodes - 1 || pos[internode] <= (int) SG->v[internode+1] );
         ++m;
         ++m;
      }
   }
   assert( n == nnodes );
   assert( m == nedges );

#ifndef NDEBUG
   for (i = 0; i < nnodes - 1; ++i)
      assert( pos[i] == (int) SG->v[i+1] );
#endif

   SCIPfreeBufferArray(scip, &internodes);
   SCIPfreeBufferArray(scip, &pos);

   return SCIP_OKAY;
}

/** return whether symmetry can be computed */
SCIP_Bool SYMcanComputeSymmetry(void)
{
   return TRUE;
}

/** static variable for holding the name of name */
static char nautyname[100];

/** return name of external program used to compute generators */
const char* SYMsymmetryGetName(void)
{
#ifdef NAUTY
   (void) snprintf(nautyname, 100, "nauty %s", NAUTYVERSION);
   return nautyname;
#else
   (void) snprintf(nautyname, 100, "traces %s", NAUTYVERSION);
   return nautyname;
#endif
}

/** return description of external program used to compute generators */
const char* SYMsymmetryGetDesc(void)
{
#ifdef NAUTY
   return "Nauty by Brendan D. McKay (https://users.cecs.anu.edu.au/~bdm/nauty/)";
#else
   return "Traces by Adolfo Piperno (https://pallini.di.uniroma1.it/)";
#endif
}

/** compute generators of symmetry group */
SCIP_RETCODE SYMcomputeSymmetryGenerators(
   SCIP*                 scip,               /**< SCIP pointer */
   int                   maxgenerators,      /**< maximal number of generators constructed (= 0 if unlimited) */
   SYM_MATRIXDATA*       matrixdata,         /**< data for MIP matrix */
   SYM_EXPRDATA*         exprdata,           /**< data for nonlinear constraints */
   int*                  nperms,             /**< pointer to store number of permutations */
   int*                  nmaxperms,          /**< pointer to store maximal number of permutations (needed for freeing storage) */
   int***                perms,              /**< pointer to store permutation generators as (nperms x npermvars) matrix */
   SCIP_Real*            log10groupsize      /**< pointer to store size of group */
   )
{
   SCIP_Bool success = FALSE;
   int* degrees;
   int* colors;
   int maxdegrees;
   int nnodes;
   int nedges;
   int ninternodes;
   int v;

   /* nauty data structures */
   sparsegraph SG;
   int* lab;
   int* ptn;
   int* orbits;

#ifdef NAUTY
   DEFAULTOPTIONS_SPARSEGRAPH(options);
   statsblk stats;
#else
   static DEFAULTOPTIONS_TRACES(options);
   TracesStats stats;
#endif

   assert( scip != NULL );
   assert( matrixdata != NULL );
   assert( exprdata != NULL );
   assert( nperms != NULL );
   assert( nmaxperms != NULL );
   assert( perms != NULL );
   assert( log10groupsize != NULL );
   assert( maxgenerators >= 0 );

   /* init */
   *nperms = 0;
   *nmaxperms = 0;
   *perms = NULL;
   *log10groupsize = 0;

   /* init options */
#ifdef NAUTY
   /* init callback functions for nauty (accumulate the group generators found by nauty) */
   options.writeautoms = FALSE;
   options.userautomproc = nautyhook;
   options.defaultptn = FALSE; /* use color classes */
#else
   /* init callback functions for traces (accumulate the group generators found by traces) */
   options.writeautoms = FALSE;
   options.userautomproc = traceshook;
   options.defaultptn = FALSE; /* use color classes */
#endif

   /** Determine number of nodes and edges */
   SCIP_CALL( determineGraphSize(scip, matrixdata, &nnodes, &nedges, &ninternodes, &degrees, &maxdegrees, &success) );

   if ( ! success )
   {
      SCIPverbMessage(scip, SCIP_VERBLEVEL_MINIMAL, 0, "Stopped symmetry computation: Symmetry graph would become too large.\n");
      return SCIP_OKAY;
   }

   /* allocate temporary array for colors */
   SCIP_CALL( SCIPallocBufferArray(scip, &colors, nnodes) );

   /* init graph */
   SG_INIT(SG);

   SG_ALLOC(SG, nnodes, 2 * nedges, "malloc");

   SG.nv = nnodes;        /* number of nodes */
   SG.nde = (size_t) (2 * nedges);   /* number of directed edges */

   /* fill graph with nodes for variables and linear constraints */
   SCIP_CALL( fillGraphByLinearConss(scip, &SG, matrixdata, nnodes, nedges, degrees, colors) );

   /* add the nodes for nonlinear constraints to the graph */
   /* SCIP_CALL( fillGraphByNonlinearConss(scip, &SG, exprdata, nnodes, nedges, degrees, colors, nusedcolors, &success) ); */

   SCIPfreeBlockMemoryArray(scip, &degrees, maxdegrees);

   /* memory allocation for nauty/traces */
   SCIP_CALL( SCIPallocBufferArray(scip, &lab, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &ptn, nnodes) );
   SCIP_CALL( SCIPallocBufferArray(scip, &orbits, nnodes) );

   /* fill in array with colors for variables */
   for (v = 0; v < nnodes; ++v)
      lab[v] = v;

   /* sort nodes according to colors */
   SCIPsortIntInt(colors, lab, nnodes);

   /* set up ptn marking new colors */
   for (v = 0; v < nnodes; ++v)
   {
      if ( v < nnodes-1 && colors[v] == colors[v+1] )
         ptn[v] = 1;  /* color class does not end */
      else
         ptn[v] = 0;  /* color class ends */
   }

   SCIPdebugMsg(scip, "Symmetry detection graph has %d nodes.\n", nnodes);

   data_.scip = scip;
   data_.npermvars = matrixdata->npermvars;
   data_.nperms = 0;
   data_.nmaxperms = 0;
   data_.maxgenerators = maxgenerators;
   data_.perms = NULL;

   /* call nauty/traces */
#ifdef NAUTY
   sparsenauty(&SG, lab, ptn, orbits, &options, &stats, NULL);
#else
   Traces(&SG, lab, ptn, orbits, &options, &stats, NULL);
#endif

   SCIPfreeBufferArray(scip, &orbits);
   SCIPfreeBufferArray(scip, &ptn);
   SCIPfreeBufferArray(scip, &lab);

   SCIPfreeBufferArray(scip, &colors);

   SG_FREE(SG);

   /* prepare return values */
   if ( data_.nperms > 0 )
   {
      *perms = data_.perms;
      *nperms = data_.nperms;
      *nmaxperms = data_.nmaxperms;
   }
   else
   {
      assert( data_.perms == NULL );
      assert( data_.nmaxperms == 0 );
   }

   /* determine log10 of symmetry group size */
   *log10groupsize = (SCIP_Real) stats.grpsize2;

   return SCIP_OKAY;
}
