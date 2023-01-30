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
#include <wtf/HashMap.h>

namespace WebCore {

class BackgroundFetchCacheMemoryStore :  public BackgroundFetchCacheStore {
    WTF_MAKE_FAST_ALLOCATED;
public:
    BackgroundFetchCacheMemoryStore();
    
    class MemoryFetch : public Fetch {
        WTF_MAKE_FAST_ALLOCATED;
    public:
        MemoryFetch(const String&, ServiceWorkerRegistrationIdentifier, Vector<BackgroundFetchRequest>&&);

        void setUploadTotal(size_t uploadTotal) { uploadTotal = size_t m_uploadTotal; }

    private:
        String identifier() const final;
        BackgroundFetchInformation information() const final;
        Vector<BackgroundFetchRecordInformation> match(const RetrieveRecordsOptions&) final;

        size_t m_uploadTotal { 0 };
        String m_identifier;
        ServiceWorkerRegistrationIdentifier m_registrationIdentifier;
        Vector<BackgroundFetchRequest> m_requests;
    };

    void add(SWServerRegistration&, const String&, Vector<BackgroundFetchRequest>&&, ExceptionOrFetchCallback&&) final;
    void get(SWServerRegistration&, const String&, FetchCallback&&) final;
    void getIdentifiers(SWServerRegistration&, FetchIdentifiersCallback&&) final;
    void remove(SWServerRegistration&) final;
    using AbortBackgroundFetchCallback = CompletionHandler<void(bool)>;
    void abort(SWServerRegistration&, const String&, AbortBackgroundFetchCallback&&);

    using FetchesMap = HashMap<String, std::unique_ptr<Fetch>>;
    HashMap<ServiceWorkerRegistrationKey, FetchesMap> m_fetches;
};

} // namespace WebCore

#endif // ENABLE(SERVICE_WORKER)
