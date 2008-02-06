/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2007 Tobias Achterberg                              */
/*                                                                           */
/*                  2002-2007 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the ZIB Academic License.         */
/*                                                                           */
/*  You should have received a copy of the ZIB Academic License              */
/*  along with SCIP; see the file COPYING. If not email to scip@zib.de.      */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#pragma ident "@(#) $Id: sepa_mcf.c,v 1.15 2008/02/06 09:37:02 bzfpfend Exp $"

#define SCIP_DEBUG
/**@file   sepa_mcf.c
 * @brief  multi-commodity-flow network cut separator
 * @author Tobias Achterberg
 *
 * We try to identify a multi-commodity flow structure in the LP relaxation of the
 * following type:
 *
 *  (1)  \sum_{a \in \delta^+(v)} f_a^k  - \sum_{a \in \delta^-(v)} f_a^k  <=  -d_v^k   for all v \in V and k \in K
 *  (2)  \sum_{k \in K} f_a^k - c_a x_a                                    <=  0        for all a \in A
 *
 * Constraints (1) are flow conservation constraints, which say that for each commodity k and node v the
 * outflow (\delta^+(v)) minus the inflow (\delta^-(v)) of a node v must not exceed the negative of the demand of
 * node v in commodity k. To say it the other way around, inflow minus outflow must be at least equal to the demand.
 * Constraints (2) are the arc capacity constraints, which say that the sum of all flow over an arc a must not
 * exceed its capacity c_a x_a, with x being a binary or integer variable.
 * c_a x_a does not need to be a single product of a capacity and an integer variable; we also accept general scalar
 * products.
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#include <assert.h>

#include "scip/sepa_mcf.h"


#define SEPA_NAME              "mcf"
#define SEPA_DESC              "multi-commodity-flow network cut separator"
#define SEPA_PRIORITY            -10000
#define SEPA_FREQ                     0
#define SEPA_MAXBOUNDDIST           0.0
#define SEPA_DELAY                FALSE /**< should separation method be delayed, if other separators found cuts? */

#define DEFAULT_MAXSIGNDISTANCE       1 /**< maximum Hamming distance of flow conservation constraint sign patterns of the same node */
#define DEFAULT_NCLUSTERS             5 /**< number of clusters to generate in the shrunken network */




/*
 * Data structures
 */

struct SCIP_McfNetwork
{
   SCIP_ROW***           nodeflowrows;       /**< nodeflowrows[v][k]: flow conservation constraint for node v and
                                              *   commodity k; NULL if this node does not exist in the commodity */
   SCIP_Real**           nodeflowscales;     /**< scaling factors to convert nodeflowrows[v][k] into a +/-1 >= row */
   SCIP_ROW**            arccapacityrows;    /**< arccapacity[a]: capacity constraint on arc a;
                                              *   NULL if uncapacitated */
   SCIP_Real*            arccapacityscales;  /**< scaling factors to convert arccapacity[a] into a <= row with
                                              *   positive entries for the flow variables */
   int*                  arcsources;         /**< source node ids of arcs */
   int*                  arctargets;         /**< target node ids of arcs */
   int                   nnodes;             /**< number of nodes in the graph */
   int                   narcs;              /**< number of arcs in the graph */
   int                   ncommodities;       /**< number of commodities */
   int                   ninconsistencies;   /**< number of inconsistencies between the commodity graphs */
};
typedef struct SCIP_McfNetwork SCIP_MCFNETWORK;


/** separator data */
struct SCIP_SepaData
{
   SCIP_MCFNETWORK*      mcfnetwork;         /**< multi-commodity-flow network structure */
   int                   maxsigndistance;    /**< maximum Hamming distance of flow conservation constraint sign patterns of the same node */
   int                   nclusters;          /**< number of clusters to generate in the shrunken network */
};

/** internal MCF extraction data to pass to subroutines */
struct mcfdata
{
   unsigned char*        flowrowsigns;       /**< potential or actual sides of rows to be used as flow conservation constraint */
   SCIP_Real*            flowrowscalars;     /**< scalar of rows to transform into +/-1 coefficients */
   SCIP_Real*            flowrowscores;      /**< score value indicating how sure we are that this is indeed a flow conservation constraint */
   unsigned char*        capacityrowsigns;   /**< potential or actual sides of rows to be used as capacity constraint */
   SCIP_Real*            capacityrowscores;  /**< score value indicating how sure we are that this is indeed a capacity constraint */
   int*                  flowcands;          /**< list of row indices that are candidates for flow conservation constraints */
   int                   nflowcands;         /**< number of elements in flow candidate list */
   int*                  capacitycands;      /**< list of row indices that are candidates for capacity constraints */
   int                   ncapacitycands;     /**< number of elements in capacity candidate list */
   SCIP_Bool*            plusflow;           /**< is column c member of a flow row with coefficient +1? */
   SCIP_Bool*            minusflow;          /**< is column c member of a flow row with coefficient -1? */
   int                   ncommodities;       /**< number of commodities */
   int*                  commoditysigns;     /**< +1: regular, -1: all arcs have opposite direction; 0: undecided */
   int                   commoditysignssize; /**< size of commoditysigns array */
   int*                  colcommodity;       /**< commodity number of each column, or -1 */
   int*                  rowcommodity;       /**< commodity number of each row, or -1 */
   int*                  colarcid;           /**< arc id of each flow column, or -1 */
   int*                  rowarcid;           /**< arc id of each capacity row, or -1 */
   int*                  rownodeid;          /**< node id of each flow conservation row, or -1 */
   int*                  newcols;            /**< columns of current commodity that have to be inspected for incident flow conservation rows */
   int                   nnewcols;           /**< number of newcols */
   int                   narcs;              /**< number of arcs in the extracted graph */
   int                   nnodes;             /**< number of nodes in the extracted graph */
   SCIP_ROW**            capacityrows;       /**< capacity row for each arc */
   int                   capacityrowssize;   /**< size of array */
   SCIP_Bool*            colisincident;      /**< temporary memory for column collection */
};
typedef struct mcfdata MCFDATA;              /**< internal MCF extraction data to pass to subroutines */

/** data structure to put on the arc heap */
struct arcentry
{
   int                   arcid;              /**< index of the arc */
   SCIP_Real             weight;             /**< weight of the arc in the separation problem */
};
typedef struct arcentry ARCENTRY;

/** arc priority queue */
struct arcqueue
{
   SCIP_PQUEUE*          pqueue;             /**< priority queue of elements */
   ARCENTRY*             arcentries;         /**< elements on the heap */
};
typedef struct arcqueue ARCQUEUE;

/** partitioning of the nodes into clusters */
struct nodepartition
{
   int*                  representatives;    /**< mapping of node ids to their representatives within their cluster */
   int*                  nodeclusters;       /**< cluster for each node id */
   int*                  clusternodes;       /**< node ids sorted by cluster */
   int*                  clusterbegin;       /**< first entry in clusternodes for each cluster (size: nclusters+1) */
   int                   nclusters;          /**< number of clusters */
};
typedef struct nodepartition NODEPARTITION;



/*
 * Local methods
 */

#define LHSPOSSIBLE  1
#define RHSPOSSIBLE  2
#define LHSASSIGNED  4
#define RHSASSIGNED  8
#define DISCARDED   16

/** creates an empty MCF network data structure */
static
SCIP_RETCODE mcfnetworkCreate(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_MCFNETWORK**     mcfnetwork          /**< MCF network structure */
   )
{
   assert(mcfnetwork != NULL);
   assert(*mcfnetwork == NULL);

   SCIP_CALL( SCIPallocMemory(scip, mcfnetwork) );
   (*mcfnetwork)->nodeflowrows = NULL;
   (*mcfnetwork)->nodeflowscales = NULL;
   (*mcfnetwork)->arccapacityrows = NULL;
   (*mcfnetwork)->arccapacityscales = NULL;
   (*mcfnetwork)->arcsources = NULL;
   (*mcfnetwork)->arctargets = NULL;
   (*mcfnetwork)->nnodes = 0;
   (*mcfnetwork)->narcs = 0;
   (*mcfnetwork)->ncommodities = 0;
   (*mcfnetwork)->ninconsistencies = 0;

   return SCIP_OKAY;
}

/** frees MCF network data structure */
static
void mcfnetworkFree(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_MCFNETWORK**     mcfnetwork          /**< MCF network structure */
   )
{
   assert(mcfnetwork != NULL);

   if( *mcfnetwork != NULL )
   {
      int v;

      for( v = 0; v < (*mcfnetwork)->nnodes; v++ )
      {
         SCIPfreeMemoryArrayNull(scip, &(*mcfnetwork)->nodeflowrows[v]);
         SCIPfreeMemoryArrayNull(scip, &(*mcfnetwork)->nodeflowscales[v]);
      }
      SCIPfreeMemoryArrayNull(scip, &(*mcfnetwork)->nodeflowrows);
      SCIPfreeMemoryArrayNull(scip, &(*mcfnetwork)->nodeflowscales);
      SCIPfreeMemoryArrayNull(scip, &(*mcfnetwork)->arccapacityrows);
      SCIPfreeMemoryArrayNull(scip, &(*mcfnetwork)->arccapacityscales);
      SCIPfreeMemoryArrayNull(scip, &(*mcfnetwork)->arcsources);
      SCIPfreeMemoryArrayNull(scip, &(*mcfnetwork)->arctargets);

      SCIPfreeMemory(scip, mcfnetwork);
   }
}

/** fills the MCF network structure with the MCF data */
static
SCIP_RETCODE mcfnetworkFill(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_MCFNETWORK*      mcfnetwork,         /**< MCF network structure */
   MCFDATA*              mcfdata             /**< internal MCF extraction data to pass to subroutines */
   )
{
   unsigned char* flowrowsigns     = mcfdata->flowrowsigns;
   SCIP_Real*     flowrowscalars   = mcfdata->flowrowscalars;
   unsigned char* capacityrowsigns = mcfdata->capacityrowsigns;
   int*           flowcands        = mcfdata->flowcands;
   int            nflowcands       = mcfdata->nflowcands;
   int            ncommodities     = mcfdata->ncommodities;
   int*           commoditysigns   = mcfdata->commoditysigns;
   int*           rowcommodity     = mcfdata->rowcommodity;
   int*           colarcid         = mcfdata->colarcid;
   int*           rownodeid        = mcfdata->rownodeid;
   int            narcs            = mcfdata->narcs;
   int            nnodes           = mcfdata->nnodes;
   SCIP_ROW**     capacityrows     = mcfdata->capacityrows;

   assert(mcfnetwork != NULL);

   if( nnodes > 0 && narcs > 0 && ncommodities > 0 )
   {
      SCIP_ROW** rows;
      int nrows;
      int v;
      int a;
      int i;

      SCIP_CALL( SCIPgetLPRowsData(scip, &rows, &nrows) );

      mcfnetwork->nnodes = nnodes;
      mcfnetwork->narcs = narcs;
      mcfnetwork->ncommodities = ncommodities;
      mcfnetwork->ninconsistencies = 0;

      /* allocate memory for arrays and initialize with default values */
      SCIP_CALL( SCIPallocMemoryArray(scip, &mcfnetwork->nodeflowrows, mcfnetwork->nnodes) );
      SCIP_CALL( SCIPallocMemoryArray(scip, &mcfnetwork->nodeflowscales, mcfnetwork->nnodes) );
      for( v = 0; v < mcfnetwork->nnodes; v++ )
      {
         int k;

         SCIP_CALL( SCIPallocMemoryArray(scip, &mcfnetwork->nodeflowrows[v], mcfnetwork->ncommodities) );
         SCIP_CALL( SCIPallocMemoryArray(scip, &mcfnetwork->nodeflowscales[v], mcfnetwork->ncommodities) );
         for( k = 0; k < mcfnetwork->ncommodities; k++ )
         {
            mcfnetwork->nodeflowrows[v][k] = NULL;
            mcfnetwork->nodeflowscales[v][k] = 0.0;
         }
      }

      SCIP_CALL( SCIPallocMemoryArray(scip, &mcfnetwork->arccapacityrows, mcfnetwork->narcs) );
      SCIP_CALL( SCIPallocMemoryArray(scip, &mcfnetwork->arccapacityscales, mcfnetwork->narcs) );
      SCIP_CALL( SCIPallocMemoryArray(scip, &mcfnetwork->arcsources, mcfnetwork->narcs) );
      SCIP_CALL( SCIPallocMemoryArray(scip, &mcfnetwork->arctargets, mcfnetwork->narcs) );
      for( a = 0; a < mcfnetwork->narcs; a++ )
      {
         mcfnetwork->arccapacityrows[a] = NULL;
         mcfnetwork->arccapacityscales[a] = 0.0;
         mcfnetwork->arcsources[a] = -1;
         mcfnetwork->arctargets[a] = -1;
      }

      /* fill in existing node data */
      for( i = 0; i < nflowcands; i++ )
      {
         int r;

         r = flowcands[i];
         assert(0 <= r && r < nrows);

         if( rownodeid[r] >= 0 )
         {
            SCIP_Real scale;
            SCIP_COL** rowcols;
            SCIP_Real* rowvals;
            int rowlen;
            int k;
            int j;

            v = rownodeid[r];
            k = rowcommodity[r];
            assert(v < nnodes);
            assert(0 <= k && k < ncommodities);
            assert((flowrowsigns[r] & (LHSASSIGNED | RHSASSIGNED)) != 0);

            /* fill in node -> row assignment */
            mcfnetwork->nodeflowrows[v][k] = rows[r];
            scale = flowrowscalars[r];
            if( (flowrowsigns[r] & LHSASSIGNED) != 0 )
               scale *= -1.0;
            if( commoditysigns[k] == -1 )
               scale *= -1.0;
            mcfnetwork->nodeflowscales[v][k] = scale;

            /* mark node to be source or target node of the incident arcs */
            rowcols = SCIProwGetCols(rows[r]);
            rowvals = SCIProwGetVals(rows[r]);
            rowlen = SCIProwGetNLPNonz(rows[r]);
            for( j = 0; j < rowlen; j++ )
            {
               int c = SCIPcolGetLPPos(rowcols[j]);
               assert(0 <= c && c < SCIPgetNLPCols(scip));
               a = colarcid[c];
               if( a >= 0 )
               {
                  assert(a < narcs);
                  /* inflows have -1 coefficients, outflows +1 */
                  if( scale * rowvals[j] < 0.0 )
                  {
                     if( mcfnetwork->arctargets[a] == -1 )
                     {
                        SCIPdebugMessage("target[arc %d] = node %d\n", a, v);
                        mcfnetwork->arctargets[a] = v;
                     }
                     else if( mcfnetwork->arctargets[a] != v )
                        mcfnetwork->ninconsistencies++;
                  }
                  else
                  {
                     if( mcfnetwork->arcsources[a] == -1 )
                     {
                        SCIPdebugMessage("source[arc %d] = node %d\n", a, v);
                        mcfnetwork->arcsources[a] = v;
                     }
                     else if( mcfnetwork->arcsources[a] != v )
                        mcfnetwork->ninconsistencies++;
                  }
               }
            }
         }
      }

      /* fill in existing arc data */
      for( a = 0; a < narcs; a++ )
      {
         int r;

         r = SCIProwGetLPPos(capacityrows[a]);
         assert(0 <= r && r < nrows);
         assert((capacityrowsigns[r] & (LHSASSIGNED | RHSASSIGNED)) != 0);
         assert(mcfdata->rowarcid[r] == a);

         mcfnetwork->arccapacityrows[a] = capacityrows[a];
         mcfnetwork->arccapacityscales[a] = 1.0;
         if( (capacityrowsigns[r] & LHSASSIGNED) != 0 )
            mcfnetwork->arccapacityscales[a] *= -1.0;
      }
   }

   return SCIP_OKAY;
}

#ifdef SCIP_DEBUG
/** displays the MCF network */
static
void mcfnetworkPrint(
   SCIP_MCFNETWORK*      mcfnetwork          /**< MCF network structure */
   )
{
   if( mcfnetwork == NULL )
      printf("MCF network is empty\n");
   else
   {
      int v;
      int a;

      for( v = 0; v < mcfnetwork->nnodes; v++ )
      {
         int k;

         printf("node %2d:\n", v);
         for( k = 0; k < mcfnetwork->ncommodities; k++ )
         {
            printf("  commodity %2d: ", k);
            if( mcfnetwork->nodeflowrows[v][k] != NULL )
               printf("<%s> [%+g]\n", SCIProwGetName(mcfnetwork->nodeflowrows[v][k]), mcfnetwork->nodeflowscales[v][k]);
            else
               printf("-\n");
         }
      }

      for( a = 0; a < mcfnetwork->narcs; a++ )
      {
         printf("arc %2d [%2d -> %2d]: ", a, mcfnetwork->arcsources[a], mcfnetwork->arctargets[a]);
         if( mcfnetwork->arccapacityrows[a] != NULL )
            printf("<%s> [%+g]\n", SCIProwGetName(mcfnetwork->arccapacityrows[a]), mcfnetwork->arccapacityscales[a]);
         else
            printf("-\n");
      }

      if( mcfnetwork->ninconsistencies > 0 )
         printf("**** Warning! There are %d inconsistencies in the network! ****\n", mcfnetwork->ninconsistencies);
   }
}

/** displays commodities and its members */
static
void printCommodities(
   SCIP*                 scip,               /**< SCIP data structure */ 
   MCFDATA*              mcfdata             /**< internal MCF extraction data to pass to subroutines */
   )
{
   unsigned char* flowrowsigns     = mcfdata->flowrowsigns;
   unsigned char* capacityrowsigns = mcfdata->capacityrowsigns;
   int            ncommodities     = mcfdata->ncommodities;
   int*           commoditysigns   = mcfdata->commoditysigns;
   int*           colcommodity     = mcfdata->colcommodity;
   int*           rowcommodity     = mcfdata->rowcommodity;
   int*           colarcid         = mcfdata->colarcid;
   int*           rownodeid        = mcfdata->rownodeid;
   SCIP_ROW**     capacityrows     = mcfdata->capacityrows;

   SCIP_COL** cols;
   SCIP_ROW** rows;
   int ncols;
   int nrows;
   int k;
   int c;
   int r;
   int a;

   cols = SCIPgetLPCols(scip);
   ncols = SCIPgetNLPCols(scip);
   rows = SCIPgetLPRows(scip);
   nrows = SCIPgetNLPRows(scip);

   for( k = 0; k < ncommodities; k++ )
   {
      printf("commodity %d (sign: %+d):\n", k, commoditysigns[k]);

      for( c = 0; c < ncols; c++ )
      {
         if( colcommodity[c] == k )
            printf(" col <%s>: arc %d\n", SCIPvarGetName(SCIPcolGetVar(cols[c])), colarcid != NULL ? colarcid[c] : -1);
      }
      for( r = 0; r < nrows; r++ )
      {
         if( rowcommodity[r] == k )
            printf(" row <%s>: node %d [sign:%+d]\n", SCIProwGetName(rows[r]), rownodeid != NULL ? rownodeid[r] : -1,
                   (flowrowsigns[r] & RHSASSIGNED) != 0 ? +1 : -1);
      }
      printf("\n");
   }

   printf("capacities:\n");
   for( a = 0; a < mcfdata->narcs; a++ )
   {
      printf("  arc %d: ", a);
      if( capacityrows[a] != NULL )
      {
         r = SCIProwGetLPPos(capacityrows[a]);
         assert(0 <= r && r < nrows);
         if( (capacityrowsigns[r] & LHSASSIGNED) != 0 )
            printf(" row <%s> [sign:-1]\n", SCIProwGetName(rows[r]));
         else if( (capacityrowsigns[r] & RHSASSIGNED) != 0 )
            printf(" row <%s> [sign:+1]\n", SCIProwGetName(rows[r]));
      }
      else
         printf(" -\n");
   }
   printf("\n");

   printf("unused columns:\n");
   for( c = 0; c < ncols; c++ )
   {
      if( colcommodity[c] == -1 )
      {
         SCIP_VAR* var = SCIPcolGetVar(cols[c]);
         printf(" col <%s> [%g,%g]\n", SCIPvarGetName(var), SCIPvarGetLbGlobal(var), SCIPvarGetUbGlobal(var));
      }
   }
   printf("\n");

   printf("unused rows:\n");
   for( r = 0; r < nrows; r++ )
   {
      if( rowcommodity[r] == -1 && (capacityrowsigns[r] & (LHSASSIGNED | RHSASSIGNED)) == 0 )
         printf(" row <%s>\n", SCIProwGetName(rows[r]));
   }
   printf("\n");
}
#endif

/** comparator method for flow and capacity row candidates */
static
SCIP_DECL_SORTINDCOMP(compCands)
{
   SCIP_Real* rowscores = (SCIP_Real*)dataptr;

   if( rowscores[ind2] < rowscores[ind1] )
      return -1;
   else if( rowscores[ind2] > rowscores[ind1] )
      return +1;
   else
      return 0;
}

/** extracts flow conservation and capacity rows from the LP */
static
SCIP_RETCODE extractRows(
   SCIP*                 scip,               /**< SCIP data structure */ 
   MCFDATA*              mcfdata             /**< internal MCF extraction data to pass to subroutines */
   )
{
   unsigned char* flowrowsigns;
   SCIP_Real*     flowrowscalars;
   SCIP_Real*     flowrowscores;
   unsigned char* capacityrowsigns;
   SCIP_Real*     capacityrowscores;
   int*           flowcands;
   int*           capacitycands;

   SCIP_ROW** rows;
   int nrows;
   int r;

   SCIP_Real maxdualflow;
   SCIP_Real maxdualcapacity;

   SCIP_CALL( SCIPgetLPRowsData(scip, &rows, &nrows) );

   /* allocate temporary memory for extraction data */
   SCIP_CALL( SCIPallocMemoryArray(scip, &mcfdata->flowrowsigns, nrows) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &mcfdata->flowrowscalars, nrows) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &mcfdata->flowrowscores, nrows) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &mcfdata->capacityrowsigns, nrows) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &mcfdata->capacityrowscores, nrows) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &mcfdata->flowcands, nrows) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &mcfdata->capacitycands, nrows) );
   flowrowsigns      = mcfdata->flowrowsigns;
   flowrowscalars    = mcfdata->flowrowscalars;
   flowrowscores     = mcfdata->flowrowscores;
   capacityrowsigns  = mcfdata->capacityrowsigns;
   capacityrowscores = mcfdata->capacityrowscores;
   flowcands         = mcfdata->flowcands;
   capacitycands     = mcfdata->capacitycands;

   maxdualflow = 0.0;
   maxdualcapacity = 0.0;
   for( r = 0; r < nrows; r++ )
   {
      SCIP_ROW* row;
      SCIP_COL** rowcols;
      SCIP_Real* rowvals;
      SCIP_Real rowlhs;
      SCIP_Real rowrhs;
      int rowlen;
      int nbinvars;
      int nintvars;
      int nimplintvars;
      int ncontvars;
      SCIP_Real coef;
      SCIP_Bool hasposcoef;
      SCIP_Bool hasnegcoef;
      SCIP_Bool hasposcontcoef;
      SCIP_Bool hasnegcontcoef;
      SCIP_Bool hasposintcoef;
      SCIP_Bool hasnegintcoef;
      SCIP_Bool hasinteger;
      SCIP_Real samecontcoef;
      SCIP_Real absdualsol;
      unsigned int rowsign;
      int i;

      row = rows[r];
      assert(SCIProwGetLPPos(row) == r);

      /* get dual solution, if available */
      absdualsol = SCIProwGetDualsol(row);
      if( absdualsol == SCIP_INVALID )
         absdualsol = 0.0;
      absdualsol = ABS(absdualsol);

      flowrowsigns[r] = 0;
      flowrowscalars[r] = 0.0;
      flowrowscores[r] = 0.0;
      capacityrowsigns[r] = 0;
      capacityrowscores[r] = 0.0;

      rowlen = SCIProwGetNNonz(row);
      if( rowlen == 0 )
         continue;
      rowcols = SCIProwGetCols(row);
      rowvals = SCIProwGetVals(row);
      rowlhs = SCIProwGetLhs(row);
      rowrhs = SCIProwGetRhs(row);

      /* identify flow conservation constraints */
      coef = ABS(rowvals[0]);
      hasposcoef = FALSE;
      hasnegcoef = FALSE;
      nbinvars = 0;
      nintvars = 0;
      nimplintvars = 0;
      ncontvars = 0;
      for( i = 0; i < rowlen; i++ )
      {
         SCIP_Real absval = ABS(rowvals[i]);
         if( !SCIPisEQ(scip, absval, coef) )
            break;

#if 0
         if( SCIPvarGetType(SCIPcolGetVar(rowcols[i])) != SCIP_VARTYPE_CONTINUOUS )
            break; /*????????????????????*/
#endif
         hasposcoef = hasposcoef || (rowvals[i] > 0.0);
         hasnegcoef = hasnegcoef || (rowvals[i] < 0.0);
         switch( SCIPvarGetType(SCIPcolGetVar(rowcols[i])) )
         {
         case SCIP_VARTYPE_BINARY:
            nbinvars++;
            break;
         case SCIP_VARTYPE_INTEGER:
            nintvars++;
            break;
         case SCIP_VARTYPE_IMPLINT:
            nimplintvars++;
            break;
         case SCIP_VARTYPE_CONTINUOUS:
            ncontvars++;
            break;
         default:
            SCIPerrorMessage("unknown variable type\n");
            SCIPABORT();
         }
      }
      if( i == rowlen )
      {
         /* Flow conservation constraints should always be a*x <= -d.
          * If lhs and rhs are finite, both sides are still valid candidates.
          */
         if( !SCIPisInfinity(scip, -rowlhs) )
            flowrowsigns[r] |= LHSPOSSIBLE;
         if( !SCIPisInfinity(scip, rowrhs) )
            flowrowsigns[r] |= RHSPOSSIBLE;
         flowrowscalars[r] = 1.0/coef;
         flowcands[mcfdata->nflowcands] = r;
         mcfdata->nflowcands++;
      }

      /* identify capacity constraints */
      hasposcontcoef = FALSE;
      hasnegcontcoef = FALSE;
      hasposintcoef = FALSE;
      hasnegintcoef = FALSE;
      hasinteger = FALSE;
      samecontcoef = 0.0;
      rowsign = 0;
      if( !SCIPisInfinity(scip, -rowlhs) )
         rowsign |= LHSPOSSIBLE;
      if( !SCIPisInfinity(scip, rowrhs) )
         rowsign |= RHSPOSSIBLE;
      for( i = 0; i < rowlen && rowsign != 0; i++ )
      {
         if( SCIPvarGetType(SCIPcolGetVar(rowcols[i])) == SCIP_VARTYPE_CONTINUOUS )
         {
            if( samecontcoef == 0.0 )
               samecontcoef = rowvals[i];
            else if( !SCIPisEQ(scip, samecontcoef, rowvals[i]) )
               samecontcoef = SCIP_REAL_MAX;
            
            if( rowvals[i] > 0.0 )
            {
               rowsign &= ~LHSPOSSIBLE;
               hasposcontcoef = TRUE;
            }
            else
            {
               rowsign &= ~RHSPOSSIBLE;
               hasnegcontcoef = TRUE;
            }
         }
         else
         {
            hasinteger = TRUE;
            if( rowvals[i] > 0.0 )
               hasposintcoef = TRUE;
            else
               hasnegintcoef = TRUE;
         }
      }
      if( i == rowlen && hasinteger && rowsign != 0 )
      {
         capacityrowsigns[r] = rowsign;
         capacitycands[mcfdata->ncapacitycands] = r;
         mcfdata->ncapacitycands++;
      }

      /* calculate flow row score */
      if( (flowrowsigns[r] & (LHSPOSSIBLE | RHSPOSSIBLE)) != 0 )
      {
         flowrowscalars[r] = 1.0;

         /* row is an equation: score +1000 */
         if( (flowrowsigns[r] & (LHSPOSSIBLE | RHSPOSSIBLE)) == (LHSPOSSIBLE | RHSPOSSIBLE) )
            flowrowscores[r] += 1000.0;

         /* row is not a capacity constraint: score +1000 */
         if( (capacityrowsigns[r] & (LHSPOSSIBLE | RHSPOSSIBLE)) == 0 )
            flowrowscores[r] += 1000.0;

         /* row does not need to be scaled: score +1000 */
         if( SCIPisEQ(scip, flowrowscalars[r], 1.0) )
            flowrowscores[r] += 1000.0;

         /* row has positive and negative coefficients: score +500 */
         if( hasposcoef && hasnegcoef )
            flowrowscores[r] += 500.0;

         /* all variables are of the same type:
          *    continuous: score +1000
          *    integer:    score  +500
          *    binary:     score  +100
          */
         if( ncontvars == rowlen )
            flowrowscores[r] += 1000.0;
         else if( nintvars + nimplintvars == rowlen )
            flowrowscores[r] += 500.0;
         else if( nbinvars == rowlen )
            flowrowscores[r] += 100.0;

         /* the longer the row, the earlier we want to process it: score +10*len/(len+10) */
         flowrowscores[r] += 10.0*rowlen/(rowlen+10.0);

         assert(flowrowscores[r] > 0.0);

         /* update maximum dual solution value for additional score tie breaking */
         maxdualflow = MAX(maxdualflow, absdualsol);

         /**@todo go through list of several model types, depending on the current model type throw away invalid constraints
          *       instead of assigning a low score
          */
      }

      /* calculate capacity row score */
      if( (capacityrowsigns[r] & (LHSPOSSIBLE | RHSPOSSIBLE)) != 0 )
      {
         capacityrowscores[r] = 1.0;

         /* row is of type f - c*x <= b: score +10 */
         if( (capacityrowsigns[r] & RHSPOSSIBLE) != 0 && hasposcontcoef && !hasnegcontcoef && !hasposintcoef && hasnegintcoef )
            capacityrowscores[r] += 10.0;
         if( (capacityrowsigns[r] & LHSPOSSIBLE) != 0 && !hasposcontcoef && hasnegcontcoef && hasposintcoef && !hasnegintcoef )
            capacityrowscores[r] += 10.0;

         /* all coefficients of continuous variables are +1 or all are -1: score +5 */
         if( SCIPisEQ(scip, ABS(samecontcoef), 1.0) )
            capacityrowscores[r] += 5.0;

         /* all coefficients of continuous variables are equal: score +2 */
         if( samecontcoef != 0.0 && samecontcoef != SCIP_REAL_MAX )
            capacityrowscores[r] += 2.0;

         /* row is a <= row with non-negative right hand side: score +1 */
         if( (capacityrowsigns[r] & RHSPOSSIBLE) != 0 && !SCIPisNegative(scip, rowrhs)  )
            capacityrowscores[r] += 1.0;

         /* row is an inequality: score +1 */
         if( SCIPisInfinity(scip, -rowlhs) != SCIPisInfinity(scip, rowrhs) )
            capacityrowscores[r] += 1.0;

         assert(capacityrowscores[r] > 0.0);

         /* update maximum dual solution value for additional score tie breaking */
         maxdualcapacity = MAX(maxdualcapacity, absdualsol);
      }
   }

   /* apply additional score tie breaking using the dual solutions */
   if( SCIPisPositive(scip, maxdualflow) )
   {
      int i;

      for( i = 0; i < mcfdata->nflowcands; i++ )
      {
         SCIP_Real dualsol;

         r = flowcands[i];
         assert(0 <= r && r < nrows);
         dualsol = SCIProwGetDualsol(rows[r]);
         if( dualsol == SCIP_INVALID )
            dualsol = 0.0;
         else if( flowrowsigns[r] == (LHSPOSSIBLE | RHSPOSSIBLE) )
            dualsol = ABS(dualsol);
         else if( flowrowsigns[r] == RHSPOSSIBLE )
            dualsol = -dualsol;
         flowrowscores[r] += dualsol/maxdualflow + 1.0;
         assert(flowrowscores[r] > 0.0);
      }
   }
   if( SCIPisPositive(scip, maxdualcapacity) )
   {
      int i;

      for( i = 0; i < mcfdata->ncapacitycands; i++ )
      {
         SCIP_Real dualsol;

         r = capacitycands[i];
         assert(0 <= r && r < nrows);
         dualsol = SCIProwGetDualsol(rows[r]);
         if( dualsol == SCIP_INVALID )
            dualsol = 0.0;
         else if( capacityrowsigns[r] == (LHSPOSSIBLE | RHSPOSSIBLE) )
            dualsol = ABS(dualsol);
         else if( capacityrowsigns[r] == RHSPOSSIBLE )
            dualsol = -dualsol;
         capacityrowscores[r] += dualsol/maxdualcapacity;
         assert(capacityrowscores[r] > 0.0);
      }
   }

   /* sort candidates by score */
   SCIPbsortInd((void*)flowrowscores, mcfdata->flowcands, mcfdata->nflowcands, compCands);
   SCIPbsortInd((void*)capacityrowscores, mcfdata->capacitycands, mcfdata->ncapacitycands, compCands);

   SCIPdebugMessage("flow conservation candidates:\n");
   for( r = 0; r < mcfdata->nflowcands; r++ )
   {
      //SCIPdebug(SCIPprintRow(scip, rows[mcfdata->flowcands[r]], NULL));
      SCIPdebugMessage("%4d [score: %2g]: %s\n", r, flowrowscores[mcfdata->flowcands[r]], SCIProwGetName(rows[mcfdata->flowcands[r]]));
   }
   SCIPdebugMessage("capacity candidates:\n");
   for( r = 0; r < mcfdata->ncapacitycands; r++ )
   {
      //SCIPdebug(SCIPprintRow(scip, rows[mcfdata->capacitycands[r]], NULL));
      SCIPdebugMessage("%4d [score: %2g]: %s\n", r, capacityrowscores[mcfdata->capacitycands[r]], SCIProwGetName(rows[mcfdata->capacitycands[r]]));
   }

   return SCIP_OKAY;
}

/** creates a new commodity */
static
SCIP_RETCODE createNewCommodity(
   SCIP*                 scip,               /**< SCIP data structure */ 
   MCFDATA*              mcfdata             /**< internal MCF extraction data to pass to subroutines */
   )
{
   /* get memory for commoditysigns array */
   assert(mcfdata->ncommodities <= mcfdata->commoditysignssize);
   if( mcfdata->ncommodities == mcfdata->commoditysignssize )
   {
      mcfdata->commoditysignssize = MAX(2*mcfdata->commoditysignssize, mcfdata->ncommodities+1);
      SCIP_CALL( SCIPreallocMemoryArray(scip, &mcfdata->commoditysigns, mcfdata->commoditysignssize) );
   }
   assert(mcfdata->ncommodities < mcfdata->commoditysignssize);

   /* create commodity */
   SCIPdebugMessage("**** creating new commodity %d ****\n", mcfdata->ncommodities);
   mcfdata->commoditysigns[mcfdata->ncommodities] = 0;
   mcfdata->ncommodities++;

   return SCIP_OKAY;
}

/** adds the given flow row and all involved columns to the current commodity */
static
void addFlowrowToCommodity(
   SCIP*                 scip,               /**< SCIP data structure */ 
   MCFDATA*              mcfdata,            /**< internal MCF extraction data to pass to subroutines */
   SCIP_ROW*             row,                /**< flow row to add to current commodity */
   unsigned char         rowsign             /**< possible flow row signs to use */
   )
{
   unsigned char* flowrowsigns   = mcfdata->flowrowsigns;
   SCIP_Bool*     plusflow       = mcfdata->plusflow;
   SCIP_Bool*     minusflow      = mcfdata->minusflow;
   int            ncommodities   = mcfdata->ncommodities;
   int*           commoditysigns = mcfdata->commoditysigns;
   int*           colcommodity   = mcfdata->colcommodity;
   int*           rowcommodity   = mcfdata->rowcommodity;
   int*           newcols        = mcfdata->newcols;

   SCIP_COL** rowcols;
   SCIP_Real* rowvals;
   int rowlen;
   int rowscale;
   int r;
   int k;
   int i;

   k = ncommodities-1;
   assert(k >= 0);

   r = SCIProwGetLPPos(row);
   assert(r >= 0);
   assert(rowcommodity[r] == -1);
   assert((flowrowsigns[r] | rowsign) == flowrowsigns[r]);
   assert((rowsign & (LHSPOSSIBLE | RHSPOSSIBLE)) == rowsign);
   assert(rowsign != 0);

   /* if the row is only useable as flow row in one direction, we cannot change the sign
    * of the whole commodity anymore
    */
   if( (flowrowsigns[r] & (LHSPOSSIBLE | RHSPOSSIBLE)) != (LHSPOSSIBLE | RHSPOSSIBLE) )
      commoditysigns[k] = +1; /* we cannot switch directions */

   /* decide the sign (direction) of the row */
   if( rowsign == LHSPOSSIBLE )
      rowsign = LHSASSIGNED;
   else if( rowsign == RHSPOSSIBLE )
      rowsign = RHSASSIGNED;
   else
   {
      SCIP_Real dualsol = SCIProwGetDualsol(row);

      assert(rowsign == (LHSPOSSIBLE | RHSPOSSIBLE));

      /* if we have a valid non-zero dual solution, choose the side which is tight */
      if( !SCIPisZero(scip, dualsol) && dualsol != SCIP_INVALID )
      {
         if( dualsol > 0.0 )
            rowsign = LHSASSIGNED;
         else
            rowsign = RHSASSIGNED;
      }
      else
      {
         SCIP_Real rowlhs = SCIProwGetLhs(row);
         SCIP_Real rowrhs = SCIProwGetRhs(row);

         /* choose row sign such that we get a*x <= -d with d non-negative */
         if( rowrhs < 0.0 )
            rowsign = RHSASSIGNED;
         else if( rowlhs > 0.0 )
            rowsign = LHSASSIGNED;
         else
            rowsign = RHSASSIGNED; /* if we are still undecided, choose rhs */
      }
   }
   if( rowsign == RHSASSIGNED )
      rowscale = +1;
   else
      rowscale = -1;
   flowrowsigns[r] |= rowsign;

   SCIPdebugMessage("adding flow row %d <%s> with sign %+d to commodity %d [score:%g]\n",
                    r, SCIProwGetName(row), rowscale, k, mcfdata->flowrowscores[r]);
   SCIPdebug( SCIPprintRow(scip, row, NULL); );

   /* add row to commodity */
   rowcommodity[r] = k;
   rowcols = SCIProwGetCols(row);
   rowvals = SCIProwGetVals(row);
   rowlen = SCIProwGetNLPNonz(row);
   for( i = 0; i < rowlen; i++ )
   {
      SCIP_Real val;
      int c;

      c = SCIPcolGetLPPos(rowcols[i]);
      assert(0 <= c && c < SCIPgetNLPCols(scip));

      /* assign column to commodity */
      if( colcommodity[c] == -1 )
      {
         assert(!plusflow[c]);
         assert(!minusflow[c]);
         assert(mcfdata->nnewcols < SCIPgetNLPCols(scip));
         colcommodity[c] = k;
         newcols[mcfdata->nnewcols] = c;
         mcfdata->nnewcols++;
      }
      assert(colcommodity[c] == k);

      /* update plusflow/minusflow */
      val = rowscale * rowvals[i];
      if( val > 0.0 )
      {
         assert(!plusflow[c]);
         plusflow[c] = TRUE;
      }
      else
      {
         assert(!minusflow[c]);
         minusflow[c] = TRUE;
      }
   }
}

/** checks whether the given row fits into the given commodity and returns the possible flow row signs */
static
void getFlowrowFit(
   SCIP*                 scip,               /**< SCIP data structure */ 
   MCFDATA*              mcfdata,            /**< internal MCF extraction data to pass to subroutines */
   SCIP_ROW*             row,                /**< flow row to check */
   int                   k,                  /**< commodity that the flow row should enter */
   unsigned char*        rowsign             /**< pointer to store the possible flow row signs */
   )
{
   unsigned char* flowrowsigns = mcfdata->flowrowsigns;
   SCIP_Bool*     plusflow     = mcfdata->plusflow;
   SCIP_Bool*     minusflow    = mcfdata->minusflow;
   int*           colcommodity = mcfdata->colcommodity;
   int*           rowcommodity = mcfdata->rowcommodity;

   SCIP_COL** rowcols;
   SCIP_Real* rowvals;
   int rowlen;
   int flowrowsign;
   int r;
   int j;

   *rowsign = 0;

   r = SCIProwGetLPPos(row);
   assert(0 <= r && r < SCIPgetNLPRows(scip));

   /* ignore rows that are already used */
   if( rowcommodity[r] != -1 )
      return;

   /* check if row is an available flow row */
   flowrowsign = flowrowsigns[r];
   assert((flowrowsign & (LHSPOSSIBLE | RHSPOSSIBLE | DISCARDED)) == flowrowsign);
   if( (flowrowsign & DISCARDED) != 0 )
      return;
   if( (flowrowsign & (LHSPOSSIBLE | RHSPOSSIBLE)) == 0 )
      return;

   /* check whether the row fits w.r.t. the signs of the coefficients */
   rowcols = SCIProwGetCols(row);
   rowvals = SCIProwGetVals(row);
   rowlen = SCIProwGetNLPNonz(row);
   for( j = 0; j < rowlen && flowrowsign != 0; j++ )
   {
      int rowc;

      rowc = SCIPcolGetLPPos(rowcols[j]);
      assert(0 <= rowc && rowc < SCIPgetNLPCols(scip));

      /* check if column already belongs to the same commodity */
      if( colcommodity[rowc] == k )
      {
         /* column only fits if it is not yet present with the same sign */
         if( plusflow[rowc] )
         {
            /* column must not be included with positive sign */
            if( rowvals[j] > 0.0 )
               flowrowsign &= ~RHSPOSSIBLE;
            else
               flowrowsign &= ~LHSPOSSIBLE;
         }
         if( minusflow[rowc] )
         {
            /* column must not be included with negative sign */
            if( rowvals[j] > 0.0 )
               flowrowsign &= ~LHSPOSSIBLE;
            else
               flowrowsign &= ~RHSPOSSIBLE;
         }
      }
      else if( colcommodity[rowc] != -1 )
      {
         /* column does not fit if it already belongs to a different commodity */
         flowrowsign = 0;
      }
   }

   if( flowrowsign == 0 )
   {
      /* we can discard the row, since it can also not be member of a different commodity */
      SCIPdebugMessage(" -> discard flow row %d <%s>\n", r, SCIProwGetName(row));
      flowrowsigns[r] |= DISCARDED;
   }

   *rowsign = flowrowsign;
}

/** returns a flow conservation row that fits into the current commodity, or NULL */
static
void getNextFlowrow(
   SCIP*                 scip,               /**< SCIP data structure */ 
   MCFDATA*              mcfdata,            /**< internal MCF extraction data to pass to subroutines */
   SCIP_ROW**            nextrow,            /**< pointer to store next row */
   unsigned char*        nextrowsign         /**< pointer to store possible signs of next row */
   )
{
   SCIP_Real* flowrowscores = mcfdata->flowrowscores;
   SCIP_Bool* plusflow      = mcfdata->plusflow;
   SCIP_Bool* minusflow     = mcfdata->minusflow;
   int*       newcols       = mcfdata->newcols;
   int        ncommodities  = mcfdata->ncommodities;

   SCIP_COL** cols;
   int k;

   assert(nextrow != NULL);
   assert(nextrowsign != NULL);

   *nextrow = NULL;
   *nextrowsign = 0;

   k = ncommodities-1;

   cols = SCIPgetLPCols(scip);
   assert(cols != NULL);

   /* check if there are any columns left in the commodity that have not yet been inspected for incident flow rows */
   while( mcfdata->nnewcols > 0 )
   {
      SCIP_COL* col;
      SCIP_ROW** colrows;
      int collen;
      SCIP_ROW* bestrow;
      unsigned char bestrowsign;
      SCIP_Real bestscore;
      int c;
      int i;

      /* pop next new column from stack */
      c = newcols[mcfdata->nnewcols-1];
      mcfdata->nnewcols--;
      assert(0 <= c && c < SCIPgetNLPCols(scip));

      /* check if this columns already as both signs */
      assert(plusflow[c] || minusflow[c]);
      if( plusflow[c] && minusflow[c] )
         continue;

      /* check whether column is incident to a valid flow row that fits into the current commodity */
      bestrow = NULL;
      bestrowsign = 0;
      bestscore = 0.0;
      col = cols[c];
      colrows = SCIPcolGetRows(col);
      collen = SCIPcolGetNLPNonz(col);
      for( i = 0; i < collen; i++ )
      {
         SCIP_ROW* row;
         unsigned char flowrowsign;

         row = colrows[i];

         /* check if row fits into the current commodity */
         getFlowrowFit(scip, mcfdata, row, k, &flowrowsign);

         /* do we have a winner? */
         if( flowrowsign != 0 )
         {
            int r = SCIProwGetLPPos(row);
            assert(0 <= r && r < SCIPgetNLPRows(scip));
            assert(flowrowscores[r] > 0.0);

            if( flowrowscores[r] > bestscore )
            {
               bestrow = row;
               bestrowsign = flowrowsign;
               bestscore = flowrowscores[r];
            }
         }
      }

      /* if there was a valid row for this column, pick the best one
       * Note: This is not the overall best row, only the one for the first column that has a valid row.
       *       However, picking the overall best row seems to be too expensive
       */
      if( bestrow != NULL )
      {
         assert(bestscore > 0.0);
         assert(bestrowsign != 0);
         *nextrow = bestrow;
         *nextrowsign = bestrowsign;
         break;
      }
   }
}

/** extracts flow conservation rows and puts them into commodities */
static
SCIP_RETCODE extractFlow(
   SCIP*                 scip,               /**< SCIP data structure */ 
   MCFDATA*              mcfdata             /**< internal MCF extraction data to pass to subroutines */
   )
{
   int* flowcands = mcfdata->flowcands;

   SCIP_Bool* plusflow;
   SCIP_Bool* minusflow;
   int* colcommodity;
   int* rowcommodity;

   SCIP_ROW** rows;
   int nrows;
   int ncols;
   int i;
   int c;
   int r;

   /* get LP data */
   rows = SCIPgetLPRows(scip);
   nrows = SCIPgetNLPRows(scip);
   ncols = SCIPgetNLPCols(scip);

   /* allocate temporary memory */
   SCIP_CALL( SCIPallocMemoryArray(scip, &mcfdata->plusflow, ncols) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &mcfdata->minusflow, ncols) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &mcfdata->colcommodity, ncols) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &mcfdata->rowcommodity, nrows) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &mcfdata->newcols, ncols) );
   plusflow = mcfdata->plusflow;
   minusflow = mcfdata->minusflow;
   colcommodity = mcfdata->colcommodity;
   rowcommodity = mcfdata->rowcommodity;

   /* 3. Extract network structure of flow conservation constraints:
    *    (a) Initialize plusflow[c] = minusflow[c] = FALSE for all columns c and other local data.
    */
   BMSclearMemoryArray(plusflow, ncols);
   BMSclearMemoryArray(minusflow, ncols);
   for( c = 0; c < ncols; c++ )
      colcommodity[c] = -1;
   for( r = 0; r < nrows; r++ )
      rowcommodity[r] = -1;

   /*    (b) As long as there are flow conservation candidates left:
    *        (i) Create new commodity and use first flow conservation constraint as newrow.
    *       (ii) Add newrow to commodity, update pluscom/minuscom accordingly.
    *      (iii) For the newly added columns search for an incident flow conservation constraint. Pick the one of highest ranking.
    *       (iv) If found, set newrow to this row and goto (ii).
    */
   for( i = 0; i < mcfdata->nflowcands; i++ )
   {
      SCIP_ROW* newrow;
      unsigned char newrowsign;

      r = flowcands[i];
      assert(0 <= r && r < nrows);
      newrow = rows[r];

      /* check if row fits into a new commodity */
      getFlowrowFit(scip, mcfdata, newrow, mcfdata->ncommodities, &newrowsign);
      if( newrowsign == 0 )
         continue;

      /* start new commodity */
      SCIP_CALL( createNewCommodity(scip, mcfdata) );

      /* fill commodity with flow conservation constraints */
      do
      {
         /* add new row to commodity */
         addFlowrowToCommodity(scip, mcfdata, newrow, newrowsign);

         /* get next row to add */
         getNextFlowrow(scip, mcfdata, &newrow, &newrowsign);
      }
      while( newrow != NULL );
   }

   return SCIP_OKAY;
}

/** identifies capacity constraints for the arcs and assigns arc ids to columns and capacity constraints */
static
SCIP_RETCODE extractCapacities(
   SCIP*                 scip,               /**< SCIP data structure */ 
   MCFDATA*              mcfdata             /**< internal MCF extraction data to pass to subroutines */
   )
{
   unsigned char* capacityrowsigns  = mcfdata->capacityrowsigns;
   SCIP_Real*     capacityrowscores = mcfdata->capacityrowscores;
   int*           colcommodity      = mcfdata->colcommodity;
   int*           rowcommodity      = mcfdata->rowcommodity;

   int* colarcid;
   int* rowarcid;

   SCIP_ROW** rows;
   SCIP_COL** cols;
   int nrows;
   int ncols;
   int r;
   int c;

   assert(mcfdata->narcs == 0);

   /* get LP data */
   SCIP_CALL( SCIPgetLPRowsData(scip, &rows, &nrows) );
   SCIP_CALL( SCIPgetLPColsData(scip, &cols, &ncols) );

   /* allocate temporary memory for extraction data */
   SCIP_CALL( SCIPallocMemoryArray(scip, &mcfdata->colarcid, ncols) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &mcfdata->rowarcid, nrows) );
   colarcid = mcfdata->colarcid;
   rowarcid = mcfdata->rowarcid;

   /* initialize arcid arrays */
   for( c = 0; c < ncols; c++ )
      colarcid[c] = -1;
   for( r = 0; r < nrows; r++ )
      rowarcid[r] = -1;

   /* for each column, search for a capacity constraint */
   for( c = 0; c < ncols; c++ )
   {
      SCIP_ROW* bestcapacityrow;
      SCIP_Real bestscore;
      SCIP_ROW** colrows;
      int collen;
      int i;

      /* ignore columns that are not flow variables */
      if( colcommodity[c] == -1 )
         continue;

      /* ignore columns that are already assigned to an arc */
      if( colarcid[c] >= 0 )
         continue;

      /* scan the column to search for valid capacity constraints */
      bestcapacityrow = NULL;
      bestscore = 0.0;
      colrows = SCIPcolGetRows(cols[c]);
      collen = SCIPcolGetNLPNonz(cols[c]);
      for( i = 0; i < collen; i++ )
      {
         r = SCIProwGetLPPos(colrows[i]);
         assert(0 <= r && r < nrows);

         /* row must not be already assigned */
         assert((capacityrowsigns[r] & (LHSPOSSIBLE | RHSPOSSIBLE)) == capacityrowsigns[r]);

         /* ignore rows that are not capacity candidates */
         if( (capacityrowsigns[r] & (LHSPOSSIBLE | RHSPOSSIBLE)) == 0 )
            continue;

         /* ignore rows that are already used as flow conservation constraints */
         if( rowcommodity[r] != -1 )
            continue;

         /* check if this capacity candidate has better score */
         assert(capacityrowscores[r] > 0.0);
         if( capacityrowscores[r] > bestscore )
         {
            bestcapacityrow = colrows[i];
            bestscore = capacityrowscores[r];
         }
      }

      /* if no capacity row has been found, leave the column unassigned */
      if( bestcapacityrow != NULL )
      {
         SCIP_COL** rowcols;
         int rowlen;

         /* store the row */
         assert(mcfdata->narcs <= mcfdata->capacityrowssize);
         if( mcfdata->narcs == mcfdata->capacityrowssize )
         {
            mcfdata->capacityrowssize = MAX(2*mcfdata->capacityrowssize, mcfdata->narcs+1);
            SCIP_CALL( SCIPreallocMemoryArray(scip, &mcfdata->capacityrows, mcfdata->capacityrowssize) );
         }
         assert(mcfdata->narcs < mcfdata->capacityrowssize);
         mcfdata->capacityrows[mcfdata->narcs] = bestcapacityrow;

         /* assign the capacity row to a new arc id */
         r = SCIProwGetLPPos(bestcapacityrow);
         assert(0 <= r && r < nrows);
         rowarcid[r] = mcfdata->narcs;

         /* decide which sign to use */
         if( (capacityrowsigns[r] & RHSPOSSIBLE) != 0 )
            capacityrowsigns[r] |= RHSASSIGNED;
         else
         {
            assert((capacityrowsigns[r] & LHSPOSSIBLE) != 0);
            capacityrowsigns[r] |= LHSASSIGNED;
         }

         SCIPdebugMessage("assigning capacity row %d <%s> with sign %+d to arc %d [score:%g]\n",
                          r, SCIProwGetName(bestcapacityrow), (capacityrowsigns[r] & RHSASSIGNED) != 0 ? +1 : -1, mcfdata->narcs,
                          mcfdata->capacityrowscores[r]);

         /* assign all involved flow variables to the new arc id */
         SCIPdebugMessage(" -> flow:");
         rowcols = SCIProwGetCols(bestcapacityrow);
         rowlen = SCIProwGetNLPNonz(bestcapacityrow);
         for( i = 0; i < rowlen; i++ )
         {
            int rowc;

            rowc = SCIPcolGetLPPos(rowcols[i]);
            assert(0 <= rowc && rowc < ncols);

            if( colcommodity[rowc] >= 0 )
            {
               SCIPdebug( printf(" x%d<%s>[%d]", rowc, SCIPvarGetName(SCIPcolGetVar(rowcols[i])), colcommodity[rowc]) );
               colarcid[rowc] = mcfdata->narcs;
            }
         }
         SCIPdebug( printf("\n") );

         /* increase number of arcs */
         mcfdata->narcs++;
      }
      else
      {
         SCIPdebugMessage("no capacity row found for column x%d <%s> in commodity %d\n", c, SCIPvarGetName(SCIPcolGetVar(cols[c])), colcommodity[c]);
      }
   }

   return SCIP_OKAY;
}

/** collects all flow columns of all commodities (except the one of the base row) that are incident to the node described by the given flow row */
static
void collectIncidentFlowCols(
   SCIP*                 scip,               /**< SCIP data structure */ 
   MCFDATA*              mcfdata,            /**< internal MCF extraction data to pass to subroutines */
   SCIP_ROW*             flowrow,            /**< flow conservation constraint that defines the node */
   int                   basecommodity       /**< commodity of the base row */
   )
{
   int*           colcommodity  = mcfdata->colcommodity;
   int*           colarcid      = mcfdata->colarcid;
   int*           newcols       = mcfdata->newcols;
   SCIP_ROW**     capacityrows  = mcfdata->capacityrows;
   SCIP_Bool*     colisincident = mcfdata->colisincident;

   SCIP_COL** rowcols;
   int rowlen;
   int i;

#ifndef NDEBUG
   /* check that the marker array is correctly initialized */
   for( i = 0; i < SCIPgetNLPCols(scip); i++ )
      assert(!colisincident[i]);
#endif

   /* loop through all flow columns in the flow conservation constraint */
   rowcols = SCIProwGetCols(flowrow);
   rowlen = SCIProwGetNLPNonz(flowrow);
   mcfdata->nnewcols = 0;
   for( i = 0; i < rowlen; i++ )
   {
      SCIP_COL** capacityrowcols;
      int capacityrowlen;
      int arcid;
      int c;
      int j;

      c = SCIPcolGetLPPos(rowcols[i]);
      assert(0 <= c && c < SCIPgetNLPCols(scip));

      /* get arc id of the column in the flow conservation constraint */
      arcid = colarcid[c];
      if( arcid == -1 )
         continue;
      assert(arcid < mcfdata->narcs);

      /* collect flow variables in the capacity constraint of this arc */
      assert(capacityrows[arcid] != NULL);
      capacityrowcols = SCIProwGetCols(capacityrows[arcid]);
      capacityrowlen = SCIProwGetNLPNonz(capacityrows[arcid]);
      for( j = 0; j < capacityrowlen; j++ )
      {
         int caprowc;

         caprowc = SCIPcolGetLPPos(capacityrowcols[j]);
         assert(0 <= caprowc && caprowc < SCIPgetNLPCols(scip));

         /* ignore columns that do not belong to a commodity, i.e., are not flow variables */
         if( colcommodity[caprowc] == -1 )
         {
            assert(colarcid[caprowc] == -1);
            continue;
         }
         assert(colarcid[caprowc] == arcid);

         /* ignore columns in the same commodity as the base row */
         if( colcommodity[caprowc] == basecommodity )
            continue;

         /* if not already done, collect the column */
         if( !colisincident[caprowc] )
         {
            assert(mcfdata->nnewcols < SCIPgetNLPCols(scip));
            colisincident[caprowc] = TRUE;
            newcols[mcfdata->nnewcols] = caprowc;
            mcfdata->nnewcols++;
         }
      }
   }
}

/** compares given row against a base node flow row and calculates a similarity score;
 *  score is 0.0 if the rows are incompatible
 */
static
void getNodeSilimarityScore(
   SCIP*                 scip,               /**< SCIP data structure */ 
   MCFDATA*              mcfdata,            /**< internal MCF extraction data to pass to subroutines */
   int                   baserowlen,         /**< length of base node flow row */
   int*                  basearcpattern,     /**< arc patern of base node flow row */
   SCIP_ROW*             row,                /**< row to compare against base node flow row */
   SCIP_Real*            score,              /**< pointer to store the similarity score */
   SCIP_Bool*            invertcommodity     /**< pointer to store whether the arcs in the commodity of the row have
                                              *   to be inverted for the row to be compatible to the base row */
   )
{
   unsigned char* flowrowsigns   = mcfdata->flowrowsigns;
   int*           commoditysigns = mcfdata->commoditysigns;
   int*           rowcommodity   = mcfdata->rowcommodity;
   int*           colarcid       = mcfdata->colarcid;

   SCIP_COL** rowcols;
   SCIP_Real* rowvals;
   int rowlen;
   int rowcom;
   int rowcomsign;
   SCIP_Bool incompatable;
   int overlap;
   int r;
   int i;

   *score = 0.0;
   *invertcommodity = FALSE;

   r = SCIProwGetLPPos(row);
   assert(0 <= r && r < SCIPgetNLPRows(scip));
   assert((flowrowsigns[r] & (LHSASSIGNED | RHSASSIGNED)) != 0);
   rowcom = rowcommodity[r];
   assert(0 <= rowcom && rowcom < mcfdata->ncommodities);
   rowcomsign = commoditysigns[rowcom];
   assert(-1 <= rowcomsign && rowcomsign <= +1);

   rowcols = SCIProwGetCols(row);
   rowvals = SCIProwGetVals(row);
   rowlen = SCIProwGetNLPNonz(row);
   incompatable = FALSE;
   overlap = 0;
   for( i = 0; i < rowlen; i++ )
   {
      int c;
      int arcid;
      int valsign;

      c = SCIPcolGetLPPos(rowcols[i]);
      assert(0 <= c && c < SCIPgetNLPCols(scip));

      arcid = colarcid[c];
      if( arcid == -1 )
         continue;
      assert(arcid < mcfdata->narcs);

      valsign = (rowvals[i] > 0.0 ? +1 : -1);
      if( (flowrowsigns[r] & LHSASSIGNED) != 0 )
         valsign *= -1;
      if( basearcpattern[arcid] == valsign )
      {
         /* both rows have the same sign for this arc */
         if( rowcomsign == -1 )
         {
            /* commodity is inverted, but we have the same sign: this is incompatible */
            incompatable = TRUE;
            break;
         }
         else
         {
            /* we must not invert arc directions in the commodity */
            rowcomsign = +1;
            overlap++;
         }
      }
      else if( basearcpattern[arcid] == -valsign )
      {
         /* both rows have opposite sign for this arc */
         if( rowcomsign == +1 )
         {
            /* commodity cannot be inverted, but we have opposite signs: this is incompatible */
            incompatable = TRUE;
            break;
         }
         else
         {
            /* we must invert arc directions in the commodity */
            rowcomsign = -1;
            overlap++;
         }
      }
      else
         assert(basearcpattern[arcid] == 0);
   }

   /* calculate the score: maximize overlap and use minimal number of non-overlapping entries as tie breaker */
   if( !incompatable && overlap >= 1 )
   {
      assert(overlap <= rowlen);
      assert(overlap <= baserowlen);
      *score = overlap - (rowlen + baserowlen - 2*overlap)/(mcfdata->narcs+1.0);
      if( overlap >= 2 ) /* overlap 1 is very dangerous, since this can also be the other end node of the arc */
         *score += 1000.0;
      *score = MAX(*score, 1e-6); /* score may get negative due to many columns in row without an arcid */
      *invertcommodity = (rowcomsign == -1);
   }
}


/** assigns node ids to flow conservation constraints */
static
SCIP_RETCODE extractNodes(
   SCIP*                 scip,               /**< SCIP data structure */ 
   MCFDATA*              mcfdata             /**< internal MCF extraction data to pass to subroutines */
   )
{
   unsigned char* flowrowsigns   = mcfdata->flowrowsigns;
   int*           commoditysigns = mcfdata->commoditysigns;
   int*           flowcands      = mcfdata->flowcands;
   int            nflowcands     = mcfdata->nflowcands;
   int*           rowcommodity   = mcfdata->rowcommodity;
   int*           colarcid       = mcfdata->colarcid;
   int*           newcols        = mcfdata->newcols;
   int*           rownodeid;
   SCIP_Bool*     colisincident;
   SCIP_Bool*     rowprocessed;

   SCIP_ROW** rows;
   SCIP_COL** cols;
   int nrows;
   int ncols;

   int* arcpattern;
   SCIP_ROW** bestflowrows;
   SCIP_Real* bestscores;
   SCIP_Bool* bestinverted;
   int r;
   int c;
   int n;

   assert(mcfdata->nnodes == 0);

   /* get LP data */
   SCIP_CALL( SCIPgetLPRowsData(scip, &rows, &nrows) );
   SCIP_CALL( SCIPgetLPColsData(scip, &cols, &ncols) );

   /* allocate temporary memory */
   SCIP_CALL( SCIPallocMemoryArray(scip, &mcfdata->rownodeid, nrows) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &mcfdata->colisincident, ncols) );
   rownodeid = mcfdata->rownodeid;
   colisincident = mcfdata->colisincident;

   /* allocate temporary local memory */
   SCIP_CALL( SCIPallocMemoryArray(scip, &arcpattern, mcfdata->narcs) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &bestflowrows, mcfdata->ncommodities) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &bestscores, mcfdata->ncommodities) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &bestinverted, mcfdata->ncommodities) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &rowprocessed, nrows) );

   /* initialize temporary memory */
   for( r = 0; r < nrows; r++ )
      rownodeid[r] = -1;
   for( c = 0; c < ncols; c++ )
      colisincident[c] = FALSE;

   /* process all flow conservation constraints that have been used */
   for( n = 0; n < nflowcands; n++ )
   {
      SCIP_COL** rowcols;
      SCIP_Real* rowvals;
      int rowlen;
      int rowscale;
      int basecommodity;
      int i;

      r = flowcands[n];
      assert(0 <= r && r < nrows);

      /* ignore rows that are not used as flow conservation constraint */
      basecommodity = rowcommodity[r];
      if( basecommodity == -1 )
         continue;
      assert((flowrowsigns[r] & (LHSASSIGNED | RHSASSIGNED)) != 0);
      assert(mcfdata->rowarcid[r] == -1);

      /* skip rows that are already assigned to a node */
      if( rownodeid[r] >= 0 )
         continue;

      /* assign row to new node id */
      SCIPdebugMessage("assigning row %d <%s> of commodity %d to node %d [score: %g]\n",
                       r, SCIProwGetName(rows[r]), basecommodity, mcfdata->nnodes, mcfdata->flowrowscores[r]);
      rownodeid[r] = mcfdata->nnodes;

      /* get the arc pattern of the flow row */
      BMSclearMemoryArray(arcpattern, mcfdata->narcs);
      rowcols = SCIProwGetCols(rows[r]);
      rowvals = SCIProwGetVals(rows[r]);
      rowlen = SCIProwGetNLPNonz(rows[r]);
      if( (flowrowsigns[r] & RHSASSIGNED) != 0 )
         rowscale = +1;
      else
         rowscale = -1;
      if( commoditysigns[basecommodity] == -1 )
         rowscale *= -1;

      for( i = 0; i < rowlen; i++ )
      {
         int arcid;

         c = SCIPcolGetLPPos(rowcols[i]);
         assert(0 <= c && c < ncols);
         arcid = colarcid[c];
         if( arcid >= 0 )
         {
            if( rowvals[i] > 0.0 )
               arcpattern[arcid] = rowscale;
            else
               arcpattern[arcid] = -rowscale;
         }
      }

      /* initialize arrays to store best flow rows */
      for( i = 0; i < mcfdata->ncommodities; i++ )
      {
         bestflowrows[i] = NULL;
         bestscores[i] = 0.0;
         bestinverted[i] = FALSE;
      }

      /* collect columns that are member of incident arc capacity constraints */
      collectIncidentFlowCols(scip, mcfdata, rows[r], basecommodity);

      /* initialize rowprocessed array */
      BMSclearMemoryArray(rowprocessed, nrows);

      /* identify flow conservation constraints in other commodities that match this node;
       * search for flow rows in the column vectors of the indicent columns
       */
      for( i = 0; i < mcfdata->nnewcols; i++ )
      {
         SCIP_ROW** colrows;
         int collen;
         int j;

         c = newcols[i];
         assert(0 <= c && c < ncols);
         assert(mcfdata->colcommodity[c] >= 0);
         assert(mcfdata->colcommodity[c] != basecommodity);

         /* clean up the marker array */
         assert(colisincident[c]);
         colisincident[c] = FALSE;

         /* scan column vector for flow conservation constraints */
         colrows = SCIPcolGetRows(cols[c]);
         collen = SCIPcolGetNLPNonz(cols[c]);
         for( j = 0; j < collen; j++ )
         {
            int colr;
            int rowcom;
            SCIP_Real score;
            SCIP_Bool invertcommodity;

            colr = SCIProwGetLPPos(colrows[j]);
            assert(0 <= colr && colr < nrows);

            /* ignore rows that have already been processed */
            if( rowprocessed[colr] )
               continue;
            rowprocessed[colr] = TRUE;

            /* ignore rows that are not flow conservation constraints in the network */
            rowcom = rowcommodity[colr];
            assert(rowcom != basecommodity);
            if( rowcom == -1 )
               continue;
            assert(rowcom == mcfdata->colcommodity[c]);
            assert((flowrowsigns[colr] & (LHSASSIGNED | RHSASSIGNED)) != 0);
            assert(mcfdata->rowarcid[colr] == -1);

            /* ignore rows that are already assigned to a node */
            if( rownodeid[colr] >= 0 )
               continue;

            /* compare row against arc pattern and calculate score */
            getNodeSilimarityScore(scip, mcfdata, rowlen, arcpattern, colrows[j], &score, &invertcommodity);
            if( score > bestscores[rowcom] )
            {
               bestflowrows[rowcom] = colrows[j];
               bestscores[rowcom] = score;
               bestinverted[rowcom] = invertcommodity;
            }
         }
      }

      /* for each commodity, pick the best flow conservation constraint to define this node */
      for( i = 0; i < mcfdata->ncommodities; i++ )
      {
         int comr;

         if( bestflowrows[i] == NULL )
            continue;

         comr = SCIProwGetLPPos(bestflowrows[i]);
         assert(0 <= comr && comr < nrows);
         assert(rowcommodity[comr] == i);
         assert((flowrowsigns[comr] & (LHSASSIGNED | RHSASSIGNED)) != 0);
         assert(rownodeid[comr] == -1);

         /* assign flow row to current node */
         SCIPdebugMessage(" -> assigning row %d <%s> of commodity %d to node %d [invert:%d]\n",
                          comr, SCIProwGetName(rows[comr]), i, mcfdata->nnodes, bestinverted[i]);
         rownodeid[comr] = mcfdata->nnodes;

         /* fix the direction of the arcs of the commodity */
         if( bestinverted[i] )
         {
            assert(commoditysigns[i] != +1);
            commoditysigns[i] = -1;
         }
         else
         {
            assert(commoditysigns[i] != -1);
            commoditysigns[i] = +1;
         }
      }
      
      /* increase number of nodes */
      mcfdata->nnodes++;
   }

   /* free local temporary memory */
   SCIPfreeMemoryArray(scip, &rowprocessed);
   SCIPfreeMemoryArray(scip, &bestinverted);
   SCIPfreeMemoryArray(scip, &bestscores);
   SCIPfreeMemoryArray(scip, &bestflowrows);
   SCIPfreeMemoryArray(scip, &arcpattern);

   return SCIP_OKAY;
}

/* if there are still undecided commodity signs, fix them to +1 */
static
void fixCommoditySigns(
   SCIP*                 scip,               /**< SCIP data structure */ 
   MCFDATA*              mcfdata             /**< internal MCF extraction data to pass to subroutines */
   )
{
   int* commoditysigns = mcfdata->commoditysigns;
   int k;

   for( k = 0; k < mcfdata->ncommodities; k++ )
   {
      if( commoditysigns[k] == 0 )
         commoditysigns[k] = +1;
   }
}

/** extracts a MCF network structure from the current LP */
static
SCIP_RETCODE mcfnetworkExtract(
   SCIP*                 scip,               /**< SCIP data structure */ 
   int                   maxsigndistance,    /**< maximum Hamming distance of flow conservation constraint sign patterns of the same node */
   SCIP_MCFNETWORK**     mcfnetwork          /**< MCF network structure */
   )
{
   SCIP_MCFNETWORK* mcf;
   MCFDATA mcfdata;

   SCIP_ROW** rows;
   SCIP_COL** cols;
   int nrows;
   int ncols;

   assert(mcfnetwork != NULL);

   /* Algorithm to identify multi-commodity-flow network with capacity constraints
    *
    * 1. Identify candidate rows for flow conservation constraints and capacity constraints in the LP.
    * 2. Sort flow conservation and capacity constraint candidates by a ranking on
    *    how sure we are that it is indeed a constraint of the desired type.
    * 3. Extract network structure of flow conservation constraints:
    *    (a) Initialize plusflow[c] = minusflow[c] = FALSE for all columns c and other local data.
    *    (b) As long as there are flow conservation candidates left:
    *        (i) Create new commodity and use first flow conservation constraint as newrow.
    *       (ii) Add newrow to commodity, update pluscom/minuscom accordingly.
    *      (iii) For the newly added columns search for an incident flow conservation constraint. Pick the one of highest ranking.
    *       (iv) If found, set newrow to this row and goto (ii).
    * 4. Identify capacity constraints for the arcs and assign arc ids to columns and capacity constraints.
    * 5. Assign node ids to flow conservation constraints.
    */

   /* create network data structure */
   SCIP_CALL( mcfnetworkCreate(scip, mcfnetwork) );
   assert(*mcfnetwork != NULL);
   mcf = *mcfnetwork;

   /* get LP data */
   SCIP_CALL( SCIPgetLPRowsData(scip, &rows, &nrows) );
   SCIP_CALL( SCIPgetLPColsData(scip, &cols, &ncols) );

   /* initialize local extraction data */
   mcfdata.flowrowsigns = NULL;
   mcfdata.flowrowscalars = NULL;
   mcfdata.flowrowscores = NULL;
   mcfdata.capacityrowsigns = NULL;
   mcfdata.capacityrowscores = NULL;
   mcfdata.flowcands = NULL;
   mcfdata.nflowcands = 0;
   mcfdata.capacitycands = NULL;
   mcfdata.ncapacitycands = 0;
   mcfdata.plusflow = NULL;
   mcfdata.minusflow = NULL;
   mcfdata.ncommodities = 0;
   mcfdata.commoditysigns = NULL;
   mcfdata.commoditysignssize = 0;
   mcfdata.colcommodity = NULL;
   mcfdata.rowcommodity = NULL;
   mcfdata.colarcid = NULL;
   mcfdata.rowarcid = NULL;
   mcfdata.rownodeid = NULL;
   mcfdata.newcols = NULL;
   mcfdata.nnewcols = 0;
   mcfdata.narcs = 0;
   mcfdata.nnodes = 0;
   mcfdata.capacityrows = NULL;
   mcfdata.capacityrowssize = 0;
   mcfdata.colisincident = NULL;

   /* 1. Identify candidate rows for flow conservation constraints and capacity constraints in the LP.
    * 2. Sort flow conservation and capacity constraint candidates by a ranking on
    *    how sure we are that it is indeed a constraint of the desired type.
    */
   SCIP_CALL( extractRows(scip, &mcfdata) );

   if( mcfdata.nflowcands > 0 && mcfdata.ncapacitycands > 0 )
   {
      /* 3. Extract network structure of flow conservation constraints. */
      SCIPdebugMessage("****** extracting flow ******\n");
      SCIP_CALL( extractFlow(scip, &mcfdata) );
#ifdef SCIP_DEBUG
      printCommodities(scip, &mcfdata);
#endif
      
      /* 4. Identify capacity constraints for the arcs and assign arc ids to columns and capacity constraints. */
      SCIPdebugMessage("****** extracting capacities ******\n");
      SCIP_CALL( extractCapacities(scip, &mcfdata) );

      /* 5. Assign node ids to flow conservation constraints. */
      SCIPdebugMessage("****** extracting nodes ******\n");
      SCIP_CALL( extractNodes(scip, &mcfdata) );

      /* if there are still undecided commodity signs, fix them to +1 */
      fixCommoditySigns(scip, &mcfdata);

      /**@todo Now we need to assign arcids to flow variables that have not yet been assigned to an arc because
       *       there was no valid capacity constraint.
       *       Go through the list of nodes and generate uncapacitated arcs in the network for the flow variables
       *       that do not yet have an arc assigned, such that the commodities still match.
       */
   }

#ifdef SCIP_DEBUG
   printCommodities(scip, &mcfdata);
#endif

   /* fill sparse network structure */
   SCIP_CALL( mcfnetworkFill(scip, *mcfnetwork, &mcfdata) );

   /* free memory */
   SCIPfreeMemoryArrayNull(scip, &mcfdata.colisincident);
   SCIPfreeMemoryArrayNull(scip, &mcfdata.capacityrows);
   SCIPfreeMemoryArrayNull(scip, &mcfdata.rownodeid);
   SCIPfreeMemoryArrayNull(scip, &mcfdata.rowarcid);
   SCIPfreeMemoryArrayNull(scip, &mcfdata.colarcid);
   SCIPfreeMemoryArrayNull(scip, &mcfdata.newcols);
   SCIPfreeMemoryArrayNull(scip, &mcfdata.rowcommodity);
   SCIPfreeMemoryArrayNull(scip, &mcfdata.colcommodity);
   SCIPfreeMemoryArrayNull(scip, &mcfdata.commoditysigns);
   SCIPfreeMemoryArrayNull(scip, &mcfdata.minusflow);
   SCIPfreeMemoryArrayNull(scip, &mcfdata.plusflow);
   SCIPfreeMemoryArrayNull(scip, &mcfdata.capacitycands);
   SCIPfreeMemoryArrayNull(scip, &mcfdata.flowcands);
   SCIPfreeMemoryArrayNull(scip, &mcfdata.capacityrowscores);
   SCIPfreeMemoryArrayNull(scip, &mcfdata.capacityrowsigns);
   SCIPfreeMemoryArrayNull(scip, &mcfdata.flowrowscores);
   SCIPfreeMemoryArrayNull(scip, &mcfdata.flowrowscalars);
   SCIPfreeMemoryArrayNull(scip, &mcfdata.flowrowsigns);

   return SCIP_OKAY;
}


/** comparison method for weighted arcs */
static
SCIP_DECL_SORTPTRCOMP(compArcs)
{
   ARCENTRY* arc1 = (ARCENTRY*)elem1;
   ARCENTRY* arc2 = (ARCENTRY*)elem2;

   if( arc1->weight > arc2->weight )
      return -1;
   else if( arc1->weight < arc2->weight )
      return +1;
   else
      return 0;
}

/** creates a priority queue and fills it with the given arc entries */
static
SCIP_RETCODE arcqueueCreate(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_MCFNETWORK*      mcfnetwork,         /**< MCF network structure */
   ARCQUEUE**            arcqueue            /**< pointer to arc priority queue */
   )
{
   int a;

   assert(mcfnetwork != NULL);
   assert(arcqueue != NULL);

   SCIP_CALL( SCIPallocMemory(scip, arcqueue) );

   /* calculate weights for arcs */
   SCIP_CALL( SCIPallocMemoryArray(scip, &(*arcqueue)->arcentries, mcfnetwork->narcs) );
   for( a = 0; a < mcfnetwork->narcs; a++ )
   {
      SCIP_Real slack;
      SCIP_Real dualsol;
      SCIP_Real scale;

      slack = SCIPgetRowFeasibility(scip, mcfnetwork->arccapacityrows[a]);
      slack = MAX(slack, 0.0); /* can only be negative due to numerics */
      dualsol = SCIProwGetDualsol(mcfnetwork->arccapacityrows[a]);
      scale = ABS(mcfnetwork->arccapacityscales[a]);

      /* shrink rows with large slack first, and from the tight rows, shrink the ones with small dual solution value first */
      (*arcqueue)->arcentries[a].arcid = a;
      (*arcqueue)->arcentries[a].weight = scale * slack - ABS(dualsol)/scale;
      SCIPdebugMessage("arc %2d <%s>: slack=%g, dualsol=%g -> weight=%g\n", a, SCIProwGetName(mcfnetwork->arccapacityrows[a]),
                       slack, dualsol, (*arcqueue)->arcentries[a].weight);
   }

   /* create priority queue */
   SCIP_CALL( SCIPpqueueCreate(&(*arcqueue)->pqueue, mcfnetwork->narcs, 2.0, compArcs) );

   /* fill priority queue with arc data */
   for( a = 0; a < mcfnetwork->narcs; a++ )
   {
      SCIP_CALL( SCIPpqueueInsert((*arcqueue)->pqueue, (void*)&(*arcqueue)->arcentries[a]) );
   }

   return SCIP_OKAY;
}

/** frees memory of an arc queue */
static
void arcqueueFree(
   SCIP*                 scip,               /**< SCIP data structure */
   ARCQUEUE**            arcqueue            /**< pointer to arc priority queue */
   )
{
   assert(arcqueue != NULL);
   assert(*arcqueue != NULL);

   SCIPpqueueFree(&(*arcqueue)->pqueue);
   SCIPfreeMemoryArray(scip, &(*arcqueue)->arcentries);
   SCIPfreeMemory(scip, arcqueue);
}

/** returns whether there are any arcs left on the queue */
static
SCIP_Bool arcqueueIsEmpty(
   ARCQUEUE*             arcqueue            /**< arc priority queue */
   )
{
   assert(arcqueue != NULL);

   return (SCIPpqueueFirst(arcqueue->pqueue) == NULL);
}

/** removes the top element from the arc priority queue and returns the arcid */
static
int arcqueueRemove(
   ARCQUEUE*             arcqueue            /**< arc priority queue */
   )
{
   ARCENTRY* arcentry;

   assert(arcqueue != NULL);

   arcentry = (ARCENTRY*)SCIPpqueueRemove(arcqueue->pqueue);
   if( arcentry != NULL )
      return arcentry->arcid;
   else
      return -1;
}

/** returns the representative node in the cluster of the given node */
static
int nodepartitionGetRepresentative(
   NODEPARTITION*        nodepartition,      /**< node partition data structure */
   int                   v                   /**< node id to get representative for */
   )
{
   int* representatives;

   assert(nodepartition != NULL);

   /* we apply a union find algorithm */
   representatives = nodepartition->representatives;
   while( v != representatives[v] )
   {
      representatives[v] = representatives[representatives[v]];
      v = representatives[v];
   }

   return v;
}

/** joins two clusters given by their representative nodes */
static
void nodepartitionJoin(
   NODEPARTITION*        nodepartition,      /**< node partition data structure */
   int                   rep1,               /**< representative of first cluster */
   int                   rep2                /**< representative of second cluster */
   )
{
   assert(nodepartition != NULL);
   assert(rep1 != rep2);
   assert(nodepartition->representatives[rep1] == rep1);
   assert(nodepartition->representatives[rep2] == rep2);

   /* make sure that the smaller representative survives
    *  -> node 0 is always a representative
    */
   if( rep1 < rep2 )
      nodepartition->representatives[rep2] = rep1;
   else
      nodepartition->representatives[rep1] = rep2;
}

/** partitions nodes into a small number of clusters */
static
SCIP_RETCODE nodepartitionCreate(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_MCFNETWORK*      mcfnetwork,         /**< MCF network structure */
   NODEPARTITION**       nodepartition,      /**< pointer to node partition data structure */
   int                   nclusters           /**< number of clusters to generate */
   )
{
   ARCQUEUE* arcqueue;
   int* clustersize;
   int nclustersleft;
   int v;
   int c;
   int pos;

   assert(mcfnetwork != NULL);
   assert(nodepartition != NULL);

   /* allocate and initialize memory */
   SCIP_CALL( SCIPallocMemory(scip, nodepartition) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &(*nodepartition)->representatives, mcfnetwork->nnodes) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &(*nodepartition)->nodeclusters, mcfnetwork->nnodes) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &(*nodepartition)->clusternodes, mcfnetwork->nnodes) );
   SCIP_CALL( SCIPallocMemoryArray(scip, &(*nodepartition)->clusterbegin, nclusters+1) );
   (*nodepartition)->nclusters = 0;

   /* we start with each node being in its own cluster */
   for( v = 0; v < mcfnetwork->nnodes; v++ )
      (*nodepartition)->representatives[v] = v;

   /* create priority queue for arcs */
   SCIP_CALL( arcqueueCreate(scip, mcfnetwork, &arcqueue) );

   /* loop over arcs in order of their weights */
   nclustersleft = mcfnetwork->nnodes;
   while( !arcqueueIsEmpty(arcqueue) && nclustersleft > nclusters )
   {
      int a;
      int sourcev;
      int targetv;
      int sourcerep;
      int targetrep;
      
      /* get the next arc */
      a = arcqueueRemove(arcqueue);
      assert(0 <= a && a < mcfnetwork->narcs);
      assert(arcqueue->arcentries[a].arcid == a);

      /* get the source and target node of the arc */
      sourcev = mcfnetwork->arcsources[a];
      targetv = mcfnetwork->arctargets[a];

      /* identify the representatives of the two nodes */
      sourcerep = nodepartitionGetRepresentative(*nodepartition, sourcev);
      targetrep = nodepartitionGetRepresentative(*nodepartition, targetv);
      assert(0 <= sourcerep && sourcerep < mcfnetwork->nnodes);
      assert(0 <= targetrep && targetrep < mcfnetwork->nnodes);
      
      /* there is nothing to do if the two nodes are already in the same cluster */
      if( sourcerep == targetrep )
         continue;

      /* shrink the arc by joining the two clusters */
      SCIPdebugMessage("shrinking arc %d <%s> with weight %g (s=%g y=%g): join representatives %d and %d\n",
                       a, SCIProwGetName(mcfnetwork->arccapacityrows[a]), arcqueue->arcentries[a].weight,
                       SCIPgetRowFeasibility(scip, mcfnetwork->arccapacityrows[a]),
                       SCIProwGetDualsol(mcfnetwork->arccapacityrows[a]), sourcerep, targetrep);
      nodepartitionJoin(*nodepartition, sourcerep, targetrep);
      nclustersleft--;
   }

   /* node 0 must be a representative due to our join procedure */
   assert((*nodepartition)->representatives[0] == 0);

   /* if there have been too few arcs to shrink the graph to the required number of clusters, join clusters with first cluster
    * to create a larger disconnected cluster
    */
   if( nclustersleft > nclusters )
   {
      for( v = 1; v < mcfnetwork->nnodes && nclustersleft > nclusters; v++ )
      {
         int rep;

         rep = nodepartitionGetRepresentative(*nodepartition, v);
         if( rep != 0 )
         {
            nodepartitionJoin(*nodepartition, 0, rep);
            nclustersleft--;
         }
      }
   }
   assert(nclustersleft <= nclusters);

   /* extract the clusters */
   SCIP_CALL( SCIPallocBufferArray(scip, &clustersize, nclusters) );
   BMSclearMemoryArray(clustersize, nclusters);
   for( v = 0; v < mcfnetwork->nnodes; v++ )
   {
      int rep;

      /* get cluster of node */
      rep = nodepartitionGetRepresentative(*nodepartition, v);
      assert(rep <= v); /* due to our joining procedure */
      if( rep == v )
      {
         /* node is its own representative: this is a new cluster */
         c = (*nodepartition)->nclusters;
         (*nodepartition)->nclusters++;
      }
      else
         c = (*nodepartition)->nodeclusters[rep];
      assert(0 <= c && c < nclusters);

      /* assign node to cluster */
      (*nodepartition)->nodeclusters[v] = c;
      clustersize[c]++;
   }

   /* fill the clusterbegin array */
   pos = 0;
   for( c = 0; c < (*nodepartition)->nclusters; c++ )
   {
      (*nodepartition)->clusterbegin[c] = pos;
      pos += clustersize[c];
   }
   assert(pos == mcfnetwork->nnodes);
   (*nodepartition)->clusterbegin[(*nodepartition)->nclusters] = mcfnetwork->nnodes;

   /* fill the clusternodes array */
   BMSclearMemoryArray(clustersize, (*nodepartition)->nclusters);
   for( v = 0; v < mcfnetwork->nnodes; v++ )
   {
      c = (*nodepartition)->nodeclusters[v];
      assert(0 <= c && c < (*nodepartition)->nclusters);
      pos = (*nodepartition)->clusterbegin[c] + clustersize[c];
      assert(pos < (*nodepartition)->clusterbegin[c+1]);
      (*nodepartition)->clusternodes[pos] = v;
      clustersize[c]++;
   }

   /* free temporary memory */
   SCIPfreeBufferArray(scip, &clustersize);

   /* free arc queue */
   arcqueueFree(scip, &arcqueue);

   return SCIP_OKAY;
}

/** frees node partition data */
static
void nodepartitionFree(
   SCIP*                 scip,               /**< SCIP data structure */
   NODEPARTITION**       nodepartition       /**< pointer to node partition data structure */
   )
{
   assert(nodepartition != NULL);
   assert(*nodepartition != NULL);

   SCIPfreeMemoryArray(scip, &(*nodepartition)->representatives);
   SCIPfreeMemoryArray(scip, &(*nodepartition)->nodeclusters);
   SCIPfreeMemoryArray(scip, &(*nodepartition)->clusternodes);
   SCIPfreeMemoryArray(scip, &(*nodepartition)->clusterbegin);
   SCIPfreeMemory(scip, nodepartition);
}

#ifdef SCIP_DEBUG
static
void nodepartitionPrint(
   NODEPARTITION*        nodepartition       /**< node partition data structure */
   )
{
   int c;

   for( c = 0; c < nodepartition->nclusters; c++ )
   {
      int i;

      printf("cluster %d:", c);
      for( i = nodepartition->clusterbegin[c]; i < nodepartition->clusterbegin[c+1]; i++ )
         printf(" %d", nodepartition->clusternodes[i]);
      printf("\n");
   }
}
#endif

/** searches and adds MCF network cuts that separate the given primal solution */
static
SCIP_RETCODE separateCuts(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_SEPA*            sepa,               /**< the cut separator itself */
   SCIP_SOL*             sol,                /**< primal solution that should be separated, or NULL for LP solution */
   SCIP_RESULT*          result              /**< pointer to store the result of the separation call */
   )
{  /*lint --e{715}*/
   SCIP_SEPADATA* sepadata;
   SCIP_MCFNETWORK* mcfnetwork;
   NODEPARTITION* nodepartition;

   assert(result != NULL);

   *result = SCIP_DIDNOTRUN;

   /* get separator data */
   sepadata = SCIPsepaGetData(sepa);
   assert(sepadata != NULL);

   /* get or extract network flow structure */
   if( sepadata->mcfnetwork == NULL )
   {
      *result = SCIP_DIDNOTFIND;
      SCIP_CALL( mcfnetworkExtract(scip, sepadata->maxsigndistance, &sepadata->mcfnetwork) );
      SCIPdebugMessage("extracted network has %d nodes, %d arcs, and %d commodities\n",
                       sepadata->mcfnetwork->nnodes, sepadata->mcfnetwork->narcs,
                       sepadata->mcfnetwork->ncommodities);
      SCIPdebug( mcfnetworkPrint(sepadata->mcfnetwork) );
   }
   assert(sepadata->mcfnetwork != NULL);
   mcfnetwork = sepadata->mcfnetwork;

   /* if the network does not have at least 2 nodes, we can stop */
   if( mcfnetwork->nnodes < 2 )
      return SCIP_OKAY;

   /* separate cuts */
   *result = SCIP_DIDNOTFIND;

   /* partition nodes into a small number of clusters */
   SCIP_CALL( nodepartitionCreate(scip, mcfnetwork, &nodepartition, sepadata->nclusters) );

   SCIPdebug( nodepartitionPrint(nodepartition) );

   /* free node partition */
   nodepartitionFree(scip, &nodepartition);

   return SCIP_OKAY;
}




/*
 * Callback methods of separator
 */

/* TODO: Implement all necessary separator methods. The methods with an #if 0 ... #else #define ... are optional */

/** destructor of separator to free user data (called when SCIP is exiting) */
static
SCIP_DECL_SEPAFREE(sepaFreeMcf)
{  /*lint --e{715}*/
   SCIP_SEPADATA* sepadata;

   /* free separator data */
   sepadata = SCIPsepaGetData(sepa);
   assert(sepadata != NULL);
   assert(sepadata->mcfnetwork == NULL);

   SCIPfreeMemory(scip, &sepadata);

   SCIPsepaSetData(sepa, NULL);

   return SCIP_OKAY;
}


/** initialization method of separator (called after problem was transformed) */
#if 0
static
SCIP_DECL_SEPAINIT(sepaInitMcf)
{  /*lint --e{715}*/
   SCIPerrorMessage("method of mcf separator not implemented yet\n");
   SCIPABORT(); /*lint --e{527}*/

   return SCIP_OKAY;
}
#else
#define sepaInitMcf NULL
#endif


/** deinitialization method of separator (called before transformed problem is freed) */
#if 0
static
SCIP_DECL_SEPAEXIT(sepaExitMcf)
{  /*lint --e{715}*/
   SCIPerrorMessage("method of mcf separator not implemented yet\n");
   SCIPABORT(); /*lint --e{527}*/

   return SCIP_OKAY;
}
#else
#define sepaExitMcf NULL
#endif


/** solving process initialization method of separator (called when branch and bound process is about to begin) */
#if 0
static
SCIP_DECL_SEPAINITSOL(sepaInitsolMcf)
{  /*lint --e{715}*/
   SCIPerrorMessage("method of mcf separator not implemented yet\n");
   SCIPABORT(); /*lint --e{527}*/

   return SCIP_OKAY;
}
#else
#define sepaInitsolMcf NULL
#endif


/** solving process deinitialization method of separator (called before branch and bound process data is freed) */
static
SCIP_DECL_SEPAEXITSOL(sepaExitsolMcf)
{  /*lint --e{715}*/
   SCIP_SEPADATA* sepadata;

   /* get separator data */
   sepadata = SCIPsepaGetData(sepa);
   assert(sepadata != NULL);

   /* free MCF network */
   mcfnetworkFree(scip, &sepadata->mcfnetwork);

   return SCIP_OKAY;
}


/** LP solution separation method of separator */
static
SCIP_DECL_SEPAEXECLP(sepaExeclpMcf)
{  /*lint --e{715}*/
   /* separate cuts on the LP solution */
   SCIP_CALL( separateCuts(scip, sepa, NULL, result) );

   return SCIP_OKAY;
}


/** arbitrary primal solution separation method of separator */
static
SCIP_DECL_SEPAEXECSOL(sepaExecsolMcf)
{  /*lint --e{715}*/
   /* separate cuts on the given primal solution */
   SCIP_CALL( separateCuts(scip, sepa, sol, result) );

   return SCIP_OKAY;
}




/*
 * separator specific interface methods
 */

/** creates the mcf separator and includes it in SCIP */
SCIP_RETCODE SCIPincludeSepaMcf(
   SCIP*                 scip                /**< SCIP data structure */
   )
{
   SCIP_SEPADATA* sepadata;

#if 1
   /* disabled, because separator is not yet finished */
   return SCIP_OKAY;
#endif

   /* create cmir separator data */
   SCIP_CALL( SCIPallocMemory(scip, &sepadata) );
   sepadata->mcfnetwork = NULL;

   /* include separator */
   SCIP_CALL( SCIPincludeSepa(scip, SEPA_NAME, SEPA_DESC, SEPA_PRIORITY, SEPA_FREQ, SEPA_MAXBOUNDDIST, SEPA_DELAY,
         sepaFreeMcf, sepaInitMcf, sepaExitMcf, 
         sepaInitsolMcf, sepaExitsolMcf,
         sepaExeclpMcf, sepaExecsolMcf,
         sepadata) );

   /* add mcf separator parameters */
   SCIP_CALL( SCIPaddIntParam(scip,
         "separating/mcf/maxsigndistance",
         "maximum Hamming distance of flow conservation constraint sign patterns of the same node",
         &sepadata->maxsigndistance, TRUE, DEFAULT_MAXSIGNDISTANCE, 0, INT_MAX, NULL, NULL) );
   SCIP_CALL( SCIPaddIntParam(scip,
         "separating/mcf/nclusters",
         "number of clusters to generate in the shrunken network",
         &sepadata->nclusters, TRUE, DEFAULT_NCLUSTERS, 2, INT_MAX, NULL, NULL) );

   return SCIP_OKAY;
}
