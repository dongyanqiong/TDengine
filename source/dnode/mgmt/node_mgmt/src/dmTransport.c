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

#define _DEFAULT_SOURCE
#include "dmMgmt.h"
#include "qworker.h"

#define INTERNAL_USER   "_dnd"
#define INTERNAL_CKEY   "_key"
#define INTERNAL_SECRET "_pwd"

static inline int32_t dmBuildNodeMsg(SRpcMsg *pMsg, SRpcMsg *pRpc) {
  SRpcConnInfo connInfo = {0};
  if (IsReq(pRpc) && rpcGetConnInfo(pRpc->info.handle, &connInfo) != 0) {
    terrno = TSDB_CODE_MND_NO_USER_FROM_CONN;
    dError("failed to build msg since %s, app:%p handle:%p", terrstr(), pRpc->info.ahandle, pRpc->info.handle);
    return -1;
  }

  memcpy(pMsg, pRpc, sizeof(SRpcMsg));
  memcpy(pMsg->conn.user, connInfo.user, TSDB_USER_LEN);
  pMsg->conn.clientIp = connInfo.clientIp;
  pMsg->conn.clientPort = connInfo.clientPort;
  return 0;
}

int32_t dmProcessNodeMsg(SMgmtWrapper *pWrapper, SRpcMsg *pMsg) {
  NodeMsgFp msgFp = pWrapper->msgFps[TMSG_INDEX(pMsg->msgType)];
  if (msgFp == NULL) {
    terrno = TSDB_CODE_MSG_NOT_PROCESSED;
    return -1;
  }

  dTrace("msg:%p, will be processed by %s", pMsg, pWrapper->name);
  pMsg->info.wrapper = pWrapper;
  return (*msgFp)(pWrapper->pMgmt, pMsg);
}

static void dmProcessRpcMsg(SDnode *pDnode, SRpcMsg *pRpc, SEpSet *pEpSet) {
  SDnodeTrans * pTrans = &pDnode->trans;
  int32_t       code = -1;
  SRpcMsg *     pMsg = NULL;
  bool          needRelease = false;
  SDnodeHandle *pHandle = &pTrans->msgHandles[TMSG_INDEX(pRpc->msgType)];
  SMgmtWrapper *pWrapper = NULL;

  dTrace("msg:%s is received, handle:%p len:%d code:0x%x app:%p refId:%" PRId64, TMSG_INFO(pRpc->msgType),
         pRpc->info.handle, pRpc->contLen, pRpc->code, pRpc->info.ahandle, pRpc->info.refId);
  pRpc->info.noResp = 0;
  pRpc->info.persistHandle = 0;
  pRpc->info.wrapper = NULL;
  pRpc->info.node = NULL;
  pRpc->info.rsp = NULL;
  pRpc->info.rspLen = 0;

  if (pRpc->msgType == TDMT_DND_NET_TEST) {
    dmProcessNetTestReq(pDnode, pRpc);
    goto _OVER_JUST_FREE;
  } else if (pRpc->msgType == TDMT_MND_SYSTABLE_RETRIEVE_RSP || pRpc->msgType == TDMT_VND_FETCH_RSP) {
    qWorkerProcessFetchRsp(NULL, NULL, pRpc);
    goto _OVER_JUST_FREE;
  } else {
  }

  if (pDnode->status != DND_STAT_RUNNING) {
    if (pRpc->msgType == TDMT_DND_SERVER_STATUS) {
      dmProcessServerStartupStatus(pDnode, pRpc);
      goto _OVER_JUST_FREE;
    } else {
      terrno = TSDB_CODE_APP_NOT_READY;
      goto _OVER_RSP_FREE;
    }
  }

  if (IsReq(pRpc) && pRpc->pCont == NULL) {
    terrno = TSDB_CODE_INVALID_MSG_LEN;
    goto _OVER_RSP_FREE;
  }

  if (pHandle->defaultNtype == NODE_END) {
    terrno = TSDB_CODE_MSG_NOT_PROCESSED;
    goto _OVER_RSP_FREE;
  } else {
    pWrapper = &pDnode->wrappers[pHandle->defaultNtype];
    if (pHandle->needCheckVgId) {
      if (pRpc->contLen > 0) {
        SMsgHead *pHead = pRpc->pCont;
        int32_t   vgId = ntohl(pHead->vgId);
        if (vgId == QNODE_HANDLE) {
          pWrapper = &pDnode->wrappers[QNODE];
        } else if (vgId == MNODE_HANDLE) {
          pWrapper = &pDnode->wrappers[MNODE];
        } else {
        }
      } else {
        terrno = TSDB_CODE_INVALID_MSG_LEN;
        goto _OVER_RSP_FREE;
      }
    }
  }

  if (dmMarkWrapper(pWrapper) != 0) {
    goto _OVER_RSP_FREE;
  } else {
    needRelease = true;
    pRpc->info.wrapper = pWrapper;
  }

  pMsg = taosAllocateQitem(sizeof(SRpcMsg), RPC_QITEM);
  if (pMsg == NULL) {
    goto _OVER;
  }

  if (dmBuildNodeMsg(pMsg, pRpc) != 0) {
    goto _OVER;
  }

  if (InParentProc(pWrapper)) {
    code = dmPutToProcCQueue(&pWrapper->proc, pMsg, DND_FUNC_REQ);
  } else {
    code = dmProcessNodeMsg(pWrapper, pMsg);
  }

_OVER:
  if (code == 0) {
    if (pWrapper != NULL && InParentProc(pWrapper)) {
      dTrace("msg:%p, is freed after push to cqueue", pMsg);
      taosFreeQitem(pMsg);
      rpcFreeCont(pRpc->pCont);
    }
  } else {
    dError("msg:%p, failed to process since %s", pMsg, terrstr());
    if (terrno != 0) code = terrno;

    if (IsReq(pRpc)) {
      if (code == TSDB_CODE_NODE_NOT_DEPLOYED || code == TSDB_CODE_NODE_OFFLINE) {
        if (pRpc->msgType > TDMT_MND_MSG && pRpc->msgType < TDMT_VND_MSG) {
          code = TSDB_CODE_NODE_REDIRECT;
        }
      }
      SRpcMsg rspMsg = {.code = code, .info = pRpc->info};
      tmsgSendRsp(&rspMsg);
    }

    dTrace("msg:%p, is freed", pMsg);
    taosFreeQitem(pMsg);
    rpcFreeCont(pRpc->pCont);
  }

  if (needRelease) {
    dmReleaseWrapper(pWrapper);
  }
  return;

_OVER_JUST_FREE:
  rpcFreeCont(pRpc->pCont);
  return;

_OVER_RSP_FREE:
  rpcFreeCont(pRpc->pCont);
  SRpcMsg simpleRsp = {.code = terrno, .info = pRpc->info};
  rpcSendResponse(&simpleRsp);
}

int32_t dmInitMsgHandle(SDnode *pDnode) {
  SDnodeTrans *pTrans = &pDnode->trans;

  for (EDndNodeType ntype = DNODE; ntype < NODE_END; ++ntype) {
    SMgmtWrapper *pWrapper = &pDnode->wrappers[ntype];
    SArray *      pArray = (*pWrapper->func.getHandlesFp)();
    if (pArray == NULL) return -1;

    for (int32_t i = 0; i < taosArrayGetSize(pArray); ++i) {
      SMgmtHandle * pMgmt = taosArrayGet(pArray, i);
      SDnodeHandle *pHandle = &pTrans->msgHandles[TMSG_INDEX(pMgmt->msgType)];
      if (pMgmt->needCheckVgId) {
        pHandle->needCheckVgId = pMgmt->needCheckVgId;
      }
      if (!pMgmt->needCheckVgId) {
        pHandle->defaultNtype = ntype;
      }
      pWrapper->msgFps[TMSG_INDEX(pMgmt->msgType)] = pMgmt->msgFp;
    }

    taosArrayDestroy(pArray);
  }

  return 0;
}

static inline int32_t dmSendReq(const SEpSet *pEpSet, SRpcMsg *pMsg) {
  SDnode *pDnode = dmInstance();
  if (pDnode->status != DND_STAT_RUNNING) {
    rpcFreeCont(pMsg->pCont);
    pMsg->pCont = NULL;
    terrno = TSDB_CODE_NODE_OFFLINE;
    dError("failed to send rpc msg since %s, handle:%p", terrstr(), pMsg->info.handle);
    return -1;
  } else {
    rpcSendRequest(pDnode->trans.clientRpc, pEpSet, pMsg, NULL);
    return 0;
  }
}

static inline void dmSendRsp(SRpcMsg *pMsg) {
  SMgmtWrapper *pWrapper = pMsg->info.wrapper;
  if (InChildProc(pWrapper)) {
    dmPutToProcPQueue(&pWrapper->proc, pMsg, DND_FUNC_RSP);
    rpcFreeCont(pMsg->pCont);
    pMsg->pCont = NULL;
  } else {
    rpcSendResponse(pMsg);
  }
}

static inline void dmSendRedirectRsp(SRpcMsg *pMsg, const SEpSet *pNewEpSet) {
  SRpcMsg rsp = {.code = TSDB_CODE_RPC_REDIRECT, .info = pMsg->info};
  SMEpSet msg = {.epSet = *pNewEpSet};
  int32_t contLen = tSerializeSMEpSet(NULL, 0, &msg);

  rsp.pCont = rpcMallocCont(contLen);
  if (rsp.pCont == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
  } else {
    tSerializeSMEpSet(rsp.pCont, contLen, &msg);
    rsp.contLen = contLen;
  }
  dmSendRsp(&rsp);
  rpcFreeCont(pMsg->pCont);
  pMsg->pCont = NULL;
}

static inline void dmRegisterBrokenLinkArg(SRpcMsg *pMsg) {
  SMgmtWrapper *pWrapper = pMsg->info.wrapper;
  if (InChildProc(pWrapper)) {
    dmPutToProcPQueue(&pWrapper->proc, pMsg, DND_FUNC_REGIST);
    rpcFreeCont(pMsg->pCont);
    pMsg->pCont = NULL;
  } else {
    rpcRegisterBrokenLinkArg(pMsg);
  }
}

static inline void dmReleaseHandle(SRpcHandleInfo *pHandle, int8_t type) {
  SMgmtWrapper *pWrapper = pHandle->wrapper;
  if (InChildProc(pWrapper)) {
    SRpcMsg msg = {.code = type, .info = *pHandle};
    dmPutToProcPQueue(&pWrapper->proc, &msg, DND_FUNC_RELEASE);
  } else {
    rpcReleaseHandle(pHandle->handle, type);
  }
}

static bool rpcRfp(int32_t code) { return code == TSDB_CODE_RPC_REDIRECT; }

int32_t dmInitClient(SDnode *pDnode) {
  SDnodeTrans *pTrans = &pDnode->trans;

  SRpcInit rpcInit = {0};
  rpcInit.label = "DND";
  rpcInit.numOfThreads = 1;
  rpcInit.cfp = (RpcCfp)dmProcessRpcMsg;
  rpcInit.sessions = 1024;
  rpcInit.connType = TAOS_CONN_CLIENT;
  rpcInit.idleTime = tsShellActivityTimer * 1000;
  rpcInit.user = INTERNAL_USER;
  rpcInit.parent = pDnode;
  rpcInit.rfp = rpcRfp;

  pTrans->clientRpc = rpcOpen(&rpcInit);
  if (pTrans->clientRpc == NULL) {
    dError("failed to init dnode rpc client");
    return -1;
  }

  dDebug("dnode rpc client is initialized");
  return 0;
}

void dmCleanupClient(SDnode *pDnode) {
  SDnodeTrans *pTrans = &pDnode->trans;
  if (pTrans->clientRpc) {
    rpcClose(pTrans->clientRpc);
    pTrans->clientRpc = NULL;
    dDebug("dnode rpc client is closed");
  }
}

int32_t dmInitServer(SDnode *pDnode) {
  SDnodeTrans *pTrans = &pDnode->trans;

  SRpcInit rpcInit = {0};
  strncpy(rpcInit.localFqdn, tsLocalFqdn, strlen(tsLocalFqdn));
  rpcInit.localPort = tsServerPort;
  rpcInit.label = "DND";
  rpcInit.numOfThreads = tsNumOfRpcThreads;
  rpcInit.cfp = (RpcCfp)dmProcessRpcMsg;
  rpcInit.sessions = tsMaxShellConns;
  rpcInit.connType = TAOS_CONN_SERVER;
  rpcInit.idleTime = tsShellActivityTimer * 1000;
  rpcInit.parent = pDnode;

  pTrans->serverRpc = rpcOpen(&rpcInit);
  if (pTrans->serverRpc == NULL) {
    dError("failed to init dnode rpc server");
    return -1;
  }

  dDebug("dnode rpc server is initialized");
  return 0;
}

void dmCleanupServer(SDnode *pDnode) {
  SDnodeTrans *pTrans = &pDnode->trans;
  if (pTrans->serverRpc) {
    rpcClose(pTrans->serverRpc);
    pTrans->serverRpc = NULL;
    dDebug("dnode rpc server is closed");
  }
}

SMsgCb dmGetMsgcb(SDnode *pDnode) {
  SMsgCb msgCb = {
      .clientRpc = pDnode->trans.clientRpc,
      .sendReqFp = dmSendReq,
      .sendRspFp = dmSendRsp,
      .sendRedirectRspFp = dmSendRedirectRsp,
      .registerBrokenLinkArgFp = dmRegisterBrokenLinkArg,
      .releaseHandleFp = dmReleaseHandle,
      .reportStartupFp = dmReportStartup,
  };
  return msgCb;
}

static void dmSendMnodeRedirectRsp(SRpcMsg *pMsg) {
  SDnode *pDnode = dmInstance();
  SEpSet  epSet = {0};
  dmGetMnodeEpSet(&pDnode->data, &epSet);

  dDebug("msg:%p, is redirected, num:%d use:%d", pMsg, epSet.numOfEps, epSet.inUse);
  for (int32_t i = 0; i < epSet.numOfEps; ++i) {
    dDebug("mnode index:%d %s:%u", i, epSet.eps[i].fqdn, epSet.eps[i].port);
    if (strcmp(epSet.eps[i].fqdn, tsLocalFqdn) == 0 && epSet.eps[i].port == tsServerPort) {
      epSet.inUse = (i + 1) % epSet.numOfEps;
    }

    epSet.eps[i].port = htons(epSet.eps[i].port);
  }

  SRpcMsg rsp = {.code = TSDB_CODE_RPC_REDIRECT, .info = pMsg->info};
  SMEpSet msg = {.epSet = epSet};
  int32_t contLen = tSerializeSMEpSet(NULL, 0, &msg);
  rsp.pCont = rpcMallocCont(contLen);
  if (rsp.pCont == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
  } else {
    tSerializeSMEpSet(rsp.pCont, contLen, &msg);
    rsp.contLen = contLen;
  }

  dmSendRsp(&rsp);
  rpcFreeCont(pMsg->pCont);
  pMsg->pCont = NULL;
}