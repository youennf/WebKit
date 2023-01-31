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

#include "BackgroundFetchOptions.h"
#include "BackgroundFetchRecordInformation.h"
#include "DOMCacheEngine.h"
#include "SWServerRegistration.h"

namespace WebCore {

BackgroundFetchCacheMemoryStore::BackgroundFetchCacheMemoryStore()
{
}

void BackgroundFetchCacheMemoryStore::initialize(SWServerRegistration&, CompletionHandler<void()>&& callback)
{
    callback();
}

void BackgroundFetchCacheMemoryStore::clearRecords(ServiceWorkerRegistrationKey key, const String& identifier, CompletionHandler<void()>&& callback)
{
    // FIXME: reduce quota usage.

    auto iterator = m_entries.find(key);
    if (iterator != m_entries.end())
        iterator->value.remove(identifier);
    callback();
}

void BackgroundFetchCacheMemoryStore::clearAllRecords(ServiceWorkerRegistrationKey key, CompletionHandler<void()>&& callback)
{
    // FIXME: reduce quota usage.

    m_entries.remove(key);
    callback();
}

void BackgroundFetchCacheMemoryStore::storeNewRecord(ServiceWorkerRegistrationKey key, const String& identifier, size_t index, const BackgroundFetchRequest&, CompletionHandler<void(StoreResult)>&& callback)
{
    // FIXME: check quota and increase quota usage.

    auto& entryMap = m_entries.ensure(key, [] { return EntriesMap(); }).iterator->value;
    auto& recordMap = entryMap.ensure(identifier, [] { return RecordMap(); }).iterator->value;
    ASSERT(!recordMap.contains(index + 1));
    recordMap.add(index + 1, makeUnique<Record>());
    callback(StoreResult::OK);
}

void BackgroundFetchCacheMemoryStore::storeRecordResponse(ServiceWorkerRegistrationKey key, const String& identifier, size_t index, ResourceResponse&& response, CompletionHandler<void(StoreResult)>&& callback)
{
    // FIXME: check quota and increase quota usage.

    auto& entryMap = m_entries.ensure(key, [] { return EntriesMap(); }).iterator->value;
    auto& recordMap = entryMap.ensure(identifier, [] { return RecordMap(); }).iterator->value;
    ASSERT(recordMap.contains(index));

    auto iterator = recordMap.find(index + 1);
    if (iterator == recordMap.end()) {
        callback(StoreResult::InternalError);
        return;
    }
    iterator->value->response = WTFMove(response);
    callback(StoreResult::OK);
}

void BackgroundFetchCacheMemoryStore::storeRecordResponseBodyChunk(ServiceWorkerRegistrationKey key, const String& identifier, size_t index, Span<const uint8_t> data, CompletionHandler<void(StoreResult)>&& callback)
{
    // FIXME: check quota and increase quota usage.

    auto& entryMap = m_entries.ensure(key, [] { return EntriesMap(); }).iterator->value;
    auto& recordMap = entryMap.ensure(identifier, [] { return RecordMap(); }).iterator->value;

    auto iterator = recordMap.find(index + 1);
    if (iterator == recordMap.end()) {
        callback(StoreResult::InternalError);
        return;
    }

    iterator->value->buffer.append(data);
    callback(StoreResult::OK);
}

} // namespace WebCore

#endif // ENABLE(SERVICE_WORKER)
