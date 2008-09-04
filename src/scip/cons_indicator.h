/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2008 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the ZIB Academic License.         */
/*                                                                           */
/*  You should have received a copy of the ZIB Academic License              */
/*  along with SCIP; see the file COPYING. If not email to scip@zib.de.      */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#pragma ident "@(#) $Id: cons_indicator.h,v 1.7 2008/09/04 13:42:38 bzfheinz Exp $"

/**@file   cons_indicator.h
 * @brief  constraint handler for indicator constraints
 * @author Marc Pfetsch
 *
 * An indicator constraint is given by a binary variable \f$z\f$ and an inequality \f$ax \leq
 * b\f$. It states that if \f$z = 1\f$ then \f$ax \leq b\f$ holds.
 *
 * This constraint is handled by adding a slack variable \f$s:\; ax - s \leq b\f$ with \f$s \geq
 * 0\f$. The constraint is enforced by fixing \f$s\f$ to 0 if \f$z = 1\f$.
 *
 * @note The constraint only implements an implication not an equivalence, i.e., it does not ensure
 * that \f$z = 1\f$ if \f$ax \leq b\f$ or equivalently if \f$s = 0\f$ holds.
 *
 * This constraint is equivalent to a linear constraint \f$ax - s \leq b\f$ and an SOS1 constraint on
 * \f$z\f$ and \f$s\f$ (at most one should be nonzero). In the indicator context we can, however,
 * separate more inequalities.
 *
 * The name indicator apparently comes from ILOG CPLEX.
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#ifndef __SCIP_CONS_INDICATOR_H__
#define __SCIP_CONS_INDICATOR_H__


#include "scip/scip.h"


/** creates the handler for indicator constraints and includes it in SCIP */
extern
SCIP_RETCODE SCIPincludeConshdlrIndicator(
   SCIP*                 scip                /**< SCIP data structure */
   );

/** creates and captures an indicator constraint */
extern
SCIP_RETCODE SCIPcreateConsIndicator(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_CONS**           cons,               /**< pointer to hold the created constraint */
   const char*           name,               /**< name of constraint */
   SCIP_VAR*             binvar,             /**< binary indicator variable */
   int                   nvars,              /**< number of variables in the inequality */
   SCIP_VAR**            vars,               /**< array with variables of inequality */
   SCIP_Real*            vals,               /**< values of variables in inequality */
   SCIP_Real             rhs,                /**< rhs of the inequality */
   SCIP_Bool             initial,            /**< should the LP relaxation of constraint be in the initial LP? Usually set to TRUE. */
   SCIP_Bool             separate,           /**< should the constraint be separated during LP processing?
                                              *   Usually set to TRUE. */
   SCIP_Bool             enforce,            /**< should the constraint be enforced during node processing?
                                              *   TRUE for model constraints, FALSE for additional, redundant constraints. */
   SCIP_Bool             check,              /**< should the constraint be checked for feasibility?
                                              *   TRUE for model constraints, FALSE for additional, redundant constraints. */
   SCIP_Bool             propagate,          /**< should the constraint be propagated during node processing?
                                              *   Usually set to TRUE. */
   SCIP_Bool             local,              /**< is constraint only valid locally?
                                              *   Usually set to FALSE. Has to be set to TRUE, e.g., for branching constraints. */
   SCIP_Bool             dynamic,            /**< is constraint subject to aging?
                                              *   Usually set to FALSE. Set to TRUE for own cuts which
                                              *   are seperated as constraints. */
   SCIP_Bool             removable,          /**< should the relaxation be removed from the LP due to aging or cleanup?
                                              *   Usually set to FALSE. Set to TRUE for 'lazy constraints' and 'user cuts'. */
   SCIP_Bool             stickingatnode      /**< should the constraint always be kept at the node where it was added, even
                                              *   if it may be moved to a more global node?
                                              *   Usually set to FALSE. Set to TRUE to for constraints that represent node data. */
   );


/** adds variable to the inequality of the indicator constraint */
extern
SCIP_RETCODE SCIPaddVarIndicator(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_CONS*            cons,               /**< constraint */
   SCIP_VAR*             var,                /**< variable to add to the inequality */
   SCIP_Real             val                 /**< value of variable */
   );

/** gets the linear constraint corresponding to the indicator constraint */
extern
SCIP_CONS* SCIPgetLinearConsIndicator(
   SCIP_CONS*            cons                /**< constraint */
   );

/** gets binary variable corresponding to indicator constraint */
extern
SCIP_VAR* SCIPgetBinaryVarIndicator(
   SCIP_CONS*            cons                /**< constraint */
   );

/** gets slack variable corresponding to indicator constraint */
extern
SCIP_VAR* SCIPgetSlackVarIndicator(
   SCIP_CONS*            cons                /**< constraint */
   );

/** checks whether indicator constraint is violated w.r.t. sol */
extern
SCIP_Bool SCIPisViolatedIndicator(
   SCIP*                 scip,               /**< SCIP data structure */
   SCIP_CONS*            cons,               /**< constraint */
   SCIP_SOL*             sol                 /**< solution, or NULL to use current node's solution */
   );

#endif
