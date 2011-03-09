/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2011 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the ZIB Academic License.         */
/*                                                                           */
/*  You should have received a copy of the ZIB Academic License              */
/*  along with SCIP; see the file COPYING. If not email to scip@zib.de.      */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/**@file cons_disjunction.c
 * @brief  constraint handler for disjunction constraints
 * @author Stefan Heinz
 * @author Michael Winkler
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#ifndef __SCIP_CONS_DISJUNCTION_H__
#define __SCIP_CONS_DISJUNCTION_H__


#include "scip/scip.h"

#ifdef __cplusplus
extern "C" {
#endif

/** creates the handler for disjunction constraints and includes it in SCIP */
extern
SCIP_RETCODE SCIPincludeConshdlrDisjunction(
   SCIP*                 scip                /**< SCIP data structure */
   );

/** creates and captures a disjunction constraint */
extern
SCIP_RETCODE SCIPcreateConsDisjunction(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_CONS**           cons,               /**< pointer to hold the created constraint */
   const char*           name,               /**< name of constraint */
   int                   nconss,             /**< number of initial constraints in disjunction */
   SCIP_CONS**           conss,              /**< initial constraint in disjunction */
   SCIP_Bool             initial,            /**< should the LP relaxation of constraint be in the initial LP?
                                              *   Usually set to TRUE. Set to FALSE for 'lazy constraints'. */
   SCIP_Bool             enforce,            /**< should the constraint be enforced during node processing?
                                              *   TRUE for model constraints, FALSE for additional, redundant constraints. */
   SCIP_Bool             check,              /**< should the constraint be checked for feasibility?
                                              *   TRUE for model constraints, FALSE for additional, redundant constraints. */
   SCIP_Bool             local,              /**< is constraint only valid locally?
                                              *   Usually set to FALSE. Has to be set to TRUE, e.g., for branching constraints. */
   SCIP_Bool             modifiable,         /**< is constraint modifiable (subject to column generation)?
                                              *   Usually set to FALSE. In column generation applications, set to TRUE if pricing
                                              *   adds coefficients to this constraint. */
   SCIP_Bool             dynamic             /**< is constraint subject to aging?
                                              *   Usually set to FALSE. Set to TRUE for own cuts which 
                                              *   are seperated as constraints. */
   );

/** adds constraint to the disjunction of constraints */
extern
SCIP_RETCODE SCIPaddConsElemDisjunction(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_CONS*            cons,               /**< disjunction constraint */
   SCIP_CONS*            addcons             /**< additional constraint in disjunction */
   );

#ifdef __cplusplus
}
#endif

#endif
