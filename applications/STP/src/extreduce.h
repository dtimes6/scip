/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2019 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the ZIB Academic License.         */
/*                                                                           */
/*  You should have received a copy of the ZIB Academic License              */
/*  along with SCIP; see the file COPYING. If not visit scip.zib.de.         */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/**@file   extreduce.h
 * @brief  This file implements extended reduction techniques for several Steiner problems.
 * @author Daniel Rehfeldt
 *
 *
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/


#ifndef APPLICATIONS_STP_SRC_EXTREDUCE_H_
#define APPLICATIONS_STP_SRC_EXTREDUCE_H_

#define STP_EXT_CLOSENODES_MAXN 64
#define STP_EXT_MAXSTACKSIZE  5000
#define STP_EXT_MAXNCOMPS     1000
#define STP_EXT_MAXDFSDEPTH 6
#define STP_EXT_MINDFSDEPTH 4
#define STP_EXT_MAXGRAD 8
#define STP_EXT_MAXEDGES 500
#define STP_EXT_EDGELIMIT 50000
#define STP_EXTTREE_MAXNEDGES 25
#define STP_EXTTREE_MAXNLEAVES 20
#define STP_EXTTREE_MAXNLEAVES_GUARD (STP_EXTTREE_MAXNLEAVES + STP_EXT_MAXGRAD)

#define EXT_STATE_NONE     0
#define EXT_STATE_EXPANDED 1
#define EXT_STATE_MARKED   2

#include "scip/scip.h"
#include "graph.h"
#include "reduce.h"
#include "completegraph.h"

/** structure for storing distances in the extension tree */
typedef struct multi_level_distances_storage MLDISTS;

/** permanent extension data */
typedef struct extension_data_permanent
{
   DCMST*                dcmst;              /**< dynamic MST */
   CSRDEPO*              msts;               /**< storage for MSTs with extending node */
   CSRDEPO*              msts_reduced;       /**< storage for MSTs without extending node */
   MLDISTS*              sds_horizontal;     /**< SDs from deepest leaves to remaining ones */
   MLDISTS*              sds_vertical;       /**< SDs between leaves of same depth */
   STP_Bool*             edgedeleted;        /**< (non-owned!) edge array to mark which directed edge can be removed */
   SCIP_Bool*            isterm;             /**< marks whether node is a terminal (or proper terminal for PC) */
   SCIP_Real*            bottleneckDistNode; /**< needs to be set to -1.0 (size nnodes) */
   SCIP_Real*            pcSdToNode;         /**< needs to be set to -1.0 for all nodes, or NULL if not Pc */
   int*                  tree_deg;           /**< -1 for forbidden nodes (e.g. PC terminals), nnodes for tail, 0 otherwise; in method: position ( > 0) for nodes in tree */
   int                   nnodes;             /**< number of nodes */
} EXTPERMA;


/** path root state */
typedef struct pathroot_state
{
   int pathroot_id;
   int pathroot_nrecomps;
} PRSTATE;

/** distance data */
typedef struct distance_data
{
   //    SCIP_Bool*  pathrootsd_isdirty;
   DHEAP* dheap;
   RANGE* closenodes_range;          /* of size nnodes */
   int* closenodes_indices;          /* of size closenodes_totalsize */
   SCIP_Real* closenodes_distances;  /* of size closenodes_totalsize */
   int* pathroot_blocksizes;         /* of size nedges / 2 */
   int* pathroot_blocksizesmax;      /* of size nedges / 2 */
   PRSTATE** pathroot_blocks;        /* of size nedges / 2 */
   SCIP_Bool* pathroot_isdirty;      /* of size nnodes */
   int* pathroot_nrecomps;           /* of size nnodes */
   DIJK* dijkdata;
   int closenodes_totalsize;
   int closenodes_maxnumber;
} DISTDATA;


/** Reduction data; just used internally.
 *  Stores information for SDs between tree vertices,
 *  reduced costs, and pseudo-ancestor conflicts. */
typedef struct reduction_data
{
   DCMST* const dcmst;
   CSRDEPO* const msts;
   CSRDEPO* const msts_reduced;
   MLDISTS* const sds_horizontal;
   MLDISTS* const sds_vertical;
   const SCIP_Real* const redCosts;
   const SCIP_Real* const rootToNodeDist;
   const PATH* const nodeTo3TermsPaths;
   const int* const nodeTo3TermsBases;
   const STP_Bool* const edgedeleted;
   int* const pseudoancestor_mark;
   const SCIP_Real cutoff;
   const SCIP_Bool equality;
   const int redCostRoot;
} REDDATA;


/** extension data; just used internally */
typedef struct extension_data
{
   int* const extstack_data;
   int* const extstack_start;
   int* const extstack_state;
   int* const tree_leaves;
   int* const tree_edges;
   int* const tree_deg;                      /**< -1 for forbidden nodes (e.g. PC terminals), nnodes for current tail,
                                                   0 otherwise; in method: position ( > 0) for nodes in tree */
   SCIP_Real* const tree_bottleneckDistNode; /**< needs to be set to -1.0 (for all nodes) */
   int* const tree_parentNode;
   SCIP_Real* const tree_parentEdgeCost;     /**< of size nnodes */
   SCIP_Real* const tree_redcostSwap;           /**< of size nnodes */
   SCIP_Real* const pcSdToNode;                /**< needs to be set to -1.0, only needed of PC */
   int* const pcSdCands;                       /**< needed only for PC */
   const SCIP_Bool* const node_isterm;         /**< marks whether node is a terminal (or proper terminal for PC) */
   REDDATA* const reddata;
   DISTDATA* const distdata;
   SCIP_Real tree_redcost;
   int tree_nDelUpArcs;
   int tree_root;
   int tree_nedges;
   int tree_depth;
   int tree_nleaves;
   int nPcSdCands;                             /**< needed only for PC */
   int extstack_ncomponents;
   const int extstack_maxncomponents;
   const int extstack_maxsize;
   const int tree_maxnleaves;
   const int tree_maxdepth;
   const int tree_maxnedges;
} EXTDATA;


/* inline methods
 */

/** returns current position in the stack */
static inline
int extStackGetPosition(
   const EXTDATA*        extdata             /**< extension data */
)
{
   assert(extdata);
   assert(extdata->extstack_ncomponents > 0);

   return (extdata->extstack_ncomponents - 1);
}



/* extreduce_base.c
 */
extern SCIP_RETCODE    extreduce_deleteArcs(SCIP*, const REDCOST*, const int*, GRAPH*, STP_Bool*, int*);
extern SCIP_RETCODE    extreduce_deleteEdges(SCIP*, const REDCOST*, const int*, GRAPH*, STP_Bool*, int*);
extern SCIP_RETCODE    extreduce_pseudodeleteNodes(SCIP*, const REDCOST*, const int*, GRAPH*, STP_Bool*, int*);
extern int             extreduce_getMaxTreeDepth(const GRAPH*);
extern int             extreduce_getMaxStackSize(void);
extern int             extreduce_getMaxStackNcomponents(const GRAPH*);
extern int             extreduce_getMaxStackNedges(const GRAPH*);
extern void            extreduce_removeEdge(SCIP*, int, GRAPH*, DISTDATA*);

/* extreduce_core.c
 */
extern SCIP_RETCODE    extreduce_checkArc(SCIP*, const GRAPH*, const REDCOST*, int, SCIP_Bool, DISTDATA*, EXTPERMA*, SCIP_Bool*);
extern SCIP_RETCODE    extreduce_checkEdge(SCIP*, const GRAPH*, const REDCOST*, int, SCIP_Bool, DISTDATA*, EXTPERMA*, SCIP_Bool*);
extern SCIP_RETCODE    extreduce_checkNode(SCIP*, const GRAPH*, const REDCOST*, int, SCIP_Bool, DISTDATA*, EXTPERMA*, SCIP_Bool*);


/* extreduce_util.c
 */
extern SCIP_RETCODE       extreduce_distDataInit(SCIP*, const GRAPH*, int, SCIP_Bool, DISTDATA*);
extern SCIP_Real          extreduce_distDataGetSD(SCIP*, const GRAPH*, int, int, DISTDATA*);
extern void               extreduce_distDataFreeMembers(SCIP*, const GRAPH*, DISTDATA*);
extern void               extreduce_distDataDeleteEdge(SCIP*, const GRAPH*, int, DISTDATA*);
extern SCIP_RETCODE       extreduce_extPermaInit(SCIP*, const GRAPH*, STP_Bool*, EXTPERMA*);
extern SCIP_Bool          extreduce_extPermaIsClean(const GRAPH*, const EXTPERMA*);
extern void               extreduce_extPermaFreeMembers(SCIP*, EXTPERMA*);
extern void               extreduce_extdataClean(EXTDATA*);
extern SCIP_Bool          extreduce_extdataIsClean(const GRAPH*, const EXTDATA*);
extern void               extreduce_reddataClean(REDDATA*);
extern SCIP_Bool          extreduce_reddataIsClean(const GRAPH*, const REDDATA*);
extern SCIP_Bool          extreduce_edgeIsValid(const GRAPH*, int);
extern SCIP_RETCODE       extreduce_mldistsInit(SCIP*, int, int, int, MLDISTS**);
extern void               extreduce_mldistsFree(SCIP*, MLDISTS**);
extern SCIP_Bool          extreduce_mldistsIsEmpty(const MLDISTS*);
extern SCIP_Bool          extreduce_mldistsEmptySlotExists(const MLDISTS*);
extern int*               extreduce_mldistsEmptySlotTargetIds(const MLDISTS*);
extern SCIP_Real*         extreduce_mldistsEmptySlotTargetDists(const MLDISTS*);
extern int                extreduce_mldistsEmptySlotLevel(const MLDISTS*);
extern void               extreduce_mldistsEmtpySlotSetBase(int, MLDISTS*);
extern void               extreduce_mldistsEmtpySlotReset(MLDISTS*);
extern void               extreduce_mldistsEmptySlotSetFilled(MLDISTS*);
extern void               extreduce_mldistsLevelAddTop(int, int, MLDISTS*);
extern void               extreduce_mldistsLevelCloseTop(MLDISTS*);
extern void               extreduce_mldistsLevelRemoveTop(MLDISTS*);
extern void               extreduce_mldistsLevelRemoveTopNonClosed(MLDISTS*);
extern int                extreduce_mldistsLevelNTargets(const MLDISTS*, int);
extern int                extreduce_mldistsLevelNTopTargets(const MLDISTS*);
extern int                extreduce_mldistsLevelNSlots(const MLDISTS*, int);
extern int                extreduce_mldistsNlevels(const MLDISTS*);
extern SCIP_Bool          extreduce_mldistsLevelContainsBase(const MLDISTS*, int, int);
extern const int*         extreduce_mldistsTargetIds(const MLDISTS*, int, int);
extern const SCIP_Real*   extreduce_mldistsTargetDists(const MLDISTS*, int, int);
extern const int*         extreduce_mldistsTopTargetIds(const MLDISTS*, int);
extern const SCIP_Real*   extreduce_mldistsTopTargetDists(const MLDISTS*, int);


/* extreduce_extmst.c
 */

extern void       extreduce_mstAddRoot(SCIP*, int, REDDATA*);
extern void       extreduce_mstCompAddLeaf(SCIP*, const GRAPH*, int, EXTDATA*, SCIP_Bool*);
extern void       extreduce_mstCompInit(SCIP*, const GRAPH*, int, EXTDATA*, SCIP_Bool*);
extern void       extreduce_mstCompRemove(const GRAPH*, EXTDATA*);
extern void       extreduce_mstLevelInit(REDDATA*, EXTDATA*);
extern void       extreduce_mstLevelAddLeaf(SCIP*, const GRAPH*, int, EXTDATA*, SCIP_Bool*);
extern void       extreduce_mstLevelClose(REDDATA*);
extern void       extreduce_mstLevelRemove(REDDATA*);
extern SCIP_Real  extreduce_extGetSD(SCIP*, const GRAPH*, int, int, EXTDATA*);


/* extreduce_dbg.c
 */
extern SCIP_Bool       extreduce_treeIsFlawed(SCIP*, const GRAPH*, const EXTDATA*);
extern SCIP_Bool       extreduce_treeIsHashed(const GRAPH*, const EXTDATA*);
extern SCIP_Bool       extreduce_nodeIsInStackTop(const GRAPH*, const EXTDATA*, int);
extern SCIP_Bool       extreduce_distCloseNodesAreValid(SCIP*, const GRAPH*, const DISTDATA*);
extern SCIP_Real       extreduce_treeGetSdMstWeight(SCIP*, const GRAPH*, EXTDATA*);
extern SCIP_Real       extreduce_treeGetSdMstExtWeight(SCIP*, const GRAPH*, int, EXTDATA*);
extern void            extreduce_printStack(const GRAPH*, const EXTDATA*);
extern void            extreduce_extendInitDebug(int*, int*);


#endif /* APPLICATIONS_STP_SRC_EXTREDUCE_H_ */
