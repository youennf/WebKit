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

#include "ServiceWorkerRegistrationKey.h"
#include <wtf/CompletionHandler.h>
#include <wtf/Expected.h>
#include <wtf/Vector.h>
#include <wtf/WeakPtr.h>

namespace WebCore {

struct BackgroundFetchInformation;
struct BackgroundFetchOptions;
struct BackgroundFetchRecordInformation;
struct BackgroundFetchRequest;
struct ExceptionData;
struct RetrieveRecordsOptions;
class SWServerRegistration;

class BackgroundFetchCacheStore : public RefCounted<BackgroundFetchCacheStore> {
public:
    virtual ~BackgroundFetchCacheStore() = default;
    
    virtual void initialize(SWServerRegistration&, CompletionHandler<void()>&&) = 0;
    virtual void clearRecords(ServiceWorkerRegistrationKey, const String&, CompletionHandler<void()>&& = [] { }) = 0;
    virtual void clearAllRecords(ServiceWorkerRegistrationKey, CompletionHandler<void()>&& = [] { }) = 0;

    enum class StoreResult { OK, QuotaError, InternalError };
    virtual void storeNewRecord(ServiceWorkerRegistrationKey, const String&, size_t, const BackgroundFetchRequest&, CompletionHandler<void(StoreResult)>&&) = 0;
    virtual void storeRecordResponse(ServiceWorkerRegistrationKey, const String&, size_t, ResourceResponse&&, CompletionHandler<void(StoreResult)>&&) = 0;
    virtual void storeRecordResponseBodyChunk(ServiceWorkerRegistrationKey, const String&, size_t, const SharedBuffer&, CompletionHandler<void(StoreResult)>&&) = 0;
/*
    
    using ExceptionOrFetchCallback = CompletionHandler<void(Expected<WeakPtr<Fetch>, ExceptionData>&&)>;
    virtual void add(SWServerRegistration&, const String&, Vector<BackgroundFetchRequest>&&, BackgroundFetchOptions&&, ExceptionOrFetchCallback&&) = 0;
    using FetchCallback = CompletionHandler<void(WeakPtr<Fetch>&&)>;
    virtual void get(SWServerRegistration&, const String&, FetchCallback&&) = 0;
    using FetchIdentifiersCallback = CompletionHandler<void(Vector<String>&&)>;
    virtual void getIdentifiers(SWServerRegistration&, FetchIdentifiersCallback&&) = 0;
    using AbortCallback = CompletionHandler<void(bool)>;
    virtual void abort(SWServerRegistration&, const String&, AbortCallback&&) = 0;
    virtual void remove(SWServerRegistration&) = 0;
*/
};

} // namespace WebCore

#endif // ENABLE(SERVICE_WORKER)
