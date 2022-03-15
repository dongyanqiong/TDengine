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

#include "syncRequestVoteReply.h"
#include "syncInt.h"
#include "syncRaftStore.h"
#include "syncUtil.h"
#include "syncVoteMgr.h"

// TLA+ Spec
// HandleRequestVoteResponse(i, j, m) ==
//    \* This tallies votes even when the current state is not Candidate, but
//    \* they won't be looked at, so it doesn't matter.
//    /\ m.mterm = currentTerm[i]
//    /\ votesResponded' = [votesResponded EXCEPT ![i] =
//                              votesResponded[i] \cup {j}]
//    /\ \/ /\ m.mvoteGranted
//          /\ votesGranted' = [votesGranted EXCEPT ![i] =
//                                  votesGranted[i] \cup {j}]
//          /\ voterLog' = [voterLog EXCEPT ![i] =
//                              voterLog[i] @@ (j :> m.mlog)]
//       \/ /\ ~m.mvoteGranted
//          /\ UNCHANGED <<votesGranted, voterLog>>
//    /\ Discard(m)
//    /\ UNCHANGED <<serverVars, votedFor, leaderVars, logVars>>
//
int32_t syncNodeOnRequestVoteReplyCb(SSyncNode* ths, SyncRequestVoteReply* pMsg) {
  int32_t ret = 0;
  syncRequestVoteReplyLog2("==syncNodeOnRequestVoteReplyCb==", pMsg);

  if (pMsg->term < ths->pRaftStore->currentTerm) {
    sTrace("DropStaleResponse, receive term:%lu, current term:%lu", pMsg->term, ths->pRaftStore->currentTerm);
    return ret;
  }

  // no need this code, because if I receive reply.term, then I must have sent for that term.
  //  if (pMsg->term > ths->pRaftStore->currentTerm) {
  //    syncNodeUpdateTerm(ths, pMsg->term);
  //  }

  assert(pMsg->term == ths->pRaftStore->currentTerm);

  if (ths->state == TAOS_SYNC_STATE_CANDIDATE) {
    votesRespondAdd(ths->pVotesRespond, pMsg);
    if (pMsg->voteGranted) {
      voteGrantedVote(ths->pVotesGranted, pMsg);
      if (voteGrantedMajority(ths->pVotesGranted)) {
        if (ths->pVotesGranted->toLeader) {
          syncNodeCandidate2Leader(ths);
          ths->pVotesGranted->toLeader = true;
        }
      }
    }
  }
  return ret;
}