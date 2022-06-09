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

#include "index.h"
#include "indexComm.h"
#include "indexInt.h"
#include "nodes.h"
#include "querynodes.h"
#include "scalar.h"
#include "tdatablock.h"
#include "vnode.h"

// clang-format off
#define SIF_ERR_RET(c) do { int32_t _code = c; if (_code != TSDB_CODE_SUCCESS) { terrno = _code; return _code; } } while (0)
#define SIF_RET(c) do { int32_t _code = c; if (_code != TSDB_CODE_SUCCESS) { terrno = _code; } return _code; } while (0)
#define SIF_ERR_JRET(c) do { code = c; if (code != TSDB_CODE_SUCCESS) { terrno = code; goto _return; } } while (0)
// clang-format on
typedef struct SIFParam {
  SHashObj *pFilter;

  SArray *result;
  char *  condValue;

  SIdxFltStatus status;
  uint8_t       colValType;
  col_id_t      colId;
  int64_t       suid;  // add later
  char          dbName[TSDB_DB_NAME_LEN];
  char          colName[TSDB_COL_NAME_LEN * 2 + 4];

  SIndexMetaArg arg;
} SIFParam;

typedef struct SIFCtx {
  int32_t       code;
  SHashObj *    pRes;    /* element is SIFParam */
  bool          noExec;  // true: just iterate condition tree, and add hint to executor plan
  SIndexMetaArg arg;
  // SIdxFltStatus st;
} SIFCtx;

static int32_t sifGetFuncFromSql(EOperatorType src, EIndexQueryType *dst) {
  if (src == OP_TYPE_GREATER_THAN) {
    *dst = QUERY_GREATER_THAN;
  } else if (src == OP_TYPE_GREATER_EQUAL) {
    *dst = QUERY_GREATER_EQUAL;
  } else if (src == OP_TYPE_LOWER_THAN) {
    *dst = QUERY_LESS_THAN;
  } else if (src == OP_TYPE_LOWER_EQUAL) {
    *dst = QUERY_LESS_EQUAL;
  } else if (src == OP_TYPE_EQUAL) {
    *dst = QUERY_TERM;
  } else if (src == OP_TYPE_LIKE || src == OP_TYPE_MATCH || src == OP_TYPE_NMATCH) {
    *dst = QUERY_REGEX;
  } else if (src == OP_TYPE_JSON_CONTAINS) {
    *dst = QUERY_PREFIX;
  } else {
    return TSDB_CODE_QRY_INVALID_INPUT;
  }
  return TSDB_CODE_SUCCESS;
}

typedef int32_t (*sif_func_t)(SIFParam *left, SIFParam *rigth, SIFParam *output);

static sif_func_t sifNullFunc = NULL;
// typedef struct SIFWalkParm
// construct tag filter operator later
// static void destroyTagFilterOperatorInfo(void *param) {
//  STagFilterOperatorInfo *pInfo = (STagFilterOperatorInfo *)param;
//}

static void sifFreeParam(SIFParam *param) {
  if (param == NULL) return;

  taosArrayDestroy(param->result);
  taosMemoryFree(param->condValue);
  taosHashCleanup(param->pFilter);
}

static int32_t sifGetOperParamNum(EOperatorType ty) {
  if (OP_TYPE_IS_NULL == ty || OP_TYPE_IS_NOT_NULL == ty || OP_TYPE_IS_TRUE == ty || OP_TYPE_IS_NOT_TRUE == ty ||
      OP_TYPE_IS_FALSE == ty || OP_TYPE_IS_NOT_FALSE == ty || OP_TYPE_IS_UNKNOWN == ty ||
      OP_TYPE_IS_NOT_UNKNOWN == ty || OP_TYPE_MINUS == ty) {
    return 1;
  }
  return 2;
}
static int32_t sifValidateColumn(SColumnNode *cn) {
  // add more check
  if (cn == NULL) {
    return TSDB_CODE_QRY_INVALID_INPUT;
  }
  if (cn->colType != COLUMN_TYPE_TAG) {
    return TSDB_CODE_QRY_INVALID_INPUT;
  }
  return TSDB_CODE_SUCCESS;
}

static SIdxFltStatus sifMergeCond(ELogicConditionType type, SIdxFltStatus ls, SIdxFltStatus rs) {
  // enh rule later
  if (type == LOGIC_COND_TYPE_AND) {
    if (ls == SFLT_NOT_INDEX || rs == SFLT_NOT_INDEX) {
      if (ls == SFLT_NOT_INDEX)
        return rs;
      else
        return ls;
    }
    return SFLT_COARSE_INDEX;
  } else if (type == LOGIC_COND_TYPE_OR) {
    return SFLT_COARSE_INDEX;
  } else if (type == LOGIC_COND_TYPE_NOT) {
    return SFLT_NOT_INDEX;
  }
  return SFLT_NOT_INDEX;
}

static int32_t sifGetValueFromNode(SNode *node, char **value) {
  // covert data From snode;
  SValueNode *vn = (SValueNode *)node;

  char *     pData = nodesGetValueFromNode(vn);
  SDataType *pType = &vn->node.resType;
  int32_t    type = pType->type;
  int32_t    valLen = 0;

  if (IS_VAR_DATA_TYPE(type)) {
    int32_t dataLen = varDataTLen(pData);
    if (type == TSDB_DATA_TYPE_JSON) {
      if (*pData == TSDB_DATA_TYPE_NULL) {
        dataLen = 0;
      } else if (*pData == TSDB_DATA_TYPE_NCHAR) {
        dataLen = varDataTLen(pData + CHAR_BYTES);
      } else if (*pData == TSDB_DATA_TYPE_DOUBLE) {
        dataLen = LONG_BYTES;
      } else if (*pData == TSDB_DATA_TYPE_BOOL) {
        dataLen = CHAR_BYTES;
      }
      dataLen += CHAR_BYTES;
    }
    valLen = dataLen;
  } else {
    valLen = pType->bytes;
  }
  char *tv = taosMemoryCalloc(1, valLen + 1);
  if (tv == NULL) {
    return TSDB_CODE_QRY_OUT_OF_MEMORY;
  }

  memcpy(tv, pData, valLen);
  *value = tv;

  return TSDB_CODE_SUCCESS;
}

static int32_t sifInitJsonParam(SNode *node, SIFParam *param, SIFCtx *ctx) {
  SOperatorNode *nd = (SOperatorNode *)node;
  assert(nodeType(node) == QUERY_NODE_OPERATOR);
  SColumnNode *l = (SColumnNode *)nd->pLeft;
  SValueNode * r = (SValueNode *)nd->pRight;

  param->colId = l->colId;
  param->colValType = l->node.resType.type;
  memcpy(param->dbName, l->dbName, sizeof(l->dbName));
  memcpy(param->colName, r->literal, strlen(r->literal));
  // sprintf(param->colName, "%s_%s", l->colName, r->literal);
  param->colValType = r->typeData;
  return 0;
  // memcpy(param->colName, l->colName, sizeof(l->colName));
}
static int32_t sifInitParam(SNode *node, SIFParam *param, SIFCtx *ctx) {
  switch (nodeType(node)) {
    case QUERY_NODE_VALUE: {
      SValueNode *vn = (SValueNode *)node;
      SIF_ERR_RET(sifGetValueFromNode(node, &param->condValue));
      param->colId = -1;
      param->colValType = (uint8_t)(vn->node.resType.type);
      if (vn->literal == NULL || strlen(vn->literal) == 0) {
        return TSDB_CODE_QRY_INVALID_INPUT;
      }
      memcpy(param->colName, vn->literal, strlen(vn->literal));
      break;
    }
    case QUERY_NODE_COLUMN: {
      SColumnNode *cn = (SColumnNode *)node;
      /*only support tag column*/
      SIF_ERR_RET(sifValidateColumn(cn));

      param->colId = cn->colId;
      param->colValType = cn->node.resType.type;
      memcpy(param->dbName, cn->dbName, sizeof(cn->dbName));
      memcpy(param->colName, cn->colName, sizeof(cn->colName));
      break;
    }
    case QUERY_NODE_NODE_LIST: {
      SNodeListNode *nl = (SNodeListNode *)node;
      if (LIST_LENGTH(nl->pNodeList) <= 0) {
        indexError("invalid length for node:%p, length: %d", node, LIST_LENGTH(nl->pNodeList));
        SIF_ERR_RET(TSDB_CODE_QRY_INVALID_INPUT);
      }
      SIF_ERR_RET(scalarGenerateSetFromList((void **)&param->pFilter, node, nl->dataType.type));
      if (taosHashPut(ctx->pRes, &node, POINTER_BYTES, param, sizeof(*param))) {
        taosHashCleanup(param->pFilter);
        indexError("taosHashPut nodeList failed, size:%d", (int32_t)sizeof(*param));
        SIF_ERR_RET(TSDB_CODE_QRY_OUT_OF_MEMORY);
      }
      break;
    }
    case QUERY_NODE_FUNCTION:
    case QUERY_NODE_OPERATOR:
    case QUERY_NODE_LOGIC_CONDITION: {
      SIFParam *res = (SIFParam *)taosHashGet(ctx->pRes, &node, POINTER_BYTES);
      if (NULL == res) {
        indexError("no result for node, type:%d, node:%p", nodeType(node), node);
        SIF_ERR_RET(TSDB_CODE_QRY_APP_ERROR);
      }
      *param = *res;
      break;
    }
    default:
      break;
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t sifInitOperParams(SIFParam **params, SOperatorNode *node, SIFCtx *ctx) {
  int32_t code = 0;
  int32_t nParam = sifGetOperParamNum(node->opType);
  if (NULL == node->pLeft || (nParam == 2 && NULL == node->pRight)) {
    indexError("invalid operation node, left: %p, rigth: %p", node->pLeft, node->pRight);
    SIF_ERR_RET(TSDB_CODE_QRY_INVALID_INPUT);
  }
  if (node->opType == OP_TYPE_JSON_GET_VALUE) {
    return code;
  }
  SIFParam *paramList = taosMemoryCalloc(nParam, sizeof(SIFParam));
  if (NULL == paramList) {
    SIF_ERR_RET(TSDB_CODE_QRY_OUT_OF_MEMORY);
  }

  if (nodeType(node->pLeft) == QUERY_NODE_OPERATOR) {
    SNode *interNode = (node->pLeft);
    SIF_ERR_JRET(sifInitJsonParam(interNode, &paramList[0], ctx));
    if (nParam > 1) {
      SIF_ERR_JRET(sifInitParam(node->pRight, &paramList[1], ctx));
    }
    paramList[0].colValType = TSDB_DATA_TYPE_JSON;
    *params = paramList;
    return TSDB_CODE_SUCCESS;
  } else {
    SIF_ERR_JRET(sifInitParam(node->pLeft, &paramList[0], ctx));
    if (nParam > 1) {
      SIF_ERR_JRET(sifInitParam(node->pRight, &paramList[1], ctx));
    }
    *params = paramList;
    return TSDB_CODE_SUCCESS;
  }
_return:
  taosMemoryFree(paramList);
  SIF_RET(code);
}
static int32_t sifInitParamList(SIFParam **params, SNodeList *nodeList, SIFCtx *ctx) {
  int32_t   code = 0;
  SIFParam *tParams = taosMemoryCalloc(nodeList->length, sizeof(SIFParam));
  if (tParams == NULL) {
    indexError("failed to calloc, nodeList: %p", nodeList);
    SIF_ERR_RET(TSDB_CODE_QRY_OUT_OF_MEMORY);
  }

  SListCell *cell = nodeList->pHead;
  for (int32_t i = 0; i < nodeList->length; i++) {
    if (NULL == cell || NULL == cell->pNode) {
      SIF_ERR_JRET(TSDB_CODE_QRY_INVALID_INPUT);
    }
    SIF_ERR_JRET(sifInitParam(cell->pNode, &tParams[i], ctx));
    cell = cell->pNext;
  }
  *params = tParams;
  return TSDB_CODE_SUCCESS;

_return:
  taosMemoryFree(tParams);
  SIF_RET(code);
}
static int32_t sifExecFunction(SFunctionNode *node, SIFCtx *ctx, SIFParam *output) {
  indexError("index-filter not support buildin function");
  return TSDB_CODE_QRY_INVALID_INPUT;
}

typedef int (*Filter)(void *a, void *b, int16_t dtype);

int sifGreaterThan(void *a, void *b, int16_t dtype) {
  __compar_fn_t func = getComparFunc(dtype, 0);
  return tDoCompare(func, QUERY_GREATER_THAN, a, b);
}
int sifGreaterEqual(void *a, void *b, int16_t dtype) {
  __compar_fn_t func = getComparFunc(dtype, 0);
  return tDoCompare(func, QUERY_GREATER_EQUAL, a, b);
}
int sifLessEqual(void *a, void *b, int16_t dtype) {
  __compar_fn_t func = getComparFunc(dtype, 0);
  return tDoCompare(func, QUERY_LESS_EQUAL, a, b);
}
int sifLessThan(void *a, void *b, int16_t dtype) {
  __compar_fn_t func = getComparFunc(dtype, 0);
  return (int)tDoCompare(func, QUERY_LESS_THAN, a, b);
}
int sifEqual(void *a, void *b, int16_t dtype) {
  __compar_fn_t func = getComparFunc(dtype, 0);
  //__compar_fn_t func = indexGetCompar(dtype);
  return (int)tDoCompare(func, QUERY_TERM, a, b);
}
static Filter sifGetFilterFunc(EIndexQueryType type, bool *reverse) {
  if (type == QUERY_LESS_EQUAL || type == QUERY_LESS_THAN) {
    *reverse = true;
  } else {
    *reverse = false;
  }
  if (type == QUERY_LESS_EQUAL)
    return sifLessEqual;
  else if (type == QUERY_LESS_THAN)
    return sifLessThan;
  else if (type == QUERY_GREATER_EQUAL)
    return sifGreaterEqual;
  else if (type == QUERY_GREATER_THAN)
    return sifGreaterThan;
  else if (type == QUERY_TERM) {
    return sifEqual;
  }
  return NULL;
}
static int32_t sifDoIndex(SIFParam *left, SIFParam *right, int8_t operType, SIFParam *output) {
  int ret = 0;

  SIndexMetaArg * arg = &output->arg;
  EIndexQueryType qtype = 0;
  SIF_ERR_RET(sifGetFuncFromSql(operType, &qtype));
  if (left->colValType == TSDB_DATA_TYPE_JSON) {
    SIndexTerm *tm = indexTermCreate(arg->suid, DEFAULT, right->colValType, left->colName, strlen(left->colName),
                                     right->condValue, strlen(right->condValue));
    if (tm == NULL) {
      return TSDB_CODE_QRY_OUT_OF_MEMORY;
    }

    SIndexMultiTermQuery *mtm = indexMultiTermQueryCreate(MUST);
    indexMultiTermQueryAdd(mtm, tm, qtype);
    ret = tIndexJsonSearch(arg->ivtIdx, mtm, output->result);
  } else {
    bool   reverse;
    Filter filterFunc = sifGetFilterFunc(qtype, &reverse);

    SMetaFltParam param = {.suid = arg->suid,
                           .cid = left->colId,
                           .type = left->colValType,
                           .val = right->condValue,
                           .reverse = reverse,
                           .filterFunc = filterFunc};

    ret = metaFilteTableIds(arg->metaEx, &param, output->result);
  }
  return ret;
}

static int32_t sifLessThanFunc(SIFParam *left, SIFParam *right, SIFParam *output) {
  int id = OP_TYPE_LOWER_THAN;
  return sifDoIndex(left, right, id, output);
}
static int32_t sifLessEqualFunc(SIFParam *left, SIFParam *right, SIFParam *output) {
  int id = OP_TYPE_LOWER_EQUAL;
  return sifDoIndex(left, right, id, output);
}

static int32_t sifGreaterThanFunc(SIFParam *left, SIFParam *right, SIFParam *output) {
  int id = OP_TYPE_GREATER_THAN;
  return sifDoIndex(left, right, id, output);
}
static int32_t sifGreaterEqualFunc(SIFParam *left, SIFParam *right, SIFParam *output) {
  int id = OP_TYPE_GREATER_EQUAL;
  return sifDoIndex(left, right, id, output);
}

static int32_t sifEqualFunc(SIFParam *left, SIFParam *right, SIFParam *output) {
  int id = OP_TYPE_EQUAL;
  return sifDoIndex(left, right, id, output);
}
static int32_t sifNotEqualFunc(SIFParam *left, SIFParam *right, SIFParam *output) {
  int id = OP_TYPE_NOT_EQUAL;
  return sifDoIndex(left, right, id, output);
}
static int32_t sifInFunc(SIFParam *left, SIFParam *right, SIFParam *output) {
  int id = OP_TYPE_IN;
  return sifDoIndex(left, right, id, output);
}
static int32_t sifNotInFunc(SIFParam *left, SIFParam *right, SIFParam *output) {
  int id = OP_TYPE_NOT_IN;
  return sifDoIndex(left, right, id, output);
}
static int32_t sifLikeFunc(SIFParam *left, SIFParam *right, SIFParam *output) {
  int id = OP_TYPE_LIKE;
  return sifDoIndex(left, right, id, output);
}
static int32_t sifNotLikeFunc(SIFParam *left, SIFParam *right, SIFParam *output) {
  int id = OP_TYPE_NOT_LIKE;
  return sifDoIndex(left, right, id, output);
}

static int32_t sifMatchFunc(SIFParam *left, SIFParam *right, SIFParam *output) {
  int id = OP_TYPE_MATCH;
  return sifDoIndex(left, right, id, output);
}
static int32_t sifNotMatchFunc(SIFParam *left, SIFParam *right, SIFParam *output) {
  int id = OP_TYPE_NMATCH;
  return sifDoIndex(left, right, id, output);
}
static int32_t sifJsonContains(SIFParam *left, SIFParam *right, SIFParam *output) {
  int id = OP_TYPE_JSON_CONTAINS;
  return sifDoIndex(left, right, id, output);
}
static int32_t sifJsonGetValue(SIFParam *left, SIFParam *rigth, SIFParam *output) {
  // return 0
  return 0;
}

static int32_t sifDefaultFunc(SIFParam *left, SIFParam *right, SIFParam *output) {
  // add more except
  return TSDB_CODE_QRY_INVALID_INPUT;
}

static int32_t sifGetOperFn(int32_t funcId, sif_func_t *func, SIdxFltStatus *status) {
  // impl later
  *status = SFLT_ACCURATE_INDEX;
  switch (funcId) {
    case OP_TYPE_GREATER_THAN:
      *func = sifGreaterThanFunc;
      return 0;
    case OP_TYPE_GREATER_EQUAL:
      *func = sifGreaterEqualFunc;
      return 0;
    case OP_TYPE_LOWER_THAN:
      *func = sifLessThanFunc;
      return 0;
    case OP_TYPE_LOWER_EQUAL:
      *func = sifLessEqualFunc;
      return 0;
    case OP_TYPE_EQUAL:
      *func = sifEqualFunc;
      return 0;
    case OP_TYPE_NOT_EQUAL:
      *status = SFLT_NOT_INDEX;
      *func = sifNotEqualFunc;
      return 0;
    case OP_TYPE_IN:
      *status = SFLT_NOT_INDEX;
      *func = sifInFunc;
      return 0;
    case OP_TYPE_NOT_IN:
      *status = SFLT_NOT_INDEX;
      *func = sifNotInFunc;
      return 0;
    case OP_TYPE_LIKE:
      *status = SFLT_NOT_INDEX;
      *func = sifLikeFunc;
      return 0;
    case OP_TYPE_NOT_LIKE:
      *status = SFLT_NOT_INDEX;
      *func = sifNotLikeFunc;
      return 0;
    case OP_TYPE_MATCH:
      *status = SFLT_NOT_INDEX;
      *func = sifMatchFunc;
      return 0;
    case OP_TYPE_NMATCH:
      *status = SFLT_NOT_INDEX;
      *func = sifNotMatchFunc;
      return 0;
    case OP_TYPE_JSON_CONTAINS:
      *status = SFLT_ACCURATE_INDEX;
      *func = sifJsonContains;
      return 0;
    case OP_TYPE_JSON_GET_VALUE:
      *status = SFLT_ACCURATE_INDEX;
      *func = sifJsonGetValue;
      return 0;
    default:
      *status = SFLT_NOT_INDEX;
      *func = sifNullFunc;
      return 0;
  }
  return 0;
}
// typedef struct filterFuncDict {

static int32_t sifExecOper(SOperatorNode *node, SIFCtx *ctx, SIFParam *output) {
  int32_t code = 0;
  int32_t nParam = sifGetOperParamNum(node->opType);
  if (nParam <= 1) {
    output->status = SFLT_NOT_INDEX;
    return code;
  }
  if (node->opType == OP_TYPE_JSON_GET_VALUE) {
    return code;
  }
  SIFParam *params = NULL;

  SIF_ERR_RET(sifInitOperParams(&params, node, ctx));
  // ugly code, refactor later
  output->arg = ctx->arg;
  sif_func_t operFn = sifNullFunc;
  code = sifGetOperFn(node->opType, &operFn, &output->status);
  if (!ctx->noExec) {
    code = operFn(&params[0], nParam > 1 ? &params[1] : NULL, output);
  }

  taosMemoryFree(params);
  return code;
}

static int32_t sifExecLogic(SLogicConditionNode *node, SIFCtx *ctx, SIFParam *output) {
  if (NULL == node->pParameterList || node->pParameterList->length <= 0) {
    indexError("invalid logic parameter list, list:%p, paramNum:%d", node->pParameterList,
               node->pParameterList ? node->pParameterList->length : 0);
    return TSDB_CODE_QRY_INVALID_INPUT;
  }

  int32_t   code = TSDB_CODE_SUCCESS;
  SIFParam *params = NULL;
  SIF_ERR_RET(sifInitParamList(&params, node->pParameterList, ctx));

  if (ctx->noExec == false) {
    for (int32_t m = 0; m < node->pParameterList->length; m++) {
      // add impl later
      if (node->condType == LOGIC_COND_TYPE_AND) {
        taosArrayAddAll(output->result, params[m].result);
      } else if (node->condType == LOGIC_COND_TYPE_OR) {
        taosArrayAddAll(output->result, params[m].result);
      } else if (node->condType == LOGIC_COND_TYPE_NOT) {
        // taosArrayAddAll(output->result, params[m].result);
      }
      taosArraySort(output->result, idxUidCompare);
      taosArrayRemoveDuplicate(output->result, idxUidCompare, NULL);
    }
  } else {
    for (int32_t m = 0; m < node->pParameterList->length; m++) {
      output->status = sifMergeCond(node->condType, output->status, params[m].status);
    }
  }
_return:
  taosMemoryFree(params);
  SIF_RET(code);
}

static EDealRes sifWalkFunction(SNode *pNode, void *context) {
  SFunctionNode *node = (SFunctionNode *)pNode;
  SIFParam       output = {.result = taosArrayInit(8, sizeof(uint64_t))};

  SIFCtx *ctx = context;
  ctx->code = sifExecFunction(node, ctx, &output);
  if (ctx->code != TSDB_CODE_SUCCESS) {
    return DEAL_RES_ERROR;
  }

  if (taosHashPut(ctx->pRes, &pNode, POINTER_BYTES, &output, sizeof(output))) {
    ctx->code = TSDB_CODE_QRY_OUT_OF_MEMORY;
    return DEAL_RES_ERROR;
  }
  return DEAL_RES_CONTINUE;
}
static EDealRes sifWalkLogic(SNode *pNode, void *context) {
  SLogicConditionNode *node = (SLogicConditionNode *)pNode;

  SIFParam output = {.result = taosArrayInit(8, sizeof(uint64_t))};

  SIFCtx *ctx = context;
  ctx->code = sifExecLogic(node, ctx, &output);
  if (ctx->code) {
    return DEAL_RES_ERROR;
  }

  if (taosHashPut(ctx->pRes, &pNode, POINTER_BYTES, &output, sizeof(output))) {
    ctx->code = TSDB_CODE_QRY_OUT_OF_MEMORY;
    return DEAL_RES_ERROR;
  }
  return DEAL_RES_CONTINUE;
}
static EDealRes sifWalkOper(SNode *pNode, void *context) {
  SOperatorNode *node = (SOperatorNode *)pNode;
  SIFParam       output = {.result = taosArrayInit(8, sizeof(uint64_t))};

  SIFCtx *ctx = context;
  ctx->code = sifExecOper(node, ctx, &output);
  if (ctx->code) {
    return DEAL_RES_ERROR;
  }
  if (taosHashPut(ctx->pRes, &pNode, POINTER_BYTES, &output, sizeof(output))) {
    ctx->code = TSDB_CODE_QRY_OUT_OF_MEMORY;
    return DEAL_RES_ERROR;
  }

  return DEAL_RES_CONTINUE;
}

EDealRes sifCalcWalker(SNode *node, void *context) {
  if (QUERY_NODE_VALUE == nodeType(node) || QUERY_NODE_NODE_LIST == nodeType(node) ||
      QUERY_NODE_COLUMN == nodeType(node)) {
    return DEAL_RES_CONTINUE;
  }
  SIFCtx *ctx = (SIFCtx *)context;
  if (QUERY_NODE_FUNCTION == nodeType(node)) {
    return sifWalkFunction(node, ctx);
  }
  if (QUERY_NODE_LOGIC_CONDITION == nodeType(node)) {
    return sifWalkLogic(node, ctx);
  }

  if (QUERY_NODE_OPERATOR == nodeType(node)) {
    // indexInfo("node type for index filter, type: %d", nodeType(node));
    return sifWalkOper(node, ctx);
  }

  // indexError("invalid node type for index filter calculating, type:%d", nodeType(node));
  ctx->code = TSDB_CODE_QRY_INVALID_INPUT;
  return DEAL_RES_ERROR;
}

void sifFreeRes(SHashObj *res) {
  void *pIter = taosHashIterate(res, NULL);
  while (pIter) {
    SIFParam *p = pIter;
    if (p) {
      sifFreeParam(p);
    }
    pIter = taosHashIterate(res, pIter);
  }
  taosHashCleanup(res);
}
static int32_t sifCalculate(SNode *pNode, SIFParam *pDst) {
  if (pNode == NULL || pDst == NULL) {
    return TSDB_CODE_QRY_INVALID_INPUT;
  }
  int32_t code = 0;
  SIFCtx  ctx = {.code = 0, .noExec = false, .arg = pDst->arg};
  ctx.pRes = taosHashInit(4, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BIGINT), false, HASH_NO_LOCK);

  if (NULL == ctx.pRes) {
    indexError("index-filter failed to taosHashInit");
    return TSDB_CODE_QRY_OUT_OF_MEMORY;
  }

  nodesWalkExprPostOrder(pNode, sifCalcWalker, &ctx);
  SIF_ERR_RET(ctx.code);

  if (pDst) {
    SIFParam *res = (SIFParam *)taosHashGet(ctx.pRes, (void *)&pNode, POINTER_BYTES);
    if (res == NULL) {
      indexError("no valid res in hash, node:(%p), type(%d)", (void *)&pNode, nodeType(pNode));
      SIF_ERR_RET(TSDB_CODE_QRY_APP_ERROR);
    }
    if (res->result != NULL) {
      taosArrayAddAll(pDst->result, res->result);
    }

    sifFreeParam(res);
    taosHashRemove(ctx.pRes, (void *)&pNode, POINTER_BYTES);
  }
  SIF_RET(code);
}

static int32_t sifGetFltHint(SNode *pNode, SIdxFltStatus *status) {
  int32_t code = TSDB_CODE_SUCCESS;
  if (pNode == NULL) {
    return TSDB_CODE_QRY_INVALID_INPUT;
  }

  SIFCtx ctx = {.code = 0, .noExec = true};
  ctx.pRes = taosHashInit(4, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BIGINT), false, HASH_NO_LOCK);
  if (NULL == ctx.pRes) {
    indexError("index-filter failed to taosHashInit");
    return TSDB_CODE_QRY_OUT_OF_MEMORY;
  }

  nodesWalkExprPostOrder(pNode, sifCalcWalker, &ctx);

  SIF_ERR_RET(ctx.code);

  SIFParam *res = (SIFParam *)taosHashGet(ctx.pRes, (void *)&pNode, POINTER_BYTES);
  if (res == NULL) {
    indexError("no valid res in hash, node:(%p), type(%d)", (void *)&pNode, nodeType(pNode));
    SIF_ERR_RET(TSDB_CODE_QRY_APP_ERROR);
  }
  *status = res->status;

  sifFreeParam(res);
  taosHashRemove(ctx.pRes, (void *)&pNode, POINTER_BYTES);

  SIF_RET(code);
}

int32_t doFilterTag(const SNode *pFilterNode, SIndexMetaArg *metaArg, SArray *result) {
  if (pFilterNode == NULL) {
    return TSDB_CODE_SUCCESS;
  }

  SFilterInfo *filter = NULL;
  // todo move to the initialization function
  // SIF_ERR_RET(filterInitFromNode((SNode *)pFilterNode, &filter, 0));

  SArray * output = taosArrayInit(8, sizeof(uint64_t));
  SIFParam param = {.arg = *metaArg, .result = output};
  SIF_ERR_RET(sifCalculate((SNode *)pFilterNode, &param));

  taosArrayAddAll(result, param.result);
  // taosArrayAddAll(result, param.result);
  sifFreeParam(&param);
  SIF_RET(TSDB_CODE_SUCCESS);
}

SIdxFltStatus idxGetFltStatus(SNode *pFilterNode) {
  SIdxFltStatus st = SFLT_NOT_INDEX;
  if (pFilterNode == NULL) {
    return SFLT_NOT_INDEX;
  }
  // SFilterInfo *filter = NULL;
  // todo move to the initialization function
  // SIF_ERR_RET(filterInitFromNode((SNode *)pFilterNode, &filter, 0));

  SIF_ERR_RET(sifGetFltHint((SNode *)pFilterNode, &st));
  return st;
}