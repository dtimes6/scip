/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2004 Tobias Achterberg                              */
/*                                                                           */
/*                  2002-2004 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the SCIP Academic Licence.        */
/*                                                                           */
/*  You should have received a copy of the SCIP Academic License             */
/*  along with SCIP; see the file COPYING. If not email to scip@zib.de.      */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#pragma ident "@(#) $Id: struct_scip.h,v 1.5 2004/03/31 14:52:59 bzfpfend Exp $"

/**@file   scipstruct.h
 * @brief  SCIP main data structure
 * @author Tobias Achterberg
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#ifndef __STRUCT_SCIP_H__
#define __STRUCT_SCIP_H__


#include "def.h"
#include "type_set.h"
#include "type_stat.h"
#include "type_clock.h"
#include "type_interrupt.h"
#include "type_mem.h"
#include "type_event.h"
#include "type_lp.h"
#include "type_prob.h"
#include "type_primal.h"
#include "type_tree.h"
#include "type_pricestore.h"
#include "type_sepastore.h"
#include "type_cutpool.h"
#include "type_branch.h"
#include "type_conflict.h"
#include "type_dialog.h"


/** SCIP main data structure */
struct Scip
{
   /* INIT */
   MEM*             mem;                /**< block memory buffers */
   SET*             set;                /**< global SCIP settings */
   INTERRUPT*       interrupt;          /**< CTRL-C interrupt data */
   DIALOGHDLR*      dialoghdlr;         /**< dialog handler for user interface */
   CLOCK*           totaltime;          /**< total SCIP running time */

   /* PROBLEM */
   STAT*            stat;               /**< dynamic problem statistics */
   PROB*            origprob;           /**< original problem data */

   /* INITSOLVE */
   EVENTFILTER*     eventfilter;        /**< event filter for global (not variable dependent) events */
   EVENTQUEUE*      eventqueue;         /**< event queue to cache events and process them later (bound change events) */
   BRANCHCAND*      branchcand;         /**< storage for branching candidates */
   TREE*            tree;               /**< branch and bound tree */
   LP*              lp;                 /**< LP data */
   PROB*            transprob;          /**< transformed problem after presolve */

   /* PRESOLVED */
   PRICESTORE*      pricestore;         /**< storage for priced variables */
   SEPASTORE*       sepastore;          /**< storage for separated cuts */
   CUTPOOL*         cutpool;            /**< global cut pool */
   CONFLICT*        conflict;           /**< conflict analysis data for propagation conflicts */
   LPCONFLICT*      lpconflict;         /**< conflict analysis data for infeasible LP conflicts */
   PSEUDOCONFLICT*  pseudoconflict;     /**< conflict analysis data for pseudo solution conflicts */
   PRIMAL*          primal;             /**< primal data and solution storage */

   STAGE            stage;              /**< SCIP operation stage */
};


#endif
