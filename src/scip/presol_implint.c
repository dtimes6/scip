/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*  Copyright (c) 2002-2024 Zuse Institute Berlin (ZIB)                      */
/*                                                                           */
/*  Licensed under the Apache License, Version 2.0 (the "License");          */
/*  you may not use this file except in compliance with the License.         */
/*  You may obtain a copy of the License at                                  */
/*                                                                           */
/*      http://www.apache.org/licenses/LICENSE-2.0                           */
/*                                                                           */
/*  Unless required by applicable law or agreed to in writing, software      */
/*  distributed under the License is distributed on an "AS IS" BASIS,        */
/*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. */
/*  See the License for the specific language governing permissions and      */
/*  limitations under the License.                                           */
/*                                                                           */
/*  You should have received a copy of the Apache-2.0 license                */
/*  along with SCIP; see the file LICENSE. If not visit scipopt.org.         */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/**@file   presol_implint.c
 * @ingroup DEFPLUGINS_PRESOL
 * @brief  Presolver that detects implicit integer variables
 * @author Rolf van der Hulst
 */

/* TODO: explore integer to implicit integer conversion */
/* TODO: fix to work in MINLP context */
/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#include <assert.h>

#include "scip/presol_implint.h"


#include "scip/pub_matrix.h"
#include "scip/pub_message.h"
#include "scip/pub_misc.h"
#include "scip/pub_network.h"
#include "scip/pub_presol.h"
#include "scip/pub_var.h"
#include "scip/scip_general.h"
#include "scip/scip_message.h"
#include "scip/scip_mem.h"
#include "scip/scip_nlp.h"
#include "scip/scip_numerics.h"
#include "scip/scip_param.h"
#include "scip/scip_presol.h"
#include "scip/scip_pricer.h"
#include "scip/scip_prob.h"
#include "scip/scip_probing.h"
#include "scip/scip_timing.h"
#include "scip/scip_var.h"

#define PRESOL_NAME            "implint"
#define PRESOL_DESC            "detects implicit integer variables"
#define PRESOL_PRIORITY         100 /**< priority of the presolver (>= 0: before, < 0: after constraint handlers); combined with propagators */
#define PRESOL_MAXROUNDS        -1 /**< maximal number of presolving rounds the presolver participates in (-1: no limit) */
#define PRESOL_TIMING           SCIP_PRESOLTIMING_EXHAUSTIVE /* timing of the presolver (fast, medium, or exhaustive) */

#define DEFAULT_CONVERTINTEGERS FALSE
#define DEFAULT_COLUMNROWRATIO  50.0

/** presolver data */
struct SCIP_PresolData
{
   SCIP_Bool convertintegers; /**< Should we detect implied integrality of columns that are integer? */
   double columnrowratio; /**< Use the network row addition algorithm when the column to row ratio becomes larger than
                            *  this threshold. Otherwise, use the column addition algorithm. */
};


/**
 * Struct that contains information about the blocks/components of the submatrix given by the continuous columns
 */
typedef struct{
   int nmatrixrows;                          /**< Number of rows in the matrix for the linear part of the problem */
   int nmatrixcols;                          /**< Number of columns in the matrix for the linear part of the problem */

   SCIP_VARTYPE * coltype;                   /**< SCIP_VARTYPE of the associated column */

   int* rowcomponent;                        /**< Maps a row to the index of the component it belongs to */
   int* colcomponent;                        /**< Maps a column to the index of the component it belongs to */

   int* componentrows;                       /**< Flattened array of array of rows that are in a given component. */
   int* componentcols;                       /**< Flattened array of array of columns that are in a given component. */
   int* componentrowstart;                   /**< The index of componentrows where the given component starts. */
   int* componentcolstart;                   /**< The index of componentcols where the given component starts. */
   int* ncomponentrows;                      /**< The number of rows in the given component. */
   int* ncomponentcols;                      /**< The number of columns in the given component */

   int ncomponents;
} MATRIX_COMPONENTS;

static
SCIP_RETCODE createMatrixComponents(
   SCIP* scip,
   SCIP_MATRIX* matrix,
   MATRIX_COMPONENTS** pmatrixcomponents
)
{
   SCIP_CALL( SCIPallocBlockMemory(scip, pmatrixcomponents) );
   MATRIX_COMPONENTS * comp = *pmatrixcomponents;

   int nrows = SCIPmatrixGetNRows(matrix);
   int ncols = SCIPmatrixGetNColumns(matrix);

   comp->nmatrixrows = nrows;
   comp->nmatrixcols = ncols;

   SCIP_CALL( SCIPallocBlockMemoryArray(scip, &comp->coltype, ncols) );
   for( int i = 0; i < ncols; ++i )
   {
      SCIP_VAR * var = SCIPmatrixGetVar(matrix,i);
      comp->coltype[i] = SCIPvarGetType(var);
   }

   SCIP_CALL( SCIPallocBlockMemoryArray(scip, &comp->rowcomponent, nrows) );
   for( int i = 0; i < nrows; ++i )
   {
      comp->rowcomponent[i] = -1;
   }
   SCIP_CALL( SCIPallocBlockMemoryArray(scip, &comp->colcomponent, ncols) );
   for( int i = 0; i < ncols; ++i )
   {
      comp->colcomponent[i] = -1;
   }

   SCIP_CALL( SCIPallocBlockMemoryArray(scip, &comp->componentrows, nrows) );
   SCIP_CALL( SCIPallocBlockMemoryArray(scip, &comp->componentcols, ncols) );
   //There will be at most ncols components
   SCIP_CALL( SCIPallocBlockMemoryArray(scip, &comp->componentrowstart, ncols) );
   SCIP_CALL( SCIPallocBlockMemoryArray(scip, &comp->componentcolstart, ncols) );
   SCIP_CALL( SCIPallocBlockMemoryArray(scip, &comp->ncomponentrows, ncols) );
   SCIP_CALL( SCIPallocBlockMemoryArray(scip, &comp->ncomponentcols, ncols) );

   comp->ncomponents = 0;

   return SCIP_OKAY;
}

static
void freeMatrixInfo(
   SCIP* scip,
   MATRIX_COMPONENTS** pmatrixcomponents
)
{
   MATRIX_COMPONENTS* comp = *pmatrixcomponents;
   SCIPfreeBlockMemoryArray(scip, &comp->ncomponentcols, comp->nmatrixcols);
   SCIPfreeBlockMemoryArray(scip, &comp->ncomponentrows, comp->nmatrixcols);
   SCIPfreeBlockMemoryArray(scip, &comp->componentcolstart, comp->nmatrixcols);
   SCIPfreeBlockMemoryArray(scip, &comp->componentrowstart, comp->nmatrixcols);
   SCIPfreeBlockMemoryArray(scip, &comp->componentcols, comp->nmatrixcols);
   SCIPfreeBlockMemoryArray(scip, &comp->componentrows, comp->nmatrixrows);
   SCIPfreeBlockMemoryArray(scip, &comp->colcomponent, comp->nmatrixcols);
   SCIPfreeBlockMemoryArray(scip, &comp->rowcomponent, comp->nmatrixrows);
   SCIPfreeBlockMemoryArray(scip, &comp->coltype, comp->nmatrixcols);

   SCIPfreeBlockMemory(scip, pmatrixcomponents);
}

static int disjointSetFind(int * disjointset, int index){
   assert(disjointset);
   int current = index;
   int next;
   //traverse down tree
   while( (next = disjointset[current]) >= 0 ){
      current = next;
   }
   int root = current;

   //compress indices along path
   current = index;
   while( (next = disjointset[current]) >= 0 ){
      disjointset[current] = root;
      current = next;
   }
   return root;
}

static int disjointSetMerge(int * disjointset, int first, int second){
   assert(disjointset);
   assert(disjointset[first] < 0);
   assert(disjointset[second] < 0);
   assert(first != second);//We cannot merge a node into itself

   //The rank is stored as a negative number: we decrement it making the negative number larger.
   //We want the new root to be the one with 'largest' rank, so smallest number. If they are equal, we decrement.
   int firstRank = disjointset[first];
   int secondRank = disjointset[second];
   if( firstRank > secondRank )
   {
      SCIPswapInts(&first, &second);
   }
   //first becomes representative
   disjointset[second] = first;
   if( firstRank == secondRank )
   {
      --disjointset[first];
   }
   return first;
}

static
SCIP_RETCODE computeContinuousComponents(
   SCIP * scip,
   SCIP_MATRIX* matrix,
   MATRIX_COMPONENTS* comp
)
{
   /* We let rows and columns share an index by mapping column i to index nrows + i*/
   int* disjointset = NULL;
   SCIP_CALL(SCIPallocBufferArray(scip, &disjointset, comp->nmatrixcols + comp->nmatrixrows));
   //First n entries belong to columns, last entries to rows
   for( int i = 0; i < comp->nmatrixcols + comp->nmatrixrows; ++i )
   {
      disjointset[i] = -1;
   }

   for( int col = 0; col < comp->nmatrixcols; ++col )
   {
      if( comp->coltype[col] != SCIP_VARTYPE_CONTINUOUS){
         continue;
      }
      int colnnonzs = SCIPmatrixGetColNNonzs(matrix, col);
      int* colrows = SCIPmatrixGetColIdxPtr(matrix, col);

      int colrep = disjointSetFind(disjointset,col);
      for( int i = 0; i < colnnonzs; ++i )
      {
         int colrow = colrows[i];
         int index = colrow + comp->nmatrixcols;
         int rowrep = disjointSetFind(disjointset,index);
         if(colrep != rowrep){
            colrep = disjointSetMerge(disjointset,colrep,rowrep);
         }
      }
   }

   /** Now, fill in the relevant data. */
   int * representativecomponent;
   SCIP_CALL(SCIPallocBufferArray(scip, &representativecomponent, comp->nmatrixcols + comp->nmatrixrows));
   for( int i = 0; i < comp->nmatrixcols + comp->nmatrixrows; ++i )
   {
      representativecomponent[i] = -1;
   }
   comp->ncomponents = 0;
   for( int col = 0; col < comp->nmatrixcols; ++col )
   {
      if( comp->coltype[col] != SCIP_VARTYPE_CONTINUOUS )
      {
         continue;
      }
      int colroot = disjointSetFind(disjointset,col);
      int component = representativecomponent[colroot];
      if( component < 0){
         //add new component
         component = comp->ncomponents;
         representativecomponent[colroot] = component;
         comp->ncomponentcols[component] = 0;
         comp->ncomponentrows[component] = 0;
         ++comp->ncomponents;
      }
      comp->colcomponent[col] = component;
      ++comp->ncomponentcols[component];
   }
   for( int row = 0; row < comp->nmatrixrows; ++row )
   {
      int rowroot = disjointSetFind(disjointset,row + comp->nmatrixcols);
      int component = representativecomponent[rowroot];
      if( component < 0){
         //Any rows that have roots that we have not seen yet are rows that have no continuous columns
         //We can safely skip these for finding the continuous connected components
         continue;
      }
      comp->rowcomponent[row] = component;
      ++comp->ncomponentrows[component];
   }
   if(comp->ncomponents != 0){
      comp->componentrowstart[0] = 0;
      comp->componentcolstart[0] = 0;
      for( int i = 1; i < comp->ncomponents; ++i )
      {
         comp->componentrowstart[i] = comp->componentrowstart[i-1] + comp->ncomponentrows[i-1];
         comp->componentcolstart[i] = comp->componentcolstart[i-1] + comp->ncomponentcols[i-1];
      }
      int * componentnextrowindex;
      int * componentnextcolindex;
      SCIP_CALL( SCIPallocBufferArray(scip,&componentnextrowindex,comp->ncomponents) );
      SCIP_CALL( SCIPallocBufferArray(scip,&componentnextcolindex,comp->ncomponents) );
      for( int i = 0; i < comp->ncomponents; ++i )
      {
         componentnextcolindex[i] = 0;
         componentnextrowindex[i] = 0;
      }

      for( int i = 0; i < comp->nmatrixcols; ++i )
      {
         int component = comp->colcomponent[i];
         if(component < 0){
            continue;
         }
         int index = comp->componentcolstart[component] + componentnextcolindex[component];
         comp->componentcols[index] = i;
         ++componentnextcolindex[component];
      }
      for( int i = 0; i < comp->nmatrixrows; ++i )
      {
         int component = comp->rowcomponent[i];
         if(component < 0){
            continue;
         }
         int index = comp->componentrowstart[component] + componentnextrowindex[component];
         comp->componentrows[index] = i;
         ++componentnextrowindex[component];
      }

#ifndef NDEBUG
      for( int i = 0; i < comp->ncomponents; ++i )
      {
         assert(componentnextrowindex[i] == comp->ncomponentrows[i]);
         assert(componentnextcolindex[i] == comp->ncomponentcols[i]);
      }
#endif

      SCIPfreeBufferArray(scip,&componentnextcolindex);
      SCIPfreeBufferArray(scip,&componentnextrowindex);
   }

   SCIPfreeBufferArray(scip,&representativecomponent);
   SCIPfreeBufferArray(scip,&disjointset);
   return SCIP_OKAY;
}

typedef struct{
   SCIP_Bool* rowintegral;                   /**< Are all the non-continuous column entries and lhs rhs integral? */
   SCIP_Bool* rowequality;                   /**< Is the row an equality? */
   SCIP_Bool* rowbadnumerics;                /**< Does the row contain large entries that make numerics difficult? */
   int* rownnonz;                            /**< Number of nonzeros in the column */
   int* rowncontinuous;                      /**< The number of those nonzeros that are in continuous columns */
   int* rowncontinuouspmone;                 /**< The number of continuous columns +-1 entries */

} MATRIX_STATISTICS;

static
SCIP_RETCODE computeMatrixStatistics(
   SCIP * scip,
   SCIP_MATRIX* matrix,
   MATRIX_STATISTICS** pstats
)
{
   SCIP_CALL( SCIPallocBuffer(scip,pstats) );
   MATRIX_STATISTICS* stats = *pstats;

   int nrows = SCIPmatrixGetNRows(matrix);

   SCIP_CALL( SCIPallocBufferArray(scip,&stats->rowintegral,nrows) );
   SCIP_CALL( SCIPallocBufferArray(scip,&stats->rowequality,nrows) );
   SCIP_CALL( SCIPallocBufferArray(scip,&stats->rowbadnumerics,nrows) );

   SCIP_CALL( SCIPallocBufferArray(scip,&stats->rownnonz,nrows) );
   SCIP_CALL( SCIPallocBufferArray(scip,&stats->rowncontinuous,nrows) );
   SCIP_CALL( SCIPallocBufferArray(scip,&stats->rowncontinuouspmone,nrows) );


   for( int i = 0; i < nrows; ++i )
   {
      double lhs = SCIPmatrixGetRowLhs(matrix,i);
      double rhs = SCIPmatrixGetRowRhs(matrix,i);
      int * cols = SCIPmatrixGetRowIdxPtr(matrix,i);
      double * vals = SCIPmatrixGetRowValPtr(matrix,i);
      int nnonz = SCIPmatrixGetRowNNonzs(matrix,i);
      stats->rownnonz[i] = nnonz;
      stats->rowequality[i] = SCIPisFeasEQ(scip,lhs,rhs) && !( SCIPisInfinity(scip,-lhs) || SCIPisInfinity(scip, rhs) );

      SCIP_Bool integral = ( SCIPisInfinity(scip,-lhs) || SCIPisIntegral(scip,lhs)) &&
                           ( SCIPisInfinity(scip,rhs) || SCIPisIntegral(scip,rhs));
      SCIP_Bool badnumerics = FALSE;

      int ncontinuous = 0;
      int ncontinuouspmone = 0;
      for( int j = 0; j < nnonz; ++j )
      {
         SCIP_Bool continuous = SCIPvarGetType(SCIPmatrixGetVar(matrix,cols[j])) == SCIP_VARTYPE_CONTINUOUS;
         double value = vals[j];
         if(continuous){
            ++ncontinuous;
         }
         if(continuous && ABS(value) == 1.0){
            ++ncontinuouspmone;
         }
         if(!continuous){
            integral = integral && SCIPisIntegral(scip,value);
         }
         if(ABS(value) > 1e7){
            badnumerics = TRUE;
         }
      }

      stats->rowncontinuous[i] = ncontinuous;
      stats->rowncontinuouspmone[i] = ncontinuouspmone;
      stats->rowintegral[i] = integral;
      stats->rowbadnumerics[i] = badnumerics;
   }


   return SCIP_OKAY;
}

static
void freeMatrixStatistics(
   SCIP* scip,
   MATRIX_STATISTICS** pstats
)
{
   MATRIX_STATISTICS* stats= *pstats;
   SCIPfreeBufferArray(scip,&stats->rowncontinuouspmone);
   SCIPfreeBufferArray(scip,&stats->rowncontinuous);
   SCIPfreeBufferArray(scip,&stats->rownnonz);
   SCIPfreeBufferArray(scip,&stats->rowequality);
   SCIPfreeBufferArray(scip,&stats->rowintegral);
   SCIPfreeBufferArray(scip,&stats->rowbadnumerics);

   SCIPfreeBuffer(scip,pstats);
}

static
SCIP_RETCODE findImpliedIntegers(
   SCIP * scip,
   SCIP_PRESOLDATA* presoldata,
   SCIP_MATRIX* matrix,
   MATRIX_COMPONENTS* comp,
   MATRIX_STATISTICS* stats,
   int* nchgvartypes
)
{
   //TODO: some checks to prevent expensive memory initialization if not necessary (e.g. there must be some candidates)
   SCIP_NETMATDEC * dec = NULL;
   SCIP_CALL(SCIPnetmatdecCreate(SCIPblkmem(scip),&dec,comp->nmatrixrows,comp->nmatrixcols));

   SCIP_NETMATDEC * transdec = NULL;
   SCIP_CALL(SCIPnetmatdecCreate(SCIPblkmem(scip),&transdec,comp->nmatrixcols,comp->nmatrixrows));

   int planarcomponents = 0;
   int goodcomponents = 0;
   int nbadnumerics = 0;
   int nbadintegrality = 0;
   int nnonnetwork = 0;

   /* Because the rows may also contain non-continuous columns, we need to remove these from the array that we
   * pass to the network matrix decomposition method. We use these working arrays for this purpose. */
   double* tempValArray;
   int* tempIdxArray;
   SCIP_CALL(SCIPallocBufferArray(scip,&tempValArray,comp->nmatrixcols));
   SCIP_CALL(SCIPallocBufferArray(scip,&tempIdxArray,comp->nmatrixcols));


   for( int component = 0; component < comp->ncomponents; ++component )
   {
      int startrow = comp->componentrowstart[component];
      int nrows = comp->ncomponentrows[component];
      SCIP_Bool componentokay = TRUE;
      for( int i = startrow; i < startrow + nrows; ++i )
      {
         int row = comp->componentrows[i];
         if(stats->rowncontinuous[row] != stats->rowncontinuouspmone[row]){
            componentokay = FALSE;
            ++nbadintegrality;
            break;
         }
         if(!stats->rowintegral[row]){
            componentokay = FALSE;
            ++nbadintegrality;
            break;
         }
         if(stats->rowbadnumerics[row]){
            componentokay = FALSE;
            ++nbadnumerics;
            break;
         }
      }
      if(!componentokay){
         continue;
      }
      int startcol = comp->componentcolstart[component];
      int ncols = comp->ncomponentcols[component];

      /* Check if the component is a network matrix */
      SCIP_Bool componentnetwork = TRUE;

      /* We use the row-wise algorithm only if the number of columns is much larger than the number of rows.
       * Generally, the column-wise algorithm will be faster, but in these extreme cases, the row algorithm is faster.
       * Only very little instances should have this at all.
       */
      if( nrows * presoldata->columnrowratio < ncols){
         for( int i = startrow; i < startrow + nrows && componentnetwork; ++i )
         {
            int row = comp->componentrows[i];
            int nrownnoz = SCIPmatrixGetRowNNonzs(matrix,row);
            int* rowcols = SCIPmatrixGetRowIdxPtr(matrix,row);
            double* rowvals = SCIPmatrixGetRowValPtr(matrix,row);
            int ncontnonz = 0;
            for( int j = 0; j < nrownnoz; ++j )
            {
               int col = rowcols[j];
               if(SCIPvarGetType(SCIPmatrixGetVar(matrix,col)) == SCIP_VARTYPE_CONTINUOUS)
               {
                  tempIdxArray[ncontnonz] = col;
                  tempValArray[ncontnonz] = rowvals[j];
                  ++ncontnonz;
                  assert(ABS(rowvals[j]) == 1.0);
               }
            }

            SCIP_CALL( SCIPnetmatdecTryAddRow(dec,row,tempIdxArray,tempValArray,ncontnonz,&componentnetwork) );
         }
      }
      else
      {
         for( int i = startcol; i < startcol + ncols && componentnetwork; ++i )
         {
            int col = comp->componentcols[i];
            int ncolnnonz = SCIPmatrixGetColNNonzs(matrix,col);
            int* colrows = SCIPmatrixGetColIdxPtr(matrix,col);
            double* colvals = SCIPmatrixGetColValPtr(matrix,col);
            SCIP_CALL( SCIPnetmatdecTryAddCol(dec,col,colrows,colvals,ncolnnonz,&componentnetwork) );
         }
      }

      if( !componentnetwork )
      {
         SCIPnetmatdecRemoveComponent(dec,&comp->componentrows[startrow], nrows, &comp->componentcols[startcol], ncols);
      }

      SCIP_Bool componenttransnetwork = TRUE;

      /* For the transposed matrix, the situation is exactly reversed because the row/column algorithms are swapped */
      if(nrows < ncols * presoldata->columnrowratio){
         for( int i = startrow; i < startrow + nrows && componenttransnetwork ; ++i )
         {
            int row = comp->componentrows[i];
            int nrownnoz = SCIPmatrixGetRowNNonzs(matrix,row);
            int* rowcols = SCIPmatrixGetRowIdxPtr(matrix,row);
            double* rowvals = SCIPmatrixGetRowValPtr(matrix,row);
            int ncontnonz = 0;
            for( int j = 0; j < nrownnoz; ++j )
            {
               int col = rowcols[j];
               if(SCIPvarGetType(SCIPmatrixGetVar(matrix,col)) == SCIP_VARTYPE_CONTINUOUS)
               {
                  tempIdxArray[ncontnonz] = col;
                  tempValArray[ncontnonz] = rowvals[j];
                  ++ncontnonz;
                  assert(ABS(rowvals[j]) == 1.0);
               }
            }

            SCIP_CALL( SCIPnetmatdecTryAddCol(transdec,row,tempIdxArray,tempValArray,ncontnonz,
                                              &componenttransnetwork) );
         }
      }
      else
      {
         for( int i = startcol; i < startcol + ncols && componenttransnetwork; ++i )
         {
            int col = comp->componentcols[i];
            int ncolnnonz = SCIPmatrixGetColNNonzs(matrix,col);
            int* colrows = SCIPmatrixGetColIdxPtr(matrix,col);
            double* colvals = SCIPmatrixGetColValPtr(matrix,col);
            SCIP_CALL( SCIPnetmatdecTryAddRow(transdec,col,colrows,colvals,ncolnnonz,&componenttransnetwork) );
         }
      }

      if( !componenttransnetwork )
      {
         SCIPnetmatdecRemoveComponent(transdec,&comp->componentcols[startcol],
                                      ncols, &comp->componentrows[startrow], nrows);
      }

      if( !componentnetwork && !componenttransnetwork){
         ++nnonnetwork;
         continue;
      }
      ++goodcomponents;
      if(componentnetwork && componenttransnetwork){
         ++planarcomponents;
      }
      for( int i = startcol; i < startcol + ncols; ++i )
      {
         int col = comp->componentcols[i];
         SCIP_VAR * var = SCIPmatrixGetVar(matrix,col);
         SCIP_Bool infeasible = FALSE;
         SCIP_CALL(SCIPchgVarType(scip,var,SCIP_VARTYPE_IMPLINT,&infeasible));
         (*nchgvartypes)++;
         assert(!infeasible);
      }
   }
   SCIPverbMessage(scip, SCIP_VERBLEVEL_FULL, NULL,
                   "implied integer components: %d (%d planar) / %d (disqualified: %d by integrality, %d by numerics, %d not network) \n",
                   goodcomponents, planarcomponents, comp->ncomponents, nbadintegrality, nbadnumerics, nnonnetwork);

   SCIPfreeBufferArray(scip,&tempIdxArray);
   SCIPfreeBufferArray(scip,&tempValArray);

   SCIPnetmatdecFree(&transdec);
   SCIPnetmatdecFree(&dec);

   return SCIP_OKAY;
}
/*
 * Callback methods of presolver
 */

/* TODO: Implement all necessary presolver methods. The methods with an #if 0 ... #else #define ... are optional */


/** copy method for constraint handler plugins (called when SCIP copies plugins) */
#if 0
static
SCIP_DECL_PRESOLCOPY(presolCopyImplint)
{  /*lint --e{715}*/
   SCIPerrorMessage("method of implint presolver not implemented yet\n");
   SCIPABORT(); /*lint --e{527}*/

   return SCIP_OKAY;
}
#else
#define presolCopyImplint NULL
#endif


/** destructor of presolver to free user data (called when SCIP is exiting) */

static
SCIP_DECL_PRESOLFREE(presolFreeImplint)
{
   SCIP_PRESOLDATA* presoldata;

   /* free presolver data */
   presoldata = SCIPpresolGetData(presol);
   assert(presoldata != NULL);

   SCIPfreeBlockMemory(scip, &presoldata);
   SCIPpresolSetData(presol, NULL);

   return SCIP_OKAY;
}


/** initialization method of presolver (called after problem was transformed) */
#if 0
static
SCIP_DECL_PRESOLINIT(presolInitImplint)
{  /*lint --e{715}*/
   SCIPerrorMessage("method of implint presolver not implemented yet\n");
   SCIPABORT(); /*lint --e{527}*/

   return SCIP_OKAY;
}
#else
#define presolInitImplint NULL
#endif


/** deinitialization method of presolver (called before transformed problem is freed) */
#if 0
static
SCIP_DECL_PRESOLEXIT(presolExitImplint)
{  /*lint --e{715}*/
   SCIPerrorMessage("method of implint presolver not implemented yet\n");
   SCIPABORT(); /*lint --e{527}*/

   return SCIP_OKAY;
}
#else
#define presolExitImplint NULL
#endif


/** presolving initialization method of presolver (called when presolving is about to begin) */
#if 0
static
SCIP_DECL_PRESOLINITPRE(presolInitpreImplint)
{  /*lint --e{715}*/
   SCIPerrorMessage("method of implint presolver not implemented yet\n");
   SCIPABORT(); /*lint --e{527}*/

   return SCIP_OKAY;
}
#else
#define presolInitpreImplint NULL
#endif


/** presolving deinitialization method of presolver (called after presolving has been finished) */
#if 0
static
SCIP_DECL_PRESOLEXITPRE(presolExitpreImplint)
{  /*lint --e{715}*/
   SCIPerrorMessage("method of implint presolver not implemented yet\n");
   SCIPABORT(); /*lint --e{527}*/

   return SCIP_OKAY;
}
#else
#define presolExitpreImplint NULL
#endif


/** execution method of presolver */

static
SCIP_DECL_PRESOLEXEC(presolExecImplint)
{
   *result = SCIP_DIDNOTRUN;

   //TODO: re-check these conditions again
   //Disable implicit integer detection if we are probing or in NLP context
   if(( SCIPgetStage(scip) != SCIP_STAGE_PRESOLVING ) || SCIPinProbing(scip) || SCIPisNLPEnabled(scip))
   {
      return SCIP_OKAY;
   }
   //Since implied integer detection relies on rows not being changed, we disable it for branch-and-price applications
   if( SCIPisStopped(scip) || SCIPgetNActivePricers(scip) > 0 )
   {
      return SCIP_OKAY;
   }

   *result = SCIP_DIDNOTFIND;

   /* Exit early if there are no candidates variables to upgrade */
   SCIP_PRESOLDATA* presoldata = SCIPpresolGetData(presol);
   if(!presoldata->convertintegers && SCIPgetNContVars(scip) == 0)
   {
      return SCIP_OKAY;
   }

   double starttime = SCIPgetSolvingTime(scip);
   SCIPverbMessage(scip, SCIP_VERBLEVEL_HIGH, NULL,
                   "   (%.1fs) implied integer detection started\n", starttime);

   SCIP_Bool initialized;
   SCIP_Bool complete;
   SCIP_Bool infeasible;
   SCIP_MATRIX* matrix = NULL;
   SCIP_Bool onlyifcomplete = TRUE;
   SCIP_CALL( SCIPmatrixCreate(scip, &matrix, onlyifcomplete, &initialized, &complete, &infeasible,
                              naddconss, ndelconss, nchgcoefs, nchgbds, nfixedvars) );
   /*If infeasibility was detected during matrix creation, we return. */
   if( infeasible )
   {
      if( initialized )
      {
         SCIPmatrixFree(scip, &matrix);
      }
      *result = SCIP_CUTOFF;
      return SCIP_OKAY;
   }

   /*For now, we only work on pure MILP's TODO; use uplocks/downlocks */
   if( !( initialized && complete ))
   {
      if( initialized )
      {
         SCIPmatrixFree(scip, &matrix);
      }
      SCIPverbMessage(scip, SCIP_VERBLEVEL_FULL, NULL,
                      "   (%.1fs) implied integer detection stopped because problem is not an MILP\n",
                      SCIPgetSolvingTime(scip));
      return SCIP_OKAY;
   }

   int beforechanged = *nchgvartypes;
   MATRIX_COMPONENTS* comp = NULL;
   MATRIX_STATISTICS* stats = NULL;
   SCIP_CALL( createMatrixComponents(scip, matrix, &comp) );
   SCIP_CALL( computeMatrixStatistics(scip, matrix, &stats) );
   SCIP_CALL( computeContinuousComponents(scip, matrix, comp) );
   SCIP_CALL( findImpliedIntegers(scip, presoldata, matrix, comp, stats, nchgvartypes) );
   int afterchanged = *nchgvartypes;


   double endtime = SCIPgetSolvingTime(scip);
   if(afterchanged == beforechanged){
      SCIPverbMessage(scip, SCIP_VERBLEVEL_HIGH, NULL,
                      "   (%.1fs) no implied integers detected (time: %.2fs)\n", endtime,endtime-starttime);
      *result = SCIP_DIDNOTFIND;
   }
   else
   {
      SCIPverbMessage(scip, SCIP_VERBLEVEL_HIGH, NULL,
                      "   (%.1fs) %d implied integers detected (time: %.2fs)\n",endtime,*nchgvartypes,endtime-starttime);

      *result = SCIP_SUCCESS;
   }
   freeMatrixStatistics(scip,&stats);
   freeMatrixInfo(scip, &comp);
   SCIPmatrixFree(scip, &matrix);
   return SCIP_OKAY;
}


/*
 * presolver specific interface methods
 */

/** creates the implint presolver and includes it in SCIP */
SCIP_RETCODE SCIPincludePresolImplint(
   SCIP*                 scip                /**< SCIP data structure */
)
{
   SCIP_PRESOLDATA* presoldata;
   SCIP_PRESOL* presol;

   /* create implint presolver data */
   SCIP_CALL( SCIPallocBlockMemory(scip, &presoldata) );

   /* include implint presolver */
   SCIP_CALL( SCIPincludePresolBasic(scip, &presol, PRESOL_NAME, PRESOL_DESC, PRESOL_PRIORITY, PRESOL_MAXROUNDS,
                                     PRESOL_TIMING, presolExecImplint, presoldata) );

   assert(presol != NULL);

   /* set non fundamental callbacks via setter functions */
   SCIP_CALL( SCIPsetPresolCopy(scip, presol, presolCopyImplint) );
   SCIP_CALL( SCIPsetPresolFree(scip, presol, presolFreeImplint) );
   SCIP_CALL( SCIPsetPresolInit(scip, presol, presolInitImplint) );
   SCIP_CALL( SCIPsetPresolExit(scip, presol, presolExitImplint) );
   SCIP_CALL( SCIPsetPresolInitpre(scip, presol, presolInitpreImplint) );
   SCIP_CALL( SCIPsetPresolExitpre(scip, presol, presolExitpreImplint) );

   SCIP_CALL( SCIPaddBoolParam(scip,
                               "presolving/implint/convertintegers",
                               "should we detect implied integrality for integer variables in the problem?",
                               &presoldata->convertintegers, TRUE, DEFAULT_CONVERTINTEGERS, NULL, NULL) );


   SCIP_CALL( SCIPaddRealParam(scip,
                               "presolving/implint/columnrowratio",
                               "Use the network row addition algorithm when the column to row ratio becomes larger than this threshold.",
                               &presoldata->columnrowratio, TRUE, DEFAULT_COLUMNROWRATIO,0.0,1e12, NULL, NULL) );
   return SCIP_OKAY;
}
