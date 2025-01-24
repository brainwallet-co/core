//
//  BWPeerManager.c
//
//  Created by Aaron Voisine on 9/2/15.
//  Copyright (c) 2015 breadwallet LLC.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.

#include "BWPeerManager.h"
#include "BWBloomFilter.h"
#include "BWSet.h"
#include "BWArray.h"
#include "BWInt.h"
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <limits.h>
#include <time.h>
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PROTOCOL_TIMEOUT      20.0
#define MAX_CONNECT_FAILURES  20 // notify user of network problems after this many connect failures in a row
#define PEER_FLAG_SYNCED      0x01
#define PEER_FLAG_NEEDSUPDATE 0x02

#define genesis_block_hash(params) UInt256Reverse((params)->checkpoints[0].hash)

typedef struct {
    BWPeerManager *manager;
    const char *hostname;
    uint64_t services;
} BWFindPeersInfo;

typedef struct {
    BWPeer *peer;
    BWPeerManager *manager;
    UInt256 hash;
} BWPeerCallbackInfo;

typedef struct {
    BWTransaction *tx;
    void *info;
    void (*callback)(void *info, int error);
} BWPublishedTx;

typedef struct {
    UInt256 txHash;
    BWPeer *peers;
} BWTxPeerList;

// true if peer is contained in the list of peers associated with txHash
static int _BWTxPeerListHasPeer(const BWTxPeerList *list, UInt256 txHash, const BWPeer *peer)
{
    for (size_t i = array_count(list); i > 0; i--) {
        if (! UInt256Eq(list[i - 1].txHash, txHash)) continue;

        for (size_t j = array_count(list[i - 1].peers); j > 0; j--) {
            if (BWPeerEq(&list[i - 1].peers[j - 1], peer)) return 1;
        }

        break;
    }

    return 0;
}

// number of peers associated with txHash
static size_t _BWTxPeerListCount(const BWTxPeerList *list, UInt256 txHash)
{
    for (size_t i = array_count(list); i > 0; i--) {
        if (UInt256Eq(list[i - 1].txHash, txHash)) return array_count(list[i - 1].peers);
    }

    return 0;
}

// adds peer to the list of peers associated with txHash and returns the new total number of peers
static size_t _BWTxPeerListAddPeer(BWTxPeerList **list, UInt256 txHash, const BWPeer *peer)
{
    for (size_t i = array_count(*list); i > 0; i--) {
        if (! UInt256Eq((*list)[i - 1].txHash, txHash)) continue;

        for (size_t j = array_count((*list)[i - 1].peers); j > 0; j--) {
            if (BWPeerEq(&(*list)[i - 1].peers[j - 1], peer)) return array_count((*list)[i - 1].peers);
        }

        array_add((*list)[i - 1].peers, *peer);
        return array_count((*list)[i - 1].peers);
    }

    array_add(*list, ((BWTxPeerList) { txHash, NULL }));
    array_new((*list)[array_count(*list) - 1].peers, PEER_MAX_CONNECTIONS);
    array_add((*list)[array_count(*list) - 1].peers, *peer);
    return 1;
}

// removes peer from the list of peers associated with txHash, returns true if peer was found
static int _BWTxPeerListRemovePeer(BWTxPeerList *list, UInt256 txHash, const BWPeer *peer)
{
    for (size_t i = array_count(list); i > 0; i--) {
        if (! UInt256Eq(list[i - 1].txHash, txHash)) continue;

        for (size_t j = array_count(list[i - 1].peers); j > 0; j--) {
            if (! BWPeerEq(&list[i - 1].peers[j - 1], peer)) continue;
            array_rm(list[i - 1].peers, j - 1);
            return 1;
        }

        break;
    }

    return 0;
}

// comparator for sorting peers by timestamp, most recent first
inline static int _peerTimestampCompare(const void *peer, const void *otherPeer)
{
    if (((const BWPeer *)peer)->timestamp < ((const BWPeer *)otherPeer)->timestamp) return 1;
    if (((const BWPeer *)peer)->timestamp > ((const BWPeer *)otherPeer)->timestamp) return -1;
    return 0;
}

// returns a hash value for a block's prevBlock value suitable for use in a hashtable
inline static size_t _BWPrevBlockHash(const void *block)
{
    return (size_t)((const BWMerkleBlock *)block)->prevBlock.u32[0];
}

// true if block and otherBlock have equal prevBlock values
inline static int _BWPrevBlockEq(const void *block, const void *otherBlock)
{
    return UInt256Eq(((const BWMerkleBlock *)block)->prevBlock, ((const BWMerkleBlock *)otherBlock)->prevBlock);
}

// returns a hash value for a block's height value suitable for use in a hashtable
inline static size_t _BWBlockHeightHash(const void *block)
{
    // (FNV_OFFSET xor height)*FNV_PRIME
    return (size_t)((0x811C9dc5 ^ ((const BWMerkleBlock *)block)->height)*0x01000193);
}

// true if block and otherBlock have equal height values
inline static int _BWBlockHeightEq(const void *block, const void *otherBlock)
{
    return (((const BWMerkleBlock *)block)->height == ((const BWMerkleBlock *)otherBlock)->height);
}

struct BWPeerManagerStruct {
    const BWChainParams *params;
    BWWallet *wallet;
    int isConnected, connectFailureCount, misbehavinCount, dnsThreadCount, maxConnectCount;
    BWPeer *peers, *downloadPeer, fixedPeer, **connectedPeers;
    char downloadPeerName[INET6_ADDRSTRLEN + 6];
    uint32_t earliestKeyTime, syncStartHeight, filterUpdateHeight, estimatedHeight;
    BWBloomFilter *bloomFilter;
    double fpRate, averageTxPerBlock;
    BWSet *blocks, *orphans, *checkpoints;
    BWMerkleBlock *lastBlock, *lastOrphan;
    BWTxPeerList *txRelays, *txRequests;
    BWPublishedTx *publishedTx;
    UInt256 *publishedTxHashes;
    void *info;
    void (*syncStarted)(void *info);
    void (*syncStopped)(void *info, int error);
    void (*txStatusUpdate)(void *info);
    void (*saveBlocks)(void *info, int replace, BWMerkleBlock *blocks[], size_t blocksCount);
    void (*savePeers)(void *info, int replace, const BWPeer peers[], size_t peersCount);
    int (*networkIsReachable)(void *info);
    void (*threadCleanup)(void *info);
    pthread_mutex_t lock;
};

static void _BWPeerManagerPeerMisbehavin(BWPeerManager *manager, BWPeer *peer)
{
    for (size_t i = array_count(manager->peers); i > 0; i--) {
        if (BWPeerEq(&manager->peers[i - 1], peer)) array_rm(manager->peers, i - 1);
    }

    if (++manager->misbehavinCount >= 10) { // clear out stored peers so we get a fresh list from DNS for next connect
        manager->misbehavinCount = 0;
        array_clear(manager->peers);
    }

    BWPeerDisconnect(peer);
}

static void _BWPeerManagerSyncStopped(BWPeerManager *manager)
{
    manager->syncStartHeight = 0;

    if (manager->downloadPeer) {
        // don't cancel timeout if there's a pending tx publish callback
        for (size_t i = array_count(manager->publishedTx); i > 0; i--) {
            if (manager->publishedTx[i - 1].callback != NULL) return;
        }

        BWPeerScheduleDisconnect(manager->downloadPeer, -1); // cancel sync timeout
    }
}

// adds transaction to list of tx to be published, along with any unconfirmed inputs
static void _BWPeerManagerAddTxToPublishList(BWPeerManager *manager, BWTransaction *tx, void *info,
                                             void (*callback)(void *, int))
{
    if (tx && tx->blockHeight == TX_UNCONFIRMED) {
        for (size_t i = array_count(manager->publishedTx); i > 0; i--) {
            if (BWTransactionEq(manager->publishedTx[i - 1].tx, tx)) return;
        }

        array_add(manager->publishedTx, ((BWPublishedTx) { tx, info, callback }));
        array_add(manager->publishedTxHashes, tx->txHash);

        for (size_t i = 0; i < tx->inCount; i++) {
            _BWPeerManagerAddTxToPublishList(manager, BWWalletTransactionForHash(manager->wallet, tx->inputs[i].txHash),
                                             NULL, NULL);
        }
    }
}

static size_t _BWPeerManagerBlockLocators(BWPeerManager *manager, UInt256 locators[], size_t locatorsCount)
{
    // append 10 most recent block hashes, decending, then continue appending, doubling the step back each time,
    // finishing with the genesis block (top, -1, -2, -3, -4, -5, -6, -7, -8, -9, -11, -15, -23, -39, -71, -135, ..., 0)
    BWMerkleBlock *block = manager->lastBlock;
    int32_t step = 1, i = 0, j;

    while (block && block->height > 0) {
        if (locators && i < locatorsCount) locators[i] = block->blockHash;
        if (++i >= 10) step *= 2;

        for (j = 0; block && j < step; j++) {
            block = BWSetGet(manager->blocks, &block->prevBlock);
        }
    }
    
    if (locators && i < locatorsCount) locators[i] = genesis_block_hash(manager->params);
    return ++i;
}

static void _setApplyFreeBlock(void *info, void *block)
{
    BWMerkleBlockFree(block);
}

static void _BWPeerManagerLoadBloomFilter(BWPeerManager *manager, BWPeer *peer)
{
    // every time a new wallet address is added, the bloom filter has to be rebuilt, and each address is only used
    // for one transaction, so here we generate some spare addresses to avoid rebuilding the filter each time a
    // wallet transaction is encountered during the chain sync
    BWWalletUnusedAddrs(manager->wallet, NULL, SEQUENCE_GAP_LIMIT_EXTERNAL + 100, 0);
    BWWalletUnusedAddrs(manager->wallet, NULL, SEQUENCE_GAP_LIMIT_INTERNAL + 100, 1);

    BWSetApply(manager->orphans, NULL, _setApplyFreeBlock);
    BWSetClear(manager->orphans); // clear out orphans that may have been received on an old filter
    manager->lastOrphan = NULL;
    manager->filterUpdateHeight = manager->lastBlock->height; 
    
    size_t addrsCount = BWWalletAllAddrs(manager->wallet, NULL, 0);
    BWAddress *addrs = malloc(addrsCount*sizeof(*addrs));
    size_t utxosCount = BWWalletUTXOs(manager->wallet, NULL, 0);
    BWUTXO *utxos = malloc(utxosCount*sizeof(*utxos));
    uint32_t blockHeight = (manager->lastBlock->height > 100) ? manager->lastBlock->height - 100 : 0;
    size_t txCount = BWWalletTxUnconfirmedBefore(manager->wallet, NULL, 0, blockHeight);
    BWTransaction **transactions = malloc(txCount*sizeof(*transactions));
    BWBloomFilter *filter;

    assert(addrs != NULL);
    assert(utxos != NULL);
    assert(transactions != NULL);
    addrsCount = BWWalletAllAddrs(manager->wallet, addrs, addrsCount);
    utxosCount = BWWalletUTXOs(manager->wallet, utxos, utxosCount);
    txCount = BWWalletTxUnconfirmedBefore(manager->wallet, transactions, txCount, blockHeight);
    filter = BWBloomFilterNew(manager->fpRate, addrsCount + utxosCount + txCount + 100, (uint32_t)BWPeerHash(peer),
                              BLOOM_UPDATE_ALL); // BUG: XXX txCount not the same as number of spent wallet outputs

    for (size_t i = 0; i < addrsCount; i++) { // add addresses to watch for tx receiveing money to the wallet
        UInt160 hash = UINT160_ZERO;

        BWAddressHash160(&hash, addrs[i].s);

        if (! UInt160IsZero(hash) && ! BWBloomFilterContainsData(filter, hash.u8, sizeof(hash))) {
            BWBloomFilterInsertData(filter, hash.u8, sizeof(hash));
        }
    }

    free(addrs);

    for (size_t i = 0; i < utxosCount; i++) { // add UTXOs to watch for tx sending money from the wallet
        uint8_t o[sizeof(UInt256) + sizeof(uint32_t)];

        UInt256Set(o, utxos[i].hash);
        UInt32SetLE(&o[sizeof(UInt256)], utxos[i].n);
        if (! BWBloomFilterContainsData(filter, o, sizeof(o))) BWBloomFilterInsertData(filter, o, sizeof(o));
    }

    free(utxos);

    for (size_t i = 0; i < txCount; i++) { // also add TXOs spent within the last 100 blocks
        for (size_t j = 0; j < transactions[i]->inCount; j++) {
            BWTxInput *input = &transactions[i]->inputs[j];
            BWTransaction *tx = BWWalletTransactionForHash(manager->wallet, input->txHash);
            uint8_t o[sizeof(UInt256) + sizeof(uint32_t)];

            if (tx && input->index < tx->outCount &&
                BWWalletContainsAddress(manager->wallet, tx->outputs[input->index].address)) {
                UInt256Set(o, input->txHash);
                UInt32SetLE(&o[sizeof(UInt256)], input->index);
                if (! BWBloomFilterContainsData(filter, o, sizeof(o))) BWBloomFilterInsertData(filter, o,sizeof(o));
            }
        }
    }

    free(transactions);
    if (manager->bloomFilter) BWBloomFilterFree(manager->bloomFilter);
    manager->bloomFilter = filter;
    // TODO: XXX if already synced, recursively add inputs of unconfirmed receives

    uint8_t data[BWBloomFilterSerialize(filter, NULL, 0)];
    size_t len = BWBloomFilterSerialize(filter, data, sizeof(data));

    BWPeerSendFilterload(peer, data, len);
}

static void _updateFilterRerequestDone(void *info, int success)
{
    BWPeer *peer = ((BWPeerCallbackInfo *)info)->peer;
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;

    free(info);

    if (success) {
        pthread_mutex_lock(&manager->lock);

        if ((peer->flags & PEER_FLAG_NEEDSUPDATE) == 0) {
            UInt256 locators[_BWPeerManagerBlockLocators(manager, NULL, 0)];
            size_t count = _BWPeerManagerBlockLocators(manager, locators, sizeof(locators)/sizeof(*locators));

            BWPeerSendGetblocks(peer, locators, count, UINT256_ZERO);
        }

        pthread_mutex_unlock(&manager->lock);
    }
}

static void _updateFilterLoadDone(void *info, int success)
{
    BWPeer *peer = ((BWPeerCallbackInfo *)info)->peer;
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;
    BWPeerCallbackInfo *peerInfo;

    free(info);

    if (success) {
        pthread_mutex_lock(&manager->lock);
        BWPeerSetNeedsFilterUpdate(peer, 0);
        peer->flags &= ~PEER_FLAG_NEEDSUPDATE;

        if (manager->lastBlock->height < manager->estimatedHeight) { // if syncing, rerequest blocks
            peerInfo = calloc(1, sizeof(*peerInfo));
            assert(peerInfo != NULL);
            peerInfo->peer = peer;
            peerInfo->manager = manager;
            BWPeerRerequestBlocks(manager->downloadPeer, manager->lastBlock->blockHash);
            BWPeerSendPing(manager->downloadPeer, peerInfo, _updateFilterRerequestDone);
        }
        else BWPeerSendMempool(peer, NULL, 0, NULL, NULL); // if not syncing, request mempool

        pthread_mutex_unlock(&manager->lock);
    }
}

static void _updateFilterPingDone(void *info, int success)
{
    BWPeer *peer = ((BWPeerCallbackInfo *)info)->peer;
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;
    BWPeerCallbackInfo *peerInfo;

    if (success) {
        pthread_mutex_lock(&manager->lock);
        peer_log(peer, "updating filter with newly created wallet addresses");
        if (manager->bloomFilter) BWBloomFilterFree(manager->bloomFilter);
        manager->bloomFilter = NULL;

        if (manager->lastBlock->height < manager->estimatedHeight) { // if we're syncing, only update download peer
            if (manager->downloadPeer) {
                _BWPeerManagerLoadBloomFilter(manager, manager->downloadPeer);
                BWPeerSendPing(manager->downloadPeer, info, _updateFilterLoadDone); // wait for pong so filter is loaded
            }
            else free(info);
        }
        else {
            free(info);

            for (size_t i = array_count(manager->connectedPeers); i > 0; i--) {
                if (BWPeerConnectStatus(manager->connectedPeers[i - 1]) != BWPeerStatusConnected) continue;
                peerInfo = calloc(1, sizeof(*peerInfo));
                assert(peerInfo != NULL);
                peerInfo->peer = manager->connectedPeers[i - 1];
                peerInfo->manager = manager;
                _BWPeerManagerLoadBloomFilter(manager, peerInfo->peer);
                BWPeerSendPing(peerInfo->peer, peerInfo, _updateFilterLoadDone); // wait for pong so filter is loaded
            }
        }

         pthread_mutex_unlock(&manager->lock);
    }
    else free(info);
}

static void _BWPeerManagerUpdateFilter(BWPeerManager *manager)
{
    BWPeerCallbackInfo *info;

    if (manager->downloadPeer && (manager->downloadPeer->flags & PEER_FLAG_NEEDSUPDATE) == 0) {
        BWPeerSetNeedsFilterUpdate(manager->downloadPeer, 1);
        manager->downloadPeer->flags |= PEER_FLAG_NEEDSUPDATE;
        peer_log(manager->downloadPeer, "filter update needed, waiting for pong");
        info = calloc(1, sizeof(*info));
        assert(info != NULL);
        info->peer = manager->downloadPeer;
        info->manager = manager;
        // wait for pong so we're sure to include any tx already sent by the peer in the updated filter
        BWPeerSendPing(manager->downloadPeer, info, _updateFilterPingDone);
    }
}

static void _BWPeerManagerUpdateTx(BWPeerManager *manager, const UInt256 txHashes[], size_t txCount,
                                   uint32_t blockHeight, uint32_t timestamp)
{
    if (blockHeight != TX_UNCONFIRMED) { // remove confirmed tx from publish list and relay counts
        for (size_t i = 0; i < txCount; i++) {
            for (size_t j = array_count(manager->publishedTx); j > 0; j--) {
                BWTransaction *tx = manager->publishedTx[j - 1].tx;

                if (! UInt256Eq(txHashes[i], tx->txHash)) continue;
                array_rm(manager->publishedTx, j - 1);
                array_rm(manager->publishedTxHashes, j - 1);
                if (! BWWalletTransactionForHash(manager->wallet, tx->txHash)) BWTransactionFree(tx);
            }

            for (size_t j = array_count(manager->txRelays); j > 0; j--) {
                if (! UInt256Eq(txHashes[i], manager->txRelays[j - 1].txHash)) continue;
                array_free(manager->txRelays[j - 1].peers);
                array_rm(manager->txRelays, j - 1);
            }
        }
    }

    BWWalletUpdateTransactions(manager->wallet, txHashes, txCount, blockHeight, timestamp);
}

// unconfirmed transactions that aren't in the mempools of any of connected peers have likely dropped off the network
static void _requestUnrelayedTxGetdataDone(void *info, int success)
{
    BWPeer *peer = ((BWPeerCallbackInfo *)info)->peer;
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;
    int isPublishing;
    size_t count = 0;

    free(info);
    pthread_mutex_lock(&manager->lock);
    if (success) peer->flags |= PEER_FLAG_SYNCED;

    for (size_t i = array_count(manager->connectedPeers); i > 0; i--) {
        peer = manager->connectedPeers[i - 1];
        if (BWPeerConnectStatus(peer) == BWPeerStatusConnected) count++;
        if ((peer->flags & PEER_FLAG_SYNCED) != 0) continue;
        count = 0;
        break;
    }

    // don't remove transactions until we're connected to maxConnectCount peers, and all peers have finished
    // relaying their mempools
    if (count >= manager->maxConnectCount) {
        UInt256 hash;
        size_t txCount = BWWalletTxUnconfirmedBefore(manager->wallet, NULL, 0, TX_UNCONFIRMED);
        BWTransaction *tx[(txCount*sizeof(BWTransaction *) <= 0x1000) ? txCount : 0x1000/sizeof(BWTransaction *)];
        
        txCount = BWWalletTxUnconfirmedBefore(manager->wallet, tx, sizeof(tx)/sizeof(*tx), TX_UNCONFIRMED);

        for (size_t i = txCount; i > 0; i--) {
            hash = tx[i - 1]->txHash;
            isPublishing = 0;

            for (size_t j = array_count(manager->publishedTx); ! isPublishing && j > 0; j--) {
                if (BWTransactionEq(manager->publishedTx[j - 1].tx, tx[i - 1]) &&
                    manager->publishedTx[j - 1].callback != NULL) isPublishing = 1;
            }
            
            if (! isPublishing && _BWTxPeerListCount(manager->txRelays, hash) == 0 &&
                _BWTxPeerListCount(manager->txRequests, hash) == 0) {
                peer_log(peer, "removing tx unconfirmed at: %d, txHash: %s", manager->lastBlock->height, u256hex(hash));
                assert(tx[i - 1]->blockHeight == TX_UNCONFIRMED);
                BWWalletRemoveTransaction(manager->wallet, hash);
            }
            else if (! isPublishing && _BWTxPeerListCount(manager->txRelays, hash) < manager->maxConnectCount) {
                // set timestamp 0 to mark as unverified
                _BWPeerManagerUpdateTx(manager, &hash, 1, TX_UNCONFIRMED, 0);
            }
        }
    }

    pthread_mutex_unlock(&manager->lock);
}

static void _BWPeerManagerRequestUnrelayedTx(BWPeerManager *manager, BWPeer *peer)
{
    BWPeerCallbackInfo *info;
    size_t hashCount = 0, txCount = BWWalletTxUnconfirmedBefore(manager->wallet, NULL, 0, TX_UNCONFIRMED);
    BWTransaction *tx[txCount];
    UInt256 txHashes[txCount];

    txCount = BWWalletTxUnconfirmedBefore(manager->wallet, tx, txCount, TX_UNCONFIRMED);

    for (size_t i = 0; i < txCount; i++) {
        if (! _BWTxPeerListHasPeer(manager->txRelays, tx[i]->txHash, peer) &&
            ! _BWTxPeerListHasPeer(manager->txRequests, tx[i]->txHash, peer)) {
            txHashes[hashCount++] = tx[i]->txHash;
            _BWTxPeerListAddPeer(&manager->txRequests, tx[i]->txHash, peer);
        }
    }

    if (hashCount > 0) {
        BWPeerSendGetdata(peer, txHashes, hashCount, NULL, 0);

        if ((peer->flags & PEER_FLAG_SYNCED) == 0) {
            info = calloc(1, sizeof(*info));
            assert(info != NULL);
            info->peer = peer;
            info->manager = manager;
            BWPeerSendPing(peer, info, _requestUnrelayedTxGetdataDone);
        }
    }
    else peer->flags |= PEER_FLAG_SYNCED;
}

static void _BWPeerManagerPublishPendingTx(BWPeerManager *manager, BWPeer *peer)
{
    for (size_t i = array_count(manager->publishedTx); i > 0; i--) {
        if (manager->publishedTx[i - 1].callback == NULL) continue;
        BWPeerScheduleDisconnect(peer, PROTOCOL_TIMEOUT); // schedule publish timeout
        break;
    }

    BWPeerSendInv(peer, manager->publishedTxHashes, array_count(manager->publishedTxHashes));
}

static void _mempoolDone(void *info, int success)
{
    BWPeer *peer = ((BWPeerCallbackInfo *)info)->peer;
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;
    int syncFinished = 0;

    free(info);

    if (success) {
        peer_log(peer, "mempool request finished");
        pthread_mutex_lock(&manager->lock);
        if (manager->syncStartHeight > 0) {
            peer_log(peer, "sync succeeded");
            syncFinished = 1;
            _BWPeerManagerSyncStopped(manager);
        }

        _BWPeerManagerRequestUnrelayedTx(manager, peer);
        BWPeerSendGetaddr(peer); // request a list of other bitcoin peers
        pthread_mutex_unlock(&manager->lock);
        if (manager->txStatusUpdate) manager->txStatusUpdate(manager->info);
        if (syncFinished && manager->syncStopped) manager->syncStopped(manager->info, 0);
    }
    else peer_log(peer, "mempool request failed");
}

static void _loadBloomFilterDone(void *info, int success)
{
    BWPeer *peer = ((BWPeerCallbackInfo *)info)->peer;
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;

    pthread_mutex_lock(&manager->lock);

    if (success) {
        BWPeerSendMempool(peer, manager->publishedTxHashes, array_count(manager->publishedTxHashes), info,
                          _mempoolDone);
        pthread_mutex_unlock(&manager->lock);
    }
    else {
        free(info);

        if (peer == manager->downloadPeer) {
            peer_log(peer, "sync succeeded");
            _BWPeerManagerSyncStopped(manager);
            pthread_mutex_unlock(&manager->lock);
            if (manager->syncStopped) manager->syncStopped(manager->info, 0);
        }
        else pthread_mutex_unlock(&manager->lock);
    }
}

static void _BWPeerManagerLoadMempools(BWPeerManager *manager)
{
    // after syncing, load filters and get mempools from other peers
    for (size_t i = array_count(manager->connectedPeers); i > 0; i--) {
        BWPeer *peer = manager->connectedPeers[i - 1];
        BWPeerCallbackInfo *info;

        if (BWPeerConnectStatus(peer) != BWPeerStatusConnected) continue;
        info = calloc(1, sizeof(*info));
        assert(info != NULL);
        info->peer = peer;
        info->manager = manager;

        if (peer != manager->downloadPeer || manager->fpRate > BLOOM_REDUCED_FALSEPOSITIVE_RATE*5.0) {
            _BWPeerManagerLoadBloomFilter(manager, peer);
            _BWPeerManagerPublishPendingTx(manager, peer);
            BWPeerSendPing(peer, info, _loadBloomFilterDone); // load mempool after updating bloomfilter
        }
        else BWPeerSendMempool(peer, manager->publishedTxHashes, array_count(manager->publishedTxHashes), info,
                               _mempoolDone);
    }
}

// returns a UINT128_ZERO terminated array of addresses for hostname that must be freed, or NULL if lookup failed
static UInt128 *_addressLookup(const char *hostname)
{
    struct addrinfo *servinfo, *p;
    UInt128 *addrList = NULL;
    size_t count = 0, i = 0;

    if (getaddrinfo(hostname, NULL, NULL, &servinfo) == 0) {
        for (p = servinfo; p != NULL; p = p->ai_next) count++;
        if (count > 0) addrList = calloc(count + 1, sizeof(*addrList));
        assert(addrList != NULL || count == 0);

        for (p = servinfo; p != NULL; p = p->ai_next) {
            if (p->ai_family == AF_INET) {
                addrList[i].u16[5] = 0xffff;
                addrList[i].u32[3] = ((struct sockaddr_in *)p->ai_addr)->sin_addr.s_addr;
                i++;
            }
            else if (p->ai_family == AF_INET6) {
                addrList[i++] = *(UInt128 *)&((struct sockaddr_in6 *)p->ai_addr)->sin6_addr;
            }
        }

        freeaddrinfo(servinfo);
    }

    return addrList;
}

static void *_findPeersThreadRoutine(void *arg)
{
    BWPeerManager *manager = ((BWFindPeersInfo *)arg)->manager;
    uint64_t services = ((BWFindPeersInfo *)arg)->services;
    UInt128 *addrList, *addr;
    time_t now = time(NULL), age;

    pthread_cleanup_push(manager->threadCleanup, manager->info);
    addrList = _addressLookup(((BWFindPeersInfo *)arg)->hostname);
    free(arg);
    pthread_mutex_lock(&manager->lock);

    for (addr = addrList; addr && ! UInt128IsZero(*addr); addr++) {
        age = 24*60*60 + BWRand(2*24*60*60); // add between 1 and 3 days
        array_add(manager->peers, ((BWPeer) { *addr, manager->params->standardPort, services, now - age, 0 }));
    }

    manager->dnsThreadCount--;
    pthread_mutex_unlock(&manager->lock);
    if (addrList) free(addrList);
    pthread_cleanup_pop(1);
    return NULL;
}

// DNS peer discovery
static void _BWPeerManagerFindPeers(BWPeerManager *manager)
{
    uint64_t services = SERVICES_NODE_NETWORK | SERVICES_NODE_BLOOM | manager->params->services;
    time_t now = time(NULL);
    struct timespec ts;
    pthread_t thread;
    pthread_attr_t attr;
    UInt128 *addr, *addrList;
    BWFindPeersInfo *info;

    if (! UInt128IsZero(manager->fixedPeer.address)) {
        array_set_count(manager->peers, 1);
        manager->peers[0] = manager->fixedPeer;
        manager->peers[0].services = services;
        manager->peers[0].timestamp = now;
    }
    else {
        for (size_t i = 1; manager->params->dnsSeeds[i]; i++) {
            info = calloc(1, sizeof(BWFindPeersInfo));
            assert(info != NULL);
            info->manager = manager;
            info->hostname = manager->params->dnsSeeds[i];
            info->services = services;
            if (pthread_attr_init(&attr) == 0 && pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0 &&
                pthread_create(&thread, &attr, _findPeersThreadRoutine, info) == 0) manager->dnsThreadCount++;
        }

        for (addr = addrList = _addressLookup(manager->params->dnsSeeds[0]); addr && ! UInt128IsZero(*addr); addr++) {
            array_add(manager->peers, ((BWPeer) { *addr, manager->params->standardPort, services, now, 0 }));
        }

        if (addrList) free(addrList);
        ts.tv_sec = 0;
        ts.tv_nsec = 1;

        do {
            pthread_mutex_unlock(&manager->lock);
            nanosleep(&ts, NULL); // pthread_yield() isn't POSIX standard :(
            pthread_mutex_lock(&manager->lock);
        } while (manager->dnsThreadCount > 0 && array_count(manager->peers) < PEER_MAX_CONNECTIONS);

        qsort(manager->peers, array_count(manager->peers), sizeof(*manager->peers), _peerTimestampCompare);
    }
}

static void _peerConnected(void *info)
{
    BWPeer *peer = ((BWPeerCallbackInfo *)info)->peer;
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;
    BWPeerCallbackInfo *peerInfo;
    time_t now = time(NULL);

    pthread_mutex_lock(&manager->lock);
    if (peer->timestamp > now + 2*60*60 || peer->timestamp < now - 2*60*60) peer->timestamp = now; // sanity check

    // TODO: XXX does this work with 0.11 pruned nodes?
    if ((peer->services & manager->params->services) != manager->params->services) {
        peer_log(peer, "unsupported node type");
        BWPeerDisconnect(peer);
    }
    else if ((peer->services & SERVICES_NODE_NETWORK) != SERVICES_NODE_NETWORK) {
        peer_log(peer, "node doesn't carry full blocks");
        BWPeerDisconnect(peer);
    }
    else if (BWPeerLastBlock(peer) + 10 < manager->lastBlock->height) {
        peer_log(peer, "node isn't synced");
        BWPeerDisconnect(peer);
    }
    else if (BWPeerVersion(peer) >= 70011 && (peer->services & SERVICES_NODE_BLOOM) != SERVICES_NODE_BLOOM) {
        peer_log(peer, "node doesn't support SPV mode");
        BWPeerDisconnect(peer);
    }
    else if (manager->downloadPeer && // check if we should stick with the existing download peer
             (BWPeerLastBlock(manager->downloadPeer) >= BWPeerLastBlock(peer) ||
              manager->lastBlock->height >= BWPeerLastBlock(peer))) {
        if (manager->lastBlock->height >= BWPeerLastBlock(peer)) { // only load bloom filter if we're done syncing
            manager->connectFailureCount = 0; // also reset connect failure count if we're already synced
            _BWPeerManagerLoadBloomFilter(manager, peer);
            _BWPeerManagerPublishPendingTx(manager, peer);
            peerInfo = calloc(1, sizeof(*peerInfo));
            assert(peerInfo != NULL);
            peerInfo->peer = peer;
            peerInfo->manager = manager;
            BWPeerSendPing(peer, peerInfo, _loadBloomFilterDone);
        }
    }
    else { // select the peer with the lowest ping time to download the chain from if we're behind
        // BUG: XXX a malicious peer can report a higher lastblock to make us select them as the download peer, if
        // two peers agree on lastblock, use one of those two instead
        for (size_t i = array_count(manager->connectedPeers); i > 0; i--) {
            BWPeer *p = manager->connectedPeers[i - 1];

            if (BWPeerConnectStatus(p) != BWPeerStatusConnected) continue;
            if ((BWPeerPingTime(p) < BWPeerPingTime(peer) && BWPeerLastBlock(p) >= BWPeerLastBlock(peer)) ||
                BWPeerLastBlock(p) > BWPeerLastBlock(peer)) peer = p;
        }
        
        if (manager->downloadPeer) {
            peer_log(peer, "selecting new download peer with higher reported lastblock");
            BWPeerDisconnect(manager->downloadPeer);
        }
        manager->downloadPeer = peer;
        manager->isConnected = 1;
        manager->estimatedHeight = BWPeerLastBlock(peer);
        _BWPeerManagerLoadBloomFilter(manager, peer);
        BWPeerSetCurrentBlockHeight(peer, manager->lastBlock->height);
        _BWPeerManagerPublishPendingTx(manager, peer);

        if (manager->lastBlock->height < BWPeerLastBlock(peer)) { // start blockchain sync
            UInt256 locators[_BWPeerManagerBlockLocators(manager, NULL, 0)];
            size_t count = _BWPeerManagerBlockLocators(manager, locators, sizeof(locators)/sizeof(*locators));

            BWPeerScheduleDisconnect(peer, PROTOCOL_TIMEOUT); // schedule sync timeout

            // request just block headers up to a week before earliestKeyTime, and then merkleblocks after that
            // we do not reset connect failure count yet incase this request times out
            if (manager->lastBlock->timestamp + 7*24*60*60 >= manager->earliestKeyTime) {
                BWPeerSendGetblocks(peer, locators, count, UINT256_ZERO);
            }
            else BWPeerSendGetheaders(peer, locators, count, UINT256_ZERO);
        }
        else { // we're already synced
            manager->connectFailureCount = 0; // reset connect failure count
            _BWPeerManagerLoadMempools(manager);
        }
    }

    pthread_mutex_unlock(&manager->lock);
}

static void _peerDisconnected(void *info, int error)
{
    BWPeer *peer = ((BWPeerCallbackInfo *)info)->peer;
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;
    BWTxPeerList *peerList;
    int willSave = 0, willReconnect = 0, txError = 0;
    size_t txCount = 0;

    //free(info);
    pthread_mutex_lock(&manager->lock);

    void *txInfo[array_count(manager->publishedTx)];
    void (*txCallback[array_count(manager->publishedTx)])(void *, int);

    if (error == EPROTO) { // if it's protocol error, the peer isn't following standard policy
        _BWPeerManagerPeerMisbehavin(manager, peer);
    }
    else if (error) { // timeout or some non-protocol related network error
        for (size_t i = array_count(manager->peers); i > 0; i--) {
            if (BWPeerEq(&manager->peers[i - 1], peer)) array_rm(manager->peers, i - 1);
        }

        manager->connectFailureCount++;

        // if it's a timeout and there's pending tx publish callbacks, the tx publish timed out
        // BUG: XXX what if it's a connect timeout and not a publish timeout?
        if (error == ETIMEDOUT && (peer != manager->downloadPeer || manager->syncStartHeight == 0 ||
                                   array_count(manager->connectedPeers) == 1)) txError = ETIMEDOUT;
    }

    for (size_t i = array_count(manager->txRelays); i > 0; i--) {
        peerList = &manager->txRelays[i - 1];

        for (size_t j = array_count(peerList->peers); j > 0; j--) {
            if (BWPeerEq(&peerList->peers[j - 1], peer)) array_rm(peerList->peers, j - 1);
        }
    }

    if (peer == manager->downloadPeer) { // download peer disconnected
        manager->isConnected = 0;
        manager->downloadPeer = NULL;
        if (manager->connectFailureCount > MAX_CONNECT_FAILURES) manager->connectFailureCount = MAX_CONNECT_FAILURES;
    }

    if (! manager->isConnected && manager->connectFailureCount == MAX_CONNECT_FAILURES) {
        _BWPeerManagerSyncStopped(manager);

        // clear out stored peers so we get a fresh list from DNS on next connect attempt
        array_clear(manager->peers);
        txError = ENOTCONN; // trigger any pending tx publish callbacks
        willSave = 1;
        peer_log(peer, "sync failed");
    }
    else if (manager->connectFailureCount < MAX_CONNECT_FAILURES) willReconnect = 1;

    if (txError) {
        for (size_t i = array_count(manager->publishedTx); i > 0; i--) {
            if (manager->publishedTx[i - 1].callback == NULL) continue;
            peer_log(peer, "transaction canceled: %s", strerror(txError));
            txInfo[txCount] = manager->publishedTx[i - 1].info;
            txCallback[txCount] = manager->publishedTx[i - 1].callback;
            txCount++;
            BWTransactionFree(manager->publishedTx[i - 1].tx);
            array_rm(manager->publishedTxHashes, i - 1);
            array_rm(manager->publishedTx, i - 1);
        }
    }

    for (size_t i = array_count(manager->connectedPeers); i > 0; i--) {
        if (manager->connectedPeers[i - 1] != peer) continue;
        array_rm(manager->connectedPeers, i - 1);
        break;
    }

    BWPeerFree(peer);
    pthread_mutex_unlock(&manager->lock);

    for (size_t i = 0; i < txCount; i++) {
        txCallback[i](txInfo[i], txError);
    }

    if (willSave && manager->savePeers) manager->savePeers(manager->info, 1, NULL, 0);
    if (willSave && manager->syncStopped) manager->syncStopped(manager->info, error);
    if (willReconnect) BWPeerManagerConnect(manager); // try connecting to another peer
    if (manager->txStatusUpdate) manager->txStatusUpdate(manager->info);
}

static void _peerRelayedPeers(void *info, const BWPeer peers[], size_t peersCount)
{
    BWPeer *peer = ((BWPeerCallbackInfo *)info)->peer;
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;
    time_t now = time(NULL);

    pthread_mutex_lock(&manager->lock);
    peer_log(peer, "relayed %zu peer(s)", peersCount);

    array_add_array(manager->peers, peers, peersCount);
    qsort(manager->peers, array_count(manager->peers), sizeof(*manager->peers), _peerTimestampCompare);

    // limit total to 2500 peers
    if (array_count(manager->peers) > 2500) array_set_count(manager->peers, 2500);
    peersCount = array_count(manager->peers);

    // remove peers more than 3 hours old, or until there are only 1000 left
    while (peersCount > 1000 && manager->peers[peersCount - 1].timestamp + 3*60*60 < now) peersCount--;
    array_set_count(manager->peers, peersCount);

    BWPeer save[peersCount];

    for (size_t i = 0; i < peersCount; i++) save[i] = manager->peers[i];
    pthread_mutex_unlock(&manager->lock);

    // peer relaying is complete when we receive <1000
    if (peersCount > 1 && peersCount < 1000 &&
        manager->savePeers) manager->savePeers(manager->info, 1, save, peersCount);
}

static void _peerRelayedTx(void *info, BWTransaction *tx)
{
    BWPeer *peer = ((BWPeerCallbackInfo *)info)->peer;
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;
    void *txInfo = NULL;
    void (*txCallback)(void *, int) = NULL;
    int isWalletTx = 0, hasPendingCallbacks = 0;
    size_t relayCount = 0;

    pthread_mutex_lock(&manager->lock);
    peer_log(peer, "relayed tx: %s", u256hex(tx->txHash));
    
    for (size_t i = array_count(manager->publishedTx); i > 0; i--) { // see if tx is in list of published tx
        if (UInt256Eq(manager->publishedTxHashes[i - 1], tx->txHash)) {
            txInfo = manager->publishedTx[i - 1].info;
            txCallback = manager->publishedTx[i - 1].callback;
            manager->publishedTx[i - 1].info = NULL;
            manager->publishedTx[i - 1].callback = NULL;
            relayCount = _BWTxPeerListAddPeer(&manager->txRelays, tx->txHash, peer);
        }
        else if (manager->publishedTx[i - 1].callback != NULL) hasPendingCallbacks = 1;
    }

    // cancel tx publish timeout if no publish callbacks are pending, and syncing is done or this is not downloadPeer
    if (! hasPendingCallbacks && (manager->syncStartHeight == 0 || peer != manager->downloadPeer)) {
        BWPeerScheduleDisconnect(peer, -1); // cancel publish tx timeout
    }

    if (manager->syncStartHeight == 0 || BWWalletContainsTransaction(manager->wallet, tx)) {
        isWalletTx = BWWalletRegisterTransaction(manager->wallet, tx);
        if (isWalletTx) tx = BWWalletTransactionForHash(manager->wallet, tx->txHash);
    }
    else {
        BWTransactionFree(tx);
        tx = NULL;
    }

    if (tx && isWalletTx) {
        // reschedule sync timeout
        if (manager->syncStartHeight > 0 && peer == manager->downloadPeer) {
            BWPeerScheduleDisconnect(peer, PROTOCOL_TIMEOUT);
        }

        if (BWWalletAmountSentByTx(manager->wallet, tx) > 0 && BWWalletTransactionIsValid(manager->wallet, tx)) {
            _BWPeerManagerAddTxToPublishList(manager, tx, NULL, NULL); // add valid send tx to mempool
        }

        // keep track of how many peers have or relay a tx, this indicates how likely the tx is to confirm
        // (we only need to track this after syncing is complete)
        if (manager->syncStartHeight == 0) relayCount = _BWTxPeerListAddPeer(&manager->txRelays, tx->txHash, peer);

        _BWTxPeerListRemovePeer(manager->txRequests, tx->txHash, peer);

        if (manager->bloomFilter != NULL) { // check if bloom filter is already being updated
            BWAddress addrs[SEQUENCE_GAP_LIMIT_EXTERNAL + SEQUENCE_GAP_LIMIT_INTERNAL];
            UInt160 hash;

            // the transaction likely consumed one or more wallet addresses, so check that at least the next <gap limit>
            // unused addresses are still matched by the bloom filter
            BWWalletUnusedAddrs(manager->wallet, addrs, SEQUENCE_GAP_LIMIT_EXTERNAL, 0);
            BWWalletUnusedAddrs(manager->wallet, addrs + SEQUENCE_GAP_LIMIT_EXTERNAL, SEQUENCE_GAP_LIMIT_INTERNAL, 1);

            for (size_t i = 0; i < SEQUENCE_GAP_LIMIT_EXTERNAL + SEQUENCE_GAP_LIMIT_INTERNAL; i++) {
                if (! BWAddressHash160(&hash, addrs[i].s) ||
                    BWBloomFilterContainsData(manager->bloomFilter, hash.u8, sizeof(hash))) continue;
                if (manager->bloomFilter) BWBloomFilterFree(manager->bloomFilter);
                manager->bloomFilter = NULL; // reset bloom filter so it's recreated with new wallet addresses
                _BWPeerManagerUpdateFilter(manager);
                break;
            }
        }
    }

    // set timestamp when tx is verified
    if (tx && relayCount >= manager->maxConnectCount && tx->blockHeight == TX_UNCONFIRMED && tx->timestamp == 0) {
        _BWPeerManagerUpdateTx(manager, &tx->txHash, 1, TX_UNCONFIRMED, (uint32_t)time(NULL));
    }

    pthread_mutex_unlock(&manager->lock);
    if (txCallback) txCallback(txInfo, 0);
}

static void _peerHasTx(void *info, UInt256 txHash)
{
    BWPeer *peer = ((BWPeerCallbackInfo *)info)->peer;
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;
    BWTransaction *tx;
    void *txInfo = NULL;
    void (*txCallback)(void *, int) = NULL;
    int isWalletTx = 0, hasPendingCallbacks = 0;
    size_t relayCount = 0;

    pthread_mutex_lock(&manager->lock);
    tx = BWWalletTransactionForHash(manager->wallet, txHash);
    peer_log(peer, "has tx: %s", u256hex(txHash));

    for (size_t i = array_count(manager->publishedTx); i > 0; i--) { // see if tx is in list of published tx
        if (UInt256Eq(manager->publishedTxHashes[i - 1], txHash)) {
            if (! tx) tx = manager->publishedTx[i - 1].tx;
            txInfo = manager->publishedTx[i - 1].info;
            txCallback = manager->publishedTx[i - 1].callback;
            manager->publishedTx[i - 1].info = NULL;
            manager->publishedTx[i - 1].callback = NULL;
            relayCount = _BWTxPeerListAddPeer(&manager->txRelays, txHash, peer);
        }
        else if (manager->publishedTx[i - 1].callback != NULL) hasPendingCallbacks = 1;
    }

    // cancel tx publish timeout if no publish callbacks are pending, and syncing is done or this is not downloadPeer
    if (! hasPendingCallbacks && (manager->syncStartHeight == 0 || peer != manager->downloadPeer)) {
        BWPeerScheduleDisconnect(peer, -1); // cancel publish tx timeout
    }

    if (tx) {
        isWalletTx = BWWalletRegisterTransaction(manager->wallet, tx);
        if (isWalletTx) tx = BWWalletTransactionForHash(manager->wallet, tx->txHash);

        // reschedule sync timeout
        if (manager->syncStartHeight > 0 && peer == manager->downloadPeer && isWalletTx) {
            BWPeerScheduleDisconnect(peer, PROTOCOL_TIMEOUT);
        }

        // keep track of how many peers have or relay a tx, this indicates how likely the tx is to confirm
        // (we only need to track this after syncing is complete)
        if (manager->syncStartHeight == 0) relayCount = _BWTxPeerListAddPeer(&manager->txRelays, txHash, peer);

        // set timestamp when tx is verified
        if (relayCount >= manager->maxConnectCount && tx && tx->blockHeight == TX_UNCONFIRMED && tx->timestamp == 0) {
            _BWPeerManagerUpdateTx(manager, &txHash, 1, TX_UNCONFIRMED, (uint32_t)time(NULL));
        }

        _BWTxPeerListRemovePeer(manager->txRequests, txHash, peer);
    }

    pthread_mutex_unlock(&manager->lock);
    if (txCallback) txCallback(txInfo, 0);
}

static void _peerRejectedTx(void *info, UInt256 txHash, uint8_t code)
{
    BWPeer *peer = ((BWPeerCallbackInfo *)info)->peer;
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;
    BWTransaction *tx, *t;

    pthread_mutex_lock(&manager->lock);
    peer_log(peer, "rejected tx: %s", u256hex(txHash));
    tx = BWWalletTransactionForHash(manager->wallet, txHash);
    _BWTxPeerListRemovePeer(manager->txRequests, txHash, peer);

    if (tx) {
        if (_BWTxPeerListRemovePeer(manager->txRelays, txHash, peer) && tx->blockHeight == TX_UNCONFIRMED) {
            // set timestamp 0 to mark tx as unverified
            _BWPeerManagerUpdateTx(manager, &txHash, 1, TX_UNCONFIRMED, 0);
        }

        // if we get rejected for any reason other than double-spend, the peer is likely misconfigured
        if (code != REJECT_SPENT && BWWalletAmountSentByTx(manager->wallet, tx) > 0) {
            for (size_t i = 0; i < tx->inCount; i++) { // check that all inputs are confirmed before dropping peer
                t = BWWalletTransactionForHash(manager->wallet, tx->inputs[i].txHash);
                if (! t || t->blockHeight != TX_UNCONFIRMED) continue;
                tx = NULL;
                break;
            }

            if (tx) _BWPeerManagerPeerMisbehavin(manager, peer);
        }
    }

    pthread_mutex_unlock(&manager->lock);
    if (manager->txStatusUpdate) manager->txStatusUpdate(manager->info);
}

static int _BWPeerManagerVerifyBlock(BWPeerManager *manager, BWMerkleBlock *block, BWMerkleBlock *prev, BWPeer *peer)
{
    int r = 1;
    
    if (! prev || ! UInt256Eq(block->prevBlock, prev->blockHash) || block->height != prev->height + 1) r = 0;

    // check if we hit a difficulty transition, and find previous transition time
    if (r && (block->height % BLOCK_DIFFICULTY_INTERVAL) == 0) {
        BWMerkleBlock *b = block;
        UInt256 prevBlock;

        for (uint32_t i = 0; b && i < BLOCK_DIFFICULTY_INTERVAL; i++) {
            b = BWSetGet(manager->blocks, &b->prevBlock);
        }

        if (! b) {
            peer_log(peer, "missing previous difficulty tansition, can't verify block: %s", u256hex(block->blockHash));
            r = 0;
        }
        else prevBlock = b->prevBlock;
        
        while (b) { // free up some memory
            b = BWSetGet(manager->blocks, &prevBlock);
            if (b) prevBlock = b->prevBlock;

            if (b && (b->height % BLOCK_DIFFICULTY_INTERVAL) != 0) {
                BWSetRemove(manager->blocks, b);
                BWMerkleBlockFree(b);
            }
        }
    }

    // verify block difficulty
    if (r && ! manager->params->verifyDifficulty(block, manager->blocks)) {
        peer_log(peer, "relayed block with invalid difficulty target %x, blockHash: %s", block->target,
                 u256hex(block->blockHash));
        r = 0;
    }

    if (r) {
        BWMerkleBlock *checkpoint = BWSetGet(manager->checkpoints, block);

        // verify blockchain checkpoints
        if (checkpoint && ! BWMerkleBlockEq(block, checkpoint)) {
            peer_log(peer, "relayed a block that differs from the checkpoint at height %"PRIu32", blockHash: %s, "
                     "expected: %s", block->height, u256hex(block->blockHash), u256hex(checkpoint->blockHash));
            r = 0;
        }
    }

    return r;
}

static void _peerRelayedBlock(void *info, BWMerkleBlock *block)
{
    BWPeer *peer = ((BWPeerCallbackInfo *)info)->peer;
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;
    size_t txCount = BWMerkleBlockTxHashes(block, NULL, 0);
    UInt256 _txHashes[(sizeof(UInt256)*txCount <= 0x1000) ? txCount : 0],
            *txHashes = (sizeof(UInt256)*txCount <= 0x1000) ? _txHashes : malloc(txCount*sizeof(*txHashes));
    size_t i, j, fpCount = 0, saveCount = 0;
    BWMerkleBlock orphan, *b, *b2, *prev, *next = NULL;
    uint32_t txTime = 0;

    assert(txHashes != NULL);
    txCount = BWMerkleBlockTxHashes(block, txHashes, txCount);
    pthread_mutex_lock(&manager->lock);
    prev = BWSetGet(manager->blocks, &block->prevBlock);

    if (prev) {
        txTime = block->timestamp/2 + prev->timestamp/2;
        block->height = prev->height + 1;
    }

    // track the observed bloom filter false positive rate using a low pass filter to smooth out variance
    if (peer == manager->downloadPeer && block->totalTx > 0) {
        for (i = 0; i < txCount; i++) { // wallet tx are not false-positives
            if (! BWWalletTransactionForHash(manager->wallet, txHashes[i])) fpCount++;
        }

        // moving average number of tx-per-block
        manager->averageTxPerBlock = manager->averageTxPerBlock*0.999 + block->totalTx*0.001;
        peer_log(peer, "user preferred fpRate: %f", manager->fpRate);

        // 1% low pass filter, also weights each block by total transactions, compared to the avarage
        manager->fpRate = manager->fpRate*(1.0 - 0.01*block->totalTx/manager->averageTxPerBlock) +
                          0.01*fpCount/manager->averageTxPerBlock;
        peer_log(peer, "adjusted preferred fpRate: %f", manager->fpRate);

        // false positive rate sanity check
        if (BWPeerConnectStatus(peer) == BWPeerStatusConnected &&
            manager->fpRate > BLOOM_DEFAULT_FALSEPOSITIVE_RATE*10.0) {
            peer_log(peer, "bloom filter false positive rate %f too high after %"PRIu32" blocks, disconnecting...",
                     manager->fpRate, manager->lastBlock->height + 1 - manager->filterUpdateHeight);

            //Resets the fpRate to the reduced fpRate to allow further connection
            manager->fpRate = BLOOM_REDUCED_FALSEPOSITIVE_RATE;

            BWPeerDisconnect(peer);
        }
        else if (manager->lastBlock->height + 500 < BWPeerLastBlock(peer) &&
                 manager->fpRate > BLOOM_REDUCED_FALSEPOSITIVE_RATE*10.0) {
            _BWPeerManagerUpdateFilter(manager); // rebuild bloom filter when it starts to degrade
        }
    }

    // ignore block headers that are newer than one week before earliestKeyTime (it's a header if it has 0 totalTx)
    if (block->totalTx == 0 && block->timestamp + 7*24*60*60 > manager->earliestKeyTime + 2*60*60) {
        BWMerkleBlockFree(block);
        block = NULL;
    }
    else if (manager->bloomFilter == NULL) { // ingore potentially incomplete blocks when a filter update is pending
        BWMerkleBlockFree(block);
        block = NULL;

        if (peer == manager->downloadPeer && manager->lastBlock->height < manager->estimatedHeight) {
            BWPeerScheduleDisconnect(peer, PROTOCOL_TIMEOUT); // reschedule sync timeout
            manager->connectFailureCount = 0; // reset failure count once we know our initial request didn't timeout
        }
    }
    else if (! prev) { // block is an orphan
        peer_log(peer, "relayed orphan block %s, previous %s, last block is %s, height %"PRIu32,
                 u256hex(block->blockHash), u256hex(block->prevBlock), u256hex(manager->lastBlock->blockHash),
                 manager->lastBlock->height);
        
        if (block->timestamp + 7*24*60*60 < time(NULL)) { // ignore orphans older than one week ago
            BWMerkleBlockFree(block);
            block = NULL;
        }
        else {
            // call getblocks, unless we already did with the previous block, or we're still syncing
            if (manager->lastBlock->height >= BWPeerLastBlock(peer) &&
                (! manager->lastOrphan || ! UInt256Eq(manager->lastOrphan->blockHash, block->prevBlock))) {
                UInt256 locators[_BWPeerManagerBlockLocators(manager, NULL, 0)];
                size_t locatorsCount = _BWPeerManagerBlockLocators(manager, locators,
                                                                   sizeof(locators)/sizeof(*locators));

                peer_log(peer, "calling getblocks");
                BWPeerSendGetblocks(peer, locators, locatorsCount, UINT256_ZERO);
            }

            BWSetAdd(manager->orphans, block); // BUG: limit total orphans to avoid memory exhaustion attack
            manager->lastOrphan = block;
        }
    }
    else if (! _BWPeerManagerVerifyBlock(manager, block, prev, peer)) { // block is invalid
        peer_log(peer, "relayed invalid block");
        BWMerkleBlockFree(block);
        block = NULL;
        _BWPeerManagerPeerMisbehavin(manager, peer);
    }
    else if (UInt256Eq(block->prevBlock, manager->lastBlock->blockHash)) { // new block extends main chain
        if ((block->height % 500) == 0 || txCount > 0 || block->height >= BWPeerLastBlock(peer)) {
            peer_log(peer, "adding block #%"PRIu32", false positive rate: %f", block->height, manager->fpRate);
        }

        BWSetAdd(manager->blocks, block);
        manager->lastBlock = block;
        if (txCount > 0) _BWPeerManagerUpdateTx(manager, txHashes, txCount, block->height, txTime);
        if (manager->downloadPeer) BWPeerSetCurrentBlockHeight(manager->downloadPeer, block->height);

        if (block->height < manager->estimatedHeight && peer == manager->downloadPeer) {
            BWPeerScheduleDisconnect(peer, PROTOCOL_TIMEOUT); // reschedule sync timeout
            manager->connectFailureCount = 0; // reset failure count once we know our initial request didn't timeout
        }

        if ((block->height % BLOCK_DIFFICULTY_INTERVAL) == 0) saveCount = 1; // save transition block immediately

        if (block->height == manager->estimatedHeight) { // chain download is complete
            saveCount = (block->height % BLOCK_DIFFICULTY_INTERVAL) + BLOCK_DIFFICULTY_INTERVAL + 1;
            _BWPeerManagerLoadMempools(manager);
        }
    }
    else if (BWSetContains(manager->blocks, block)) { // we already have the block (or at least the header)
        if ((block->height % 500) == 0 || txCount > 0 || block->height >= BWPeerLastBlock(peer)) {
            peer_log(peer, "relayed existing block #%"PRIu32, block->height);
        }

        b = manager->lastBlock;
        while (b && b->height > block->height) b = BWSetGet(manager->blocks, &b->prevBlock); // is block in main chain?

        if (BWMerkleBlockEq(b, block)) { // if it's not on a fork, set block heights for its transactions
            if (txCount > 0) _BWPeerManagerUpdateTx(manager, txHashes, txCount, block->height, txTime);
            if (block->height == manager->lastBlock->height) manager->lastBlock = block;
        }

        b = BWSetAdd(manager->blocks, block);

        if (b != block) {
            if (BWSetGet(manager->orphans, b) == b) BWSetRemove(manager->orphans, b);
            if (manager->lastOrphan == b) manager->lastOrphan = NULL;
            BWMerkleBlockFree(b);
        }
    }
    else if (manager->lastBlock->height < BWPeerLastBlock(peer) &&
             block->height > manager->lastBlock->height + 1) { // special case, new block mined durring rescan
        peer_log(peer, "marking new block #%"PRIu32" as orphan until rescan completes", block->height);
        BWSetAdd(manager->orphans, block); // mark as orphan til we're caught up
        manager->lastOrphan = block;
    }
    else if (block->height <= manager->params->checkpoints[manager->params->checkpointsCount - 1].height) { // old fork
        peer_log(peer, "ignoring block on fork older than most recent checkpoint, block #%"PRIu32", hash: %s",
                 block->height, u256hex(block->blockHash));
        BWMerkleBlockFree(block);
        block = NULL;
    }
    else { // new block is on a fork
        peer_log(peer, "chain fork reached height %"PRIu32, block->height);
        BWSetAdd(manager->blocks, block);

        if (block->height > manager->lastBlock->height) { // check if fork is now longer than main chain
            b = block;
            b2 = manager->lastBlock;

            while (b && b2 && ! BWMerkleBlockEq(b, b2)) { // walk back to where the fork joins the main chain
                b = BWSetGet(manager->blocks, &b->prevBlock);
                if (b && b->height < b2->height) b2 = BWSetGet(manager->blocks, &b2->prevBlock);
            }

            peer_log(peer, "reorganizing chain from height %"PRIu32", new height is %"PRIu32, b->height, block->height);

            BWWalletSetTxUnconfirmedAfter(manager->wallet, b->height); // mark tx after the join point as unconfirmed

            b = block;

            while (b && b2 && b->height > b2->height) { // set transaction heights for new main chain
                size_t count = BWMerkleBlockTxHashes(b, NULL, 0);
                uint32_t height = b->height, timestamp = b->timestamp;

                if (count > txCount) {
                    txHashes = (txHashes != _txHashes) ? realloc(txHashes, count*sizeof(*txHashes)) :
                               malloc(count*sizeof(*txHashes));
                    assert(txHashes != NULL);
                    txCount = count;
                }

                count = BWMerkleBlockTxHashes(b, txHashes, count);
                b = BWSetGet(manager->blocks, &b->prevBlock);
                if (b) timestamp = timestamp/2 + b->timestamp/2;
                if (count > 0) BWWalletUpdateTransactions(manager->wallet, txHashes, count, height, timestamp);
            }

            manager->lastBlock = block;

            if (block->height == manager->estimatedHeight) { // chain download is complete
                saveCount = (block->height % BLOCK_DIFFICULTY_INTERVAL) + BLOCK_DIFFICULTY_INTERVAL + 1;
                _BWPeerManagerLoadMempools(manager);
            }
        }
    }

    if (txHashes != _txHashes) free(txHashes);

    if (block && block->height != BLOCK_UNKNOWN_HEIGHT) {
        if (block->height > manager->estimatedHeight) manager->estimatedHeight = block->height;

        // check if the next block was received as an orphan
        orphan.prevBlock = block->blockHash;
        next = BWSetRemove(manager->orphans, &orphan);
    }

    BWMerkleBlock *saveBlocks[saveCount];

    for (i = 0, b = block; b && i < saveCount; i++) {
        assert(b->height != BLOCK_UNKNOWN_HEIGHT); // verify all blocks to be saved are in the chain
        saveBlocks[i] = b;
        b = BWSetGet(manager->blocks, &b->prevBlock);
    }

    // make sure the set of blocks to be saved starts at a difficulty interval
    j = (i > 0) ? saveBlocks[i - 1]->height % BLOCK_DIFFICULTY_INTERVAL : 0;
    if (j > 0) i -= (i > BLOCK_DIFFICULTY_INTERVAL - j) ? BLOCK_DIFFICULTY_INTERVAL - j : i;
    assert(i == 0 || (saveBlocks[i - 1]->height % BLOCK_DIFFICULTY_INTERVAL) == 0);
    pthread_mutex_unlock(&manager->lock);
    if (i > 0 && manager->saveBlocks) manager->saveBlocks(manager->info, (i > 1 ? 1 : 0), saveBlocks, i);

    if (block && block->height != BLOCK_UNKNOWN_HEIGHT && block->height >= BWPeerLastBlock(peer) &&
        manager->txStatusUpdate) {
        manager->txStatusUpdate(manager->info); // notify that transaction confirmations may have changed
    }

    if (next) _peerRelayedBlock(info, next);
}

static void _peerDataNotfound(void *info, const UInt256 txHashes[], size_t txCount,
                             const UInt256 blockHashes[], size_t blockCount)
{
    BWPeer *peer = ((BWPeerCallbackInfo *)info)->peer;
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;

    pthread_mutex_lock(&manager->lock);

    for (size_t i = 0; i < txCount; i++) {
        _BWTxPeerListRemovePeer(manager->txRelays, txHashes[i], peer);
        _BWTxPeerListRemovePeer(manager->txRequests, txHashes[i], peer);
    }

    pthread_mutex_unlock(&manager->lock);
}

static void _peerSetFeePerKb(void *info, uint64_t feePerKb)
{
    BWPeer *p, *peer = ((BWPeerCallbackInfo *)info)->peer;
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;
    uint64_t maxFeePerKb = 0, secondFeePerKb = 0;

    pthread_mutex_lock(&manager->lock);

    for (size_t i = array_count(manager->connectedPeers); i > 0; i--) { // find second highest fee rate
        p = manager->connectedPeers[i - 1];
        if (BWPeerConnectStatus(p) != BWPeerStatusConnected) continue;
        if (BWPeerFeePerKb(p) > maxFeePerKb) secondFeePerKb = maxFeePerKb, maxFeePerKb = BWPeerFeePerKb(p);
    }

    if (secondFeePerKb*3/2 > DEFAULT_FEE_PER_KB && secondFeePerKb*3/2 <= MAX_FEE_PER_KB &&
        secondFeePerKb*3/2 > BWWalletFeePerKb(manager->wallet)) {
        peer_log(peer, "increasing feePerKb to %"PRIu64" based on feefilter messages from peers", secondFeePerKb*3/2);
        BWWalletSetFeePerKb(manager->wallet, secondFeePerKb*3/2);
    }

    pthread_mutex_unlock(&manager->lock);
}

//static void _peerRequestedTxPingDone(void *info, int success)
//{
//    BWPeer *peer = ((BWPeerCallbackInfo *)info)->peer;
//    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;
//    UInt256 txHash = ((BWPeerCallbackInfo *)info)->hash;
//
//    free(info);
//    pthread_mutex_lock(&manager->lock);
//
//    if (success && ! _BWTxPeerListHasPeer(manager->txRequests, txHash, peer)) {
//        _BWTxPeerListAddPeer(&manager->txRequests, txHash, peer);
//        BWPeerSendGetdata(peer, &txHash, 1, NULL, 0); // check if peer will relay the transaction back
//    }
//
//    pthread_mutex_unlock(&manager->lock);
//}

static BWTransaction *_peerRequestedTx(void *info, UInt256 txHash)
{
    BWPeer *peer = ((BWPeerCallbackInfo *)info)->peer;
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;
//    BWPeerCallbackInfo *pingInfo;
    BWTransaction *tx = NULL;
    void *txInfo = NULL;
    void (*txCallback)(void *, int) = NULL;
    int hasPendingCallbacks = 0, error = 0;

    pthread_mutex_lock(&manager->lock);

    for (size_t i = array_count(manager->publishedTx); i > 0; i--) {
        if (UInt256Eq(manager->publishedTxHashes[i - 1], txHash)) {
            tx = manager->publishedTx[i - 1].tx;
            txInfo = manager->publishedTx[i - 1].info;
            txCallback = manager->publishedTx[i - 1].callback;
            manager->publishedTx[i - 1].info = NULL;
            manager->publishedTx[i - 1].callback = NULL;

            if (tx && ! BWWalletTransactionIsValid(manager->wallet, tx)) {
                error = EINVAL;
                array_rm(manager->publishedTx, i - 1);
                array_rm(manager->publishedTxHashes, i - 1);

                if (! BWWalletTransactionForHash(manager->wallet, txHash)) {
                    BWTransactionFree(tx);
                    tx = NULL;
                }
            }
        }
        else if (manager->publishedTx[i - 1].callback != NULL) hasPendingCallbacks = 1;
    }

    // cancel tx publish timeout if no publish callbacks are pending, and syncing is done or this is not downloadPeer
    if (! hasPendingCallbacks && (manager->syncStartHeight == 0 || peer != manager->downloadPeer)) {
        BWPeerScheduleDisconnect(peer, -1); // cancel publish tx timeout
    }

    if (tx && ! error) {
        _BWTxPeerListAddPeer(&manager->txRelays, txHash, peer);
        BWWalletRegisterTransaction(manager->wallet, tx);
    }

//    pingInfo = calloc(1, sizeof(*pingInfo));
//    assert(pingInfo != NULL);
//    pingInfo->peer = peer;
//    pingInfo->manager = manager;
//    pingInfo->hash = txHash;
//    BWPeerSendPing(peer, pingInfo, _peerRequestedTxPingDone);
    pthread_mutex_unlock(&manager->lock);
    if (txCallback) txCallback(txInfo, error);
    return tx;
}

static int _peerNetworkIsReachable(void *info)
{
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;

    return (manager->networkIsReachable) ? manager->networkIsReachable(manager->info) : 1;
}

static void _peerThreadCleanup(void *info)
{
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;

    free(info);
    if (manager->threadCleanup) manager->threadCleanup(manager->info);
}

static void _dummyThreadCleanup(void *info)
{
}

// returns a newly allocated BWPeerManager struct that must be freed by calling BWPeerManagerFree()
BWPeerManager *BWPeerManagerNew(const BWChainParams *params, BWWallet *wallet, uint32_t earliestKeyTime,
                                BWMerkleBlock *blocks[], size_t blocksCount, 
                                const BWPeer peers[], size_t peersCount,
                                double fpRate)
{
    BWPeerManager *manager = calloc(1, sizeof(*manager));
    BWMerkleBlock orphan, *block = NULL;

    assert(manager != NULL);
    assert(params != NULL);
    assert(params->standardPort != 0);
    assert(wallet != NULL);
    assert(blocks != NULL || blocksCount == 0);
    assert(peers != NULL || peersCount == 0);
    manager->params = params;
    manager->wallet = wallet;
    manager->earliestKeyTime = earliestKeyTime;
    manager->averageTxPerBlock = 1400;
    manager->maxConnectCount = PEER_MAX_CONNECTIONS;
    array_new(manager->peers, peersCount);
    if (peers) array_add_array(manager->peers, peers, peersCount);
    qsort(manager->peers, array_count(manager->peers), sizeof(*manager->peers), _peerTimestampCompare);
    array_new(manager->connectedPeers, PEER_MAX_CONNECTIONS);
    manager->blocks = BWSetNew(BWMerkleBlockHash, BWMerkleBlockEq, blocksCount);
    manager->orphans = BWSetNew(_BWPrevBlockHash, _BWPrevBlockEq, blocksCount); // orphans are indexed by prevBlock
    manager->checkpoints = BWSetNew(_BWBlockHeightHash, _BWBlockHeightEq, 100); // checkpoints are indexed by height
    manager->fpRate = fpRate; //loading the preferred rate

    for (size_t i = 0; i < manager->params->checkpointsCount; i++) {
        block = BWMerkleBlockNew();
        block->height = manager->params->checkpoints[i].height;
        block->blockHash = UInt256Reverse(manager->params->checkpoints[i].hash);
        block->timestamp = manager->params->checkpoints[i].timestamp;
        block->target = manager->params->checkpoints[i].target;
        BWSetAdd(manager->checkpoints, block);
        BWSetAdd(manager->blocks, block);
        if (i == 0 || block->timestamp + 7*24*60*60 < manager->earliestKeyTime) manager->lastBlock = block;
    }

    block = NULL;

    for (size_t i = 0; blocks && i < blocksCount; i++) {
        assert(blocks[i]->height != BLOCK_UNKNOWN_HEIGHT); // height must be saved/restored along with serialized block
        BWSetAdd(manager->orphans, blocks[i]);

        if ((blocks[i]->height % BLOCK_DIFFICULTY_INTERVAL) == 0 &&
            (! block || blocks[i]->height > block->height)) block = blocks[i]; // find last transition block
    }

    while (block) {
        BWSetAdd(manager->blocks, block);
        manager->lastBlock = block;
        orphan.prevBlock = block->prevBlock;
        BWSetRemove(manager->orphans, &orphan);
        orphan.prevBlock = block->blockHash;
        block = BWSetGet(manager->orphans, &orphan);
    }

    array_new(manager->txRelays, 10);
    array_new(manager->txRequests, 10);
    array_new(manager->publishedTx, 10);
    array_new(manager->publishedTxHashes, 10);
    pthread_mutex_init(&manager->lock, NULL);
    manager->threadCleanup = _dummyThreadCleanup;
    return manager;
}

// not thread-safe, set callbacks once before calling BWPeerManagerConnect()
// info is a void pointer that will be passed along with each callback call
// void syncStarted(void *) - called when blockchain syncing starts
// void syncStopped(void *, int) - called when blockchain syncing stops, error is an errno.h code
// void txStatusUpdate(void *) - called when transaction status may have changed such as when a new block arrives
// void saveBlocks(void *, int, BWMerkleBlock *[], size_t) - called when blocks should be saved to the persistent store
// - if replace is true, remove any previously saved blocks first
// void savePeers(void *, int, const BWPeer[], size_t) - called when peers should be saved to the persistent store
// - if replace is true, remove any previously saved peers first
// int networkIsReachable(void *) - must return true when networking is available, false otherwise
// void threadCleanup(void *) - called before a thread terminates to faciliate any needed cleanup
void BWPeerManagerSetCallbacks(BWPeerManager *manager, void *info,
                               void (*syncStarted)(void *info),
                               void (*syncStopped)(void *info, int error),
                               void (*txStatusUpdate)(void *info),
                               void (*saveBlocks)(void *info, int replace, BWMerkleBlock *blocks[], size_t blocksCount),
                               void (*savePeers)(void *info, int replace, const BWPeer peers[], size_t peersCount),
                               int (*networkIsReachable)(void *info),
                               void (*threadCleanup)(void *info))
{
    assert(manager != NULL);
    manager->info = info;
    manager->syncStarted = syncStarted;
    manager->syncStopped = syncStopped;
    manager->txStatusUpdate = txStatusUpdate;
    manager->saveBlocks = saveBlocks;
    manager->savePeers = savePeers;
    manager->networkIsReachable = networkIsReachable;
    manager->threadCleanup = (threadCleanup) ? threadCleanup : _dummyThreadCleanup;
}

// specifies a single fixed peer to use when connecting to the bitcoin network
// set address to UINT128_ZERO to revert to default behavior
void BWPeerManagerSetFixedPeer(BWPeerManager *manager, UInt128 address, uint16_t port)
{
    assert(manager != NULL);
    BWPeerManagerDisconnect(manager);
    pthread_mutex_lock(&manager->lock);
    manager->maxConnectCount = UInt128IsZero(address) ? PEER_MAX_CONNECTIONS : 1;
    manager->fixedPeer = ((BWPeer) { address, port, 0, 0, 0 });
    array_clear(manager->peers);
    pthread_mutex_unlock(&manager->lock);
}

uint16_t BWPeerManagerStandardPort(BWPeerManager *manager)
{
    assert(manager != NULL);
    pthread_mutex_lock(&manager->lock);
    uint16_t port = manager->params->standardPort;
    pthread_mutex_unlock(&manager->lock);
    return port;
}

// current connect status
BWPeerStatus BWPeerManagerConnectStatus(BWPeerManager *manager)
{
    BWPeerStatus status = BWPeerStatusDisconnected;
    
    assert(manager != NULL);
    pthread_mutex_lock(&manager->lock);
    if (manager->isConnected != 0) status = BWPeerStatusConnected;

    for (size_t i = array_count(manager->connectedPeers); i > 0 && status == BWPeerStatusDisconnected; i--) {
        if (BWPeerConnectStatus(manager->connectedPeers[i - 1]) == BWPeerStatusDisconnected) continue;
        status = BWPeerStatusConnecting;
    }

    pthread_mutex_unlock(&manager->lock);
    return status;
}

// connect to bitcoin peer-to-peer network (also call this whenever networkIsReachable() status changes)
void BWPeerManagerConnect(BWPeerManager *manager)
{
    assert(manager != NULL);
    pthread_mutex_lock(&manager->lock);
    if (manager->connectFailureCount >= MAX_CONNECT_FAILURES) manager->connectFailureCount = 0; //this is a manual retry

    if ((! manager->downloadPeer || manager->lastBlock->height < manager->estimatedHeight) &&
        manager->syncStartHeight == 0) {
        manager->syncStartHeight = manager->lastBlock->height + 1;
        pthread_mutex_unlock(&manager->lock);
        if (manager->syncStarted) manager->syncStarted(manager->info);
        pthread_mutex_lock(&manager->lock);
    }

    for (size_t i = array_count(manager->connectedPeers); i > 0; i--) {
        BWPeer *p = manager->connectedPeers[i - 1];

        if (BWPeerConnectStatus(p) == BWPeerStatusConnecting) BWPeerConnect(p);
    }

    if (array_count(manager->connectedPeers) < manager->maxConnectCount) {
        time_t now = time(NULL);
        BWPeer *peers;

        if (array_count(manager->peers) < manager->maxConnectCount ||
            manager->peers[manager->maxConnectCount - 1].timestamp + 3*24*60*60 < now) {
            _BWPeerManagerFindPeers(manager);
        }

        array_new(peers, 100);
        array_add_array(peers, manager->peers,
                        (array_count(manager->peers) < 100) ? array_count(manager->peers) : 100);

        while (array_count(peers) > 0 && array_count(manager->connectedPeers) < manager->maxConnectCount) {
            size_t i = BWRand((uint32_t)array_count(peers)); // index of random peer
            BWPeerCallbackInfo *info;

            i = i*i/array_count(peers); // bias random peer selection toward peers with more recent timestamp

            for (size_t j = array_count(manager->connectedPeers); i != SIZE_MAX && j > 0; j--) {
                if (! BWPeerEq(&peers[i], manager->connectedPeers[j - 1])) continue;
                array_rm(peers, i); // already in connectedPeers
                i = SIZE_MAX;
            }

            if (i != SIZE_MAX) {
                info = calloc(1, sizeof(*info));
                assert(info != NULL);
                info->manager = manager;
                info->peer = BWPeerNew(manager->params->magicNumber);
                *info->peer = peers[i];
                array_rm(peers, i);
                array_add(manager->connectedPeers, info->peer);
                BWPeerSetCallbacks(info->peer, info, _peerConnected, _peerDisconnected, _peerRelayedPeers,
                                   _peerRelayedTx, _peerHasTx, _peerRejectedTx, _peerRelayedBlock, _peerDataNotfound,
                                   _peerSetFeePerKb, _peerRequestedTx, _peerNetworkIsReachable, _peerThreadCleanup);
                BWPeerSetEarliestKeyTime(info->peer, manager->earliestKeyTime);
                BWPeerConnect(info->peer);
            }
        }

        array_free(peers);
    }

    if (array_count(manager->connectedPeers) == 0) {
        peer_log(&BW_PEER_NONE, "sync failed");
        _BWPeerManagerSyncStopped(manager);
        pthread_mutex_unlock(&manager->lock);
        if (manager->syncStopped) manager->syncStopped(manager->info, ENETUNREACH);
    }
    else pthread_mutex_unlock(&manager->lock);
}

void BWPeerManagerDisconnect(BWPeerManager *manager)
{
    struct timespec ts;
    size_t peerCount, dnsThreadCount;

    assert(manager != NULL);
    pthread_mutex_lock(&manager->lock);
    peerCount = array_count(manager->connectedPeers);
    dnsThreadCount = manager->dnsThreadCount;

    for (size_t i = peerCount; i > 0; i--) {
        manager->connectFailureCount = MAX_CONNECT_FAILURES; // prevent futher automatic reconnect attempts
        BWPeerDisconnect(manager->connectedPeers[i - 1]);
    }

    pthread_mutex_unlock(&manager->lock);
    ts.tv_sec = 0;
    ts.tv_nsec = 1;

    while (peerCount > 0 || dnsThreadCount > 0) {
        nanosleep(&ts, NULL); // pthread_yield() isn't POSIX standard :(
        pthread_mutex_lock(&manager->lock);
        peerCount = array_count(manager->connectedPeers);
        dnsThreadCount = manager->dnsThreadCount;
        pthread_mutex_unlock(&manager->lock);
    }
}

// rescans blocks and transactions after earliestKeyTime (a new random download peer is also selected due to the
// possibility that a malicious node might lie by omitting transactions that match the bloom filter)
void BWPeerManagerRescan(BWPeerManager *manager)
{
    assert(manager != NULL);
    pthread_mutex_lock(&manager->lock);

    if (manager->isConnected) {
        // start the chain download from the most recent checkpoint that's at least a week older than earliestKeyTime
        for (size_t i = manager->params->checkpointsCount; i > 0; i--) {
            if (i - 1 == 0 || manager->params->checkpoints[i - 1].timestamp + 7*24*60*60 < manager->earliestKeyTime) {
                UInt256 hash = UInt256Reverse(manager->params->checkpoints[i - 1].hash);

                manager->lastBlock = BWSetGet(manager->blocks, &hash);
                break;
            }
        }

        if (manager->downloadPeer) { // disconnect the current download peer so a new random one will be selected
            for (size_t i = array_count(manager->peers); i > 0; i--) {
                if (BWPeerEq(&manager->peers[i - 1], manager->downloadPeer)) array_rm(manager->peers, i - 1);
            }

            BWPeerDisconnect(manager->downloadPeer);
        }

        manager->syncStartHeight = 0; // a syncStartHeight of 0 indicates that syncing hasn't started yet
        pthread_mutex_unlock(&manager->lock);
        BWPeerManagerConnect(manager);
    }
    else pthread_mutex_unlock(&manager->lock);
}

// the (unverified) best block height reported by connected peers
uint32_t BWPeerManagerEstimatedBlockHeight(BWPeerManager *manager)
{
    uint32_t height;

    assert(manager != NULL);
    pthread_mutex_lock(&manager->lock);
    height = (manager->lastBlock->height < manager->estimatedHeight) ? manager->estimatedHeight :
             manager->lastBlock->height;
    pthread_mutex_unlock(&manager->lock);
    return height;
}

// current proof-of-work verified best block height
uint32_t BWPeerManagerLastBlockHeight(BWPeerManager *manager)
{
    uint32_t height;

    assert(manager != NULL);
    pthread_mutex_lock(&manager->lock);
    height = manager->lastBlock->height;
    pthread_mutex_unlock(&manager->lock);
    return height;
}

// current proof-of-work verified best block timestamp (time interval since unix epoch)
uint32_t BWPeerManagerLastBlockTimestamp(BWPeerManager *manager)
{
    uint32_t timestamp;

    assert(manager != NULL);
    pthread_mutex_lock(&manager->lock);
    timestamp = manager->lastBlock->timestamp;
    pthread_mutex_unlock(&manager->lock);
    return timestamp;
}

// current network sync progress from 0 to 1
// startHeight is the block height of the most recent fully completed sync
double BWPeerManagerSyncProgress(BWPeerManager *manager, uint32_t startHeight)
{
    double progress;

    assert(manager != NULL);
    pthread_mutex_lock(&manager->lock);
    if (startHeight == 0) startHeight = manager->syncStartHeight;

    if (! manager->downloadPeer && manager->syncStartHeight == 0) {
        progress = 0.0;
    }
    else if (! manager->downloadPeer || manager->lastBlock->height < manager->estimatedHeight) {
        if (manager->lastBlock->height > startHeight && manager->estimatedHeight > startHeight) {
            progress = 0.1 + 0.9*(manager->lastBlock->height - startHeight)/(manager->estimatedHeight - startHeight);
        }
        else progress = 0.05;
    }
    else progress = 1.0;

    pthread_mutex_unlock(&manager->lock);
    return progress;
}

// returns the number of currently connected peers
size_t BWPeerManagerPeerCount(BWPeerManager *manager)
{
    size_t count = 0;

    assert(manager != NULL);
    pthread_mutex_lock(&manager->lock);

    for (size_t i = array_count(manager->connectedPeers); i > 0; i--) {
        if (BWPeerConnectStatus(manager->connectedPeers[i - 1]) != BWPeerStatusDisconnected) count++;
    }

    pthread_mutex_unlock(&manager->lock);
    return count;
}

// description of the peer most recently used to sync blockchain data
const char *BWPeerManagerDownloadPeerName(BWPeerManager *manager)
{
    assert(manager != NULL);
    pthread_mutex_lock(&manager->lock);

    if (manager->downloadPeer) {
        sprintf(manager->downloadPeerName, "%s:%d", BWPeerHost(manager->downloadPeer), manager->downloadPeer->port);
    }
    else manager->downloadPeerName[0] = '\0';

    pthread_mutex_unlock(&manager->lock);
    return manager->downloadPeerName;
}

static void _publishTxInvDone(void *info, int success)
{
    BWPeer *peer = ((BWPeerCallbackInfo *)info)->peer;
    BWPeerManager *manager = ((BWPeerCallbackInfo *)info)->manager;

    free(info);
    pthread_mutex_lock(&manager->lock);
    _BWPeerManagerRequestUnrelayedTx(manager, peer);
    pthread_mutex_unlock(&manager->lock);
}

// publishes tx to bitcoin network (do not call BWTransactionFree() on tx afterward)
void BWPeerManagerPublishTx(BWPeerManager *manager, BWTransaction *tx, void *info,
                            void (*callback)(void *info, int error))
{
    assert(manager != NULL);
    assert(tx != NULL && BWTransactionIsSigned(tx));
    if (tx) pthread_mutex_lock(&manager->lock);

    if (tx && ! BWTransactionIsSigned(tx)) {
        pthread_mutex_unlock(&manager->lock);
        BWTransactionFree(tx);
        tx = NULL;
        if (callback) callback(info, EINVAL); // transaction not signed
    }
    else if (tx && ! manager->isConnected) {
        int connectFailureCount = manager->connectFailureCount;

        pthread_mutex_unlock(&manager->lock);

        if (connectFailureCount >= MAX_CONNECT_FAILURES ||
            (manager->networkIsReachable && ! manager->networkIsReachable(manager->info))) {
            BWTransactionFree(tx);
            tx = NULL;
            if (callback) callback(info, ENOTCONN); // not connected to bitcoin network
        }
        else pthread_mutex_lock(&manager->lock);
    }

    if (tx) {
        size_t i, count = 0;

        tx->timestamp = (uint32_t)time(NULL); // set timestamp to publish time
        _BWPeerManagerAddTxToPublishList(manager, tx, info, callback);

        for (i = array_count(manager->connectedPeers); i > 0; i--) {
            if (BWPeerConnectStatus(manager->connectedPeers[i - 1]) == BWPeerStatusConnected) count++;
        }

        for (i = array_count(manager->connectedPeers); i > 0; i--) {
            BWPeer *peer = manager->connectedPeers[i - 1];
            BWPeerCallbackInfo *peerInfo;

            if (BWPeerConnectStatus(peer) != BWPeerStatusConnected) continue;

            // instead of publishing to all peers, leave out downloadPeer to see if tx propogates/gets relayed back
            // TODO: XXX connect to a random peer with an empty or fake bloom filter just for publishing
            if (peer != manager->downloadPeer || count == 1) {
                _BWPeerManagerPublishPendingTx(manager, peer);
                peerInfo = calloc(1, sizeof(*peerInfo));
                assert(peerInfo != NULL);
                peerInfo->peer = peer;
                peerInfo->manager = manager;
                BWPeerSendPing(peer, peerInfo, _publishTxInvDone);
            }
        }

        pthread_mutex_unlock(&manager->lock);
    }
}

// number of connected peers that have relayed the given unconfirmed transaction
size_t BWPeerManagerRelayCount(BWPeerManager *manager, UInt256 txHash)
{
    size_t count = 0;

    assert(manager != NULL);
    assert(! UInt256IsZero(txHash));
    pthread_mutex_lock(&manager->lock);

    for (size_t i = array_count(manager->txRelays); i > 0; i--) {
        if (! UInt256Eq(manager->txRelays[i - 1].txHash, txHash)) continue;
        count = array_count(manager->txRelays[i - 1].peers);
        break;
    }

    pthread_mutex_unlock(&manager->lock);
    return count;
}

// frees memory allocated for manager
void BWPeerManagerFree(BWPeerManager *manager)
{
    assert(manager != NULL);
    pthread_mutex_lock(&manager->lock);
    array_free(manager->peers);
    for (size_t i = array_count(manager->connectedPeers); i > 0; i--) BWPeerFree(manager->connectedPeers[i - 1]);
    array_free(manager->connectedPeers);
    BWSetApply(manager->blocks, NULL, _setApplyFreeBlock);
    BWSetFree(manager->blocks);
    BWSetApply(manager->orphans, NULL, _setApplyFreeBlock);
    BWSetFree(manager->orphans);
    BWSetFree(manager->checkpoints);
    for (size_t i = array_count(manager->txRelays); i > 0; i--) free(manager->txRelays[i - 1].peers);
    array_free(manager->txRelays);
    for (size_t i = array_count(manager->txRequests); i > 0; i--) free(manager->txRequests[i - 1].peers);
    array_free(manager->txRequests);
    array_free(manager->publishedTx);
    array_free(manager->publishedTxHashes);
    pthread_mutex_unlock(&manager->lock);
    pthread_mutex_destroy(&manager->lock);
    free(manager);
}
