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
#include "BackgroundFetchCacheMemoryStore.h"

#if ENABLE(SERVICE_WORKER)

#include "BackgroundFetchRecordInformation.h"
#include "DOMCacheEngine.h"
#include "SWServerRegistration.h"

namespace WebCore {

BackgroundFetchCacheMemoryStore::BackgroundFetchCacheMemoryStore()
{
}

void BackgroundFetchCacheMemoryStore::add(SWServerRegistration& registration, const String& backgroundFetchIdentifier, Vector<BackgroundFetchRequest>&& requests, ExceptionOrFetchCallback&& callback)
{
    auto& fetches = m_fetches.ensure(registration.key(), [] { return FetchesMap(); }).iterator->value;
    auto result = fetches.ensure(backgroundFetchIdentifier, [&]() mutable {
        return makeUnique<MemoryFetch>(backgroundFetchIdentifier, registration.identifier(), WTFMove(requests));
    });
    if (!result.isNewEntry) {
        callback(makeUnexpected(ExceptionData { TypeError, "A background fetch registration already exists"_s }));
        return;
    }

    size_t uploadTotal = 0;
    for (auto& request : m_requests) {
        if (auto body = request.httpBody())
            uploadTotal += body->lengthInBytes();
    }
    // FIXME: we should do a quota check with uploadTotal.

    callback(WeakPtr { *result.iterator->value });
}

void BackgroundFetchCacheMemoryStore::get(SWServerRegistration& registration, const String& backgroundFetchIdentifier, FetchCallback&& callback)
{
    auto iterator = m_fetches.find(registration.key());
    if (iterator == m_fetches.end()) {
        callback({ });
        return;
    }
    auto& map = iterator->value;
    auto fetchIterator = map.find(backgroundFetchIdentifier);
    if (fetchIterator == map.end()) {
        callback(nullptr);
        return;
    }
    callback(fetchIterator->value.get());
}

void BackgroundFetchCacheMemoryStore::getIdentifiers(SWServerRegistration& registration, FetchIdentifiersCallback&& callback)
{
    auto iterator = m_fetches.find(registration.key());
    if (iterator == m_fetches.end()) {
        callback({ });
        return;
    }
    callback(copyToVector(iterator->value.keys()));
}

void BackgroundFetchCacheMemoryStore::remove(SWServerRegistration& registration)
{
    m_fetches.take(registration.key());
}

void BackgroundFetchCacheMemoryStore::abort(SWServerRegistration& registration, const String& backgroundFetchIdentifier, AbortCallback&& callback)
{
    auto iterator = m_fetches.find(registration.key());
    if (iterator == m_fetches.end()) {
        callback(false);
        return;
    }
    auto& map = iterator->value;
    auto fetchIterator = map.find(backgroundFetchIdentifier);
    if (fetchIterator == map.end()) {
        callback(false);
        return;
    }
    map.remove(fetchIterator);
    callback(true);
}

BackgroundFetchCacheMemoryStore::MemoryFetch::MemoryFetch(const String& backgroundFetchIdentifier, ServiceWorkerRegistrationIdentifier registrationIdentifier, Vector<BackgroundFetchRequest>&& requests)
    : m_identifier(backgroundFetchIdentifier)
    , m_registrationIdentifier(registrationIdentifier)
    , m_requests(WTFMove(requests))
{
}

String BackgroundFetchCacheMemoryStore::MemoryFetch::identifier() const
{
    return m_identifier;
}

BackgroundFetchInformation BackgroundFetchCacheMemoryStore::MemoryFetch::information() const
{
    return { m_registrationIdentifier, m_identifier, 0, 0, 0, 0 };
}

Vector<BackgroundFetchRecordInformation> BackgroundFetchCacheMemoryStore::MemoryFetch::match(const RetrieveRecordsOptions& options)
{
    WebCore::CacheQueryOptions queryOptions { options.ignoreSearch, options.ignoreMethod, options.ignoreVary };
    
    Vector<BackgroundFetchRecordInformation> records;
    for (auto& request : m_requests) {
        if (DOMCacheEngine::queryCacheMatch(options.request, request.internalRequest, { }, queryOptions))
            records.append(BackgroundFetchRecordInformation { request.internalRequest, request.options, request.guard, request.httpHeaders, request.referrer });
    }

    return records;
}

} // namespace WebCore

#endif // ENABLE(SERVICE_WORKER)
