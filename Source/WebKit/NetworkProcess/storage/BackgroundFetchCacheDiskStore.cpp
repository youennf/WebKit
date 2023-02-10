/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "BackgroundFetchCacheDiskStore.h"

#if ENABLE(SERVICE_WORKER)

#include "NetworkStorageManager.h"
#include <WebCore/BackgroundFetchOptions.h>
#include <WebCore/BackgroundFetchRecordInformation.h>
#include <WebCore/DOMCacheEngine.h>
#include <WebCore/SWServerRegistration.h>

namespace WebKit {

using namespace WebCore;

BackgroundFetchCacheDiskStore::BackgroundFetchCacheDiskStore(NetworkStorageManager& manager, SuspendableWorkQueue& queue)
    : m_manager(manager)
    , m_queue(queue)
{
    //    : m_path(path)
      //  , m_salt(valueOrDefault(FileSystem::readOrMakeSalt(saltFilePath())))
        //, m_ioQueue(WorkQueue::create("com.apple.WebKit.BackgroundFetchCacheDiskStore"))
}

BackgroundFetchCacheDiskStore::~BackgroundFetchCacheDiskStore()
{
}

void BackgroundFetchCacheDiskStore::initialize(BackgroundFetchCache& cache, const ServiceWorkerRegistrationKey& key, CompletionHandler<void()>&& callback)
{
    m_entries.
    m_queue->dispatch([protectedThis = Ref { *this }, weakCache = WeakPtr { cache }, key = key.isolatedCopy(), callback = WTFMove(callback)]() mutable {
        // Initialize disk paths, get lists of background fetches, send them back to main thread
        callOnMainRunLoop([weakCache = WTFMove(weakCache), callback = WTFMove(callback)]() mutable {
            // notify disk cache of existing background fetches.
            callback();
        });
    });
}

void BackgroundFetchCacheDiskStore::clearRecords(const ServiceWorkerRegistrationKey& key, const String& identifier, CompletionHandler<void()>&& callback)
{
    UNUSED_PARAM(key);
    UNUSED_PARAM(identifier);
    UNUSED_PARAM(callback);
}

void BackgroundFetchCacheDiskStore::clearAllRecords(const ServiceWorkerRegistrationKey& key, CompletionHandler<void()>&& callback)
{
    UNUSED_PARAM(key);
    UNUSED_PARAM(callback);
}

void BackgroundFetchCacheDiskStore::storeNewRecord(const ServiceWorkerRegistrationKey& key, const String& identifier, size_t index, const BackgroundFetchRequest&, CompletionHandler<void(StoreResult)>&& callback)
{
    UNUSED_PARAM(key);
    UNUSED_PARAM(identifier);
    UNUSED_PARAM(index);
    UNUSED_PARAM(callback);
}

void BackgroundFetchCacheDiskStore::storeRecordResponse(const ServiceWorkerRegistrationKey& key, const String& identifier, size_t index, ResourceResponse&&, CompletionHandler<void(StoreResult)>&& callback)
{
    UNUSED_PARAM(key);
    UNUSED_PARAM(identifier);
    UNUSED_PARAM(index);
    UNUSED_PARAM(callback);
}

void BackgroundFetchCacheDiskStore::storeRecordResponseBodyChunk(const ServiceWorkerRegistrationKey& key, const String& identifier, size_t index, const SharedBuffer&, CompletionHandler<void(StoreResult)>&& callback)
{
    UNUSED_PARAM(key);
    UNUSED_PARAM(identifier);
    UNUSED_PARAM(index);
    UNUSED_PARAM(callback);
}

void BackgroundFetchCacheDiskStore::retrieveResponseBody(const ServiceWorkerRegistrationKey& key, const String& identifier, size_t index, RetrieveRecordResponseBodyCallback&& callback)
{
    UNUSED_PARAM(key);
    UNUSED_PARAM(identifier);
    UNUSED_PARAM(index);
    UNUSED_PARAM(callback);
}

} // namespace WebKit

#endif // ENABLE(SERVICE_WORKER)
