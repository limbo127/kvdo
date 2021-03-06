/*
 * Copyright (c) 2018 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/uds-releases/flanders/src/uds/pageCache.h#2 $
 */

#ifndef PAGE_CACHE_H_
#define PAGE_CACHE_H_

#include "cacheCounters.h"
#include "chapterIndex.h"
#include "common.h"
#include "compiler.h"
#include "indexConfig.h"
#include "opaqueTypes.h"
#include "permassert.h"
#include "request.h"

STAILQ_HEAD(udsQueueHead, request);
typedef struct udsQueueHead UdsQueueHead;

typedef struct cachedPage {
  /* whether this page is currently being read asynchronously */
  bool                readPending;
  /* if equal to numCacheEntries, the page is invalid */
  unsigned int        physicalPage;
  /* the value of the volume clock when this page was cached */
  uint64_t            birth;
  /* the value of the volume clock when this page was last used */
  uint64_t            lastUsed;
  /* the cache page data */
  byte               *data;
  /* the chapter index page. This is here, even for record pages */
  ChapterIndexPage    indexPage;
} CachedPage;

enum {
  VOLUME_CACHE_MAX_ENTRIES              = (UINT16_MAX >> 1),
  VOLUME_CACHE_QUEUED_FLAG              = (1 << 15),
  VOLUME_CACHE_DEFAULT_MAX_QUEUED_READS = 4096
};

typedef struct queuedRead {
  /* whether this queue entry is invalid */
  bool         invalid;
  /* whether this queue entry has a pending read on it */
  bool         reserved;
  /* physical page to read */
  unsigned int physicalPage;
  /* queue of requests waiting on a queued read */
  UdsQueueHead queueHead;
} QueuedRead;

/**
 * Reason for invalidating a cache entry, used for gathering statistics
 **/
typedef enum invalidationReason {
  INVALIDATION_EVICT,           // cache is full, goodbye
  INVALIDATION_EXPIRE,          // your chapter is being overwritten
  INVALIDATION_ERROR,           // error happened; don't try to use data
  INVALIDATION_INIT_SHUTDOWN
} InvalidationReason;

typedef struct __attribute__((aligned(CACHE_LINE_BYTES))) invalidateCounter {
  uint32_t counter;
  unsigned int page;
} InvalidateCounter;

typedef struct pageCache {
  /* the number of index entries */
  unsigned int            numIndexEntries;
  /* The max number of cached entries */
  uint16_t                numCacheEntries;
  /*
   * The index used to quickly access page in cache - top bit is a 'queued'
   * flag
   */
  volatile uint16_t      *index;
  /* The cache */
  CachedPage             *cache;
  /* The data buffer for the cache */
  byte                   *data;
  /* Cache counters for stats */
  CacheCounters           counters;

  /* Queued reads, as a circular array, with first and last indexes */
  QueuedRead             *readQueue;
  /**
   * Entries are enqueued at readQueueLast.
   * To 'reserve' entries, we get the entry pointed to by readQueueLastRead
   * and increment last read.  This is done with a lock so if another reader
   * thread reserves a read, it will grab the next one.  After every read
   * is completed, the reader thread calls releaseReadQueueEntry which
   * increments readQueueFirst until it is equal to readQueueLastRead, but only
   * if the value pointed to by readQueueFirst is no longer pending.
   * This means that if n reads are outstanding, readQueueFirst may not
   * be incremented until the last of the reads finishes.
   *
   *  First                    Last
   * ||    |    |    |    |    |    ||
   *   LR   (1)   (2)
   *
   * Read thread 1 increments last read (1), then read thread 2 increments it
   * (2). When each read completes, it checks to see if it can increment first,
   * when all concurrent reads have completed, readQueueFirst should equal
   * readQueuLastRead.
   *
   **/
  uint16_t                    readQueueFirst;
  uint16_t                    readQueueLastRead;
  uint16_t                    readQueueLast;
  /* The size of the read queue */
  unsigned int                readQueueMaxSize;
  /* An counter for each zone to keep track of when a search is occurring
   * within that zone.
   */
  volatile InvalidateCounter *searchPendingCounters;
  unsigned int                zoneCount;
  /* Page access counter */
  uint64_t                    clock;
  /* the geometry governing the volume */
  const Geometry             *geometry;
} PageCache;

/**
 * Allocate a cache for a volume.
 *
 * @param geometry          The geometry governing the volume
 * @param chaptersInCache   The size (in chapters) of the page cache
 * @param readQueueMaxSize  The maximum size of the read queue
 * @param zoneCount         The number of zones in the index
 * @param cachePtr          A pointer to hold the new page cache
 *
 * @return UDS_SUCCESS or an error code
 **/
int makePageCache(const Geometry  *geometry,
                  unsigned int     chaptersInCache,
                  unsigned int     readQueueMaxSize,
                  unsigned int     zoneCount,
                  PageCache      **cachePtr)
  __attribute__((warn_unused_result));

/**
 * Clean up a volume's cache
 *
 * @param cache the volumecache
 **/
void freePageCache(PageCache *cache);

/**
 * Invalidates a pages cache
 *
 * @param cache the page cache
 *
 * @return UDS_SUCCESS or an error code
 **/
int invalidatePageCache(PageCache *cache)
  __attribute__((warn_unused_result));

/**
 * Invalidates a page cache for a particular chapter
 *
 * @param cache           the page cache
 * @param chapter         the chapter
 * @param pagesPerChapter the number of pages per chapter
 * @param reason          the reason for invalidation
 *
 * @return UDS_SUCCESS or an error code
 **/
int invalidatePageCacheForChapter(PageCache          *cache,
                                  unsigned int        chapter,
                                  unsigned int        pagesPerChapter,
                                  InvalidationReason  reason)
  __attribute__((warn_unused_result));

/**
 * Invalidates a page from the cache.
 *
 * @param cache   the cache
 * @param page    the cached page
 * @param reason  the reason for invalidation, for stats
 **/
int invalidatePageInCache(PageCache          *cache,
                          CachedPage         *page,
                          InvalidationReason  reason);

/**
 * Find a page, invalidate it, and make its memory the least recent.  This
 * method is only exposed for the use of unit tests.
 *
 * @param cache        The cache containing the page
 * @param physicalPage The id of the page to invalidate
 * @param readQueue    The queue of pending reads (may be NULL)
 * @param reason       The reason for the invalidation, for stats
 * @param mustFind     If <code>true</code>, it is an error if the page
 *                     can't be found
 *
 * @return UDS_SUCCESS or an error code
 **/
int findInvalidateAndMakeLeastRecent(PageCache          *cache,
                                     unsigned int        physicalPage,
                                     QueuedRead         *readQueue,
                                     InvalidationReason  reason,
                                     bool                mustFind);

/**
 * Make the page the most recent in the cache
 *
 * @param cache   the page cache
 * @param pagePtr the page to make most recent
 *
 * @return UDS_SUCCESS or an error code
 **/
void makePageMostRecent(PageCache *cache, CachedPage *pagePtr);

/**
 * Verifies that a page is in the cache.  This method is only exposed for the
 * use of unit tests.
 *
 * @param cache the cache to verify
 * @param page the page to find
 *
 * @return UDS_SUCCESS or an error code
 **/
int assertPageInCache(PageCache *cache, CachedPage *page)
  __attribute__((warn_unused_result));

/**
 * Gets a page from the cache.
 *
 * @param [in] cache        the page cache
 * @param [in] physicalPage the page number
 * @param [in] probeType    the type of cache access being done (CacheProbeType
 *                          optionally OR'ed with CACHE_PROBE_IGNORE_FAILURE)
 * @param [out] pagePtr     the found page
 *
 * @return UDS_SUCCESS or an error code
 **/
int getPageFromCache(PageCache     *cache,
                     unsigned int   physicalPage,
                     int            probeType,
                     CachedPage   **pagePtr)
  __attribute__((warn_unused_result));

/**
 * Enqueue a read request
 *
 * @param cache        the page cache
 * @param request      the request that depends on the read
 * @param physicalPage the physicalPage for the request
 *
 * @return UDS_QUEUED  if the page was queued
 *         UDS_SUCCESS if the queue was full
 *         an error code if there was an error
 **/
int enqueueRead(PageCache *cache, Request *request, unsigned int physicalPage)
  __attribute__((warn_unused_result));

/**
 * Reserves a queued read for future dequeuing, but does not remove it from
 * the queue. Must call releaseReadQueueEntry to complete the process
 *
 * @param cache          the page cache
 * @param queuePos       the position in the read queue for this pending read
 * @param queuedRequests a list of requests for the pending read
 * @param physicalPage   the physicalPage for the requests
 * @param invalid        whether or not this entry is invalid
 *
 * @return UDS_SUCCESS or an error code
 **/
bool reserveReadQueueEntry(PageCache    *cache,
                           unsigned int *queuePos,
                           UdsQueueHead *queuedRequests,
                           unsigned int *physicalPage,
                           bool         *invalid);

/**
 * Releases a read from the queue, allowing it to be reused by future
 * enqueues
 *
 * @param cache      the page cache
 * @param queuePos   queue entry position
 *
 * @return UDS_SUCCESS or an error code
 **/
void releaseReadQueueEntry(PageCache    *cache,
                           unsigned int  queuePos);

/**
 * Check for the page cache read queue being empty.
 *
 * @param cache  the page cache for which to check the read queue.
 *
 * @return  true if the read queue for cache is empty, false otherwise.
 **/
static INLINE bool readQueueIsEmpty(PageCache *cache)
{
  return (cache->readQueueFirst == cache->readQueueLast);
}

/**
 * Check for the page cache read queue being full.
 *
 * @param cache  the page cache for which to check the read queue.
 *
 * @return  true if the read queue for cache is full, false otherwise.
 **/
static INLINE bool readQueueIsFull(PageCache *cache)
{
  return (cache->readQueueFirst ==
    (cache->readQueueLast + 1) % cache->readQueueMaxSize);
}

/**
 * Selects a page in the cache to be used for a read.
 *
 * This will clear the pointer in the page map and
 * set readPending to true on the cache page
 *
 * @param cache          the page cache
 * @param pagePtr        the page to add
 *
 * @return UDS_SUCCESS or an error code
 **/
int selectVictimInCache(PageCache     *cache,
                        CachedPage   **pagePtr)
  __attribute__((warn_unused_result));

/**
 * Completes an async page read in the cache, so that
 * the page can now be used for incoming requests.
 *
 * This will invalidate the old cache entry and point
 * the page map for the new page to this entry
 *
 * @param cache          the page cache
 * @param physicalPage   the page number
 * @param page           the page to complete processing on
 *
 * @return UDS_SUCCESS or an error code
 **/
int putPageInCache(PageCache    *cache,
                   unsigned int  physicalPage,
                   CachedPage   *page)
  __attribute__((warn_unused_result));

/**
 * Cancels an async page read in the cache, so that
 * the page can now be used for incoming requests.
 *
 * This will invalidate the old cache entry and clear
 * the read queued flag on the page map entry, if it
 * was set.
 *
 * @param cache          the page cache
 * @param physicalPage   the page number to clear the queued read flag on
 * @param page           the page to cancel processing on
 *
 * @return UDS_SUCCESS or an error code
 **/
void cancelPageInCache(PageCache    *cache,
                       unsigned int  physicalPage,
                       CachedPage   *page);

/**
 * Get the page cache size
 *
 * @param cache the page cache
 *
 * @return the size of the page cache
 **/
size_t getPageCacheSize(PageCache *cache)
  __attribute__((warn_unused_result));

/**
 * Get the page cache stat counters
 *
 * @param cache     the page cache
 * @param counters  the cache's counters
 **/
void getPageCacheCounters(PageCache *cache, CacheCounters *counters);

/**
 * Determines whether a given value indicates that a search is occuring.
 *
 * @param counterValue    the value to check
 *
 * @return                true if a search is pending, false otherwise
 **/
static INLINE bool searchPending(unsigned int counterValue)
{
  return counterValue % 2 == 1;
}

/**
 * Determines whether there is a search occuring for the given zone.
 *
 * @param cache           the page cache
 * @param zoneNumber      the zone number to increment
 *
 * @return                true if a search is pending, false otherwise
 **/
static INLINE bool isSearchPending(PageCache    *cache,
                                   unsigned int  zoneNumber)
{
  return searchPending(cache->searchPendingCounters[zoneNumber].counter);
}

/**
 * Assert that a search is occuring in the specified zone.
 *
 * @param cache           the page cache
 * @param zoneNumber      the zone number to check
 *
 * @return UDS_SUCCESS or an error code
 **/
#define ASSERT_SEARCH_IS_PENDING(cache, zoneNumber)             \
  ASSERT_LOG_ONLY(isSearchPending((cache), (zoneNumber)),       \
                  "Search is pending for zone %u",              \
                  (zoneNumber))

/**
 * Increment the counter for the specified zone to signal that a search has
 * begun. Also set which page is being searched.
 *
 * @param cache           the page cache
 * @param physicalPage    the page that the zone is searching
 * @param zoneNumber      the zone number to increment
 *
 * @return                the cache's counters
 **/
static INLINE void beginPendingSearch(PageCache    *cache,
                                      unsigned int  physicalPage,
                                      unsigned int  zoneNumber)
{
  cache->searchPendingCounters[zoneNumber].page = physicalPage;
  cache->searchPendingCounters[zoneNumber].counter++;
  ASSERT_SEARCH_IS_PENDING(cache, zoneNumber);
}

/**
 * Increment the counter for the specified zone to signal that a search has
 * finished. We do not need to reset the page since we only should ever
 * look at the page value if the counter indicates a search is ongoing.
 *
 * @param cache           the page cache
 * @param zoneNumber      the zone number to increment
 *
 * @return                the cache's counters
 **/
static INLINE void endPendingSearch(PageCache    *cache,
                                    unsigned int  zoneNumber)
{
  ASSERT_SEARCH_IS_PENDING(cache, zoneNumber);
  cache->searchPendingCounters[zoneNumber].counter++;
}

/**
 * Wait for all pending searches on a page in the cache to complete
 *
 * @param cache           the page cache
 * @param physicalPage    the page to check searches on
 *
 * @return UDS_SUCCESS or an error code
 **/
int waitForPendingSearches(PageCache *cache, unsigned int physicalPage)
  __attribute__((warn_unused_result));

#endif /* PAGE_CACHE_H_ */
