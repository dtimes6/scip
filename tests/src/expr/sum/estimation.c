/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*  Copyright (c) 2002-2025 Zuse Institute Berlin (ZIB)                      */
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

/**@file   estimation.c
 * @brief  tests estimation of sums
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#include "scip/expr_sum.c"
#include "../estimation.h"

Test(estimation, sum, .init = setup, .fini = teardown,
   .description = "test separation for a sum expression"
   )
{
   SCIP_EXPR* expr;
   SCIP_Real refpoint[2] = { 0., 0. };
   SCIP_INTERVAL bounds[2];
   SCIP_Real coefs[2];
   SCIP_Real constant;
   SCIP_Bool islocal;
   SCIP_Bool success;
   SCIP_Bool branchcand[2] = { TRUE, TRUE };

   SCIP_CALL( SCIPcreateExprSum(scip, &expr, 0, NULL, NULL, 1.5, NULL, NULL) );
   SCIP_CALL( SCIPappendExprSumExpr(scip, expr, xexpr, 2.3) );
   SCIP_CALL( SCIPappendExprSumExpr(scip, expr, yexpr, -5.1) );

   SCIP_CALL( estimateSum(scip, expr, bounds, bounds, refpoint, TRUE, SCIP_INVALID, coefs, &constant, &islocal, &success, branchcand) );

   cr_expect(success);
   cr_expect_eq(coefs[0], 2.3);
   cr_expect_eq(coefs[1], -5.1);
   cr_expect_eq(constant, 1.5);
   cr_expect_not(islocal);
   cr_expect_not(branchcand[0]);
   cr_expect_not(branchcand[1]);

   SCIP_CALL( SCIPreleaseExpr(scip, &expr) );
}
