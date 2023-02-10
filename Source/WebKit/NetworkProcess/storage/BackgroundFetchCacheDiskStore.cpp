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
#include <wtf/FileSystem.h>

namespace WebKit {

using namespace WebCore;

static String computeFetchPath(const String& rootPath, const String& /* identifier */)
{
    return rootPath;
}

static String computeRecordPath(const String& rootPath, const String& /* identifier */, size_t)
{
    return rootPath;
}

static String computeRecordResponsePath(const String& rootPath, const String& /* identifier */, size_t)
{
    return rootPath;
}

static String computeRecordResponseBodyPath(const String& rootPath, const String& /* identifier */, size_t)
{
    return rootPath;
}

BackgroundFetchCacheDiskStore::BackgroundFetchCacheDiskStore(NetworkStorageManager& manager, SuspendableWorkQueue& queue)
    : m_manager(manager)
    , m_managerQueue(queue)
    , m_ioQueue(WorkQueue::create("com.apple.WebKit.BackgroundFetchCacheDiskStore"))
{
}

BackgroundFetchCacheDiskStore::~BackgroundFetchCacheDiskStore()
{
}

void BackgroundFetchCacheDiskStore::initialize(BackgroundFetchCache& cache, const ServiceWorkerRegistrationKey& key, CompletionHandler<void()>&& callback)
{
    if (m_registrations.contains(key)) {
        m_managerQueue->dispatch([callback = WTFMove(callback)]() mutable {
            callOnMainRunLoop(WTFMove(callback));
        });
        return;
    }

    m_managerQueue->dispatch([protectedThis = Ref { *this }, weakCache = WeakPtr { cache }, key = key.isolatedCopy(), callback = WTFMove(callback)]() mutable {
        // Compute registration path and create it if needed.
        // Read existing bg fetches.
        String registrationPath;
        callOnMainRunLoop([protectedThis = WTFMove(protectedThis), weakCache = WTFMove(weakCache), key = WTFMove(key).isolatedCopy(), registrationPath = WTFMove(registrationPath).isolatedCopy(), callback = WTFMove(callback)]() mutable {
            // notify disk cache of existing background fetches
            protectedThis->m_registrations.add(WTFMove(key), WTFMove(registrationPath));
            callback();
        });
    });
}

void BackgroundFetchCacheDiskStore::clearRecords(const ServiceWorkerRegistrationKey& key, const String& identifier, CompletionHandler<void()>&& callback)
{
    auto registrationPath = m_registrations.get(key);
    ASSERT(!registrationPath.isEmpty());
    if (registrationPath.isEmpty()) {
        callback();
        return;
    }

    m_ioQueue->dispatch([protectedThis = Ref { *this }, path = computeFetchPath(registrationPath, identifier).isolatedCopy(), callback = WTFMove(callback)]() mutable {
        FileSystem::deleteNonEmptyDirectory(path);
        callOnMainRunLoop(WTFMove(callback));
    });
}

void BackgroundFetchCacheDiskStore::clearAllRecords(const ServiceWorkerRegistrationKey& key, CompletionHandler<void()>&& callback)
{
    auto registrationPath = m_registrations.get(key);
    ASSERT(!registrationPath.isEmpty());
    if (registrationPath.isEmpty()) {
        callback();
        return;
    }

    m_ioQueue->dispatch([protectedThis = Ref { *this }, path = WTFMove(registrationPath).isolatedCopy(), callback = WTFMove(callback)]() mutable {
        FileSystem::deleteNonEmptyDirectory(path);
        callOnMainRunLoop(WTFMove(callback));
    });
}

void BackgroundFetchCacheDiskStore::storeNewRecord(const ServiceWorkerRegistrationKey& key, const String& identifier, size_t index, const BackgroundFetchRequest&, CompletionHandler<void(StoreResult)>&& callback)
{
    auto registrationPath = m_registrations.get(key);
    ASSERT(!registrationPath.isEmpty());
    if (registrationPath.isEmpty()) {
        callback(StoreResult::InternalError);
        return;
    }

    // Serialize the fetch request data
    m_ioQueue->dispatch([protectedThis = Ref { *this }, path = computeRecordPath(registrationPath, identifier, index).isolatedCopy(), callback = WTFMove(callback)]() mutable {
        // Store serialized data in path
        callOnMainRunLoop([callback = WTFMove(callback)]() mutable {
            callback(StoreResult::InternalError);
        });
    });
    UNUSED_PARAM(index);
}

void BackgroundFetchCacheDiskStore::storeRecordResponse(const ServiceWorkerRegistrationKey& key, const String& identifier, size_t index, ResourceResponse&&, CompletionHandler<void(StoreResult)>&& callback)
{
    auto registrationPath = m_registrations.get(key);
    ASSERT(!registrationPath.isEmpty());
    if (registrationPath.isEmpty()) {
        callback(StoreResult::InternalError);
        return;
    }

    // Serialize the response data
    m_ioQueue->dispatch([protectedThis = Ref { *this }, path = computeRecordResponsePath(registrationPath, identifier, index).isolatedCopy(), callback = WTFMove(callback)]() mutable {
        // Store serialized data in path
        callOnMainRunLoop([callback = WTFMove(callback)]() mutable {
            callback(StoreResult::InternalError);
        });
    });
}

void BackgroundFetchCacheDiskStore::storeRecordResponseBodyChunk(const ServiceWorkerRegistrationKey& key, const String& identifier, size_t index, const SharedBuffer& buffer, CompletionHandler<void(StoreResult)>&& callback)
{
    auto registrationPath = m_registrations.get(key);
    ASSERT(!registrationPath.isEmpty());
    if (registrationPath.isEmpty()) {
        callback(StoreResult::InternalError);
        return;
    }

    m_ioQueue->dispatch([protectedThis = Ref { *this }, path = computeRecordResponseBodyPath(registrationPath, identifier, index).isolatedCopy(), buffer = Ref { buffer }, callback = WTFMove(callback)]() mutable {
        // Store buffer in path.
        callOnMainRunLoop([callback = WTFMove(callback)]() mutable {
            callback(StoreResult::InternalError);
        });
    });
}

void BackgroundFetchCacheDiskStore::retrieveResponseBody(const ServiceWorkerRegistrationKey& key, const String& identifier, size_t index, RetrieveRecordResponseBodyCallback&& callback)
{
    auto registrationPath = m_registrations.get(key);
    ASSERT(!registrationPath.isEmpty());
    if (registrationPath.isEmpty()) {
        callback(RefPtr<SharedBuffer> { });
        return;
    }

    m_ioQueue->dispatch([protectedThis = Ref { *this }, path = computeRecordResponseBodyPath(registrationPath, identifier, index).isolatedCopy(), callback = WTFMove(callback)]() mutable {
        callOnMainRunLoop([buffer = SharedBuffer::createWithContentsOfFile(path), callback = WTFMove(callback)] {
            bool isNull = !buffer;
            callback(WTFMove(buffer));
            if (!isNull)
                callback(RefPtr<SharedBuffer> { });
        });
    });
}

} // namespace WebKit

#endif // ENABLE(SERVICE_WORKER)
