/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2005 Tobias Achterberg                              */
/*                                                                           */
/*                  2002-2005 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the SCIP Academic License.        */
/*                                                                           */
/*  You should have received a copy of the SCIP Academic License             */
/*  along with SCIP; see the file COPYING. If not email to scip@zib.de.      */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#pragma ident "@(#) $Id: debug.h,v 1.3 2005/05/17 12:03:07 bzfpfend Exp $"

/**@file   debug.h
 * @brief  methods for debugging
 * @author Tobias Achterberg
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#ifndef __DEBUG_H__
#define __DEBUG_H__

/** uncomment this define to activate debugging on given solution */
/*#define DEBUG_SOLUTION "dcmulti.origsol"*/


#include "scip/def.h"
#include "scip/type_retcode.h"
#include "scip/type_lp.h"
#include "scip/type_prob.h"


#ifdef DEBUG_SOLUTION

/** checks whether given row is valid for the debugging solution */
extern
RETCODE SCIPdebugCheckRow(
   ROW*             row,                /**< row to check for validity */
   SET*             set                 /**< global SCIP settings */
   );

/** checks whether given lower bound is valid for the debugging solution */
extern
RETCODE SCIPdebugCheckLb(
   VAR*             var,                /**< problem variable */
   SET*             set,                /**< global SCIP settings */
   Real             lb                  /**< lower bound */
   );

/** checks whether given upper bound is valid for the debugging solution */
extern
RETCODE SCIPdebugCheckUb(
   VAR*             var,                /**< problem variable */
   SET*             set,                /**< global SCIP settings */
   Real             ub                  /**< upper bound */
   );

/** checks whether given variable bound is valid for the debugging solution */
extern
RETCODE SCIPdebugCheckVbound(
   VAR*             var,                /**< problem variable x in x <= b*z + d  or  x >= b*z + d */
   SET*             set,                /**< global SCIP settings */
   BOUNDTYPE        vbtype,             /**< type of variable bound (LOWER or UPPER) */
   VAR*             vbvar,              /**< variable z    in x <= b*z + d  or  x >= b*z + d */
   Real             vbcoef,             /**< coefficient b in x <= b*z + d  or  x >= b*z + d */
   Real             vbconstant          /**< constant d    in x <= b*z + d  or  x >= b*z + d */
   );

/** checks whether given implication is valid for the debugging solution */
extern
RETCODE SCIPdebugCheckImplic(
   VAR*             var,                /**< problem variable  */
   SET*             set,                /**< global SCIP settings */
   Bool             varfixing,          /**< FALSE if y should be added in implications for x == 0, TRUE for x == 1 */
   VAR*             implvar,            /**< variable y in implication y <= b or y >= b */
   BOUNDTYPE        impltype,           /**< type       of implication y <= b (SCIP_BOUNDTYPE_UPPER) or y >= b (SCIP_BOUNDTYPE_LOWER) */
   Real             implbound           /**< bound b    in implication y <= b or y >= b */
   );

#else

#define SCIPdebugCheckRow(row,set) SCIP_OKAY
#define SCIPdebugCheckLb(var,set,lb) SCIP_OKAY
#define SCIPdebugCheckUb(var,set,ub) SCIP_OKAY
#define SCIPdebugCheckVbound(var,set,vbtype,vbvar,vbcoef,vbconstant) SCIP_OKAY
#define SCIPdebugCheckImplic(var,set,varfixing,implvar,impltype,implbound) SCIP_OKAY

#endif


#endif
