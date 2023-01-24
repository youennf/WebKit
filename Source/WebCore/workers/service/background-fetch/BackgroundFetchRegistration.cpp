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
#include "BackgroundFetchRegistration.h"

#if ENABLE(SERVICE_WORKER)

#include "JSBackgroundFetchRecord.h"
#include <wtf/IsoMallocInlines.h>

namespace WebCore {

WTF_MAKE_ISO_ALLOCATED_IMPL(BackgroundFetchRegistration);

BackgroundFetchRegistration::BackgroundFetchRegistration(ScriptExecutionContext& context, BackgroundFetchInformation&& information)
    : ActiveDOMObject(&context)
    , m_information(WTFMove(information))
{
}

BackgroundFetchRegistration::~BackgroundFetchRegistration()
{
}

BackgroundFetchResult BackgroundFetchRegistration::result()
{
    return { };
}

BackgroundFetchFailureReason BackgroundFetchRegistration::failureReason()
{
    return { };
}

bool BackgroundFetchRegistration::recordsAvailable()
{
    return false;
}

void BackgroundFetchRegistration::abort(ScriptExecutionContext& context, DOMPromiseDeferred<IDLBoolean>&& promise)
{
    SWClientConnection::fromScriptExecutionContext(context)->abortBackgroundFetch(registrationIdentifier(), id(), [promise = WTFMove(promise)](auto&& result) mutable {
        promise.resolve(result);
    });
}

static ExceptionOr<ResourceRequest> requestFromInfo(ScriptExecutionContext& context, std::optional<BackgroundFetchRegistration::RequestInfo>&& info)
{
    if (!info)
        return ResourceRequest { };

    ResourceRequest resourceRequest;
    auto requestOrException = FetchRequest::create(context, WTFMove(*info), { });
    if (requestOrException.hasException())
        return requestOrException.releaseException();

    return requestOrException.releaseReturnValue()->resourceRequest();
}

void BackgroundFetchRegistration::match(ScriptExecutionContext& context, RequestInfo&& info, const CacheQueryOptions& options, DOMPromiseDeferred<IDLInterface<BackgroundFetchRecord>>&& promise)
{
    auto requestOrException = requestFromInfo(context, WTFMove(info));
    if (requestOrException.hasException()) {
        promise.reject(requestOrException.releaseException());
        return;
    }

    bool shouldRetrieveResponses = false;
    RetrieveRecordsOptions retrieveOptions { requestOrException.releaseReturnValue(), context.crossOriginEmbedderPolicy(), *context.securityOrigin(), options.ignoreSearch, options.ignoreMethod, options.ignoreVary, shouldRetrieveResponses };

    SWClientConnection::fromScriptExecutionContext(context)->matchBackgroundFetch(registrationIdentifier(), id(), WTFMove(retrieveOptions), [weakContext = WeakPtr { context }, promise = WTFMove(promise)](auto&& results) mutable {
        if (!weakContext)
            return;

        if (!results.size()) {
            promise.reject(Exception { TypeError, "No matching record"_s });
            return;
        }

        promise.resolve(BackgroundFetchRecord::create(*weakContext, WTFMove(results[0])));
    });
}

void BackgroundFetchRegistration::matchAll(ScriptExecutionContext& context, std::optional<RequestInfo>&& info, const CacheQueryOptions& options, DOMPromiseDeferred<IDLSequence<IDLInterface<BackgroundFetchRecord>>>&& promise)
{
    auto requestOrException = requestFromInfo(context, WTFMove(info));
    if (requestOrException.hasException()) {
        promise.reject(requestOrException.releaseException());
        return;
    }

    bool shouldRetrieveResponses = false;
    RetrieveRecordsOptions retrieveOptions { requestOrException.releaseReturnValue(), context.crossOriginEmbedderPolicy(), *context.securityOrigin(), options.ignoreSearch, options.ignoreMethod, options.ignoreVary, shouldRetrieveResponses };

    SWClientConnection::fromScriptExecutionContext(context)->matchBackgroundFetch(registrationIdentifier(), id(), WTFMove(retrieveOptions), [weakContext = WeakPtr { context }, promise = WTFMove(promise)](auto&& results) mutable {
        if (!weakContext)
            return;

        auto records = WTF::map(results, [&weakContext](auto& result) {
            return BackgroundFetchRecord::create(*weakContext, WTFMove(result));
        });

        promise.resolve(WTFMove(records));
    });
 }

const char* BackgroundFetchRegistration::activeDOMObjectName() const
{
    return "BackgroundFetchRegistration";
}

void BackgroundFetchRegistration::stop()
{
}

bool BackgroundFetchRegistration::virtualHasPendingActivity() const
{
    return false;
}

} // namespace WebCore

#endif // ENABLE(SERVICE_WORKER)


