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
#include "BackgroundFetchCache.h"

#if ENABLE(SERVICE_WORKER)

#include "ExceptionData.h"

namespace WebCore {

void BackgroundFetchCache::startBackgroundFetch(SWServerRegistration& registration, const String& backgroundFetchIdentifier, Vector<BackgroundFetchRequest>&& requests, ExceptionOrBackgroundFetchInformationCallback&& callback)
{
    m_store->add(registration, backgroundFetchIdentifier, WTFMove(requests), [callback = WTFMove(callback)](auto&& fetchOrError) mutable {
        if (!fetchOrError.has_value()) {
            callback(makeUnexpected(WTFMove(fetchOrError.error())));
            return;
        }
        callback(fetchOrError.value()->information());
    });
}

void BackgroundFetchCache::backgroundFetchInformation(SWServerRegistration& registration, const String& backgroundFetchIdentifier, ExceptionOrBackgroundFetchInformationCallback&& callback)
{
    m_store->get(registration, backgroundFetchIdentifier, [callback = WTFMove(callback)](auto&& fetch) mutable {
        if (!fetch) {
            callback({ });
            return;
        }
        callback(fetch->information());
    });
}

void BackgroundFetchCache::backgroundFetchIdentifiers(SWServerRegistration& registration, BackgroundFetchIdentifiersCallback&& callback)
{
    m_store->getAll(registration, [callback = WTFMove(callback)](auto&& fetches) mutable {
        callback(WTF::map(fetches, [](auto& fetch) {
            return fetch->identifier();
        }));
    });
}

void BackgroundFetchCache::abortBackgroundFetch(SWServerRegistration& registration, const String& backgroundFetchIdentifier, AbortBackgroundFetchCallback&& callback)
{
    m_store->get(registration, backgroundFetchIdentifier, [callback = WTFMove(callback)](auto&& fetch) mutable {
        if (fetch)
            fetch->abort();
        callback(!!fetch);
    });
}

void BackgroundFetchCache::matchBackgroundFetch(SWServerRegistration& registration, const String& backgroundFetchIdentifier, RetrieveRecordsOptions&& options, MatchBackgroundFetchCallback&& callback)
{
    m_store->get(registration, backgroundFetchIdentifier, [options = WTFMove(options).isolatedCopy(), callback = WTFMove(callback)](auto&& fetch) mutable {
        if (!fetch) {
            callback({ });
            return;
        }
        callback(fetch->match(options));
    });
}

void BackgroundFetchCache::remove(SWServerRegistration& registration)
{
    m_store->getAll(registration, [](auto&& fetches) {
        for (auto& fetch: fetches)
            fetch->abort();
    });
}

} // namespace WebCore

#endif // ENABLE(SERVICE_WORKER)
