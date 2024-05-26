/*
 * Copyright (C) 2017 Apple Inc. All rights reserved.
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
#include "WebCacheStorageConnection.h"

#include "NetworkConnectionToWebProcessMessages.h"
#include "NetworkProcessConnection.h"
#include "NetworkProcessMessages.h"
#include "NetworkStorageManagerMessages.h"
#include "WebCacheStorageProvider.h"
#include "WebCoreArgumentCoders.h"
#include "WebProcess.h"
#include <wtf/MainThread.h>
#include <wtf/NativePromise.h>

namespace WebKit {

WebCacheStorageConnection::WebCacheStorageConnection()
{
}

WebCacheStorageConnection::~WebCacheStorageConnection()
{
}

Ref<IPC::Connection> WebCacheStorageConnection::connection()
{
    {
        Locker lock(m_connectionLock);
        if (m_connection)
            return *m_connection;
    }

    RefPtr<IPC::Connection> connection;
    callOnMainThreadAndWait([&connection]() mutable {
        connection = &WebProcess::singleton().ensureNetworkProcessConnection().connection();
    });

    {
        Locker lock(m_connectionLock);
        m_connection = connection;
    }

    return connection.releaseNonNull();
}

auto WebCacheStorageConnection::open(const WebCore::ClientOrigin& origin, const String& cacheName) -> Ref<OpenPromise>
{
    return connection()->sendWithPromisedReply(Messages::NetworkStorageManager::CacheStorageOpenCache(origin, cacheName))->whenSettled(RunLoop::current(), [](auto&& result) {
        return result ? OpenPromise::createAndSettle(WTFMove(result.value())) : OpenPromise::createAndReject(WebCore::DOMCacheEngine::Error::Internal);
    });
}

auto WebCacheStorageConnection::remove(WebCore::DOMCacheIdentifier cacheIdentifier) -> Ref<RemovePromise>
{
    return connection()->sendWithPromisedReply(Messages::NetworkStorageManager::CacheStorageRemoveCache(cacheIdentifier))->whenSettled(RunLoop::current(), [](auto&& result) {
        return result ? RemovePromise::createAndSettle(WTFMove(result.value())) : RemovePromise::createAndReject(WebCore::DOMCacheEngine::Error::Internal);
    });
}

auto WebCacheStorageConnection::retrieveCaches(const WebCore::ClientOrigin& origin, uint64_t updateCounter) -> Ref<RetrieveCachesPromise>
{
    return connection()->sendWithPromisedReply(Messages::NetworkStorageManager::CacheStorageAllCaches(origin, updateCounter))->whenSettled(RunLoop::current(), [](auto&& result) {
        return result ? RetrieveCachesPromise::createAndSettle(WTFMove(result.value())) : RetrieveCachesPromise::createAndReject(WebCore::DOMCacheEngine::Error::Internal);
    });
}

auto WebCacheStorageConnection::retrieveRecords(WebCore::DOMCacheIdentifier cacheIdentifier, WebCore::RetrieveRecordsOptions&& options) -> Ref<RetrieveRecordsPromise>
{
    return connection()->sendWithPromisedReply(Messages::NetworkStorageManager::CacheStorageRetrieveRecords(cacheIdentifier, options))->whenSettled(RunLoop::current(), [](auto&& result) {
        return result ? RetrieveRecordsPromise::createAndSettle(WTFMove(result.value())) : RetrieveRecordsPromise::createAndReject(WebCore::DOMCacheEngine::Error::Internal);
    });
}

auto WebCacheStorageConnection::batchDeleteOperation(WebCore::DOMCacheIdentifier cacheIdentifier, const WebCore::ResourceRequest& request, WebCore::CacheQueryOptions&& options) -> Ref<BatchPromise>
{
    return connection()->sendWithPromisedReply(Messages::NetworkStorageManager::CacheStorageRemoveRecords(cacheIdentifier, request, options))->whenSettled(RunLoop::current(), [](auto&& result) {
        return result ? BatchPromise::createAndSettle(WTFMove(result.value())) : BatchPromise::createAndReject(WebCore::DOMCacheEngine::Error::Internal);
    });
}

auto WebCacheStorageConnection::batchPutOperation(WebCore::DOMCacheIdentifier cacheIdentifier, Vector<WebCore::DOMCacheEngine::CrossThreadRecord>&& records) -> Ref<BatchPromise>
{
    return connection()->sendWithPromisedReply(Messages::NetworkStorageManager::CacheStoragePutRecords(cacheIdentifier, WTFMove(records)))->whenSettled(RunLoop::current(), [](auto&& result) {
        return result ? BatchPromise::createAndSettle(WTFMove(result.value())) : BatchPromise::createAndReject(WebCore::DOMCacheEngine::Error::Internal);
    });
}

void WebCacheStorageConnection::reference(WebCore::DOMCacheIdentifier cacheIdentifier)
{
    Locker connectedIdentifierCountersLock { m_connectedIdentifierCountersLock };
    Locker connectionLocker { m_connectionLock };
    if (m_connectedIdentifierCounters.add(cacheIdentifier).isNewEntry && m_connection)
        m_connection->send(Messages::NetworkStorageManager::CacheStorageReference(cacheIdentifier), 0);
}

void WebCacheStorageConnection::dereference(WebCore::DOMCacheIdentifier cacheIdentifier)
{
    Locker connectedIdentifierCountersLock { m_connectedIdentifierCountersLock };
    Locker connectionLocker { m_connectionLock };
    if (m_connectedIdentifierCounters.remove(cacheIdentifier) && m_connection)
        m_connection->send(Messages::NetworkStorageManager::CacheStorageDereference(cacheIdentifier), 0);
}

void WebCacheStorageConnection::lockStorage(const WebCore::ClientOrigin& origin)
{
    Locker clientOriginLockRequestCountersLocker { m_clientOriginLockRequestCountersLock };
    Locker connectionLocker { m_connectionLock };
    if (m_clientOriginLockRequestCounters.add(origin).isNewEntry && m_connection)
        m_connection->send(Messages::NetworkStorageManager::LockCacheStorage { origin }, 0);
}

void WebCacheStorageConnection::unlockStorage(const WebCore::ClientOrigin& origin)
{
    Locker clientOriginLockRequestCountersLocker { m_clientOriginLockRequestCountersLock };
    Locker connectionLocker { m_connectionLock };
    if (m_clientOriginLockRequestCounters.remove(origin) && m_connection)
        m_connection->send(Messages::NetworkStorageManager::UnlockCacheStorage { origin }, 0);
}

auto WebCacheStorageConnection::clearMemoryRepresentation(const WebCore::ClientOrigin& origin) -> Ref<CompletionPromise>
{
    return connection()->sendWithPromisedReply(Messages::NetworkStorageManager::CacheStorageClearMemoryRepresentation { origin })->whenSettled(RunLoop::current(), [](auto&& result) {
        return result ? CompletionPromise::createAndResolve() : CompletionPromise::createAndReject(WebCore::DOMCacheEngine::Error::Internal);
    });
}

auto WebCacheStorageConnection::engineRepresentation() -> Ref<EngineRepresentationPromise>
{
    return connection()->sendWithPromisedReply(Messages::NetworkStorageManager::CacheStorageRepresentation { })->whenSettled(RunLoop::current(), [](auto&& result) {
        return result ? EngineRepresentationPromise::createAndSettle(WTFMove(result.value())) : EngineRepresentationPromise::createAndReject(WebCore::DOMCacheEngine::Error::Internal);
    });
}

void WebCacheStorageConnection::updateQuotaBasedOnSpaceUsage(const WebCore::ClientOrigin& origin)
{
    connection()->send(Messages::NetworkStorageManager::ResetQuotaUpdatedBasedOnUsageForTesting(origin), 0);
}

void WebCacheStorageConnection::networkProcessConnectionClosed()
{
    Locker connectedIdentifierCountersLocker { m_connectedIdentifierCountersLock };
    Locker clientOriginLockRequestCountersLocker { m_clientOriginLockRequestCountersLock };
    Locker connectionLocker { m_connectionLock };

    m_connectedIdentifierCounters.clear();
    m_clientOriginLockRequestCounters.clear();
    m_connection = nullptr;
}

}
