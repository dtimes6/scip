/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2018 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the ZIB Academic License.         */
/*                                                                           */
/*  You should have received a copy of the ZIB Academic License              */
/*  along with SCIP; see the file COPYING. If not visit scip.zib.de.         */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/**@file   reader_dec.c
 * @brief  file reader for decompositions in the constraint based dec-file format.
 * @author Gregor Hendel
 *
 * This reader allows to read a file containing decompositions for constraints of the current original problem. The
 * standard line ending for this format is '.dec'. The content of the file should obey the following format
 *
 *     \\ place for comments and statistics
 *     NBLOCKS
 *     2
 *     BLOCK 0
 *     consA
 *     consB
 *     [...]
 *     BLOCK 1
 *     consC
 *     consD
 *     [...]
 *     MASTERCONSS
 *     linkingcons
 *
 * A block in a problem decomposition is a set of constraints that are independent from all other blocks after removing
 * the special blocks of linking constraints denoted as MASTERCONSS.
 *
 * Imagine the following example, which involves seven variables
 * and the five constraints from the file above. The asterisks (*) indicate that the variable affects the feasibility
 * of the constraint. In the special case of a linear optimization problem, the asterisks correspond to the
 * nonzero entries of the constraint matrix.
 *
 *                     x1  x2  x3  x4  x5  x6  x7
 *            consA     *       *                 \ BLOCK 0
 *            consB     *   *                     /
 *            consC                 *   *         \ BLOCK 1
 *            consD                     *   *     /
 *     linkingconss     *   *   *   *   *   *   * > MASTERCONSS
 *
 * The nonzero pattern has been chosen in a way that after the removal of the last constraint 'linkingcons', the remaining problem
 * consists of two independent parts, namely the blocks '0' and '1'.
 *
 * The corresponding variable labels are inferred from the constraint labels. A variable is assigned the label
 *
 * - of its unique block, if it only occurs in exactly 1 named block, and probably in MASTERCONSS.
 * - the special label of a linking variable if it occurs only in the master constraints or in 2 or even more named blocks.
 *
 * @note A trivial decomposition is to assign all constraints of a problem to the MASTERCONSS.
 *
 * @note The number of blocks is the number of named blocks: a trivial decomposition should have 0 blocks
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#include "scip/pub_fileio.h"
#include "scip/pub_message.h"
#include "scip/pub_misc.h"
#include "scip/pub_reader.h"
#include "scip/pub_var.h"
#include "scip/reader_dec.h"
#include "scip/scip_general.h"
#include "scip/scip_message.h"
#include "scip/scip_numerics.h"
#include "scip/scip_prob.h"
#include "scip/scip_reader.h"
#include "scip/scip_solve.h"
#include "scip/scip_var.h"
#include "scip/decomp.h"
#include "scip/scip_mem.h"
#include "scip/type_decomp.h"
#include <string.h>

#define READER_NAME             "decreader"
#define READER_DESC             "file reader for constraint decompositions"
#define READER_EXTENSION        "dec"

/*
 * local methods
 */

/* enumerator for the current section */
enum Dec_Section {
   DEC_SECTION_INIT      = 0,                /**< initial section before the number of blocks is specified */
   DEC_SECTION_NBLOCKS   = 1,                /**< section that contains the number of */
   DEC_SECTION_BLOCK     = 2,                /**< */
   DEC_SECTION_MASTER    = 3                 /**< */
};
typedef enum Dec_Section DEC_SECTION;

/** reads the given decomposition file */
static
SCIP_RETCODE readDecomposition(
   SCIP*                 scip,               /**< SCIP data structure */   
   const char*           filename            /**< name of the input file */
   )
{
   SCIP_FILE* file;
   SCIP_CONS** conss;
   SCIP_CONS** scip_conss;
   SCIP_Bool error;
   int lineno;
   int nblocks;
   int currblock;
   int* labels;
   int nconss;
   int consptr;
   DEC_SECTION section;

   SCIP_DECOMP* decomp;
   SCIP_DECOMPSTORE* decompstore;

   assert(scip != NULL);
   assert(filename != NULL);

   /* cannot read a decomposition after problem has been transformed */
   if( SCIPgetStage(scip) != SCIP_STAGE_PROBLEM )
   {
      SCIPwarningMessage(scip, "Cannot read decomposition after problem has been transformed.\n");

      return SCIP_OKAY;
   }

   /* open input file */
   file = SCIPfopen(filename, "r");
   if( file == NULL )
   {
      SCIPerrorMessage("cannot open file <%s> for reading\n", filename);
      SCIPprintSysError(filename);
      return SCIP_NOFILE;
   }

   decompstore = SCIPgetDecompstore(scip);
   assert(decompstore != NULL);

   /* read the file */
   error = FALSE;
   lineno = 0;
   nblocks = -1;

   /* use the number of constraints of the problem as buffer storage size */
   nconss = SCIPgetNConss(scip);

   SCIP_CALL( SCIPallocBufferArray(scip, &conss, nconss) );
   SCIP_CALL( SCIPallocBufferArray(scip, &labels, nconss) );

   /* start parsing the file */
   section = DEC_SECTION_INIT;
   consptr = 0;
   while( !SCIPfeof(file) && !error )
   {
      char buffer[SCIP_MAXSTRLEN];
      char consname[SCIP_MAXSTRLEN];
      SCIP_CONS* cons;
      int nread;

      /* get next line */
      if( SCIPfgets(buffer, (int) sizeof(buffer), file) == NULL )
         break;
      lineno++;

      /* check if a new section begins */
      if( strncmp(buffer, "NBLOCKS", 7) == 0 )
      {
         section = DEC_SECTION_NBLOCKS;
         continue;
      }
      else if( strncmp(buffer, "BLOCK", 5) == 0 )
      {
         section = DEC_SECTION_BLOCK;

         nread = sscanf(buffer, "BLOCK %1018d\n", &currblock);
         if( nread < 1 )
         {
            error = TRUE;
            break;
         }

         SCIPdebugMsg(scip, "Switching block to %d\n", currblock);
         continue;
      }
      else if( strncmp(buffer, "MASTERCONSS", 11) == 0 )
      {
         section = DEC_SECTION_MASTER;
         currblock = SCIP_DECOMP_LINKCONS;

         SCIPdebugMsg(scip, "Continuing with master constraint block.\n");

         continue;
      }

      /* base the parsing on the currently active section */
      switch (section)
      {
         case DEC_SECTION_INIT:
            break;
         case DEC_SECTION_NBLOCKS:
            /* read in number of blocks */
            assert(nblocks == -1);
            nread = sscanf(buffer, "%1024d\n", &nblocks);
            if( nread < 1 )
               error = TRUE;

            SCIPdebugMsg(scip, "Decomposition with %d number of blocks\n", nblocks);
            break;
         case DEC_SECTION_BLOCK:
         case DEC_SECTION_MASTER:
            /* read constraint name in both cases */
            nread = sscanf(buffer, "%1024s\n", consname);
            if( nread < 1 )
               error = TRUE;
            break;

         default:
            break;
      }

      if( section == DEC_SECTION_NBLOCKS || section == DEC_SECTION_INIT )
         continue;

      cons = SCIPfindCons(scip, consname);

      /* check if the constraint does not exist */
      if( cons == NULL )
      {
         SCIPwarningMessage(scip, "Constraint <%s> in line %d does not exist.\n", consname, lineno);
         continue;
      }

      /* check if buffer storage capacity has been reached, which means that there is a duplicate constraint entry */
      if( consptr == nconss )
      {
         SCIPerrorMessage("Error: Too many constraints in decomposition file: Is there a double entry?\n");
         error = TRUE;
         break;
      }

      /* store constraint and corresponding label */
      conss[consptr] = cons;
      labels[consptr] = currblock;
      ++consptr;
   }

   /* close input file */
   SCIPfclose(file);

   /* create a decomposition and add it to the decomposition storage of SCIP */
   if( ! error )
   {
      SCIP_CALL( SCIPdecompCreate(&decomp, SCIPblkmem(scip), TRUE) );

      SCIP_CALL( SCIPdecompSetConsLabels(decomp, conss, labels, consptr) );
      SCIPdebugMsg(scip, "Setting labels for %d constraints.\n", consptr);

      scip_conss = SCIPgetConss(scip);

      SCIPdebugMsg(scip, "Using %d SCIP constraints for labeling variables.\n", nconss);
      SCIP_CALL( SCIPdecompComputeVarsLabels(scip, decomp, scip_conss, nconss) );

      SCIP_CALL( SCIPdecompstoreAdd(decompstore, decomp) );


      /* display result */
      SCIPverbMessage(scip, SCIP_VERBLEVEL_NORMAL, NULL, "Added decomposition <%s> with %d blocks to SCIP\n", filename, nblocks);
   }
   else
   {
      SCIPerrorMessage("Errors parsing decomposition <%s>. No decomposition added\n.", filename);
   }


   SCIPfreeBufferArray(scip, &labels);
   SCIPfreeBufferArray(scip, &conss);

   if( error )
      return SCIP_READERROR;
   else
      return SCIP_OKAY;
}

/*
 * Callback methods of reader
 */

/** copy method for reader plugins (called when SCIP copies plugins) */
static
SCIP_DECL_READERCOPY(readerCopyDec)
{  /*lint --e{715}*/
   assert(scip != NULL);
   assert(reader != NULL);
   assert(strcmp(SCIPreaderGetName(reader), READER_NAME) == 0);

   /* call inclusion method of reader */
   SCIP_CALL( SCIPincludeReaderDec(scip) );

   return SCIP_OKAY;
}

/** problem reading method of reader */
static
SCIP_DECL_READERREAD(readerReadDec)
{  /*lint --e{715}*/
   assert(reader != NULL);
   assert(strcmp(SCIPreaderGetName(reader), READER_NAME) == 0);
   assert(result != NULL);

   *result = SCIP_DIDNOTRUN;

   if( SCIPgetStage(scip) < SCIP_STAGE_PROBLEM )
   {
      SCIPerrorMessage("reading of decomposition file is only possible after a problem was created\n");
      return SCIP_READERROR;
   }

   SCIP_CALL( readDecomposition(scip, filename) );

   *result = SCIP_SUCCESS;

   return SCIP_OKAY;
}

/*
 * dec file reader specific interface methods
 */

/** includes the dec file reader in SCIP */
SCIP_RETCODE SCIPincludeReaderDec(
   SCIP*                 scip                /**< SCIP data structure */
   )
{
   SCIP_READER* reader;

   /* include reader */
   SCIP_CALL( SCIPincludeReaderBasic(scip, &reader, READER_NAME, READER_DESC, READER_EXTENSION, NULL) );

   /* set non fundamental callbacks via setter functions */
   SCIP_CALL( SCIPsetReaderCopy(scip, reader, readerCopyDec) );
   SCIP_CALL( SCIPsetReaderRead(scip, reader, readerReadDec) );

   return SCIP_OKAY;
}
