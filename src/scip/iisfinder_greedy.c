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

/**@file   iisfinder_greedy.c
 * @brief  greedy deletion and addition filter heuristic to compute (I)ISs
 * @author Marc Pfetsch
 * @author Mark Turner
 *
 */

#include <assert.h>
#include <string.h>

#include "scip/scip_iisfinder.h"
#include <scip/iisfinder_greedy.h>

#define IISFINDER_NAME           "greedy"
#define IISFINDER_DESC           "greedy deletion or addition constraint deletion"
#define IISFINDER_PRIORITY        8000
#define RANDSEED                  0x5EED

#define DEFAULT_BATCHSIZE        10
#define DEFAULT_NODELIMPERITER   -1L
#define DEFAULT_TIMELIMPERITER   1e+20
#define DEFAULT_ADDITIVE         TRUE
#define DEFAULT_CONSERVATIVE     TRUE
#define DEFAULT_DYNAMICREORDERING TRUE
#define DEFAULT_DELAFTERADD      TRUE

/*
 * Data structures
 */

/** IIS finder data */
struct SCIP_IISfinderData
{
   SCIP_RANDNUMGEN*      randnumgen;         /**< random generator for sorting constraints */
   SCIP_Real             timelimperiter;     /**< time limit per individual solve call */
   SCIP_Bool             additive;           /**< whether an additive approach instead of deletion based approach should be used */
   SCIP_Bool             conservative;       /**< should a node or time limit solve be counted as feasible when deleting constraints */
   SCIP_Bool             dynamicreordering;  /**< should satisfied constraints outside the batch of an intermediate solve be added during the additive method */
   SCIP_Bool             delafteradd;        /**< should the deletion routine be performed after the addition routine (in the case of additive) */
   SCIP_Longint          nodelimperiter;     /**< maximum number of nodes per individual solve call */
   int                   batchsize;          /**< the number of constraints to delete or add per iteration */
};

/*
 * Local methods
 */

static
SCIP_RETCODE setLimits(
   SCIP*                scip,                /**< SCIP instance to analyze */
   SCIP_IIS*            iis,                 /**< IIS data structure containing subscip  */
   SCIP_Real            timelim,             /**< total time limit allowed of the whole call */
   SCIP_Real            timelimperiter,      /**< time limit per individual solve call */
   SCIP_Longint         nodelim,             /**< node limit allowed for the whole call */
   SCIP_Longint         nodelimperiter       /**< maximum number of nodes per individual solve call */
   )
{
   /* solve problem until first solution is found or infeasibility has been proven */
   SCIP_CALL( SCIPsetRealParam(scip, "limits/time", MAX(MIN(timelim - SCIPiisGetTime(iis), timelimperiter), 0)) );
   if( nodelim == -1 && nodelimperiter == -1 )
      SCIP_CALL( SCIPsetLongintParam(scip, "limits/nodes", nodelim) );
   else if( nodelim == -1 || nodelimperiter == -1 )
      SCIP_CALL( SCIPsetLongintParam(scip, "limits/nodes", MAX(nodelim, nodelimperiter)) );
   else
      SCIP_CALL( SCIPsetLongintParam(scip, "limits/nodes", MIN(nodelim, nodelimperiter)) );
   return SCIP_OKAY;
}

static
void revertBndChgs(
   SCIP*                 scip,               /**< SCIP instance to analyze */
   SCIP_VAR**            vars,               /**< the array of variables whose bounds are changed */
   SCIP_Real*            bounds,             /**< the array of original bounds for the variables */
   int*                  idxs,               /**< the indices of the vars (in the vars array) that have been deleted */
   int                   ndelbounds,         /**< the number of bounds that will be deleted */
   SCIP_Bool             islb                /**< are the bounds that are being deleted LBs? */
   )
{
   int i;
   
   for (i = 0; i < ndelbounds; ++i)
   {
      if( islb )
      {
         if( !SCIPisInfinity(scip, -bounds[i]) )
            SCIPchgVarLb(scip, vars[idxs[i]], bounds[i]);
      }
      else
      {
         if( !SCIPisInfinity(scip, bounds[i]) )
            SCIPchgVarUb(scip, vars[idxs[i]], bounds[i]);
      }
   }
}

static
SCIP_RETCODE revertConssDeletions(
   SCIP*                 scip,               /**< SCIP instance to analyze */
   SCIP_CONS**           conss,              /**< the array of constraints where some have been deleted */
   int*                  idxs,               /**< the indices of the cons (in the conss array) that have been deleted */
   int                   ndelconss,          /**< the number of constraints that have been deleted */
   SCIP_Bool             releaseonly         /**< Should the constraints just be released instead of addded back */
   )
{
   int i;
   
   if( !releaseonly )
   {
      for (i = 0; i < ndelconss; ++i)
      {
         SCIP_CALL( SCIPaddCons(scip, conss[idxs[i]]) );
      }
   }
   for (i = 0; i < ndelconss; ++i)
   {
      SCIP_CALL( SCIPreleaseCons(scip, &conss[idxs[i]]) );
   }
   return SCIP_OKAY;
}

/** solve subproblem for deletionFilter */
static
SCIP_RETCODE deletionSubproblem(
   SCIP_IIS*             iis,                /**< IIS data structure containing subscip */
   SCIP_CONS**           conss,              /**< The array of constraints (may be a superset of the current constraints) */
   SCIP_VAR**            vars,               /**< the array of vars */
   int*                  idxs,               /**< the indices of the constraints / vars that will be deleted / bounds removed */
   int                   ndels,              /**< the number of bounds that will be deleted */
   SCIP_Real             timelim,            /**< The global time limit on the IIS finder call */
   SCIP_Real             timelimperiter,     /**< time limit per individual solve call */
   SCIP_Longint          nodelim,            /**< The global node limit on the IIS finder call */
   SCIP_Longint          nodelimperiter,     /**< maximum number of nodes per individual solve call */
   SCIP_Bool             conservative,       /**< whether we treat a subproblem to be feasible, if it reaches some status that could result in infeasible, e.g. node limit */
   SCIP_Bool             delbounds,          /**< whether bounds should be deleted instead of constraints */
   SCIP_Bool             islb,               /**< are the bounds that are being deleted LBs? */
   SCIP_Bool*            deleted,            /**< have the deleted bounds or constraints stayed deleted */
   SCIP_Bool*            stop,               /**< pointer to store whether we have to stop */
   SCIP_Bool*            alldeletionssolved  /**< pointer to store whether all the subscips solved */
   )
{
   SCIP* scip;
   SCIP_Real* bounds;
   SCIP_RETCODE retcode;
   SCIP_STATUS status;
   int i;
   
   *deleted = FALSE;
   *stop = FALSE;
   scip = SCIPiisGetSubscip(iis);
   
   /* remove bounds or constraints*/
   if( delbounds )
   {
      SCIP_CALL( SCIPallocBlockMemoryArray(scip, &bounds, ndels) );
      for (i = 0; i < ndels; ++i)
      {
         if( islb )
         {
            bounds[i] = SCIPvarGetLbOriginal(vars[idxs[i]]);
            if( !SCIPisInfinity(scip, -bounds[i]) )
               SCIPchgVarLb(scip, vars[idxs[i]], -SCIPinfinity(scip));
         }
         else
         {
            bounds[i] = SCIPvarGetUbOriginal(vars[idxs[i]]);
            if( !SCIPisInfinity(scip, bounds[i]) )
               SCIPchgVarUb(scip, vars[idxs[i]], SCIPinfinity(scip));
         }
      }
   }
   else
   {
      for (i = 0; i < ndels; ++i)
      {
         assert( SCIPconsIsInProb(conss[idxs[i]]) );
         SCIP_CALL( SCIPcaptureCons(scip, conss[idxs[i]]) );
         SCIP_CALL( SCIPdelCons(scip, conss[idxs[i]]) );
      }
   }
   
   /* solve problem until first solution is found or infeasibility has been proven */
   setLimits(scip, iis, timelim, timelimperiter, nodelim, nodelimperiter);
   retcode = SCIPsolve(scip);
   SCIPiisAddNNodes(iis, SCIPgetNTotalNodes(scip));
   
   if( retcode != SCIP_OKAY )
   {
      SCIP_CALL( SCIPfreeTransform(scip) );
      SCIPdebugMessage("Error in sub-scip with deleted constraints / bounds. Re-adding them.\n");
      if( delbounds )
      {
         revertBndChgs(scip, vars, bounds, idxs, ndels, islb);
         SCIPfreeBlockMemoryArray(scip, &bounds, ndels);
      }
      else
         SCIP_CALL( revertConssDeletions(scip, conss, idxs, ndels, FALSE) );
      *alldeletionssolved = FALSE;
      return SCIP_OKAY;
   }
   
   status = SCIPgetStatus(scip);
   SCIP_CALL( SCIPfreeTransform(scip) );
   
   /* check status and handle accordingly */
   switch ( status )
   {
      case SCIP_STATUS_USERINTERRUPT:    /* if a user interrupt occurred, just stop */
         SCIPdebugMessage("User interrupt. Stopping. \n");
         if( delbounds )
            revertBndChgs(scip, vars, bounds, idxs, ndels, islb);
         else
            SCIP_CALL( revertConssDeletions(scip, conss, idxs, ndels, FALSE) );
         *stop = TRUE;
         *alldeletionssolved = FALSE;
         break;
      
      case SCIP_STATUS_TIMELIMIT:        /* if we reached some status that may have ended up in an infeasible problem */
      case SCIP_STATUS_NODELIMIT:
      case SCIP_STATUS_TOTALNODELIMIT:
      case SCIP_STATUS_STALLNODELIMIT:
      case SCIP_STATUS_MEMLIMIT:
      case SCIP_STATUS_GAPLIMIT:
      case SCIP_STATUS_RESTARTLIMIT:
      case SCIP_STATUS_INFORUNBD:
         *alldeletionssolved = FALSE;
         SCIPdebugMessage("Some limit reached. Keeping bounds / constraints removed if non-conservative. \n");
         if( !conservative )
         {
            SCIPiisSetValid(iis, FALSE);
            *deleted = TRUE;
         }
         if( conservative && delbounds )
            revertBndChgs(scip, vars, bounds, idxs, ndels, islb);
         SCIP_CALL( revertConssDeletions(scip, conss, idxs, ndels, !conservative) );
         break;
      
      case SCIP_STATUS_INFEASIBLE:       /* if the problem is infeasible */
         SCIPdebugMessage("Subproblem with bounds / constraints removed infeasible. Keep them removed.\n");
         SCIPiisSetValid(iis, TRUE);
         if( !delbounds )
            SCIP_CALL( revertConssDeletions(scip, conss, idxs, ndels, TRUE) );
         *deleted = TRUE;
         break;
      
      case SCIP_STATUS_BESTSOLLIMIT:     /* we found a solution */
      case SCIP_STATUS_SOLLIMIT:
      case SCIP_STATUS_OPTIMAL:
      case SCIP_STATUS_UNBOUNDED:
         SCIPdebugMessage("Found solution to subproblem with bounds / constraints removed. Add them back.\n");
         if( delbounds )
            revertBndChgs(scip, vars, bounds, idxs, ndels, islb);
         else
            SCIP_CALL( revertConssDeletions(scip, conss, idxs, ndels, FALSE) );
         break;
      
      case SCIP_STATUS_UNKNOWN:
      default:
         *alldeletionssolved = FALSE;
         SCIPerrorMessage("Unexpected return status %d in removed bounds subproblem. Exiting... \n", status);
         if( delbounds )
            SCIPfreeBlockMemoryArray(scip, &bounds, ndels);
         return SCIP_ERROR;
   }
   
   if( delbounds )
      SCIPfreeBlockMemoryArray(scip, &bounds, ndels);
   
   return SCIP_OKAY;
}

/** solve subproblem for additionFilter */
static
SCIP_RETCODE additionSubproblem(
   SCIP_IIS*             iis,                /**< IIS data structure containing subscip  */
   SCIP_Real             timelim,            /**< The global time limit on the IIS finder call */
   SCIP_Real             timelimperiter,     /**< time limit per individual solve call */
   SCIP_Longint          nodelim,            /**< The global node limit on the IIS finder call */
   SCIP_Longint          nodelimperiter,     /**< maximum number of nodes per individual solve call */
   SCIP_Bool*            feasible,           /**< pointer to store whether the problem is feasible */
   SCIP_Bool*            stop                /**< pointer to store whether we have to stop */
)
{
   SCIP* scip;
   SCIP_RETCODE retcode;
   SCIP_STATUS status;
   
   assert( stop != NULL );
   scip = SCIPiisGetSubscip(iis);
   
   *stop = FALSE;
   
   /* solve problem until first solution is found or infeasibility has been proven */
   setLimits(scip, iis, timelim, timelimperiter, nodelim, nodelimperiter);
   retcode = SCIPsolve(scip);
   
   if( retcode != SCIP_OKAY )
   {
      SCIPdebugMessage("Error in sub-scip with added constraints. Keep added constraints.\n");
      return SCIP_ERROR;
   }
   
   SCIPiisAddNNodes(iis, SCIPgetNTotalNodes(scip));
   status = SCIPgetStatus(scip);
   
   /* check status */
   switch ( status )
   {
      case SCIP_STATUS_USERINTERRUPT:    /* if an user interrupt occurred, just stop */
         SCIPdebugMessage("User interrupt. Stopping. \n");
         *stop = TRUE;
         break;
      
      case SCIP_STATUS_NODELIMIT:        /* if we reached some limit */
      case SCIP_STATUS_TIMELIMIT:
      case SCIP_STATUS_TOTALNODELIMIT:
      case SCIP_STATUS_STALLNODELIMIT:
      case SCIP_STATUS_MEMLIMIT:
      case SCIP_STATUS_GAPLIMIT:
      case SCIP_STATUS_SOLLIMIT:
      case SCIP_STATUS_RESTARTLIMIT:
      case SCIP_STATUS_INFORUNBD:
         SCIPdebugMessage("Some limit reached. Added constraint batch failed to induce infeasibility. Continue adding.\n");
         break;
      
      case SCIP_STATUS_INFEASIBLE:       /* if the problem is infeasible */
         SCIPdebugMessage("Subproblem with added constraints infeasible. Final batch of constraints added.\n");
         *feasible = FALSE;
         break;
      
      case SCIP_STATUS_BESTSOLLIMIT:     /* we found a solution */
      case SCIP_STATUS_OPTIMAL:
      case SCIP_STATUS_UNBOUNDED:
         SCIPdebugMessage("Found solution of subproblem with added constraints. Keep adding constraint batches.\n");
         *feasible = TRUE;
         break;
      
      case SCIP_STATUS_UNKNOWN:
      default:
         SCIPerrorMessage("Unexpected return status %d. Exiting ...\n", status);
         return SCIP_ERROR;
   }
   
   return SCIP_OKAY;
}


/** Deletion filter to greedily remove constraints to obtain an (I)IS */
static
SCIP_RETCODE deletionFilterBatch(
   SCIP_IIS*             iis,                /**< IIS data structure containing subscip  */
   SCIP_Real             timelim,            /**< The global time limit on the IIS finder call */
   SCIP_Longint          nodelim,            /**< The global node limit on the IIS finder call */
   SCIP_Bool             silent,             /**< should the run be performed silently without printing progress information */
   SCIP_RANDNUMGEN*      randnumgen,         /**< random number generator */
   SCIP_Real             timelimperiter,     /**< time limit per individual solve call */
   SCIP_Longint          nodelimperiter,     /**< maximum number of nodes per individual solve call */
   SCIP_Bool             removebounds,       /**< Whether the algorithm should remove bounds as well as constraints */
   SCIP_Bool             conservative,       /**< should a node or time limit solve be counted as feasible when deleting constraints */
   int                   batchsize,          /**< the number of constraints to delete or add per iteration */
   SCIP_Bool*            alldeletionssolved  /**< pointer to store whether all the subscips solved */
   )
{
   SCIP* scip;
   SCIP_CONS** conss;
   SCIP_VAR** vars;
   int* order;
   int* idxs;
   SCIP_Bool stopiter;
   SCIP_Bool deleted;
   int nconss;
   int nvars;
   int i;
   int j;
   int k;
   
   scip = SCIPiisGetSubscip(iis);
   assert( scip != NULL );
   
   SCIP_CALL( SCIPallocBlockMemoryArray(scip, &idxs, batchsize) );
   
   /* Get constraint information */
   nconss = SCIPgetNOrigConss(scip);
   SCIP_CALL( SCIPallocBlockMemoryArray(scip, &conss, nconss) );
   SCIP_CALL( SCIPduplicateBlockMemoryArray(scip, &conss, SCIPgetOrigConss(scip), nconss) );

   /* reset problem */
   SCIP_CALL( SCIPfreeTransform(scip) );
   assert( SCIPgetStage(scip) == SCIP_STAGE_PROBLEM );

   /* prepare random order for constraints */
   SCIP_CALL( SCIPallocBlockMemoryArray(scip, &order, nconss) );
   for (i = 0; i < nconss; ++i)
      order[i] = i;
   SCIPrandomPermuteIntArray(randnumgen, order, 0, nconss);

   /* Loop through all batches of constraints in random order */
   stopiter = FALSE;
   i = 0;
   while( i < nconss )
   {
      k = 0;
      for( j = i; j < MIN(i + batchsize, nconss); j++ )
      {
         idxs[k] = order[j];
         k++;
      }
      i = i + k;

      /* treat subproblem */
      SCIP_CALL( deletionSubproblem(iis, conss, NULL, idxs, k, timelim, timelimperiter, nodelim, nodelimperiter,
                                    conservative, FALSE, FALSE, &deleted, &stopiter, alldeletionssolved) );
      if( !silent && deleted )
         SCIPiisfinderInfoMessage(iis, FALSE);
      
      if( timelim - SCIPiisGetTime(iis) <= 0 || ( nodelim != -1 && SCIPiisGetNNodes(iis) > nodelim ) || stopiter )
         break;
      
      assert( SCIPgetStage(scip) == SCIP_STAGE_PROBLEM );
   }
   
   SCIPfreeBlockMemoryArray(scip, &order, nconss);
   SCIPfreeBlockMemoryArray(scip, &conss, nconss);
   
   if( timelim - SCIPiisGetTime(iis) <= 0 || ( nodelim != -1 && SCIPiisGetNNodes(iis) > nodelim ) || stopiter )
      goto TERMINATE;
   
   /* Repeat the above procedure but for bounds instead of constraints */
   if( removebounds )
   {
      nvars = SCIPgetNOrigVars(scip);
      SCIP_CALL( SCIPallocBlockMemoryArray(scip, &vars, nvars) );
      SCIP_CALL( SCIPduplicateBlockMemoryArray(scip, &vars, SCIPgetOrigVars(scip), nvars) );
   
      SCIP_CALL( SCIPallocBlockMemoryArray(scip, &order, nvars) );
      for (i = 0; i < nvars; ++i)
         order[i] = i;
      SCIPrandomPermuteIntArray(randnumgen, order, 0, nvars);
   
      i = 0;
      while( i < nvars )
      {
         k = 0;
         /* Do not delete bounds of binary variables or bother with calculations of free variables */
         for( j = i; j < nvars; j++ )
         {
            if( k >= batchsize )
               break;
            if( SCIPvarGetType(vars[order[j]]) == SCIP_VARTYPE_BINARY )
               continue;
            else if( SCIPisInfinity(scip, -SCIPvarGetLbOriginal(vars[order[j]])) && SCIPisInfinity(scip, SCIPvarGetUbOriginal(vars[order[j]])) )
               continue;
            else
            {
               idxs[k] = order[j];
               k++;
            }
         }
         if( k == 0 )
            break;
         i = i + k;
         
         /* treat subproblem with LB deletions */
         SCIP_CALL( deletionSubproblem(iis, NULL, vars, idxs, k, timelim, timelimperiter, nodelim, nodelimperiter,
                                       conservative, TRUE, TRUE, &deleted, &stopiter, alldeletionssolved) );
         if( timelim - SCIPiisGetTime(iis) <= 0 || ( nodelim != -1 && SCIPiisGetNNodes(iis) > nodelim ) || stopiter )
            break;
         if( !silent && deleted )
            SCIPiisfinderInfoMessage(iis, FALSE);
         
         /* treat subproblem with UB deletions */
         SCIP_CALL( deletionSubproblem(iis, NULL, vars, idxs, k, timelim, timelimperiter, nodelim, nodelimperiter,
                                       conservative, TRUE, FALSE, &deleted, &stopiter, alldeletionssolved) );
   
         if( timelim - SCIPiisGetTime(iis) <= 0 || ( nodelim != -1 && SCIPiisGetNNodes(iis) > nodelim ) || stopiter )
            break;
         if( !silent && deleted )
            SCIPiisfinderInfoMessage(iis, FALSE);
      }
   
      SCIPfreeBlockMemoryArray(scip, &order, nvars);
      SCIPfreeBlockMemoryArray(scip, &vars, nvars);
   }
   
   goto TERMINATE;
   
TERMINATE:
   SCIPfreeBlockMemoryArray(scip, &idxs, batchsize);
   return SCIP_OKAY;
   
}

/** Addition filter to greedily add constraints to obtain an (I)IS */
static
SCIP_RETCODE additionFilterBatch(
   SCIP_IIS*             iis,                /**< IIS data structure containing subscip  */
   SCIP_Real             timelim,            /**< The global time limit on the IIS finder call */
   SCIP_Longint          nodelim,            /**< The global node limit on the IIS finder call */
   SCIP_Bool             silent,             /**< should the run be performed silently without printing progress information */
   SCIP_RANDNUMGEN*      randnumgen,         /**< random number generator */
   SCIP_Real             timelimperiter,     /**< time limit per individual solve call */
   SCIP_Longint          nodelimperiter,     /**< maximum number of nodes per individual solve call */
   SCIP_Bool             dynamicreordering,  /**< should satisfied constraints outside the batch of an intermediate solve be added during the additive method */
   int                   batchsize           /**< the number of constraints to delete or add per iteration */
   )
{
   SCIP* scip;
   SCIP_CONS** conss;
   SCIP_SOL* sol;
   SCIP_SOL* copysol;
   SCIP_Bool* inIS;
   int* order;
   SCIP_Bool feasible;
   SCIP_Bool stopiter;
   SCIP_RETCODE retcode;
   SCIP_RESULT result;
   int nconss;
   int i;
   int j;
   int k;
   
   scip = SCIPiisGetSubscip(iis);
   assert( SCIPiisGetValid(iis) );
   assert( scip != NULL );
   
   /* Get constraint information */
   nconss = SCIPgetNOrigConss(scip);
   SCIP_CALL( SCIPallocBlockMemoryArray(scip, &conss, nconss) );
   SCIP_CALL( SCIPduplicateBlockMemoryArray(scip, &conss, SCIPgetOrigConss(scip), nconss) );
   
   /* Initialise information for whether a constraint is in the final infeasible system */
   SCIP_CALL( SCIPallocBlockMemoryArray(scip, &inIS, nconss) );
   
   /* First capture and then delete all constraints */
   assert( SCIPgetStage(scip) == SCIP_STAGE_PROBLEM );
   for( i = 0; i < nconss; ++i )
   {
      assert( SCIPconsIsInProb(conss[i]) );
      SCIP_CALL( SCIPcaptureCons(scip, conss[i]) );
      SCIP_CALL( SCIPdelCons(scip, conss[i]) );
      inIS[i] = FALSE;
   }
   SCIPiisSetValid(iis, FALSE);
   
   /* Prepare random order in which the constraints will be added back */
   SCIP_CALL( SCIPallocBlockMemoryArray(scip, &order, nconss) );
   for (i = 0; i < nconss; ++i)
      order[i] = i;
   SCIPrandomPermuteIntArray(randnumgen, order, 0, nconss);
   
   /* Continue to add constraints until an infeasible status is reached */
   i = 0;
   feasible = TRUE;
   stopiter = FALSE;
   while( feasible )
   {
      /* Add the next batch of constraints */
      k = 0;
      for( j = i; j < nconss; ++j )
      {
         if( k >= batchsize )
            break;
         if( !inIS[order[j]] )
         {
            SCIP_CALL( SCIPaddCons(scip, conss[order[j]]) );
            SCIP_CALL( SCIPreleaseCons(scip, &conss[order[j]]) );
            inIS[order[j]] = TRUE;
            ++k;
         }
      }
      if( k == 0 )
         break;
      i = i + k;
      
      /* Solve the reduced problem */
      retcode = additionSubproblem(iis, timelim, timelimperiter, nodelim, nodelimperiter, &feasible, &stopiter);
      if( !silent )
         SCIPiisfinderInfoMessage(iis, FALSE);
      if( timelim - SCIPiisGetTime(iis) <= 0 || ( nodelim != -1 && SCIPiisGetNNodes(iis) > nodelim ) || stopiter )
      {
         SCIP_CALL( SCIPfreeTransform(scip) );
         break;
      }
      
      if( retcode == SCIP_OKAY )
      {
         /* free transform and copy solution if there is one */
         sol = SCIPgetBestSol(scip);
         if( sol != NULL )
         {
            SCIP_CALL( SCIPcreateSolCopyOrig(scip, &copysol, sol) );
            SCIP_CALL( SCIPunlinkSol(scip, copysol) );
         }
         SCIP_CALL( SCIPfreeTransform(scip) );
         assert( SCIPgetStage(scip) == SCIP_STAGE_PROBLEM );
   
         /* Add any other constraints that are also feasible for the current solution */
         if( feasible && !stopiter && copysol != NULL && dynamicreordering )
         {
            k = 0;
            for( j = i; j < nconss; ++j )
            {
               if( !inIS[order[j]] )
               {
                  SCIP_CALL( SCIPcheckCons(scip, conss[order[j]], copysol, FALSE, FALSE, FALSE, &result) );
                  if( result == SCIP_FEASIBLE )
                  {
                     SCIP_CALL( SCIPaddCons(scip, conss[order[j]]) );
                     SCIP_CALL( SCIPreleaseCons(scip, &conss[order[j]]) );
                     inIS[order[j]] = TRUE;
                     k++;
                  }
               }
            }
            if( k > 0 )
            {
               SCIPdebugMessage( "Added %d many constraints dynamically.\n", k);
               if( ! silent )
                  SCIPiisfinderInfoMessage(iis, FALSE);
            }
         }
      }
      else
      {
         SCIP_CALL( SCIPfreeTransform(scip) );
         assert( SCIPgetStage(scip) == SCIP_STAGE_PROBLEM );
      }
   }
   
   /* Release any cons not in the IS */
   for( i = 0; i < nconss; ++i )
   {
      if( !inIS[order[i]] )
         SCIP_CALL( SCIPreleaseCons(scip, &conss[order[i]]) );
         
   }
   
   SCIPfreeBlockMemoryArray(scip, &order, nconss);
   SCIPfreeBlockMemoryArray(scip, &inIS, nconss);
   SCIPfreeBlockMemoryArray(scip, &conss, nconss);
   if( feasible )
      SCIPiisSetValid(iis, FALSE);
   else
      SCIPiisSetValid(iis, TRUE);
   
   return SCIP_OKAY;
}

/*
 * Callback methods of IIS finder
 */


/** copy method for IIS finder plugin (called when SCIP copies plugins) */
static
SCIP_DECL_IISFINDERCOPY(iisfinderCopyGreedy)
{  /*lint --e{715}*/
   assert(scip != NULL);
   assert(iisfinder != NULL);
   assert(strcmp(SCIPiisfinderGetName(iisfinder), IISFINDER_NAME) == 0);
   
   /* call inclusion method of IIS finder */
   SCIP_CALL( SCIPincludeIISfinderGreedy(scip) );
   
   return SCIP_OKAY;
}

/** destructor of IIS finder to free user data (called when SCIP is exiting) */
/**! [SnippetIISfinderFreeGreedy] */
static
SCIP_DECL_IISFINDERFREE(iisfinderFreeGreedy)
{  /*lint --e{715}*/
   SCIP_IISFINDERDATA* iisfinderdata;
   
   iisfinderdata = SCIPiisfinderGetData(iisfinder);
   SCIPfreeRandom(scip, &(iisfinderdata->randnumgen));
   
   SCIPfreeBlockMemory(scip, &iisfinderdata);
   
   SCIPiisfinderSetData(iisfinder, NULL);
   
   return SCIP_OKAY;
}
/**! [SnippetIISfinderFreeGreedy] */

/** IIS finder generation method of IIS */
static
SCIP_DECL_IISFINDEREXEC(iisfinderExecGreedy)
{  /*lint --e{715}*/
   struct SCIP_IISfinderData* iisfinderdata;
   
   assert(iisfinder != NULL);
   assert(result != NULL);
   
   *result = SCIP_SUCCESS;
   
   iisfinderdata = SCIPiisfinderGetData(iisfinder);
   assert(iisfinderdata != NULL);
   
   SCIP_CALL( SCIPexecIISfinderGreedy(iis, timelim, nodelim, removebounds, silent, iisfinderdata->randnumgen,
            iisfinderdata->timelimperiter, iisfinderdata->additive, iisfinderdata->conservative,
            iisfinderdata->dynamicreordering, iisfinderdata->delafteradd,
            iisfinderdata->nodelimperiter, iisfinderdata->batchsize, result) );
   
   return SCIP_OKAY;
}


/*
 * IIS finder specific interface methods
 */

/** creates the greedy IIS finder and includes it in SCIP */
SCIP_RETCODE SCIPincludeIISfinderGreedy(
   SCIP*                 scip                /**< SCIP data structure */
   )
{
   SCIP_IISFINDERDATA* iisfinderdata;
   SCIP_IISFINDER* iisfinder;
   
   /* create greedy IIS finder data */
   SCIP_CALL( SCIPallocBlockMemory(scip, &iisfinderdata) );
   BMSclearMemory(iisfinderdata);
   SCIP_CALL( SCIPcreateRandom(scip, &(iisfinderdata)->randnumgen, RANDSEED, TRUE) );
   
   SCIP_CALL( SCIPincludeIISfinderBasic(scip, &iisfinder, IISFINDER_NAME, IISFINDER_DESC, IISFINDER_PRIORITY,
                                        iisfinderExecGreedy, iisfinderdata) );
   
   assert(iisfinder != NULL);
   
   /* set non fundamental callbacks via setter functions */
   SCIP_CALL( SCIPsetIISfinderCopy(scip, iisfinder, iisfinderCopyGreedy) );
   SCIP_CALL( SCIPsetIISfinderFree(scip, iisfinder, iisfinderFreeGreedy) );
   
   /* add greedy IIS finder parameters */
   SCIP_CALL( SCIPaddIntParam(scip,
         "iis/" IISFINDER_NAME "/batchsize",
         "batch size of constraints that are removed or added in one iteration",
         &iisfinderdata->batchsize, FALSE, DEFAULT_BATCHSIZE, 1, INT_MAX, NULL, NULL) );
   
   SCIP_CALL( SCIPaddRealParam(scip,
         "iis/" IISFINDER_NAME "/timelimperiter",
         "time limit of optimization process for each individual subproblem",
         &iisfinderdata->timelimperiter, FALSE, DEFAULT_TIMELIMPERITER, 0.0, SCIP_INVALID/10.0, NULL, NULL) );
   
   SCIP_CALL( SCIPaddLongintParam(scip,
         "iis/" IISFINDER_NAME "/nodelimperiter",
         "node limit of optimization process for each individual subproblem",
         &iisfinderdata->nodelimperiter, FALSE, DEFAULT_NODELIMPERITER, -1, INT_MAX, NULL, NULL) );
   
   SCIP_CALL( SCIPaddBoolParam(scip,
         "iis/" IISFINDER_NAME "/additive",
         "should an additive constraint approach be used instead of deletion",
         &iisfinderdata->additive, FALSE, DEFAULT_ADDITIVE, NULL, NULL) );
   
   SCIP_CALL( SCIPaddBoolParam(scip,
         "iis/" IISFINDER_NAME "/conservative",
         "should a hit limit (e.g. node  time) solve be counted as feasible when deleting constraints",
         &iisfinderdata->conservative, TRUE, DEFAULT_CONSERVATIVE, NULL, NULL) );
   
   SCIP_CALL( SCIPaddBoolParam(scip,
         "iis/" IISFINDER_NAME "/dynamicreordering",
         "should satisfied constraints outside the batch of an intermediate solve be added during the additive method",
         &iisfinderdata->dynamicreordering, TRUE, DEFAULT_DYNAMICREORDERING, NULL, NULL) );
   
   SCIP_CALL( SCIPaddBoolParam(scip,
         "iis/" IISFINDER_NAME "/delafteradd",
         "should the deletion routine be performed after the addition routine (in the case of additive)",
         &iisfinderdata->delafteradd, TRUE, DEFAULT_DELAFTERADD, NULL, NULL) );
   
   return SCIP_OKAY;
}

/** perform a greedy addition or deletion algorithm to obtain an infeasible subsystem (IS).
 *
 *  This is the generation method for the greedy IIS finder rule.
 *  Depending on the parameter choices, constraints are either greedily added from an empty problem,
 *  or deleted from a complete problem. In the case of constraints being added, this is done until the problem
 *  becomes infeasible, after which one can then begin deleting constraints. In the case of deleting constraints,
 *  this is done until no more constraints (or batches of constraints) can be deleted without making
 *  the problem feasible.
 */
SCIP_RETCODE SCIPexecIISfinderGreedy(
   SCIP_IIS*             iis,                /**< IIS data structure */
   SCIP_Real             timelim,            /**< The global time limit on the IIS finder call */
   SCIP_Longint          nodelim,            /**< The global node limit on the IIS finder call */
   SCIP_Bool             removebounds,       /**< Whether the algorithm should remove bounds as well as constraints */
   SCIP_Bool             silent,             /**< should the run be performed silently without printing progress information */
   SCIP_RANDNUMGEN*      randnumgen,         /**< random number generator */
   SCIP_Real             timelimperiter,     /**< time limit per individual solve call */
   SCIP_Bool             additive,           /**< whether an additive approach instead of deletion based approach should be used */
   SCIP_Bool             conservative,       /**< should a hit limit (e.g. node / time) solve be counted as feasible when deleting constraints */
   SCIP_Bool             dynamicreordering,  /**< should satisfied constraints outside the batch of an intermediate solve be added during the additive method */
   SCIP_Bool             delafteradd,        /**< should the deletion routine be performed after the addition routine (in the case of additive) */
   SCIP_Longint          nodelimperiter,     /**< maximum number of nodes per individual solve call */
   int                   batchsize,          /**< the number of constraints to delete or add per iteration */
   SCIP_RESULT*          result              /**< pointer to store the result of the IIS finder run */
   )
{
   SCIP* scip;
   SCIP_Bool alldeletionssolved = TRUE;
   
   scip = SCIPiisGetSubscip(iis);
   
   if( additive )
   {
      if( !silent )
      {
         SCIPinfoMessage(scip, NULL, "----- STARTING GREEDY ADDITION ALGORITHM -----\n");
      }
      SCIPiisfinderInfoMessage(iis, TRUE);
      SCIP_CALL( additionFilterBatch(iis, timelim, nodelim, silent, randnumgen, timelimperiter, nodelimperiter, dynamicreordering, batchsize) );
      SCIPiisSetIrreducible(iis, FALSE);
      if( timelim - SCIPiisGetTime(iis) <= 0 || ( nodelim != -1 && SCIPiisGetNNodes(iis) > nodelim ) )
      {
         *result = SCIP_SUCCESS;
         return SCIP_OKAY;
      }
   }
   else
   {
      if( !silent )
      {
         SCIPinfoMessage(scip, NULL, "----- STARTING GREEDY DELETION ALGORITHM -----\n");
      }
      SCIPiisfinderInfoMessage(iis, TRUE);
      SCIP_CALL( deletionFilterBatch(iis, timelim, nodelim, silent, randnumgen, timelimperiter, nodelimperiter, removebounds, conservative, batchsize, &alldeletionssolved) );
      if( timelim - SCIPiisGetTime(iis) <= 0 || ( nodelim != -1 && SCIPiisGetNNodes(iis) > nodelim ) )
      {
         *result = SCIP_SUCCESS;
         return SCIP_OKAY;
      }
      if( alldeletionssolved && batchsize == 1 )
         SCIPiisSetIrreducible(iis, TRUE);
   }
   
   if( delafteradd && additive )
   {
      if( !silent )
      {
         SCIPinfoMessage(scip, NULL, "----- STARTING GREEDY DELETION ALGORITHM FOLLOWING COMPLETED ADDITION ALGORITHM -----\n");
      }
      SCIPiisfinderInfoMessage(iis, TRUE);
      SCIP_CALL( deletionFilterBatch(iis, timelim, nodelim, silent, randnumgen, timelimperiter, nodelimperiter, removebounds, conservative, batchsize, &alldeletionssolved) );
      if( alldeletionssolved && batchsize == 1 )
         SCIPiisSetIrreducible(iis, TRUE);
      if( timelim - SCIPiisGetTime(iis) <= 0 || ( nodelim != -1 && SCIPiisGetNNodes(iis) > nodelim ) )
      {
         *result = SCIP_SUCCESS;
         return SCIP_OKAY;
      }
   }
   
   return SCIP_OKAY;
}
