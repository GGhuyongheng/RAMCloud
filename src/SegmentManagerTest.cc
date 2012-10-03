/* Copyright (c) 2012 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "TestUtil.h"

#include "SegmentManager.h"
#include "SegmentIterator.h"
#include "LogMetadata.h"
#include "ServerConfig.h"
#include "ServerRpcPool.h"

namespace RAMCloud {

/**
 * Unit tests for SegmentManager.
 */
class SegmentManagerTest : public ::testing::Test {
  public:
    Context context;
    ServerId serverId;
    ServerList serverList;
    ServerConfig serverConfig;
    ReplicaManager replicaManager;
    SegletAllocator allocator;
    SegmentManager segmentManager;

    SegmentManagerTest()
        : context(),
          serverId(ServerId(57, 0)),
          serverList(&context),
          serverConfig(ServerConfig::forTesting()),
          replicaManager(&context, serverId, 0),
          allocator(serverConfig),
          segmentManager(&context, serverConfig, serverId,
                         allocator, replicaManager)
    {
    }

  private:
    DISALLOW_COPY_AND_ASSIGN(SegmentManagerTest);
};

TEST_F(SegmentManagerTest, constructor)
{
    serverConfig.master.diskExpansionFactor = 0.99;
    EXPECT_THROW(SegmentManager(&context,
                                serverConfig,
                                serverId,
                                allocator,
                                replicaManager),
                 SegmentManagerException);

    EXPECT_EQ(0U, segmentManager.nextSegmentId);
    EXPECT_EQ(0, segmentManager.logIteratorCount);
    EXPECT_EQ(256U, segmentManager.maxSegments);
    EXPECT_EQ(segmentManager.maxSegments - 2,
              segmentManager.freeSlots.size());
    EXPECT_EQ(2U, allocator.getFreeCount(SegletAllocator::EMERGENCY_HEAD));
}

TEST_F(SegmentManagerTest, destructor) {
    SegletAllocator allocator2(serverConfig);
    Tub<SegmentManager> mgr;
    mgr.construct(&context, serverConfig, serverId, allocator2, replicaManager);
    EXPECT_EQ(2U, allocator2.getFreeCount(SegletAllocator::EMERGENCY_HEAD));
    EXPECT_EQ(0U, allocator2.getFreeCount(SegletAllocator::CLEANER));
    EXPECT_EQ(254U, allocator2.getFreeCount(SegletAllocator::DEFAULT));
    mgr->allocHead(false);
    mgr->allocHead(false);
    EXPECT_EQ(252U, allocator2.getFreeCount(SegletAllocator::DEFAULT));

    mgr.destroy();
    EXPECT_EQ(254U, allocator2.getFreeCount(SegletAllocator::DEFAULT));
}

static bool
allocFilter(string s)
{
    return s == "alloc";
}

TEST_F(SegmentManagerTest, allocHead) {
    TestLog::Enable _(allocFilter);

    EXPECT_EQ(static_cast<LogSegment*>(NULL), segmentManager.getHeadSegment());
    LogSegment* head = segmentManager.allocHead(false);
    EXPECT_EQ("alloc: for head of log", TestLog::get());
    EXPECT_NE(static_cast<LogSegment*>(NULL), head);
    EXPECT_EQ(head, segmentManager.getHeadSegment());

    SegmentIterator it(*head);
    EXPECT_FALSE(it.isDone());
    EXPECT_EQ(LOG_ENTRY_TYPE_SEGHEADER, it.getType());
    Buffer buffer;
    it.appendToBuffer(buffer);
    EXPECT_EQ(Segment::INVALID_SEGMENT_ID,
              buffer.getStart<SegmentHeader>()->headSegmentIdDuringCleaning);

    it.next();
    EXPECT_FALSE(it.isDone());
    EXPECT_EQ(LOG_ENTRY_TYPE_LOGDIGEST, it.getType());

    it.next();
    EXPECT_FALSE(it.isDone());
    EXPECT_EQ(LOG_ENTRY_TYPE_SAFEVERSION, it.getType());

    it.next();
    EXPECT_TRUE(it.isDone());

    LogSegment* oldHead = head;
    head = segmentManager.allocHead(false);
    EXPECT_NE(static_cast<LogSegment*>(NULL), head);
    EXPECT_NE(head, oldHead);
    EXPECT_EQ(head, segmentManager.getHeadSegment());
    EXPECT_EQ(oldHead,
       &segmentManager.segmentsByState[SegmentManager::NEWLY_CLEANABLE].back());
    EXPECT_TRUE(oldHead->closed);

    int successes = 0;
    while (segmentManager.allocHead(false) != NULL)
        successes++;
    EXPECT_EQ(252, successes);
    EXPECT_EQ(253U,
        segmentManager.segmentsByState[SegmentManager::NEWLY_CLEANABLE].size());
}

TEST_F(SegmentManagerTest, allocSurvivor) {
    TestLog::Enable _(allocFilter);

    segmentManager.initializeSurvivorReserve(1);
    LogSegment* s = segmentManager.allocSurvivor(5);
    EXPECT_NE(static_cast<LogSegment*>(NULL), s);
    EXPECT_EQ("alloc: for cleaner", TestLog::get());

    SegmentIterator it(*s);
    EXPECT_FALSE(it.isDone());
    EXPECT_EQ(LOG_ENTRY_TYPE_SEGHEADER, it.getType());
    Buffer buffer;
    it.appendToBuffer(buffer);
    EXPECT_EQ(5U,
        buffer.getStart<SegmentHeader>()->headSegmentIdDuringCleaning);

    segmentManager.testing_allocSurvivorMustNotBlock = true;
    EXPECT_EQ(static_cast<LogSegment*>(NULL), segmentManager.allocSurvivor(12));
}

TEST_F(SegmentManagerTest, cleaningComplete) {
    LogSegment* cleaned = segmentManager.allocHead(false);
    EXPECT_NE(static_cast<LogSegment*>(NULL), cleaned);
    segmentManager.allocHead(false);
    EXPECT_NE(cleaned, segmentManager.getHeadSegment());

    segmentManager.initializeSurvivorReserve(1);
    LogSegment* survivor = segmentManager.allocSurvivor(3);
    EXPECT_NE(static_cast<LogSegment*>(NULL), survivor);

    ServerRpcPoolInternal::currentEpoch = 17530;

    LogSegmentVector clean;
    clean.push_back(cleaned);
    segmentManager.cleaningComplete(clean);

    EXPECT_EQ(1U, segmentManager.segmentsByState[
        SegmentManager::CLEANABLE_PENDING_DIGEST].size());
    EXPECT_EQ(survivor, &segmentManager.segmentsByState[
        SegmentManager::CLEANABLE_PENDING_DIGEST].back());

    EXPECT_EQ(1U, segmentManager.segmentsByState[
        SegmentManager::FREEABLE_PENDING_DIGEST_AND_REFERENCES].size());
    EXPECT_EQ(cleaned, &segmentManager.segmentsByState[
        SegmentManager::FREEABLE_PENDING_DIGEST_AND_REFERENCES].back());
    EXPECT_EQ(17531U, ServerRpcPoolInternal::currentEpoch);
    EXPECT_EQ(17530U, cleaned->cleanedEpoch);
}

TEST_F(SegmentManagerTest, cleanableSegments) {
    LogSegmentVector cleanable;

    EXPECT_EQ(0U, segmentManager.segmentsByState[
        SegmentManager::NEWLY_CLEANABLE].size());
    segmentManager.cleanableSegments(cleanable);
    EXPECT_EQ(0U, cleanable.size());

    segmentManager.allocHead(false);
    EXPECT_EQ(0U, segmentManager.segmentsByState[
        SegmentManager::NEWLY_CLEANABLE].size());
    segmentManager.cleanableSegments(cleanable);
    EXPECT_EQ(0U, cleanable.size());

    segmentManager.allocHead(false);
    EXPECT_EQ(1U, segmentManager.segmentsByState[
        SegmentManager::NEWLY_CLEANABLE].size());
    segmentManager.cleanableSegments(cleanable);
    EXPECT_EQ(0U, segmentManager.segmentsByState[
        SegmentManager::NEWLY_CLEANABLE].size());
    EXPECT_EQ(1U, segmentManager.segmentsByState[
        SegmentManager::CLEANABLE].size());
    EXPECT_EQ(1U, cleanable.size());
    EXPECT_EQ(cleanable[0], &segmentManager.segmentsByState[
        SegmentManager::CLEANABLE].back());

    cleanable.clear();
    EXPECT_EQ(0U, segmentManager.segmentsByState[
        SegmentManager::NEWLY_CLEANABLE].size());
    segmentManager.allocHead(false);
    EXPECT_EQ(1U, segmentManager.segmentsByState[
        SegmentManager::NEWLY_CLEANABLE].size());
    segmentManager.cleanableSegments(cleanable);
    EXPECT_EQ(0U, segmentManager.segmentsByState[
        SegmentManager::NEWLY_CLEANABLE].size());
    EXPECT_EQ(2U, segmentManager.segmentsByState[
        SegmentManager::CLEANABLE].size());
    EXPECT_EQ(1U, cleanable.size());
}

TEST_F(SegmentManagerTest, logIteratorCreated_and_logIteratorDestroyed) {
    EXPECT_EQ(0, segmentManager.logIteratorCount);
    segmentManager.logIteratorCreated();
    EXPECT_EQ(1, segmentManager.logIteratorCount);
    segmentManager.logIteratorCreated();
    EXPECT_EQ(2, segmentManager.logIteratorCount);
    segmentManager.logIteratorDestroyed();
    segmentManager.logIteratorDestroyed();
    EXPECT_EQ(0, segmentManager.logIteratorCount);
}

TEST_F(SegmentManagerTest, getActiveSegments) {
    LogSegmentVector active;

    EXPECT_THROW(segmentManager.getActiveSegments(0, active),
        SegmentManagerException);
    segmentManager.logIteratorCreated();
    EXPECT_NO_THROW(segmentManager.getActiveSegments(0, active));
    EXPECT_EQ(0U, active.size());

    LogSegment* newlyCleanable = segmentManager.allocHead(false);
    LogSegment* cleanable = segmentManager.allocHead(false);
    LogSegment* freeablePendingJunk = segmentManager.allocHead(false);
    LogSegment* head = segmentManager.allocHead(false);

    // "newlyCleanable" is in the correct state already, as is "head"
    segmentManager.changeState(*cleanable, SegmentManager::NEWLY_CLEANABLE);
    segmentManager.changeState(*freeablePendingJunk,
        SegmentManager::FREEABLE_PENDING_DIGEST_AND_REFERENCES);

    segmentManager.getActiveSegments(0, active);
    EXPECT_EQ(4U, active.size());
    EXPECT_EQ(newlyCleanable, active[0]);
    EXPECT_EQ(cleanable, active[1]);
    EXPECT_EQ(freeablePendingJunk, active[2]);
    EXPECT_EQ(head, active[3]);

    active.clear();
    segmentManager.getActiveSegments(2, active);
    EXPECT_EQ(2U, active.size());
    EXPECT_EQ(freeablePendingJunk, active[0]);
    EXPECT_EQ(head, active[1]);

    active.clear();
    segmentManager.getActiveSegments(head->id + 1, active);
    EXPECT_EQ(0U, active.size());

    segmentManager.logIteratorDestroyed();
}

TEST_F(SegmentManagerTest, initializeSurvivorSegmentReserve) {
    LogSegment* nullSeg = NULL;
    segmentManager.testing_allocSurvivorMustNotBlock = true;
    EXPECT_EQ(nullSeg, segmentManager.allocSurvivor(42));
    EXPECT_TRUE(segmentManager.initializeSurvivorReserve(1));
    LogSegment* s = segmentManager.allocSurvivor(42);
    EXPECT_NE(nullSeg, s);
}

TEST_F(SegmentManagerTest, indexOperator) {
    for (uint32_t i = 0; i < segmentManager.maxSegments + 5; i++)
        EXPECT_THROW(segmentManager[i], SegmentManagerException);
    LogSegment* head = segmentManager.allocHead(false);
    EXPECT_EQ(&*segmentManager.segments[head->slot],
        &segmentManager[head->slot]);
}

#if 0 // XXXX reenable me once Ryan fixes the RS::free() bug!
TEST_F(SegmentManagerTest, doesIdExist) {
    EXPECT_FALSE(segmentManager.doesIdExist(0));

    LogSegment* oldHead = segmentManager.allocHead(false);
    EXPECT_TRUE(segmentManager.doesIdExist(0));
    EXPECT_FALSE(segmentManager.doesIdExist(1));

    segmentManager.allocHead(false);
    EXPECT_TRUE(segmentManager.doesIdExist(0));
    EXPECT_TRUE(segmentManager.doesIdExist(1));

    segmentManager.free(oldHead);
    EXPECT_FALSE(segmentManager.doesIdExist(0));
    EXPECT_TRUE(segmentManager.doesIdExist(1));
}
#endif

// getFreeSegmentCount, getMaximumSegmentCount, getSegletSize, & getSegmentSize
// aren't paricularly interesting

TEST_F(SegmentManagerTest, writeHeader) {
    LogSegment* s = segmentManager.alloc(SegletAllocator::DEFAULT, 42);
    SegmentIterator sanity(*s);
    EXPECT_TRUE(sanity.isDone());

    *const_cast<uint64_t*>(&s->id) = 5723;
    segmentManager.writeHeader(s, 28);
    SegmentIterator it(*s);
    EXPECT_FALSE(it.isDone());
    Buffer buffer;
    it.appendToBuffer(buffer);
    const SegmentHeader* header = buffer.getStart<SegmentHeader>();
    EXPECT_EQ(*serverId, header->logId);
    EXPECT_EQ(5723U, header->segmentId);
    EXPECT_EQ(serverConfig.segmentSize, header->capacity);
    EXPECT_EQ(28U, header->headSegmentIdDuringCleaning);
}

TEST_F(SegmentManagerTest, writeDigest) {
    // TODO(steve): write me.
}

TEST_F(SegmentManagerTest, getHeadSegment) {
    EXPECT_EQ(static_cast<LogSegment*>(NULL), segmentManager.getHeadSegment());
    segmentManager.allocHead(false);
    EXPECT_NE(static_cast<LogSegment*>(NULL), segmentManager.getHeadSegment());
    EXPECT_EQ(&segmentManager.segmentsByState[SegmentManager::HEAD].back(),
        segmentManager.getHeadSegment());
}

TEST_F(SegmentManagerTest, changeState) {
    LogSegment* s = segmentManager.allocHead(false);
    EXPECT_EQ(SegmentManager::HEAD, *segmentManager.states[s->slot]);
    EXPECT_EQ(1U, segmentManager.segmentsByState[SegmentManager::HEAD].size());
    EXPECT_EQ(0U, segmentManager.segmentsByState[
        SegmentManager::CLEANABLE].size());
    segmentManager.changeState(*s, SegmentManager::CLEANABLE);
    EXPECT_EQ(SegmentManager::CLEANABLE, *segmentManager.states[s->slot]);
    EXPECT_EQ(0U, segmentManager.segmentsByState[SegmentManager::HEAD].size());
    EXPECT_EQ(1U, segmentManager.segmentsByState[
        SegmentManager::CLEANABLE].size());
}

TEST_F(SegmentManagerTest, alloc) {
    // TODO(steve): write me.
}

#if 0   // XXXXXX- another RS::free() issue
TEST_F(SegmentManagerTest, free) {
    LogSegment* s = segmentManager.allocHead(false);

    EXPECT_EQ(5U, segmentManager.freeSlots.size());
    EXPECT_TRUE(contains(segmentManager.idToSlotMap, s->id));
    EXPECT_EQ(1U, segmentManager.segmentsByState[SegmentManager::HEAD].size());
    EXPECT_EQ(1U, segmentManager.allSegments.size());
    EXPECT_TRUE(segmentManager.states[s->slot]);
    EXPECT_TRUE(segmentManager.segments[s->slot]);

    uint64_t id = s->id;
    uint32_t slot = s->slot;

    segmentManager.free(s);
    EXPECT_EQ(6U, segmentManager.freeSlots.size());
    EXPECT_FALSE(contains(segmentManager.idToSlotMap, id));
    EXPECT_EQ(0U, segmentManager.segmentsByState[SegmentManager::HEAD].size());
    EXPECT_EQ(0U, segmentManager.allSegments.size());
    EXPECT_FALSE(segmentManager.states[slot]);
    EXPECT_FALSE(segmentManager.segments[slot]);
}
#endif

TEST_F(SegmentManagerTest, addToLists) {
    EXPECT_EQ(0U, segmentManager.segmentsByState[SegmentManager::HEAD].size());
    EXPECT_EQ(0U, segmentManager.allSegments.size());
    LogSegment*s = segmentManager.allocHead(false);
    // allocHead implicitly calls addToLists...
    EXPECT_EQ(1U, segmentManager.segmentsByState[SegmentManager::HEAD].size());
    EXPECT_EQ(s, &segmentManager.segmentsByState[SegmentManager::HEAD].back());
    EXPECT_EQ(1U, segmentManager.allSegments.size());
    EXPECT_EQ(s, &segmentManager.allSegments.back());
}

TEST_F(SegmentManagerTest, removeFromLists) {
    LogSegment* s = segmentManager.allocHead(false);
    EXPECT_EQ(1U, segmentManager.segmentsByState[SegmentManager::HEAD].size());
    EXPECT_EQ(1U, segmentManager.allSegments.size());
    segmentManager.removeFromLists(*s);
    EXPECT_EQ(0U, segmentManager.segmentsByState[SegmentManager::HEAD].size());
    EXPECT_EQ(0U, segmentManager.allSegments.size());

    segmentManager.addToLists(*s);
}

// Need a do-nothing subclass of the abstract parent type.
class TestServerRpc : public Transport::ServerRpc {
    void sendReply() {}
    string getClientServiceLocator() { return ""; }
};

#if 0 /// XXX more RS bullshit
TEST_F(SegmentManagerTest, freeUnreferencedSegments) {
    LogSegment* freeable = segmentManager.allocHead(false);
    segmentManager.allocHead(false);

    ServerRpcPoolInternal::currentEpoch = 8;
    ServerRpcPool<TestServerRpc> pool;
    TestServerRpc* rpc = pool.construct();

    segmentManager.changeState(*freeable,
        SegmentManager::FREEABLE_PENDING_REFERENCES);

    freeable->cleanedEpoch = 8;
    segmentManager.freeUnreferencedSegments();
    EXPECT_EQ(1U, segmentManager.segmentsByState[
        SegmentManager::FREEABLE_PENDING_REFERENCES].size());

    freeable->cleanedEpoch = 9;
    segmentManager.freeUnreferencedSegments();
    EXPECT_EQ(1U, segmentManager.segmentsByState[
        SegmentManager::FREEABLE_PENDING_REFERENCES].size());

    freeable->cleanedEpoch = 7;
    segmentManager.freeUnreferencedSegments();
    EXPECT_EQ(0U, segmentManager.segmentsByState[
        SegmentManager::FREEABLE_PENDING_REFERENCES].size());

    pool.destroy(rpc);
}
#endif

} // namespace RAMCloud