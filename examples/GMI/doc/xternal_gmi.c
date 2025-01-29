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

/**@file   xternal_gmi.c
 * @brief  main document page
 * @author Marc Pfetsch
 */

/*--+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

/**@page GMI_MAIN Gomory Mixed Integer Cut
 * @version  1.0
 * @author   Giacomo Nannicini
 * @author   Marc Pfetsch
 *
 *
 * This example provides a textbook implementation of Gomory mixed integer (GMI) cuts.
 *
 * The default implementation in SCIP does not produce GMI cuts in the strict sense, since it applies the CMIR function
 * to the aggregated row. This function can, among other things, take variable bounds into account. Thus, the resulting
 * cuts cannot be used for comparison with standard GMI cuts. This example remedies this situation.
 *
 * The implementation has been used in the paper
 *
 * G. Cornuejols, F. Margot and G. Nannicini:@n
 * On the safety of Gomory cut generators.@n
 * Math. Program. Comput. 5(4), 2013.
 *
 * Installation
 * ------------
 *
 * See the @ref INSTALL_APPLICATIONS_EXAMPLES "Install file"
 */
