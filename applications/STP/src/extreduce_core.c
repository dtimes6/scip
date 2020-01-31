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

/**@file   extreduce_core.c
 * @brief  extended reductions for Steiner tree problems
 * @author Daniel Rehfeldt
 *
 * This file implements extended reduction techniques for several Steiner problems.
 *
 * A list of all interface methods can be found in extreduce.h.
 *
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/
//#define SCIP_DEBUG
//#define STP_DEBUG_EXT

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "graph.h"
#include "portab.h"
#include "extreduce.h"
#define EXT_REDCOST_NRECOMP 10


/** insertion sort; todo
 * : could be speed-up by use of sentinel value at position 0
 * : do something special: maybe sort index array
 * */
static inline
void sortDescendingIntRealReal(
   int*                  keyArr,             /**< key array of size 'nentries' */
   SCIP_Real*            dataArr1,           /**< array of size 'nentries' */
   SCIP_Real*            dataArr2,           /**< array of size 'nentries' */
   int                   nentries            /**< number of entries */
)
{
   assert(keyArr && dataArr1 && dataArr2);
   assert(nentries >= 1);

   for( int i = 1; i < nentries; i++ )
   {
      int j;
      const int currKey = keyArr[i];
      const SCIP_Real currData1 = dataArr1[i];
      const SCIP_Real currData2 = dataArr2[i];

      for( j = i - 1; j >= 0 && currKey > keyArr[j]; j-- )
      {
         keyArr[j + 1] = keyArr[j];
         dataArr1[j + 1] = dataArr1[j];
         dataArr2[j + 1] = dataArr2[j];
      }

      keyArr[j + 1] = currKey;
      dataArr1[j + 1] = currData1;
      dataArr2[j + 1] = currData2;
   }

#ifndef NDEBUG
   for( int i = 1; i < nentries; i++ )
      assert(keyArr[i - 1] >= keyArr[i] );
#endif
}


/** helper for rooted tree reduced cost computation */
static inline
SCIP_Real getMinDistCombination(
   const SCIP_Real*      firstTermDist,      /**< array of size 'nentries' */
   const SCIP_Real*      secondTermDist,     /**< array of size 'nentries' */
   int                   nentries            /**< number of entries to check */
)
{
   SCIP_Real min;

   assert(firstTermDist && secondTermDist);
   assert(nentries >= 1);

   if( nentries == 1 )
   {
      min = firstTermDist[0];
   }
   else
   {
      int i;
      SCIP_Real secondSum = 0.0;
      min = FARAWAY;

      for( i = 0; i < nentries; i++ )
      {
         assert(LE(firstTermDist[i], secondTermDist[i]));
         assert(LE(secondTermDist[i], FARAWAY));

         if( EQ(secondTermDist[i], FARAWAY) )
            break;

         secondSum += secondTermDist[i];
      }

      assert(LT(secondSum, FARAWAY));

      /* is there an index i with secondTermDist[i] == FARAWAY? */
      if( i < nentries )
      {
         assert(EQ(secondTermDist[i], FARAWAY));

         min = firstTermDist[i];
         for( int j = 0; j < nentries; j++ )
         {
            if( j == i )
               continue;

            min += secondTermDist[j];
         }
      }
      else
      {
         for( i = 0; i < nentries; i++ )
         {
            const SCIP_Real distCombination = secondSum + firstTermDist[i] - secondTermDist[i];

            assert(LT(secondTermDist[i], FARAWAY));

            if( distCombination < min )
               min = distCombination;
         }

         assert(LE(min, secondSum));
      }
   }

   return min;
}


/** returns root of top component on the stack */
static inline
int extStackGetTopRoot(
   const GRAPH*          graph,              /**< graph data structure */
   const EXTDATA*        extdata             /**< extension data */
)
{
   const int stackpos = extStackGetPosition(extdata);
   const int comproot = graph->tail[extdata->extstack_data[extdata->extstack_start[stackpos]]];

   assert(comproot >= 0);
   assert(extdata->tree_deg[comproot] >= 1 || comproot == extdata->tree_root);

   return comproot;
}


/** Adds non-expanded components (i.e. sets of edges extending at a certain leaf) to the stack.
 *  Components are ordered such that smallest component is added last. */
static inline
void extStackAddCompsNonExpanded(
   const GRAPH*          graph,              /**< graph data structure */
   const int*            extedgesstart,      /**< starts of extension edges for one components */
   const int*            extedges,           /**< extensions edges */
   int                   nextendable_leaves, /**< number of leaves that can be extended */
   EXTDATA*              extdata             /**< extension data */
   )
{
   int* const extstack_data = extdata->extstack_data;
   int* const extstack_start = extdata->extstack_start;
   int* const extstack_state = extdata->extstack_state;
   int stackpos = extStackGetPosition(extdata);
   int datasize = extstack_start[stackpos + 1];
   int extsize[STP_EXT_MAXGRAD];
   int extindex[STP_EXT_MAXGRAD];

   assert(nextendable_leaves > 0);
   assert(extedgesstart[nextendable_leaves] - extedgesstart[0] > 0);
   assert(datasize + (extedgesstart[nextendable_leaves] - extedgesstart[0]) <= extdata->extstack_maxsize);

   for( int i = 0; i < nextendable_leaves; i++ )
   {
      assert(extedgesstart[i + 1] >= 0);

      extsize[i] = extedgesstart[i + 1] - extedgesstart[i];
      extindex[i] = i;
      assert(extsize[i] >= 0);
   }

   SCIPsortDownIntInt(extsize, extindex, nextendable_leaves);

   /* put the non-empty extensions on the stack, with smallest last */
   for( int i = 0; i < nextendable_leaves; i++ )
   {
      const int index = extindex[i];

      if( extsize[i] == 0 )
      {
#ifndef NDEBUG
         assert(i > 0);
         assert(extedgesstart[index + 1] - extedgesstart[index] == 0);

         for( int j = i; j < nextendable_leaves; j++ )
            assert(extsize[j] == 0);
#endif

         break;
      }

      for( int j = extedgesstart[index]; j < extedgesstart[index + 1]; j++ )
      {
         assert(extedges[j] >= 0);
         extstack_data[datasize++] = extedges[j];
      }

      assert(stackpos < extdata->extstack_maxsize - 2);

      extstack_state[++stackpos] = EXT_STATE_NONE;
      extstack_start[stackpos + 1] = datasize;
   }

#ifdef SCIP_DEBUG
   printf("added extending edges:  \n");

   for( int i = extstack_start[extdata->extstack_ncomponents]; i < extstack_start[stackpos + 1]; i++ )
      graph_edge_printInfo(graph, extstack_data[i]);
#endif

   extdata->extstack_ncomponents = stackpos + 1;

   assert(extdata->extstack_ncomponents <= extdata->extstack_maxncomponents);
}


/** no reversed tree possible? */
static inline
SCIP_Bool extRedcostReverseTreeRuledOut(
   const GRAPH*          graph,              /**< graph data structure */
   const EXTDATA*        extdata             /**< extension data */
)
{
   const REDDATA* const reddata = extdata->reddata;
   const int comproot = extStackGetTopRoot(graph, extdata);

   return (reddata->redCostRoot == extdata->tree_root || GE(extdata->tree_redcostSwap[comproot], FARAWAY));
}


/** update reduced cost for added edge */
static inline
void extRedcostAddEdge(
   const GRAPH*          graph,              /**< graph data structure */
   int                   edge,               /**< edge to be added */
   SCIP_Bool             noReversedTree,     /**< don't consider reversed tree? */
   const REDDATA*        reddata,            /**< reduction data */
   EXTDATA*              extdata             /**< extension data */
)
{
   const SCIP_Real* const redcost = reddata->redCosts;
   SCIP_Real* const tree_redcostSwap = extdata->tree_redcostSwap;
   const STP_Bool* const edgedeleted = reddata->edgedeleted;
   const SCIP_Bool edgeIsDeleted = (edgedeleted && edgedeleted[edge]);
   const int head = graph->head[edge];

   if( noReversedTree || (edgedeleted && edgedeleted[flipedge(edge)]) )
   {
      tree_redcostSwap[head] = FARAWAY;
   }
   else
   {
      const int tail = graph->tail[edge];

      tree_redcostSwap[head] = tree_redcostSwap[tail] + redcost[flipedge(edge)];

      if( !edgeIsDeleted )
         tree_redcostSwap[head] -= redcost[edge];

      assert(LT(tree_redcostSwap[head], FARAWAY));
   }

   if( !edgeIsDeleted )
   {
      extdata->tree_redcost += redcost[edge];
      assert(LT(extdata->tree_redcost, FARAWAY));
   }
   else
   {
      extdata->tree_nDelUpArcs++;
   }
}


/** update reduced cost for removed edge */
static inline
void extRedcostRemoveEdge(
   int                   edge,               /**< edge to be added */
   const REDDATA*        reddata,            /**< reduction data */
   EXTDATA*              extdata             /**< extension data */
)
{
   const SCIP_Real* const redcost = reddata->redCosts;
   const STP_Bool* const edgedeleted = reddata->edgedeleted;
   const SCIP_Bool edgeIsDeleted = (edgedeleted && edgedeleted[edge]);

   if( !edgeIsDeleted )
   {
      extdata->tree_redcost -= redcost[edge];
      assert(LT(extdata->tree_redcost, FARAWAY));
   }
   else
   {
      extdata->tree_nDelUpArcs--;
      assert(extdata->tree_nDelUpArcs >= 0);
   }
}


/** can we extend the tree from given leaf? */
inline static
SCIP_Bool extLeafIsExtendable(
   const GRAPH*          graph,              /**< graph data structure */
   const SCIP_Bool*      isterm,             /**< marks whether node is a terminal (or proper terminal for PC) */
   int                   leaf                /**< the leaf */
)
{
   assert(graph && isterm);
   assert(leaf >= 0 && leaf < graph->knots);

   // todo if not a terminal, check whether number of neigbhors not contained in current tree < STP_EXT_MAXGRAD

   return (!isterm[leaf] && graph->grad[leaf] <= STP_EXT_MAXGRAD);
}


/** Finds position of given leaf in leaves data.
 *  Returns -1 if leaf could not be found. */
static inline
int extLeafFindPos(
   const EXTDATA*        extdata,            /**< extension data */
   int                   leaf                /**< leaf to find */
)
{
   int i;
   const int* const tree_leaves = extdata->tree_leaves;

   assert(tree_leaves);
   assert(extdata->tree_nleaves > 1);
   assert(leaf >= 0 && extdata->tree_deg[leaf] >= 1);

   for( i = extdata->tree_nleaves - 1; i >= 0; i-- )
   {
      const int currleaf = tree_leaves[i];

      if( currleaf == leaf )
         break;
   }

   return i;
}


/** adds a new leaf */
static inline
void extLeafAdd(
   int                   leaf,              /**< leaf to add */
   EXTDATA*              extdata            /**< extension data */
)
{
   assert(extdata && extdata->tree_leaves);
   assert(leaf >= 0 && extdata->tree_deg[leaf] == 0);

   extdata->tree_leaves[(extdata->tree_nleaves)++] = leaf;
}


/** remove entry from leaves list */
static inline
void extLeafRemove(
   int                   leaf,              /**< leaf to remove */
   EXTDATA*              extdata            /**< extension data */
)
{
   int* const tree_leaves = extdata->tree_leaves;
   int nleaves;
   int position;

   assert(extdata->tree_deg[leaf] == 1);

   /* switch last leaf and leaf to be removed */
   assert(extdata->tree_nleaves > 1);

   position = extLeafFindPos(extdata, leaf);
   assert(position > 0);

   extdata->tree_nleaves--;

   nleaves = extdata->tree_nleaves;

   for( int p = position; p < nleaves; p++ )
   {
      tree_leaves[p] = tree_leaves[p + 1];
   }
}


/** Remove top component from leaves, and restore the root of the component as a leaf
 *  NOTE: assumes that the tree_deg has already been adapted */
static inline
void extLeafRemoveTop(
   int                   topsize,           /**< size of top to remove */
   int                   toproot,           /**< root of the top component */
   EXTDATA*              extdata            /**< extension data */
)
{
   assert(extdata);
   assert(topsize >= 1 && topsize < extdata->tree_nleaves);
   assert(toproot >= 0);
   assert(extdata->tree_deg[toproot] == 1);

#ifndef NDEBUG
   {
      const int* const tree_deg = extdata->tree_deg;
      const int* const tree_leaves = extdata->tree_leaves;
      const int nleaves = extdata->tree_nleaves;

      for( int i = 1; i <= topsize; i++ )
      {
         const int leaf = tree_leaves[nleaves - i];
         assert(tree_deg[leaf] == 0);
      }
   }
#endif

   extdata->tree_nleaves -= topsize;
   extdata->tree_leaves[(extdata->tree_nleaves)++] = toproot;

   assert(extdata->tree_nleaves >= 1);
}

/** gets reduced cost of current tree rooted at leave 'root', called direct if tree cannot */
static
SCIP_Real extTreeGetDirectedRedcostProper(
   const GRAPH*          graph,              /**< graph data structure */
   const EXTDATA*        extdata,            /**< extension data */
   int                   root                /**< the root for the orientation */
)
{
   int nearestTerms[STP_EXTTREE_MAXNLEAVES_GUARD];
   SCIP_Real firstTermDist[STP_EXTTREE_MAXNLEAVES_GUARD];
   SCIP_Real secondTermDist[STP_EXTTREE_MAXNLEAVES_GUARD];
   const int* const tree_leaves = extdata->tree_leaves;
   const int nleaves = extdata->tree_nleaves;
   const SCIP_Real swapcost = extdata->tree_redcostSwap[root];
   const REDDATA* const reddata = extdata->reddata;
   const PATH* const nodeTo3TermsPaths = reddata->nodeTo3TermsPaths;
   const int* const next3Terms = reddata->nodeTo3TermsBases;
   const SCIP_Bool* const isterm = extdata->node_isterm;
   const int* tree_deg = extdata->tree_deg;

   SCIP_Real redcost_directed = extdata->tree_redcost + reddata->rootToNodeDist[root] + swapcost;
   const int nnodes = graph->knots;
   int leavescount = 0;

#ifndef NDEBUG
   SCIP_Real redcost_debug = redcost_directed;
   for( int i = 0; i < STP_EXTTREE_MAXNLEAVES_GUARD; i++ )
   {
      nearestTerms[i] = -1;
      firstTermDist[i] = -1.0;
      secondTermDist[i] = -1.0;
   }

   assert(LT(redcost_directed, FARAWAY));
#endif

   for( int j = 0; j < nleaves; j++ )
   {
      int i;
      int term;
      const int leaf = tree_leaves[j];

      if( leaf == root || isterm[leaf] )
      {
         assert(leaf == root || EQ(0.0, nodeTo3TermsPaths[leaf].dist));
         continue;
      }

      /* find closest valid terminal for extension */
      for( i = 0; i < 3; i++ )
      {
         term = next3Terms[leaf + i * nnodes];

         if( term == UNKNOWN )
            break;

         assert(graph_pc_isPcMw(graph) || Is_term(graph->term[term]));
         assert(term >= 0 && term < graph->knots);
         assert(term != leaf);

         /* terminal not in current tree?*/
         if( tree_deg[term] == 0 )
            break;

         if( tree_deg[term] < 0 )
         {
            assert(graph_pc_isPcMw(graph) && tree_deg[term] == -1);
            assert(Is_pseudoTerm(graph->term[term]));
            break;
         }
      }

      /* no terminal reachable? */
      if( term == UNKNOWN )
      {
         assert(i < 3 && GE(nodeTo3TermsPaths[leaf + i * nnodes].dist, FARAWAY));
         return FARAWAY;
      }

      /* all terminals in current tree? */
      if( i == 3 )
         i = 2;

      nearestTerms[leavescount] = term;
      firstTermDist[leavescount] = nodeTo3TermsPaths[leaf + i * nnodes].dist;
      secondTermDist[leavescount] = (i < 2) ? nodeTo3TermsPaths[leaf + (i + 1) * nnodes].dist : firstTermDist[leavescount];

      assert(LE(nodeTo3TermsPaths[leaf].dist, firstTermDist[leavescount]));
      assert(LE(firstTermDist[leavescount], secondTermDist[leavescount]));

      //   printf("i %d \n", i);
 // printf("term=%d, first=%f second=%f def=%f \n", term, firstTermDist[leavescount], secondTermDist[leavescount], nodeTo3TermsPaths[leaf].dist);
      leavescount++;

#ifndef NDEBUG
      redcost_debug += nodeTo3TermsPaths[leaf].dist;
      assert(leavescount <= STP_EXTTREE_MAXNLEAVES_GUARD);
#endif
   }

   if( leavescount > 0 )
   {
      int first = 0;

      sortDescendingIntRealReal(nearestTerms, firstTermDist, secondTermDist, leavescount);

      for( int i = 1; i < leavescount; i++ )
      {
         assert(nearestTerms[i] >= 0 && firstTermDist[i] >= 0.0 && secondTermDist[i] >= 0.0);
         if( nearestTerms[i] != nearestTerms[i - 1] )
         {
            const int n = i - first;
            redcost_directed += getMinDistCombination(firstTermDist + first, secondTermDist + first, n);
            first = i;
         }
      }

      redcost_directed += getMinDistCombination(firstTermDist + first, secondTermDist + first, leavescount - first);
   }

 //  printf("redcost_directed=%f redcost_debug=%f  \n", redcost_directed, redcost_debug );

   assert(GE(redcost_directed, redcost_debug));

   return redcost_directed;
}


/** gets reduced cost of current tree rooted at leave 'root' */
static inline
SCIP_Real extTreeGetDirectedRedcost(
   const GRAPH*          graph,              /**< graph data structure */
   const EXTDATA*        extdata,            /**< extension data */
   int                   root                /**< the root for the orientation */
)
{
   const SCIP_Real* const tree_redcostSwap = extdata->tree_redcostSwap;

   assert(extdata->tree_nleaves > 1 && extdata->tree_nleaves < STP_EXTTREE_MAXNLEAVES_GUARD);
   assert(extdata->tree_leaves[0] == extdata->tree_root);
   assert(root >= 0 && root < graph->knots);

   /* are there any deleted arcs in the directed tree? */
   if( root == extdata->tree_root && extdata->tree_nDelUpArcs > 0 )
   {
      return FARAWAY;
   }

   /* is the rooting possible? */
   if( LT(tree_redcostSwap[root], FARAWAY) )
   {
      return extTreeGetDirectedRedcostProper(graph, extdata, root);
   }

   return FARAWAY;
}


/** gets reduced cost bound of current tree */
static
SCIP_Real extTreeGetRedcostBound(
   const GRAPH*          graph,              /**< graph data structure */
   const EXTDATA*        extdata             /**< extension data */
)
{
   const int* const tree_leaves = extdata->tree_leaves;
   const int nleaves = extdata->tree_nleaves;
   SCIP_Real tree_redcost;

   assert(graph && extdata);
   assert(nleaves > 1 && tree_leaves[0] == extdata->tree_root);

   tree_redcost = FARAWAY;

   // todo perhaps needs to be adapted for pseudo elimination tests...

   /* take each leaf as root of the tree */
   for( int i = 0; i < nleaves; i++ )
   {
      const int leaf = tree_leaves[i];
      const SCIP_Real tree_redcost_new = extTreeGetDirectedRedcost(graph, extdata, leaf);
      int todo;
      // break early here!

      tree_redcost = MIN(tree_redcost, tree_redcost_new);
   }

   return tree_redcost;
}

/** removes root of stack top component from tree */
static inline
void extTreeStackTopRootRemove(
   const GRAPH*          graph,              /**< graph data structure */
   EXTDATA*              extdata             /**< extension data */
)
{
   const int comproot = extStackGetTopRoot(graph, extdata);

   /* update tree leaves array todo might need to be changed for pseudo-elimination */
   if( comproot != extdata->tree_root )
   {
      extLeafRemove(comproot, extdata);
   }
   else
   {
      assert(extdata->tree_nleaves == 1);
      assert(extdata->tree_deg[comproot] == 1);

      extdata->tree_deg[comproot] = 0;
   }
}


/** adds top component of stack to tree */
static
void extTreeStackTopAdd(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   EXTDATA*              extdata,            /**< extension data */
   SCIP_Bool*            conflict            /**< conflict found? */
)
{
   const int* const extstack_data = extdata->extstack_data;
   const int* const extstack_start = extdata->extstack_start;
   int* const tree_edges = extdata->tree_edges;
   int* const tree_deg = extdata->tree_deg;
   int* const tree_parentNode = extdata->tree_parentNode;
   SCIP_Real* const tree_parentEdgeCost = extdata->tree_parentEdgeCost;
   REDDATA* const reddata = extdata->reddata;
   int* const pseudoancestor_mark = reddata->pseudoancestor_mark;
   const int stackpos = extStackGetPosition(extdata);
   const int comproot = extStackGetTopRoot(graph, extdata);
   int conflictIteration = -1;
   const SCIP_Bool noReversedRedCostTree = extRedcostReverseTreeRuledOut(graph, extdata);

   assert(!(*conflict));
   assert(stackpos >= 0);
   assert(extstack_start[stackpos + 1] - extstack_start[stackpos] > 0);
   assert(comproot >= 0 && comproot < graph->knots);
   assert(extdata->extstack_state[stackpos] == EXT_STATE_EXPANDED);

   extTreeStackTopRootRemove(graph, extdata);

   /* add top expanded component to tree data */
   for( int i = extstack_start[stackpos]; i < extstack_start[stackpos + 1]; i++ )
   {
      const int edge = extstack_data[i];
      const int head = graph->head[edge];

      assert(extdata->tree_nedges < extdata->extstack_maxsize);
      assert(edge >= 0 && edge < graph->edges);
      assert(tree_deg[head] == 0);
      assert(tree_deg[comproot] > 0 || comproot == extdata->tree_root);
      assert(comproot == graph->tail[edge]);

      extRedcostAddEdge(graph, edge, noReversedRedCostTree, reddata, extdata);
      extLeafAdd(head, extdata);

      tree_deg[head] = 1;
      tree_edges[(extdata->tree_nedges)++] = edge;
      tree_parentNode[head] = comproot;
      tree_parentEdgeCost[head] = graph->cost[edge];
      tree_deg[comproot]++;

      /* no conflict found yet? */
      if( conflictIteration == -1 )
      {
         assert(*conflict == FALSE);

         graph_pseudoAncestors_hashEdgeDirty(graph->pseudoancestors, edge, TRUE, conflict, pseudoancestor_mark);

         if( *conflict )
         {
            SCIPdebugMessage("pseudoancestor conflict for edge %d \n", edge);
            conflictIteration = i;
            assert(conflictIteration >= 0);
         }
      }
   }

   /* conflict found? */
   if( conflictIteration != -1 )
   {
      assert(*conflict && conflictIteration >= 0);

      for( int i = extstack_start[stackpos]; i < conflictIteration; i++ )
      {
         const int edge = extstack_data[i];
         graph_pseudoAncestors_unhashEdge(graph->pseudoancestors, edge, pseudoancestor_mark);
      }
   }

   extdata->tree_depth++;

   assert(!extreduce_treeIsFlawed(scip, graph, extdata));
}


/** removes top component of stack from tree */
static inline
void extTreeStackTopRemove(
   const GRAPH*          graph,              /**< graph data structure */
   SCIP_Bool             ancestor_conflict,  /**< with ancestor conflict? */
   EXTDATA*              extdata             /**< extension data */
)
{
   REDDATA* const reddata = extdata->reddata;
   int* const pseudoancestor_mark = reddata->pseudoancestor_mark;
   int* const extstack_data = extdata->extstack_data;
   int* const extstack_start = extdata->extstack_start;
   int* const tree_deg = extdata->tree_deg;
   const int stackpos = extStackGetPosition(extdata);
   const int stackstart = extstack_start[stackpos];
   const int comproot = extStackGetTopRoot(graph, extdata);
   const int compsize = extstack_start[stackpos + 1] - extstack_start[stackpos];

   assert(compsize > 0);
   assert(tree_deg[comproot] > 1);
   assert(extdata->extstack_state[stackpos] != EXT_STATE_NONE);

   /* remove top component */
   for( int i = stackstart; i < extstack_start[stackpos + 1]; i++ )
   {
      const int edge = extstack_data[i];
      const int head = graph->head[edge];

      assert(edge >= 0 && edge < graph->edges);
      assert(comproot == graph->tail[edge]);
      assert(tree_deg[head] == 1 && tree_deg[comproot] > 1);

      extRedcostRemoveEdge(edge, reddata, extdata);

      tree_deg[head] = 0;
      tree_deg[comproot]--;

      if( !ancestor_conflict ) /* in case of a conflict, edge is unhashed already */
         graph_pseudoAncestors_unhashEdge(graph->pseudoancestors, edge, pseudoancestor_mark);
   }

   assert(tree_deg[comproot] == 1);

  // for( int k = extdata->tree_nleaves - 1; k >= extdata->tree_nleaves - compsize; k-- ) printf("bt remove leaf %d \n", extdata->tree_leaves[k]);

   (extdata->tree_nedges) -= compsize;
   (extdata->tree_depth)--;

   /* finally, remove top component from leaves and MST storages and restore the component root */
   extLeafRemoveTop(compsize, comproot, extdata);
   extreduce_mstCompRemove(graph, extdata);

   assert(extdata->tree_nedges >= 0 && extdata->tree_depth >= 0);
}


/** can any extension via edge be ruled out by using simple test?? */
static inline
SCIP_Bool extTreeRuleOutEdgeSimple(
   const GRAPH*          graph,              /**< graph data structure */
   EXTDATA*              extdata,            /**< extension data */
   int                   edge                /**< edge to be tested */
)
{
   const REDDATA* const reddata = extdata->reddata;
   const int extvert = graph->head[edge];
   const int* const tree_deg = extdata->tree_deg;

   if( tree_deg[extvert] != 0 )
   {
#ifdef SCIP_DEBUG
      SCIPdebugMessage("simple rule-out (edge-to-tree)  ");
      graph_edge_printInfo(graph, edge);
#endif

      return TRUE;
   }

   if( graph_pseudoAncestors_edgeIsHashed(graph->pseudoancestors, edge, reddata->pseudoancestor_mark) )
   {
#ifdef SCIP_DEBUG

      SCIPdebugMessage("simple rule-out (ancestor)  ");
      graph_edge_printInfo(graph, edge);
#endif

      return TRUE;
   }

   return FALSE;
}


/** Can any extension via edge be ruled out?
 *  NOTE: This method also computes SDs from head of 'edge' to all leaves below the current component
 *        unless a rule-out is possible. */
static
SCIP_Bool extTreeRuleOutEdge(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   EXTDATA*              extdata,            /**< extension data */
   int                   edge                /**< edge to be tested */
)
{
   SCIP_Bool ruledOut = FALSE;

   assert(graph && extdata);
   assert(!extTreeRuleOutEdgeSimple(graph, extdata, edge));

   /*  add 'extvert' with edge weights corresponding to special distances from extvert to tree leaves
    *  (and check for early rule-out) */
   extreduce_mstLevelAddLeaf(scip, graph, edge, extdata, &ruledOut);

   // todo cant we just move the whole thing?
   return ruledOut;
}


#if 0
/** can any extension via edge except for only the edge itself be ruled out? */
static
SCIP_Bool extRuleOutEdgeCombinations(
   const GRAPH*          graph,              /**< graph data structure */
   const EXTDATA*        extdata,            /**< extension data */
   int                   extedge             /**< edge to be tested */
)
{
   REDDATA* const reddata = extdata->reddata;
   const SCIP_Real tree_redcost = extTreeGetRedcostBound(scip, graph, extdata);
   const SCIP_Real cutoff = reddata->cutoff;
   const int base = graph->tail[extedge];
   const int extvert = graph->head[extedge];
   const SCIP_Real* const redcost = reddata->redCosts;

   assert(extedge >= 0 && extedge < graph->edges);
   assert(extdata->tree_deg[base] == 1 && base != extdata->reddata->redCostRoot);

   // add to leaves?

#ifdef SCIP_DEBUG
        printf("edge combinations ruled out: ");
        graph_edge_printInfo(graph, e);
#endif

   if( extvert == extdata->reddata->redCostRoot )
   {
      assert(Is_term(graph->term[extvert]));

      tree_redcost = FARAWAY;
   }
   else
      tree_redcost += redcost[extedge] + nodeTo3TermsPaths[extvert].dist - nodeTo3TermsPaths[base].dist;

   if( reddata->edgedeleted != NULL && reddata->edgedeleted[flipedge(extedge)] )
      checkReverseTrees = FALSE;

}
#endif


/** Can current tree be peripherally ruled out by using reduced costs arguments? */
static inline
SCIP_Bool extTreeRuleOutPeriphRedcosts(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   EXTDATA*              extdata             /**< extension data */
)
{
   REDDATA* const reddata = extdata->reddata;
   const SCIP_Real tree_redcost = extTreeGetRedcostBound(graph, extdata);
   const SCIP_Real cutoff = reddata->cutoff;

   if( reddata->equality ? GE(tree_redcost, cutoff) : GT(tree_redcost, cutoff) )
   {
      SCIPdebugMessage("Rule-out periph (with red.cost=%f) \n", tree_redcost);
      return TRUE;
   }

   return FALSE;
}


/** Can current tree be peripherally ruled out by using MST based arguments? */
static inline
SCIP_Bool extTreeRuleOutPeriphMst(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   EXTDATA*              extdata             /**< extension data */
)
{
   const int* const extstack_data = extdata->extstack_data;
   const int* const extstack_start = extdata->extstack_start;
   const int stackpos = extStackGetPosition(extdata);
   const int stackstart = extstack_start[stackpos];
   const int stackend = extstack_start[stackpos + 1];
   SCIP_Bool ruledOut = FALSE;

   assert(EXT_STATE_EXPANDED == extdata->extstack_state[stackpos]);

   /* add nodes (with special distances) to MST
    * and compare with tree bottleneck distances for early rule-out */
   for( int i = stackstart; i != stackend; i++ )
   {
      const int edge2leaf = extstack_data[i];

      /* add vertex to MST graph and check for bottleneck shortcut */

      if( i == stackstart )
         extreduce_mstCompInit(scip, graph, edge2leaf, extdata, &ruledOut);
      else
         extreduce_mstCompAddLeaf(scip, graph, edge2leaf, extdata, &ruledOut);

      /* early rule-out? */
      if( ruledOut )
      {
         SCIPdebugMessage("Rule-out periph (via bottleneck) \n");
         return TRUE;
      }
   }

   /* todo now we have the MST! compute its cost */

#ifdef STP_DEBUG_EXT
   {
      // todo assert that the weights are the same! weight!
      const SCIP_Real mstweight = extreduce_treeGetSdMstWeight(scip, graph, extdata);
   //   printf("mstobj=%f \n", mstweight);
   }
#endif


#ifndef NDEBUG
   for( int i = extstack_start[stackpos]; i < extstack_start[stackpos + 1]; i++ )
   {
      assert( graph_edge_nPseudoAncestors(graph, extstack_data[i]) == 0
            || graph_pseudoAncestors_edgeIsHashed(graph->pseudoancestors, extstack_data[i], extdata->reddata->pseudoancestor_mark));
   }
#endif

   if( ruledOut )
   {
      SCIPdebugMessage("Rule-out periph (via MST) \n");
      return TRUE;
   }

   return FALSE;
}


/** Can current tree be peripherally ruled out?
 *  NOTE: If tree cannot be ruled-out, the current component will be put into the MST storage 'reddata->msts' */
static inline
SCIP_Bool extTreeRuleOutPeriph(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   EXTDATA*              extdata             /**< extension data */
)
{
   if( extTreeRuleOutPeriphRedcosts(scip, graph, extdata) )
      return TRUE;

   if( extTreeRuleOutPeriphMst(scip, graph, extdata) )
      return TRUE;

   return FALSE;
}


/** Stores extensions of tree from current (expanded and marked) stack top that cannot be ruled-out.
 *  Also computes SDs from leaves of any extension to the tree leaves and other extension leaves. */
static
void extTreeFindExtensions(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   EXTDATA*              extdata,            /**< extension data */
   int*                  extedgesstart,      /**< starts of extension edges for one components */
   int*                  extedges,           /**< extensions edges */
   int*                  nextensions,        /**< number of all extensions */
   int*                  nextendableleaves,  /**< number of leaves that can be extended */
   SCIP_Bool*            with_ruledout_leaf  /**< one leaf could already be ruled out? */
)
{
   const int* const extstack_data = extdata->extstack_data;
   const int* const extstack_start = extdata->extstack_start;
   const int stackpos = extStackGetPosition(extdata);
   const SCIP_Bool* const isterm = extdata->node_isterm;

#ifndef NDEBUG
   assert(graph && extdata && extedgesstart && extedges && nextensions && nextendableleaves && with_ruledout_leaf);
   assert(stackpos >= 0);
   assert(EXT_STATE_MARKED == extdata->extstack_state[stackpos]);
   assert(!(*with_ruledout_leaf));
   assert(*nextensions == 0 && *nextendableleaves == 0);

   extreduce_extendInitDebug(extedgesstart, extedges);
#endif

   extedgesstart[0] = 0;

   /* loop over all leaves of top component */
   for( int i = extstack_start[stackpos]; i < extstack_start[stackpos + 1]; i++ )
   {
      int nleafextensions = 0;
      const int leaf = graph->head[extstack_data[i]];

      assert(extstack_data[i] >= 0 && extstack_data[i] < graph->edges);

      /* extensions from leaf not possible? */
      if( !extLeafIsExtendable(graph, isterm, leaf) )
         continue;

      /* assemble feasible single edge extensions from leaf */
      for( int e = graph->outbeg[leaf]; e != EAT_LAST; e = graph->oeat[e] )
      {
         if( extTreeRuleOutEdgeSimple(graph, extdata, e) )
         {
            continue;
         }

         assert(*nextensions < STP_EXT_MAXGRAD * STP_EXT_MAXGRAD);
         extedges[(*nextensions)++] = e;
         nleafextensions++;
      }

      if( nleafextensions == 0 )
      {
         *with_ruledout_leaf = TRUE;
         return;
      }

      extedgesstart[++(*nextendableleaves)] = *nextensions;
   }

   /* todo find sds to neighbors and save them  */

   assert(*nextensions >= *nextendableleaves);
   assert(*nextendableleaves <= STP_EXT_MAXGRAD);
}

// todo: move into extreduce_util
/** recompute reduced costs */
static
void extTreeRecompRedCosts(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   EXTDATA*              extdata             /**< extension data */
)
{
#ifndef NDEBUG
   const int tree_nDelUpArcs = extdata->tree_nDelUpArcs;
#endif
   REDDATA* const reddata = extdata->reddata;
   const STP_Bool* const edgedeleted = reddata->edgedeleted;
   SCIP_Real treecost = 0.0;
   const SCIP_Real* const redcost = reddata->redCosts;
   const int* const tree_edges = extdata->tree_edges;
   const int tree_nedges = extdata->tree_nedges;

   extdata->tree_nDelUpArcs = 0;

   assert(!extreduce_treeIsFlawed(scip, graph, extdata));

   for( int i = 0; i < tree_nedges; i++ )
   {
      const int edge = tree_edges[i];
      const SCIP_Bool edgeIsDeleted = (edgedeleted && edgedeleted[edge]);

      assert(edge >= 0 && edge < graph->edges);

      if( !edgeIsDeleted )
      {
         treecost += redcost[edge];
         assert(LT(treecost, FARAWAY));
      }
      else
      {
         extdata->tree_nDelUpArcs++;
      }
   }

   assert(SCIPisEQ(scip, treecost, extdata->tree_redcost));
   assert(tree_nDelUpArcs == extdata->tree_nDelUpArcs);

   extdata->tree_redcost = treecost;
}


/** synchronize tree with the stack */
static inline
void extTreeSyncWithStack(
   SCIP*                 scip,               /**< SCIP */
   const GRAPH*          graph,              /**< graph data structure */
   EXTDATA*              extdata,            /**< extension data */
   int*                  nupdatestalls,      /**< update stalls counter */
   SCIP_Bool*            conflict            /**< conflict found? */
)
{
   const int stackposition = extStackGetPosition(extdata);

   assert(scip && graph && extdata && nupdatestalls && conflict);
   assert(!(*conflict));

#ifdef SCIP_DEBUG
   extreduce_printStack(graph, extdata);
#endif

   /* is current component expanded? */
   if( extdata->extstack_state[stackposition] == EXT_STATE_EXPANDED )
   {
      /* add top component to tree */
      extTreeStackTopAdd(scip, graph, extdata, conflict);
   }

   /* recompute reduced costs? */
   if( ++(*nupdatestalls) > EXT_REDCOST_NRECOMP )
   {
      extTreeRecompRedCosts(scip, graph, extdata);
      *nupdatestalls = 0;
   }

#ifndef NDEBUG
   if( !(*conflict) )
   {
      assert(extreduce_treeIsHashed(graph, extdata));
   }
#endif
}


/** should we truncate from current component? */
static
SCIP_Bool extTruncate(
   const GRAPH*          graph,              /**< graph data structure */
   const EXTDATA*        extdata             /**< extension data */
)
{
   const int* const extstack_data = extdata->extstack_data;
   const int* const extstack_start = extdata->extstack_start;
   const SCIP_Bool* const  isterm = extdata->node_isterm;
   const int stackpos = extStackGetPosition(extdata);

   assert(extdata->extstack_state[stackpos] == EXT_STATE_MARKED);
   assert(extstack_start[stackpos] < extdata->extstack_maxsize);

   if( extdata->tree_depth >= extdata->tree_maxdepth )
   {
      SCIPdebugMessage("truncate (depth too high) \n");
      return TRUE;
   }

   if( extdata->tree_nedges >= extdata->tree_maxnedges )
   {
      SCIPdebugMessage("truncate (too many tree edges) \n");
      return TRUE;
   }

   if( extdata->tree_nleaves >= extdata->tree_maxnleaves )
   {
      SCIPdebugMessage("truncate (too many leaves) \n");
      return TRUE;
   }

   if( extdata->extstack_ncomponents >= extdata->extstack_maxncomponents - 1 )
   {
      SCIPdebugMessage("truncate (too many stack components) \n");
      return TRUE;
   }

   /* check whether at least one leaf is extendable */
   for( int i = extstack_start[stackpos]; i < extstack_start[stackpos + 1]; i++ )
   {
      const int edge = extstack_data[i];
      const int leaf = graph->head[edge];

      assert(edge >= 0 && edge < graph->edges);
      assert(extdata->tree_deg[leaf] > 0);

      if( extLeafIsExtendable(graph, isterm, leaf) )
         return FALSE;
   }

   SCIPdebugMessage("truncate (non-promising) \n");
   return TRUE;
}


/** top component is rebuilt, and
 *  if success == TRUE: goes back to first marked component
 *  if success == FALSE: goes back to first marked or non-expanded component
 *   */
static
void extBacktrack(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   SCIP_Bool             success,            /**< backtrack from success? */
   SCIP_Bool             ancestor_conflict,  /**< backtrack triggered by ancestor conflict? */
   EXTDATA*              extdata             /**< extension data */
)
{
   int* const extstack_state = extdata->extstack_state;
   int stackpos = extStackGetPosition(extdata);
   const int extstate_top = extstack_state[stackpos];

   assert(graph && extdata);
   assert(extdata->extstack_start[stackpos + 1] - extdata->extstack_start[stackpos] > 0);

   /* top component already expanded (including marked)? */
   if( extstate_top != EXT_STATE_NONE )
   {
      extTreeStackTopRemove(graph, ancestor_conflict, extdata);
   }

   stackpos--;

   /* backtrack */
   if( success )
   {
      if( extstack_state[stackpos] != EXT_STATE_EXPANDED  )
      {
         /* the MST level associated top component cannot be used anymore, because the next component is not a sibling */
         extreduce_mstLevelRemove(extdata->reddata);
      }

      while( extstack_state[stackpos] == EXT_STATE_NONE )
      {
         stackpos--;
         assert(stackpos >= 0);
      }

      SCIPdebugMessage("backtrack SUCCESS \n");
      assert(extstack_state[stackpos] == EXT_STATE_EXPANDED || extstack_state[stackpos] == EXT_STATE_MARKED);
   }
   else
   {
      /* the MST level associated with top component cannot be used anymore, because siblings will be removed */
      extreduce_mstLevelRemove(extdata->reddata);

      while( extstack_state[stackpos] == EXT_STATE_EXPANDED )
      {
         stackpos--;
         assert(stackpos >= 0);
      }

      SCIPdebugMessage("backtrack FAILURE \n");
      assert(extstack_state[stackpos] == EXT_STATE_NONE || extstack_state[stackpos] == EXT_STATE_MARKED);
   }

   extdata->extstack_ncomponents = stackpos + 1;

   assert(extdata->extstack_ncomponents <= extdata->extstack_maxncomponents);
   assert(!extreduce_treeIsFlawed(scip, graph, extdata));
}


/** Builds components from top edges and adds them.
 *  Backtracks if stack is too full.
 *  Helper method for 'extStackTopExpand' */
static inline
void extStackAddCompsExpanded(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   int                   nextedges,          /**< number of edges for extension */
   const int*            extedges,           /**< array of edges for extension */
   EXTDATA*              extdata,            /**< extension data */
   SCIP_Bool*            success             /**< success pointer */
)
{
   int* const extstack_data = extdata->extstack_data;
   int* const extstack_start = extdata->extstack_start;
   int* const extstack_state = extdata->extstack_state;
   int stackpos = extStackGetPosition(extdata);
   int datasize = extstack_start[stackpos];
   const uint32_t powsize = (uint32_t) pow(2.0, nextedges);
   const int newstacksize_upper = (datasize + (int) powsize * (nextedges + 1) / 2);
   const int newncomponents_upper = extdata->extstack_ncomponents + powsize;

   assert(nextedges > 0 && nextedges < 32);
   assert(powsize >= 2);

   /* stack too full? */
   if( newstacksize_upper > extdata->extstack_maxsize || newncomponents_upper >= extdata->extstack_maxncomponents )
   {
	   SCIPdebugMessage("stack too full, cannot expand \n");

	  // assert(extstack_state[stackpos] != EXT_STATE_MARKED );
	   assert(extstack_state[stackpos] == EXT_STATE_NONE );

      *success = FALSE;
      extBacktrack(scip, graph, *success, FALSE, extdata);

      return;
   }

   /* compute and add components (overwrite previous, non-expanded component) */
   // todo we probably want to order so that the smallest components are put last!
   for( uint32_t counter = 1; counter < powsize; counter++ )
   {
      for( unsigned int j = 0; j < (unsigned int) nextedges; j++ )
      {
         /* Check if jth bit in counter is set */
         if( counter & ((unsigned int) 1 << j) )
         {
            assert(datasize < extdata->extstack_maxsize);
            assert(extedges[j] >= 0);

            extstack_data[datasize++] = extedges[j];
            SCIPdebugMessage(" head %d \n", graph->head[extedges[j]]);
         }
      }

      SCIPdebugMessage("... added \n");
      assert(stackpos < extdata->extstack_maxsize - 1);

      extstack_state[stackpos] = EXT_STATE_EXPANDED;
      extstack_start[++stackpos] = datasize;

      assert(extstack_start[stackpos] - extstack_start[stackpos - 1] > 0);
   }

   assert(stackpos > extStackGetPosition(extdata));
   assert(stackpos >= extdata->extstack_ncomponents);

   extdata->extstack_ncomponents = stackpos;

   assert(extdata->extstack_ncomponents <= extdata->extstack_maxncomponents);
}


/** Collects edges top component of stack that we need to consider for extension
 *  (i.e. which cannot be ruled out).
 *  Helper method for 'extStackTopExpand' */
static inline
void extStackTopCollectExtEdges(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   EXTDATA*              extdata,            /**< extension data */
   int*                  extedges,           /**< array of collected edges */
   int*                  nextedges           /**< number of edges */
)
{
   const int* const extstack_data = extdata->extstack_data;
   const int* const extstack_start = extdata->extstack_start;
   const int stackpos = extStackGetPosition(extdata);

   assert(*nextedges == 0);
   assert(extstack_start[stackpos + 1] - extstack_start[stackpos] >= 1);

   /* collect edges for components (and try to rule each of them out) */
   for( int i = extstack_start[stackpos]; i < extstack_start[stackpos + 1]; i++ )
   {
      const int edge = extstack_data[i];

      assert(*nextedges < STP_EXT_MAXGRAD);
      assert(edge >= 0 && edge < graph->edges);
      assert(extdata->tree_deg[graph->head[edge]] == 0);

      /* NOTE: as a side-effect this method computes the SDs from 'leaf' to all tree leaves in 'sds_vertical',
       * unless the edge is ruled out
       * todo remove this intermediary function!
       * */
      if( extTreeRuleOutEdge(scip, graph, extdata, edge) )
      {
         continue;
      }

#if 0
     if( extRuleOutEdgeCombinations(graph, extdata, e) )
     {
        // todo: need some marker here to say that we have a single edge!
        continue;
     }
#endif

      extedges[(*nextedges)++] = edge;
   }
}


/** Expands top component of stack.
 *  I.e.. adds all possible subsets of the top component that cannot be ruled-out.
 *  Note: This method can backtrack:
 *        1. If stack is full (with success set to FALSE),
 *        2. If all edges of the component can be ruled-out (with success set to TRUE).
 *  Note: This method computes SDs for newly added leaves! */
static
void extStackTopExpand(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   EXTDATA*              extdata,            /**< extension data */
   SCIP_Bool*            success             /**< success pointer */
)
{
   int extedges[STP_EXT_MAXGRAD];
   REDDATA* const reddata = extdata->reddata;
   int nextedges = 0;
   const int stackpos = extStackGetPosition(extdata);
#ifndef NDEBUG
   const int* const extstack_state = extdata->extstack_state;

   for( int i = 0; i < STP_EXT_MAXGRAD; i++ )
      extedges[i] = -1;
#endif

   assert(scip && graph && success);
   assert(EXT_STATE_NONE == extstack_state[stackpos]);

   extreduce_mstLevelInit(reddata, extdata);

   /* Note: Also computes SDs for leaves that are not ruled-out! */
   extStackTopCollectExtEdges(scip, graph, extdata, extedges, &nextedges);

   extreduce_mstLevelClose(reddata);

   /* everything ruled out already? */
   if( nextedges == 0 )
   {
      *success = TRUE;

      assert(extstack_state[stackpos] == EXT_STATE_NONE);

      /* not the initial component? */
      if( stackpos != 0 )
      {
         extBacktrack(scip, graph, *success, FALSE, extdata);
      }
      else
      {
         extreduce_mstLevelRemove(reddata);
      }
   }
   else
   {
      /* use the just collected edges 'extedges' to build components and add them to the stack */
      extStackAddCompsExpanded(scip, graph, nextedges, extedges, extdata, success);

#ifndef NDEBUG
      {
         const int stackpos_new = extStackGetPosition(extdata);
         assert(extstack_state[stackpos_new] == EXT_STATE_EXPANDED || (stackpos_new < stackpos) );
      }
#endif
   }
}


/** Extends top component of stack.
 *  Backtracks if stack is full.
 *  Will not add anything in case of rule-out of at least one extension node. */
static
void extExtend(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   EXTDATA*              extdata,            /**< extension data */
   SCIP_Bool*            success             /**< success pointer, FALSE iff no extension was possible */
)
{
   int* extedges = NULL;
   int* extedgesstart = NULL;
   int* const extstack_start = extdata->extstack_start;
   const int stackpos = extStackGetPosition(extdata);
   int nextendable_leaves = 0;
   int nextensions = 0;
   SCIP_Bool has_ruledout_leaf = FALSE;

   assert(stackpos >= 0);
   assert(EXT_STATE_MARKED == extdata->extstack_state[stackpos]);
   assert(extstack_start[stackpos + 1] - extstack_start[stackpos] <= STP_EXT_MAXGRAD);

   SCIP_CALL_ABORT( SCIPallocBufferArray(scip, &extedges, STP_EXT_MAXGRAD * STP_EXT_MAXGRAD) );
   SCIP_CALL_ABORT( SCIPallocBufferArray(scip, &extedgesstart, STP_EXT_MAXGRAD + 1) );

   extTreeFindExtensions(scip, graph, extdata, extedgesstart, extedges, &nextensions,
         &nextendable_leaves, &has_ruledout_leaf);

   if( has_ruledout_leaf )
   {
      *success = TRUE;

      SCIPdebugMessage("ruled-out one leaf \n");
   }
   else if( nextendable_leaves == 0 )  /* found no valid extensions? */
   {
      *success = FALSE;

      assert(nextensions == 0);
      SCIPdebugMessage("no valid extensions found \n");
   }
   else  /* found non-empty valid extensions */
   {
      const int stacksize_new = extstack_start[stackpos + 1] + (extedgesstart[nextendable_leaves] - extedgesstart[0]);
      assert(nextendable_leaves > 0 && nextensions > 0);

      /* stack too small? */
      if( stacksize_new > extdata->extstack_maxsize )
      {
         *success = FALSE;
         assert(EXT_STATE_MARKED == extdata->extstack_state[extStackGetPosition(extdata)]);

         SCIPdebugMessage("stack is full! need to backtrack \n");

         extBacktrack(scip, graph, *success, FALSE, extdata);
      }
      else
      {
         *success = TRUE;

         extStackAddCompsNonExpanded(graph, extedgesstart, extedges, nextendable_leaves, extdata);

         /* try to expand last (and smallest!) component, which currently is just a set of edges */
         extStackTopExpand(scip, graph, extdata, success);
      }
   }

   SCIPfreeBufferArray(scip, &extedgesstart);
   SCIPfreeBufferArray(scip, &extedges);
}


/** Adds extensions initial component to stack (needs to be star component rooted in root).
  * If no extensions are added, then the component has been ruled-out. */
static
void extProcessInitialComponent(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   const int*            compedges,          /**< component edges */
   int                   ncompedges,         /**< number of component edges */
   int                   root,               /**< root of the component */
   EXTDATA*              extdata,            /**< extension data */
   SCIP_Bool*            ruledOut            /**< initial component ruled out? */
)
{
   SCIP_Bool success;
   SCIP_Bool conflict;

   assert(compedges);
   assert(ncompedges >= 1 && ncompedges < STP_EXT_MAXGRAD);
   assert(ncompedges < extdata->extstack_maxsize);
   assert(root >= 0 && root < graph->knots);
   assert(!(*ruledOut));

#ifdef SCIP_DEBUG
   printf("\n --- ADD initial component --- \n\n");
#endif

   // todo method needs to be adapted for pseudo-elimination!
   // need extra expand method that only expands edge sets S with |S| >= 3
   // maybe add a dummy first entry to stack...if that is reached the elimination is successful
   assert(ncompedges == 1);

   for( int i = 0; i < ncompedges; i++ )
   {
      const int e = compedges[i];
      const int tail = graph->tail[e];

      assert(e >= 0 && e < graph->edges);
      assert(tail == root);

      SCIPdebugMessage("edge %d: %d->%d \n", e, graph->tail[e], graph->head[e]);

      extdata->extstack_data[i] = e;
      extLeafAdd(tail, extdata);
      extreduce_mstAddRoot(scip, tail, extdata->reddata);
   }

   extdata->tree_root = root;
   extdata->extstack_ncomponents = 1;
   extdata->extstack_state[0] = EXT_STATE_NONE;
   extdata->extstack_start[0] = 0;
   extdata->extstack_start[1] = ncompedges;
   extdata->tree_parentNode[root] = -1;
   extdata->tree_redcostSwap[root] = 0.0;
   extdata->tree_parentEdgeCost[root] = -1.0;

   assert(ncompedges > 1 || extdata->tree_leaves[0] == root);
   assert(extdata->tree_deg[root] == 0);
   assert(extdata->tree_nleaves == ncompedges);

   extdata->tree_deg[root] = 1;

   /* expand the single edge */
   success = TRUE;
   extStackTopExpand(scip, graph, extdata, &success);

   assert(success);
   assert(extStackGetPosition(extdata) == 0);

   /* early rule-out? */
   if( extdata->extstack_state[0] == EXT_STATE_NONE )
   {
      *ruledOut = TRUE;

      /* necessary because this edge will be deleted in clean-up otherwise */
      graph_pseudoAncestors_hashEdge(graph->pseudoancestors, compedges[0], extdata->reddata->pseudoancestor_mark);

      return;
   }

   assert(extdata->extstack_state[0] == EXT_STATE_EXPANDED);

   conflict = FALSE;
   extTreeStackTopAdd(scip, graph, extdata, &conflict);

   assert(!conflict);

   /* NOTE: necessary to keep the MST graph up-to-date */
   if( extTreeRuleOutPeriph(scip, graph, extdata) )
   {
      *ruledOut = TRUE;
      return;
   }

   /* the single edge component could not be ruled-out, so set its stage to 'marked' */
   extdata->extstack_state[extStackGetPosition(extdata)] = EXT_STATE_MARKED;

   extExtend(scip, graph, extdata, &success);

   assert(success);
}


/** cleans-up after trying to rule out an arc */
static
void extCheckArcPostClean(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   int                   edge,               /**< directed edge to be checked */
   EXTDATA*              extdata             /**< extension data */
)
{
   MLDISTS* const sds_vertical = extdata->reddata->sds_vertical;
   int* const tree_deg = extdata->tree_deg;
   const int head = graph->head[edge];
   const int tail = graph->tail[edge];
   const int vert_nlevels = extreduce_mldistsNlevels(sds_vertical);

   tree_deg[head] = 0;
   tree_deg[tail] = 0;

   assert(vert_nlevels == 1 || vert_nlevels == 0);

   if( vert_nlevels == 1 )
      extreduce_mldistsLevelRemoveTop(sds_vertical);

   graph_pseudoAncestors_unhashEdge(graph->pseudoancestors, edge, extdata->reddata->pseudoancestor_mark);
   extreduce_extdataClean(extdata);
   extreduce_reddataClean(extdata->reddata);

   assert(extreduce_reddataIsClean(graph, extdata->reddata) && extreduce_extdataIsClean(graph, extdata));
}


/** Check whether edge can be deleted.
 *  Only extends from the 'head' of the edge! */
static
void extCheckArcFromHead(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   int                   edge,               /**< directed edge to be checked */
   EXTDATA*              extdata,            /**< extension data */
   SCIP_Bool*            deletable           /**< is arc deletable? */
)
{
   const int tail = graph->tail[edge];
   int* const extstack_state = extdata->extstack_state;
   int nupdatestalls = 0;
   SCIP_Bool success = TRUE;
   SCIP_Bool conflict = FALSE;

   assert(extreduce_extdataIsClean(graph, extdata));
   assert(extreduce_reddataIsClean(graph, extdata->reddata));
   assert(!(*deletable));

   /* put 'edge' on the stack */
   extProcessInitialComponent(scip, graph, &edge, 1, tail, extdata, deletable);

   /* early rule-out? */
   if( *deletable )
   {
      extCheckArcPostClean(scip, graph, edge, extdata);
      return;
   }

   assert(extstack_state[0] == EXT_STATE_MARKED);

   // todo put this loop in some 'extCheckComponent' and add extProcessInitialEdge, extProcessInitialPseudoNode
   /* limited DFS backtracking; stops once back at 'edge' */
   while( extdata->extstack_ncomponents > 1 )
   {
      const int stackposition = extStackGetPosition(extdata);
      conflict = FALSE;

      extTreeSyncWithStack(scip, graph, extdata, &nupdatestalls, &conflict);

      /* has current component already been extended? */
      if( extstack_state[stackposition] == EXT_STATE_MARKED )
      {
         extBacktrack(scip, graph, success, FALSE, extdata);
         continue;
      }

      /* component not expanded yet? */
      if( extstack_state[stackposition] != EXT_STATE_EXPANDED )
      {
         assert(extstack_state[stackposition] == EXT_STATE_NONE);

         extStackTopExpand(scip, graph, extdata, &success);
         continue;
      }

      assert(extstack_state[stackposition] == EXT_STATE_EXPANDED);

      if( conflict || extTreeRuleOutPeriph(scip, graph, extdata) )
      {
         success = TRUE;
         extBacktrack(scip, graph, success, conflict, extdata);
         continue;
      }

      /* the component could not be ruled-out, so set its stage to 'marked' */
      extstack_state[stackposition] = EXT_STATE_MARKED;

      if( extTruncate(graph, extdata) )
      {
         success = FALSE;
         extBacktrack(scip, graph, success, FALSE, extdata);
         continue;
      }

      /* neither ruled out nor truncated, so extend */
      extExtend(scip, graph, extdata, &success);

   } /* DFS loop */

   *deletable = success;

   assert(extdata->tree_deg[graph->tail[edge]] == 1);
   assert(extdata->tree_deg[graph->head[edge]] == 1);
   assert(extdata->tree_nedges == 1);

   extCheckArcPostClean(scip, graph, edge, extdata);
}


/** check (directed) arc */
SCIP_RETCODE extreduce_checkArc(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   const REDCOST*        redcostdata,        /**< reduced cost data structures */
   int                   edge,               /**< edge to be checked */
   SCIP_Bool             equality,           /**< delete edge also in case of reduced cost equality? */
   DISTDATA*             distdata,           /**< data for distance computations */
   EXTPERMA*             extpermanent,       /**< extension data */
   SCIP_Bool*            edgeIsDeletable     /**< is edge deletable? */
)
{
   const SCIP_Bool* isterm = extpermanent->isterm;
   const int root = redcostdata->redCostRoot;
   const SCIP_Real* redcost = redcostdata->redEdgeCost;
   const SCIP_Real* rootdist = redcostdata->rootToNodeDist;
   const PATH* nodeToTermpaths = redcostdata->nodeTo3TermsPaths;
   const SCIP_Real cutoff = redcostdata->cutoff;
   const int head = graph->head[edge];
   const int tail = graph->tail[edge];
   const SCIP_Real edgebound = redcost[edge] + rootdist[tail] + nodeToTermpaths[head].dist;
   SCIP_Bool restoreAntiArcDeleted = FALSE;
   STP_Bool* const edgedeleted = extpermanent->edgedeleted;

   assert(scip && graph && redcost && rootdist && nodeToTermpaths && distdata);
   assert(edge >= 0 && edge < graph->edges);
   assert(!graph_pc_isPcMw(graph) || !graph->extended);
   assert(graph->mark[tail] && graph->mark[head]);
   assert(graph_isMarked(graph));
   assert(extreduce_extPermaIsClean(graph, extpermanent));

   /* trivial rule-out? */
   if( SCIPisGT(scip, edgebound, cutoff) || (equality && SCIPisEQ(scip, edgebound, cutoff)) || head == root )
   {
      *edgeIsDeletable = TRUE;
      return SCIP_OKAY;
   }

   if( edgedeleted && !edgedeleted[flipedge(edge)] )
   {
      edgedeleted[flipedge(edge)] = TRUE;
      restoreAntiArcDeleted = TRUE;
   }

   *edgeIsDeletable = FALSE;

   /* can we extend from 'edge'? */
   if( extLeafIsExtendable(graph, isterm, head) )
   {
      int* extstack_data;
      int* extstack_start;
      int* extstack_state;
      int* tree_edges;
      int* tree_leaves;
      int* tree_parentNode;
      int* pcSdCands = NULL;
      SCIP_Real* tree_parentEdgeCost;
      SCIP_Real* tree_redcostSwap;
      int* pseudoancestor_mark;
      const int nnodes = graph->knots;
      const int maxstacksize = extreduce_getMaxStackSize();
      const int maxncomponents = extreduce_getMaxStackNcomponents(graph);

      SCIP_CALL( SCIPallocBufferArray(scip, &extstack_data, maxstacksize) );
      SCIP_CALL( SCIPallocBufferArray(scip, &extstack_start, maxncomponents + 1) );
      SCIP_CALL( SCIPallocBufferArray(scip, &extstack_state, maxncomponents + 1) );
      SCIP_CALL( SCIPallocBufferArray(scip, &tree_edges, nnodes) );
      SCIP_CALL( SCIPallocBufferArray(scip, &tree_leaves, nnodes) );
      SCIP_CALL( SCIPallocBufferArray(scip, &tree_parentNode, nnodes) );
      SCIP_CALL( SCIPallocBufferArray(scip, &tree_parentEdgeCost, nnodes) );
      SCIP_CALL( SCIPallocBufferArray(scip, &tree_redcostSwap, nnodes) );
      if( graph_pc_isPc(graph) )
         SCIP_CALL( SCIPallocBufferArray(scip, &pcSdCands, nnodes) );

      SCIP_CALL( SCIPallocCleanBufferArray(scip, &pseudoancestor_mark, nnodes) );

      // todo: mode the initialization to extra method! Avoids also code dublication
      {
         REDDATA reddata = { .dcmst = extpermanent->dcmst, .msts = extpermanent->msts,
            .msts_reduced = extpermanent->msts_reduced,
            .sds_horizontal = extpermanent->sds_horizontal, .sds_vertical = extpermanent->sds_vertical,
            .redCosts = redcost, .rootToNodeDist = rootdist, .nodeTo3TermsPaths = nodeToTermpaths,
            .nodeTo3TermsBases = redcostdata->nodeTo3TermsBases, .edgedeleted = edgedeleted,
            .pseudoancestor_mark = pseudoancestor_mark, .cutoff = cutoff, .equality = equality, .redCostRoot = root };
         EXTDATA extdata = { .extstack_data = extstack_data, .extstack_start = extstack_start,
            .extstack_state = extstack_state, .extstack_ncomponents = 0, .tree_leaves = tree_leaves,
            .tree_edges = tree_edges, .tree_deg = extpermanent->tree_deg, .tree_nleaves = 0,
            .tree_bottleneckDistNode = extpermanent->bottleneckDistNode, .tree_parentNode = tree_parentNode,
            .tree_parentEdgeCost = tree_parentEdgeCost, .tree_redcostSwap = tree_redcostSwap, .tree_redcost = 0.0,
            .tree_nDelUpArcs = 0, .tree_root = -1, .tree_nedges = 0, .tree_depth = 0,
			.extstack_maxsize = maxstacksize, .extstack_maxncomponents = maxncomponents,
            .pcSdToNode = extpermanent->pcSdToNode, .pcSdCands = pcSdCands, .nPcSdCands = -1,
			.tree_maxdepth = extreduce_getMaxTreeDepth(graph),
            .tree_maxnleaves = STP_EXTTREE_MAXNLEAVES,
            .tree_maxnedges = STP_EXTTREE_MAXNEDGES, .node_isterm = isterm, .reddata = &reddata, .distdata = distdata };

         extCheckArcFromHead(scip, graph, edge, &extdata, edgeIsDeletable);
      }

      assert(extreduce_extPermaIsClean(graph, extpermanent));

      SCIPfreeCleanBufferArray(scip, &pseudoancestor_mark);
      SCIPfreeBufferArrayNull(scip, &pcSdCands);
      SCIPfreeBufferArray(scip, &tree_redcostSwap);
      SCIPfreeBufferArray(scip, &tree_parentEdgeCost);
      SCIPfreeBufferArray(scip, &tree_parentNode);
      SCIPfreeBufferArray(scip, &tree_leaves);
      SCIPfreeBufferArray(scip, &tree_edges);
      SCIPfreeBufferArray(scip, &extstack_state);
      SCIPfreeBufferArray(scip, &extstack_start);
      SCIPfreeBufferArray(scip, &extstack_data);
   }

   if( restoreAntiArcDeleted )
   {
      assert(edgedeleted);
      edgedeleted[flipedge(edge)] = FALSE;
   }

   return SCIP_OKAY;
}


/** check edge */
SCIP_RETCODE extreduce_checkEdge(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   const REDCOST*        redcostdata,        /**< reduced cost data structures */
   int                   edge,               /**< edge to be checked */
   SCIP_Bool             equality,           /**< delete edge also in case of reduced cost equality? */
   DISTDATA*             distdata,           /**< data for distance computations */
   EXTPERMA*             extpermanent,       /**< extension data */
   SCIP_Bool*            edgeIsDeletable     /**< is edge deletable? */
)
{
   const SCIP_Bool* isterm = extpermanent->isterm;
   const int root = redcostdata->redCostRoot;
   const SCIP_Real* redcost = redcostdata->redEdgeCost;
   const SCIP_Real* rootdist = redcostdata->rootToNodeDist;
   const PATH* nodeToTermpaths = redcostdata->nodeTo3TermsPaths;
   const SCIP_Real cutoff = redcostdata->cutoff;
   const int head = graph->head[edge];
   const int tail = graph->tail[edge];

   assert(scip && graph && redcost && rootdist && nodeToTermpaths && edgeIsDeletable && distdata && extpermanent);
   assert(edge >= 0 && edge < graph->edges);
   assert(!graph_pc_isPcMw(graph) || !graph->extended);
   assert(graph->mark[tail] && graph->mark[head]);
   assert(graph_isMarked(graph));
   assert(extreduce_extPermaIsClean(graph, extpermanent));

   *edgeIsDeletable = FALSE;

   /* can we extend from 'edge'? */
   if( extLeafIsExtendable(graph, isterm, tail) || extLeafIsExtendable(graph, isterm, head) )
   {
      int* extstack_data;
      int* extstack_start;
      int* extstack_state;
      int* tree_edges;
      int* tree_leaves;
      int* tree_parentNode;
      int* pcSdCands = NULL;
      SCIP_Real* tree_parentEdgeCost;
      SCIP_Real* tree_redcostSwap;
      int* pseudoancestor_mark;
      const int nnodes = graph->knots;
      const int maxstacksize = extreduce_getMaxStackSize();
      const int maxncomponents = extreduce_getMaxStackNcomponents(graph);

      SCIP_CALL( SCIPallocBufferArray(scip, &extstack_data, maxstacksize) );
      SCIP_CALL( SCIPallocBufferArray(scip, &extstack_start, maxncomponents + 1) );
      SCIP_CALL( SCIPallocBufferArray(scip, &extstack_state, maxncomponents + 1) );
      SCIP_CALL( SCIPallocBufferArray(scip, &tree_edges, nnodes) );
      SCIP_CALL( SCIPallocBufferArray(scip, &tree_leaves, nnodes) );
      SCIP_CALL( SCIPallocBufferArray(scip, &tree_parentNode, nnodes) );
      SCIP_CALL( SCIPallocBufferArray(scip, &tree_parentEdgeCost, nnodes) );
      SCIP_CALL( SCIPallocBufferArray(scip, &tree_redcostSwap, nnodes) );
      if( graph_pc_isPc(graph) )
         SCIP_CALL( SCIPallocBufferArray(scip, &pcSdCands, nnodes) );

      SCIP_CALL( SCIPallocCleanBufferArray(scip, &pseudoancestor_mark, nnodes) );

      {
         REDDATA reddata = { .dcmst = extpermanent->dcmst, .msts = extpermanent->msts,
            .msts_reduced = extpermanent->msts_reduced,
            .sds_horizontal = extpermanent->sds_horizontal, .sds_vertical = extpermanent->sds_vertical,
            .redCosts = redcost, .rootToNodeDist = rootdist, .nodeTo3TermsPaths = nodeToTermpaths,
            .nodeTo3TermsBases = redcostdata->nodeTo3TermsBases, .edgedeleted = extpermanent->edgedeleted,
            .pseudoancestor_mark = pseudoancestor_mark, .cutoff = cutoff, .equality = equality, .redCostRoot = root };
         EXTDATA extdata = { .extstack_data = extstack_data, .extstack_start = extstack_start,
            .extstack_state = extstack_state, .extstack_ncomponents = 0, .tree_leaves = tree_leaves,
            .tree_edges = tree_edges, .tree_deg = extpermanent->tree_deg, .tree_nleaves = 0,
            .tree_bottleneckDistNode = extpermanent->bottleneckDistNode, .tree_parentNode = tree_parentNode,
            .tree_parentEdgeCost = tree_parentEdgeCost, .tree_redcostSwap = tree_redcostSwap, .tree_redcost = 0.0,
            .tree_nDelUpArcs = 0, .tree_root = -1, .tree_nedges = 0, .tree_depth = 0,
			.extstack_maxsize = maxstacksize, .extstack_maxncomponents = maxncomponents,
            .pcSdToNode = extpermanent->pcSdToNode, .pcSdCands = pcSdCands, .nPcSdCands = -1,
			.tree_maxdepth = extreduce_getMaxTreeDepth(graph),
			.tree_maxnleaves = STP_EXTTREE_MAXNLEAVES,
            .tree_maxnedges = STP_EXTTREE_MAXNEDGES, .node_isterm = isterm, .reddata = &reddata, .distdata = distdata };

         /* can we extend from head? */
         if( extLeafIsExtendable(graph, isterm, head) )
            extCheckArcFromHead(scip, graph, edge, &extdata, edgeIsDeletable);

         /* try to extend from tail? */
         if( !(*edgeIsDeletable) && extLeafIsExtendable(graph, isterm, tail) )
            extCheckArcFromHead(scip, graph, flipedge(edge), &extdata, edgeIsDeletable);
      }

      assert(extreduce_extPermaIsClean(graph, extpermanent));

      SCIPfreeCleanBufferArray(scip, &pseudoancestor_mark);
      SCIPfreeBufferArrayNull(scip, &pcSdCands);
      SCIPfreeBufferArray(scip, &tree_redcostSwap);
      SCIPfreeBufferArray(scip, &tree_parentEdgeCost);
      SCIPfreeBufferArray(scip, &tree_parentNode);
      SCIPfreeBufferArray(scip, &tree_leaves);
      SCIPfreeBufferArray(scip, &tree_edges);
      SCIPfreeBufferArray(scip, &extstack_state);
      SCIPfreeBufferArray(scip, &extstack_start);
      SCIPfreeBufferArray(scip, &extstack_data);
   }

   return SCIP_OKAY;
}


/** check node for possible  */
SCIP_RETCODE extreduce_checkNode(
   SCIP*                 scip,               /**< SCIP data structure */
   const GRAPH*          graph,              /**< graph data structure */
   const REDCOST*        redcostdata,        /**< reduced cost data structures */
   int                   node,               /**< node to be checked */
   SCIP_Bool             equality,           /**< delete edge also in case of reduced cost equality? */
   DISTDATA*             distdata,           /**< data for distance computations */
   EXTPERMA*             extpermanent,       /**< extension data */
   SCIP_Bool*            isPseudoDeletable   /**< is node pseudo-deletable? */
)
{
   // todo: fill!

   // todo: this function (or subfunction) should put one MST after the other on the stack, in order to be able to find
   // pseudo-deletable edges!

   return SCIP_OKAY;
}
