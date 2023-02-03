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
#include "BackgroundFetch.h"

#if ENABLE(SERVICE_WORKER)

#include "BackgroundFetchInformation.h"
#include "BackgroundFetchRecordInformation.h"
#include "CacheQueryOptions.h"
#include "SWServerRegistration.h"

namespace WebCore {

BackgroundFetch::BackgroundFetch(SWServerRegistration& registration, const String& identifier, Vector<BackgroundFetchRequest>&& requests, BackgroundFetchOptions&& options, Ref<BackgroundFetchCacheStore>&& store, NotificationCallback&& notificationCallback)
    : m_identifier(WTFMove(identifier))
    , m_options(WTFMove(options))
    , m_registrationKey(registration.key())
    , m_registrationIdentifier(registration.identifier())
    , m_store(WTFMove(store))
    , m_notificationCallback(WTFMove(notificationCallback))
    , m_origin { m_registrationKey.topOrigin(), SecurityOriginData::fromURL(m_registrationKey.scope()) }
{
    size_t index = 0;
    m_records.reserveInitialCapacity(requests.size());
    for (auto& request : requests) {
        m_store->storeNewRecord(m_registrationKey, m_identifier, index, request, [weakThis = WeakPtr { *this }](auto result) {
            if (weakThis)
                weakThis->handleStoreResult(result);
        });
        m_records.uncheckedAppend(makeUniqueRef<Record>(*this, WTFMove(request), index++));
    }
}

BackgroundFetch::~BackgroundFetch()
{
    abort();
}

BackgroundFetchInformation BackgroundFetch::information() const
{
    return { m_registrationIdentifier, m_identifier, m_uploadTotal, 0, m_downloadTotal, 0 };
}

void BackgroundFetch::match(const RetrieveRecordsOptions& options, MatchBackgroundFetchCallback&& callback)
{
    WebCore::CacheQueryOptions queryOptions { options.ignoreSearch, options.ignoreMethod, options.ignoreVary };
    
    Vector<BackgroundFetchRecordInformation> records;
    for (auto& record : m_records) {
        // FIXME: use record response if available.
        if (record->isMatching(options.request, queryOptions))
            records.append(record->information());
    }

    callback(WTFMove(records));
}

void BackgroundFetch::abort()
{
    m_abortFlag = true;
    m_store->clearRecords(m_registrationKey, m_identifier, [] { });
    auto records = std::exchange(m_records, { });
    for (auto& record : records)
        record->abort();
}

void BackgroundFetch::perform(const CreateLoaderCallback& createLoaderCallback)
{
    m_currentDownloadSize = 0;
    for (auto& record : m_records)
        record->complete(createLoaderCallback);
}

void BackgroundFetch::storeResponse(size_t index, ResourceResponse&& response)
{
    ASSERT(index < m_records.size());
    if (!response.isSuccessful()) {
        updateBackgroundFetchStatus(BackgroundFetchResult::Failure, BackgroundFetchFailureReason::BadStatus);
        return;
    }
    // FIXME: We need to send a notification to resolve the promise.
    m_store->storeRecordResponse(m_registrationKey, m_identifier, index, WTFMove(response), [weakThis = WeakPtr { *this }](auto result) {
        if (weakThis)
            weakThis->handleStoreResult(result);
    });
}

void BackgroundFetch::storeResponseBodyChunk(size_t index, const SharedBuffer& data)
{
    ASSERT(index < m_records.size());
    m_currentDownloadSize += data.size();
    if (m_currentDownloadSize >= m_downloadTotal) {
        updateBackgroundFetchStatus(BackgroundFetchResult::Failure, BackgroundFetchFailureReason::DownloadTotalExceeded);
        return;
    }

    // FIXME: We need to send a notification to fire a progress event.
    m_store->storeRecordResponseBodyChunk(m_registrationKey, m_identifier, index, data, [weakThis = WeakPtr { *this }](auto result) {
        if (weakThis)
            weakThis->handleStoreResult(result);
    });
}

void BackgroundFetch::didFinishRecord(size_t index, const ResourceError& error)
{
    ASSERT(index < m_records.size());
    if (error.isNull()) {
        recordIsCompleted(index);
        return;
    }
    // FIXME: We probably want to handle recoverable errors. For now, all errors are terminal.
    // We could use NetworkStateNotifier.
    updateBackgroundFetchStatus(BackgroundFetchResult::Failure, BackgroundFetchFailureReason::FetchError);
}

void BackgroundFetch::handleStoreResult(BackgroundFetchCacheStore::StoreResult result)
{
    switch (result) {
    case BackgroundFetchCacheStore::StoreResult::OK:
        return;
    case BackgroundFetchCacheStore::StoreResult::QuotaError:
        updateBackgroundFetchStatus(BackgroundFetchResult::Failure, BackgroundFetchFailureReason::QuotaExceeded);
        return;
    case BackgroundFetchCacheStore::StoreResult::InternalError:
        updateBackgroundFetchStatus(BackgroundFetchResult::Failure, BackgroundFetchFailureReason::FetchError);
        return;
    }
}

void BackgroundFetch::recordIsCompleted(size_t index)
{
    m_records[index]->setAsCompleted();
    if (anyOf(m_records, [](auto& record) { return !record->isCompleted(); }))
        return;
    updateBackgroundFetchStatus(BackgroundFetchResult::Success, BackgroundFetchFailureReason::EmptyString);
}

void BackgroundFetch::updateBackgroundFetchStatus(BackgroundFetchResult result, BackgroundFetchFailureReason failureReason)
{
    if (m_result != BackgroundFetchResult::EmptyString)
        return;
    ASSERT(m_failureReason == BackgroundFetchFailureReason::EmptyString);

    m_isActive = false;
    m_result = result;
    m_failureReason = failureReason;
    m_notificationCallback(information());
}

void BackgroundFetch::unsetRecordsAvailableFlag()
{
    ASSERT(m_recordsAvailableFlag);
    m_recordsAvailableFlag = false;
    m_store->clearRecords(m_registrationKey, m_identifier);
    m_notificationCallback(information());
}

BackgroundFetch::Record::Record(BackgroundFetch& fetch, BackgroundFetchRequest&& request, size_t index)
    : m_fetch(fetch)
    , m_request(WTFMove(request))
    , m_index(index)
{
}

bool BackgroundFetch::Record::isMatching(const ResourceRequest& request, const CacheQueryOptions& options) const
{
    return DOMCacheEngine::queryCacheMatch(request, m_request.internalRequest, m_response, options);
}

BackgroundFetchRecordInformation BackgroundFetch::Record::information() const
{
    return BackgroundFetchRecordInformation { m_request.internalRequest, m_request.options, m_request.guard, m_request.httpHeaders, m_request.referrer };
}

void BackgroundFetch::Record::complete(const CreateLoaderCallback& createLoaderCallback)
{
    ASSERT(!m_loader);
    // FIXME: Handle Range headers
    m_loader = createLoaderCallback(*this, ResourceRequest { m_request.internalRequest }, FetchOptions { m_request.options }, m_fetch->m_origin);
}

void BackgroundFetch::Record::abort()
{
    if (!m_loader)
        return;
    m_loader->abort();
    m_loader = nullptr;
}

void BackgroundFetch::Record::didSendData(uint64_t)
{
    // FIXME: notify registration
}

void BackgroundFetch::Record::didReceiveResponse(ResourceResponse&& response)
{
    m_fetch->storeResponse(m_index, WTFMove(response));
}

void BackgroundFetch::Record::didReceiveResponseBodyChunk(const SharedBuffer& data)
{
    m_responseDataSize += data.size();
    m_fetch->storeResponseBodyChunk(m_index, WTFMove(data));
}

void BackgroundFetch::Record::didFinish(const ResourceError& error)
{
    m_fetch->didFinishRecord(m_index, error);
}

} // namespace WebCore

#endif // ENABLE(SERVICE_WORKER)
