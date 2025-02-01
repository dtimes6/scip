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

/**@file   xml.h
 * @brief  declarations for XML parsing
 * @author Thorsten Koch
 * @author Marc Pfetsch
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#ifndef __SCIP_XML_H__
#define __SCIP_XML_H__

#ifdef __cplusplus
extern "C" {
#endif


typedef struct XML_ATTR_struct XML_ATTR;

struct XML_ATTR_struct
{
   char*                 name;
   char*                 value;
   XML_ATTR*             next;
};

typedef struct XML_NODE_struct XML_NODE;

struct XML_NODE_struct
{
   char*                 name;
   int                   lineno;
   XML_ATTR*             attrlist;
   XML_NODE*             parent;
   XML_NODE*             prevsibl;
   XML_NODE*             nextsibl;
   XML_NODE*             firstchild;
   XML_NODE*             lastchild;
   char*                 data;           /* does not come together with children */
};

/** Parse file */
SCIP_EXPORT
XML_NODE* SCIPxmlProcess(
   const char*           filename            /**< XML file name */
   );

/** create new node */
SCIP_EXPORT
XML_NODE* SCIPxmlNewNode(
   const char*           name,
   int                   lineno
   );

/** create new attribute */
SCIP_EXPORT
XML_ATTR* SCIPxmlNewAttr(
   const char*           name,
   const char*           value
   );

/** add attribute */
SCIP_EXPORT
void SCIPxmlAddAttr(
   XML_NODE*             n,
   XML_ATTR*             a
   );

/** append child node */
SCIP_EXPORT
void SCIPxmlAppendChild(
   XML_NODE*             parent,
   XML_NODE*             child
   );

/** free node */
SCIP_EXPORT
void SCIPxmlFreeNode(
   XML_NODE*             node
   );

/** output node */
SCIP_EXPORT
void SCIPxmlShowNode(
   const XML_NODE*       root
   );

/** get attribute value */
SCIP_EXPORT
const char* SCIPxmlGetAttrval(
   const XML_NODE*       node,
   const char*           name
   );

/** return first node */
SCIP_EXPORT
const XML_NODE* SCIPxmlFirstNode(
   const XML_NODE*       node,
   const char*           name
   );

/** return next node */
SCIP_EXPORT
const XML_NODE* SCIPxmlNextNode(
   const XML_NODE*       node,
   const char*           name
   );

/** find node */
SCIP_EXPORT
const XML_NODE* SCIPxmlFindNode(
   const XML_NODE*       node,
   const char*           name
   );

/** find node with bound on the depth */
SCIP_EXPORT
const XML_NODE* SCIPxmlFindNodeMaxdepth(
   const XML_NODE*       node,               /**< current node - use start node to begin */
   const char*           name,               /**< name of tag to search for */
   int                   depth,              /**< current depth - start with 0 */
   int                   maxdepth            /**< maximal depth */
   );

/** return next sibling */
SCIP_EXPORT
const XML_NODE* SCIPxmlNextSibl(
   const XML_NODE*       node
   );

/** return previous sibling */
SCIP_EXPORT
const XML_NODE* SCIPxmlPrevSibl(
   const XML_NODE*       node
   );

/** return first child */
SCIP_EXPORT
const XML_NODE* SCIPxmlFirstChild(
   const XML_NODE*       node
   );

/** return last child */
SCIP_EXPORT
const XML_NODE* SCIPxmlLastChild(
   const XML_NODE*       node
   );

/** return name of node */
SCIP_EXPORT
const char* SCIPxmlGetName(
   const XML_NODE*       node
   );

/** get line number */
SCIP_EXPORT
int SCIPxmlGetLine(
   const XML_NODE*       node
   );

/** get data */
SCIP_EXPORT
const char* SCIPxmlGetData(
   const XML_NODE*       node
   );

/** find PCDATA */
SCIP_EXPORT
const char* SCIPxmlFindPcdata(
   const XML_NODE*       node,
   const char*           name
   );

#ifdef __cplusplus
}
#endif

#endif
