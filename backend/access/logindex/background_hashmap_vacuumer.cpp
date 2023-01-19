//
// Created by pang65 on 1/14/23.
//

#include "c.h"
#include "postgres.h"
#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>
#include <iostream>
#include "access/logindex_hashmap.h"
#include <atomic>
#include "storage/kv_interface.h"
#include "tcop/storage_server.h"
#include <sys/time.h>
#include <pthread.h>
#include <cstdlib>
#include <access/background_hashmap_vacuumer.h>

#define ITER_BATCH_SIZE 10
#define MAX_REPLAY_VERSION_SIZE 20

void VacuumHashNode(HashNodeHead* head, HashNodeEle* ele, BufferTag bufferTag);
void BackgroundReplayHeadNode(HashNodeHead * head);

//#define ENABLE_DEBUG_INFO2

//#define ENABLE_DEBUG_INFO
// Use try_rdlock to get a bucketLock, if failed, iterate to next bucket immediately
// Then iterate head list, when try_wrlock head lock successfully, vacuum that head node list
bool BackgroundHashMapCleanRocksdb(HashMap hashMap) {

    int currentBucketID = gettid() % hashMap->bucketNum;
    HashNodeHead *headNodes[ITER_BATCH_SIZE];
    int recordNumber = 0;
    struct timeval now;

    while(1) { // Iterate all buckets
        usleep(1000);
        // Iterate every bucket one by one
        currentBucketID = (currentBucketID+1) % hashMap->bucketNum;
        // How many heads we have processed
        int currentFinishHeadNum = 0;
        // flag: finish iterate all the nodes in this bucket
        int finishIterThisBucket = 0;


        // Set the initial iter to bucket's first
        HashNodeHead *iter = hashMap->bucketList[currentBucketID].nodeList;
        if (pthread_mutex_trylock(&(hashMap->bucketList[currentBucketID].replayLock)) != 0) { // Other replay process is processing this bucket
            continue;
        }
        // Now get the replay lock, check whether it has enough interval before last vacuum
        gettimeofday(&now, NULL);
        if (now.tv_sec - hashMap->bucketList[currentBucketID].lastReplayTime.tv_sec < 60) {
            pthread_mutex_unlock(&(hashMap->bucketList[currentBucketID].replayLock));
            continue;
        }


        while(1) { // Iterate all the head nodes in this bucket
#ifdef ENABLE_DEBUG_INFO2
            printf("%s %d, background_vacuumer %d, start vacuum bucket %d\n", __func__ , __LINE__, gettid(), currentBucketID);
            fflush(stdout);
#endif
            recordNumber = 0;

            // Add a lock to this bucket
            pthread_rwlock_rdlock(&hashMap->bucketList[currentBucketID].bucketLock);


            for(int i = 0; i < currentFinishHeadNum; i++) {
                if(iter!=NULL) {
                    iter = iter->nextHead;
                };
            }
            // Collect ITER_BENCH_SIZE heads
            for(int i = 0; i < ITER_BATCH_SIZE; i++) {
                if(iter!=NULL) {
                    headNodes[recordNumber++] = iter;
                } else {
                    finishIterThisBucket = 1;
                    break;
                }
            }

            // Release this lock
            pthread_rwlock_unlock(&hashMap->bucketList[currentBucketID].bucketLock);

#ifdef ENABLE_DEBUG_INFO2
            printf("%s %d, background_vacuumer %d, got %d heads\n", __func__ , __LINE__, gettid(), recordNumber);
            fflush(stdout);
#endif

            // ITER these ITER_BATCH_SIZE heads
            for(int i = 0; i < recordNumber; i++) {
                if( pthread_rwlock_trywrlock(&(headNodes[i]->headLock)) != 0) { // failed to grab lock, skip this node
                    continue;
                }

#ifdef ENABLE_DEBUG_INFO2
                printf("%s %d, background_vacuumer %d, vacuuming %d head\n", __func__ , __LINE__, gettid(), i);
                fflush(stdout);
#endif
                // TODO: check finish_vacuum_time, and replay this head

                struct timeval now;
                gettimeofday(&now, NULL);
                if(now.tv_sec - headNodes[i]->finishVacuumTime.tv_sec >= 120) {
                    BackgroundReplayHeadNode(headNodes[i]);
                    headNodes[i]->finishVacuumTime = now;
                }

#ifdef ENABLE_DEBUG_INFO2
                printf("%s %d, background_vacuumer %d, finish vacuum %d head\n", __func__ , __LINE__, gettid(), i);
                fflush(stdout);
#endif

                pthread_rwlock_unlock(&(headNodes[i]->headLock));
            }

            // Skip these replayed heads in the next turn
            currentFinishHeadNum += recordNumber;
            // If we finish iterate all the heads in this bucket, iterate next bucket.
            if(finishIterThisBucket) {
                break;
            }
        }


        // update the last replay time for this bucket
        gettimeofday(&now, NULL);
        hashMap->bucketList[currentBucketID].lastReplayTime.tv_sec = now.tv_sec;
        hashMap->bucketList[currentBucketID].lastReplayTime.tv_usec = now.tv_usec;
        // now other replay process can hold the lock again.
        pthread_mutex_unlock(&(hashMap->bucketList[currentBucketID].replayLock));

    }


}

// Before
void BackgroundReplayHeadNode(HashNodeHead * head) {
#ifdef ENABLE_DEBUG_INFO
    printf("%s %d\n", __func__ , __LINE__);
    fflush(stdout);
#endif

    uint64_t replayedLsn = head->replayedLsn;
    uint64_t lsnList[MAX_REPLAY_VERSION_SIZE];
    int listSize = 0;
    char* basePage = NULL;
    BufferTag bufferTag;
    RelFileNode rnode;

    rnode.spcNode = head->key.SpcID;
    rnode.dbNode = head->key.DbID;
    rnode.relNode = head->key.RelID;

    INIT_BUFFERTAG(bufferTag, rnode, (ForkNumber)head->key.ForkNum, head->key.BlkNum);
    int foundBasePage = 1;

    if(replayedLsn == 0) { // Don't have basePage in RocksDB
        if(GetPageFromRocksdb(bufferTag, 1, &basePage) == 0) {
            if(SyncGetRelSize(bufferTag.rnode, bufferTag.forkNum, 0) == -1) {
                foundBasePage = 0;
            } else {
                // If not created by RpcMdExtend, get page from StandAlone process
                basePage = (char*) malloc(BLCKSZ);
                GetBasePage(rnode, (ForkNumber)head->key.ForkNum, (BlockNumber)head->key.BlkNum, basePage);
            }
        }
    } else {
        GetPageFromRocksdb(bufferTag, replayedLsn, &basePage);
    }

#ifdef ENABLE_DEBUG_INFO
    printf("%s %d\n", __func__ , __LINE__);
    fflush(stdout);
#endif
    // If all LSNs in head have been replayed, skip it
    if(replayedLsn < head->lsnEntry[head->entryNum-1].lsn) {
        for(int i = 0; i < head->entryNum; i++) {
            if(replayedLsn < head->lsnEntry[i].lsn) {
#ifdef ENABLE_DEBUG_INFO
                printf("%s %d, %lu, %lu\n", __func__ , __LINE__, replayedLsn, head->lsnEntry[i].lsn);
                fflush(stdout);
#endif
                lsnList[listSize++] = head->lsnEntry[i].lsn;
            }
            if(listSize >= MAX_REPLAY_VERSION_SIZE) {
                break;
            }
        }
    }

#ifdef ENABLE_DEBUG_INFO
    printf("%s %d\n", __func__ , __LINE__);
    fflush(stdout);
#endif
    HashNodeEle *ele = head->nextEle;
    while(ele != NULL) {
        HashNodeEle *nextEle = ele->nextEle;
        if(replayedLsn < ele->lsnEntry[ele->entryNum-1].lsn) {
           for(int i = 0; i < ele->entryNum; i++) {
               if(replayedLsn < ele->lsnEntry[i].lsn) {
                   lsnList[listSize++] = ele->lsnEntry[i].lsn;
               }
               if(listSize >= MAX_REPLAY_VERSION_SIZE) {
                   break;
               }
           }

           // Check whether we have collected enough version LSNs
           if(listSize == MAX_REPLAY_VERSION_SIZE) {

               if(lsnList[listSize-1] == ele->lsnEntry[ele->entryNum-1].lsn
                    && ele->nextEle != NULL) {
                   VacuumHashNode(head, ele, bufferTag);
               }
               break;
           }
        }

        if(ele->nextEle != NULL) { // If there have following nodes, free the current ele node
            VacuumHashNode(head, ele, bufferTag);
        }

        ele = nextEle;
    }

#ifdef ENABLE_DEBUG_INFO
    printf("%s %d\n", __func__ , __LINE__);
    fflush(stdout);
#endif
    // For now, we have collect 0~MAX_REPLAY_VERSION_SIZE versions from this element node

    if(listSize > 0) {
        char * replayedPage = NULL;
        if(!foundBasePage) {
            basePage = (char*) malloc(BLCKSZ);
            ApplyOneLsnWithoutBasePage(bufferTag.rnode, bufferTag.forkNum, bufferTag.blockNum, lsnList[0], basePage);
        }

        // For now, we should have base pages
        if (!foundBasePage) {
            if(listSize-1 <= 0) { // only has one version and already been replayed
                PutPage2Rocksdb(bufferTag, lsnList[0], basePage);
                free(basePage);
                head->replayedLsn = lsnList[0];
#ifdef ENABLE_DEBUG_INFO
                printf("%s %d\n", __func__ , __LINE__);
                fflush(stdout);
#endif
                return;
            }
        }

#ifdef ENABLE_DEBUG_INFO
        printf("%s %d\n", __func__ , __LINE__);
        fflush(stdout);
#endif
        // we have other following version to be replayed
        replayedPage = (char*) malloc(BLCKSZ);
        if (!foundBasePage) {
#ifdef ENABLE_DEBUG_INFO
            printf("%s %d\n", __func__ , __LINE__);
            fflush(stdout);
#endif
            ApplyLsnList(bufferTag.rnode, bufferTag.forkNum, bufferTag.blockNum, lsnList + 1, listSize - 1, basePage, replayedPage);
        }else {
#ifdef ENABLE_DEBUG_INFO
            printf("%s %d, basePageLsn = %d, listSize=%d\n", __func__ , __LINE__, PageGetLSN(basePage), listSize);
            for(int i = 0; i< listSize; i++)
                printf("%lu, ", lsnList[i]);
            printf("\n");
            fflush(stdout);
#endif
            ApplyLsnList(bufferTag.rnode, bufferTag.forkNum, bufferTag.blockNum, lsnList, listSize, basePage, replayedPage);
        }
#ifdef ENABLE_DEBUG_INFO
        printf("%s %d\n", __func__ , __LINE__);
        fflush(stdout);
#endif
        PutPage2Rocksdb(bufferTag, lsnList[listSize-1], replayedPage);
        DeletePageFromRocksdb(bufferTag, replayedLsn);
        head->replayedLsn = lsnList[listSize-1];
        free(basePage);
        free(replayedPage);
#ifdef ENABLE_DEBUG_INFO
        printf("%s %d\n", __func__ , __LINE__);
        fflush(stdout);
#endif
        return;
    }

#ifdef ENABLE_DEBUG_INFO
    printf("%s %d\n", __func__ , __LINE__);
    fflush(stdout);
#endif
    if(replayedLsn == 0) {
        PutPage2Rocksdb(bufferTag, 1, basePage);
        head->replayedLsn = 1;
    }

    free(basePage);
#ifdef ENABLE_DEBUG_INFO
    printf("%s %d\n", __func__ , __LINE__);
    fflush(stdout);
#endif
    return;
}


// Delete corresponding RocksDb pages and hashNodeEle
// Should remember ele->prev/next in advance, ele will be erased in this func
void VacuumHashNode(HashNodeHead* head, HashNodeEle* ele, BufferTag bufferTag) {
    for(int i = 0; i < ele->entryNum; i++) {
        DeletePageFromRocksdb(bufferTag, ele->lsnEntry[i].lsn);
    }
    if(ele == head->nextEle) { // if it is the first node
        head->nextEle = ele->nextEle;
        if(ele->nextEle != NULL) {
            ele->nextEle->prevEle = NULL;
        }
    } else {
        ele->prevEle->nextEle = ele->nextEle;
        if(ele->nextEle != NULL) {
            ele->nextEle->prevEle = ele->prevEle;
        }
    }
    free(ele);
}






















