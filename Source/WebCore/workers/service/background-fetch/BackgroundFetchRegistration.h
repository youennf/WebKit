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

#include "ActiveDOMObject.h"
#include "BackgroundFetchResult.h"
#include "BackgroundFetchFailureReason.h"
#include "EventTarget.h"
#include "JSDOMPromiseDeferred.h"
#include <wtf/text/WTFString.h>

namespace WebCore {

class BackgroundFetchRecord;
struct CacheQueryOptions;
class FetchRequest;

class BackgroundFetchRegistration : public RefCounted<BackgroundFetchRegistration>, public EventTarget, public ActiveDOMObject {
    WTF_MAKE_ISO_ALLOCATED(BackgroundFetchRegistration);
public:
    static Ref<BackgroundFetchRegistration> create(ScriptExecutionContext& context) { return adoptRef(*new BackgroundFetchRegistration(context)); }
    ~BackgroundFetchRegistration();

    String id() const { return m_id; }
    uint64_t uploadTotal() const { return m_uploadTotal; }
    uint64_t uploaded() const { return m_uploaded; }
    uint64_t downloadTotal() const { return m_downloadTotal; }
    uint64_t downloaded() const { return m_downloaded; }

    BackgroundFetchResult result();
    BackgroundFetchFailureReason failureReason();
    bool recordsAvailable();

    using RequestInfo = std::variant<RefPtr<FetchRequest>, String>;

    void abort(DOMPromiseDeferred<IDLBoolean>&&);
    void match(RequestInfo&&, CacheQueryOptions&&, DOMPromiseDeferred<IDLInterface<BackgroundFetchRecord>>&&);
    void matchAll(std::optional<RequestInfo>&&, CacheQueryOptions&&, DOMPromiseDeferred<IDLSequence<IDLInterface<BackgroundFetchRecord>>>&&);

    using RefCounted::ref;
    using RefCounted::deref;

private:
    explicit BackgroundFetchRegistration(ScriptExecutionContext&);

    // EventTarget
    EventTargetInterface eventTargetInterface() const final { return BackgroundFetchRegistrationEventTargetInterfaceType; }
    ScriptExecutionContext* scriptExecutionContext() const final { return ActiveDOMObject::scriptExecutionContext(); }
    void refEventTarget() final { ref(); }
    void derefEventTarget() final { deref(); }

    // ActiveDOMObject
    const char* activeDOMObjectName() const final;
    void stop() final;
    bool virtualHasPendingActivity() const final;

    String m_id;
    uint64_t m_uploadTotal { 0 };
    uint64_t m_uploaded { 0 };
    uint64_t m_downloadTotal { 0 };
    uint64_t m_downloaded { 0 };
};

} // namespace WebCore

#endif // ENABLE(SERVICE_WORKER)
