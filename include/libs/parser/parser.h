/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _TD_PARSER_H_
#define _TD_PARSER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "parsenodes.h"

typedef struct SParseContext {
  SParseBasicCtx   ctx;
  int8_t           schemaAttached; // denote if submit block is built with table schema or not
  const char      *pSql;           // sql string
  size_t           sqlLen;         // length of the sql string
  char            *pMsg;           // extended error message if exists to help identifying the problem in sql statement.
  int32_t          msgLen;         // max length of the msg
} SParseContext;

/**
 * Parse the sql statement and then return the SQueryStmtInfo as the result of bounded AST.
 * @param pSql     sql string
 * @param length   length of the sql string
 * @param id       operator id, generated by uuid generator
 * @param msg      extended error message if exists.
 * @return         error code
 */
int32_t qParseQuerySql(SParseContext* pContext, SQueryNode** pQuery);

bool qIsDdlQuery(const SQueryNode* pQuery);

void qDestroyQuery(SQueryNode* pQueryNode);

/**
 * Convert a normal sql statement to only query tags information to enable that the subscribe client can be aware quickly of the true vgroup ids that
 * involved in the subscribe procedure.
 * @param pSql
 * @param length
 * @param pConvertSql
 * @return
 */
int32_t qParserConvertSql(const char* pStr, size_t length, char** pConvertSql);

void assignExprInfo(SExprInfo* dst, const SExprInfo* src);
void columnListCopy(SArray* dst, const SArray* src, uint64_t uid);
void columnListDestroy(SArray* pColumnList);

void dropAllExprInfo(SArray** pExprInfo, int32_t numOfLevel);

typedef struct SSourceParam {
  SArray            *pExprNodeList; //Array<struct tExprNode*>
  SArray            *pColumnList;   //Array<struct SColumn>
  int32_t    num;
} SSourceParam;

SExprInfo* createExprInfo(STableMetaInfo* pTableMetaInfo, const char* funcName, SSourceParam* pSource, SSchema* pResSchema, int16_t interSize);
int32_t copyExprInfoList(SArray* dst, const SArray* src, uint64_t uid, bool deepcopy);
int32_t copyAllExprInfo(SArray* dst, const SArray* src, bool deepcopy);
int32_t getExprFunctionLevel(const SQueryStmtInfo* pQueryInfo);

STableMetaInfo* getMetaInfo(const SQueryStmtInfo* pQueryInfo, int32_t tableIndex);
SSchema *getOneColumnSchema(const STableMeta* pTableMeta, int32_t colIndex);

int32_t getNewResColId();
void addIntoSourceParam(SSourceParam* pSourceParam, tExprNode* pNode, SColumn* pColumn);

#ifdef __cplusplus
}
#endif

#endif /*_TD_PARSER_H_*/
