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
#pragma once

#if ENABLE(SERVICE_WORKER)

#include "BackgroundFetchCacheStore.h"
#include "ResourceResponse.h"
#include "SharedBuffer.h"
#include <wtf/HashMap.h>

namespace WebCore {

class BackgroundFetchCacheMemoryStore :  public BackgroundFetchCacheStore {
public:
    static Ref<BackgroundFetchCacheMemoryStore> create() { return adoptRef(*new BackgroundFetchCacheMemoryStore()); }

    void initialize(SWServerRegistration&, CompletionHandler<void()>&&) final;
    void clearRecords(ServiceWorkerRegistrationKey, const String&, CompletionHandler<void()>&&) final;
    void clearAllRecords(ServiceWorkerRegistrationKey, CompletionHandler<void()>&&) final;
    void storeNewRecord(ServiceWorkerRegistrationKey, const String&, size_t, const BackgroundFetchRequest&, CompletionHandler<void(StoreResult)>&&) final;
    void storeRecordResponse(ServiceWorkerRegistrationKey, const String&, size_t, ResourceResponse&&, CompletionHandler<void(StoreResult)>&&) final;
    void storeRecordResponseBodyChunk(ServiceWorkerRegistrationKey, const String&, size_t, Span<const uint8_t>, CompletionHandler<void(StoreResult)>&&) final;

private:
    BackgroundFetchCacheMemoryStore();

    struct Record {
        WTF_MAKE_FAST_ALLOCATED;
    public:
        ResourceResponse response;
        SharedBufferBuilder buffer;
    };
    
    using RecordMap = HashMap<size_t, std::unique_ptr<Record>>;
    using EntriesMap = HashMap<String, RecordMap>;
    HashMap<ServiceWorkerRegistrationKey, EntriesMap> m_entries;
};

} // namespace WebCore

#endif // ENABLE(SERVICE_WORKER)
