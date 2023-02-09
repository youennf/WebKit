/*
 * Copyright (C) 2016 Canon Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted, provided that the following conditions
 * are required to be met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Canon Inc. nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY CANON INC. AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL CANON INC. AND ITS CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "FetchBodyOwner.h"
#include "FetchHeaders.h"
#include "FetchResponseLoader.h"
#include "ReadableStreamSink.h"
#include "ResourceResponse.h"
#include <JavaScriptCore/TypedArrays.h>
#include <wtf/Span.h>
#include <wtf/WeakPtr.h>

namespace JSC {
class CallFrame;
class JSValue;
};

namespace WebCore {

class AbortSignal;
class FetchRequest;
class ReadableStreamSource;

class FetchResponse final : public FetchBodyOwner {
public:
    using Type = ResourceResponse::Type;

    struct Init {
        unsigned short status { 200 };
        AtomString statusText;
        std::optional<FetchHeaders::Init> headers;
    };

    WEBCORE_EXPORT static Ref<FetchResponse> create(ScriptExecutionContext*, std::optional<FetchBody>&&, FetchHeaders::Guard, ResourceResponse&&);

    static ExceptionOr<Ref<FetchResponse>> create(ScriptExecutionContext&, std::optional<FetchBody::Init>&&, Init&&);
    static Ref<FetchResponse> error(ScriptExecutionContext&);
    static ExceptionOr<Ref<FetchResponse>> redirect(ScriptExecutionContext&, const String& url, int status);

    using NotificationCallback = Function<void(ExceptionOr<Ref<FetchResponse>>&&)>;
    static void fetch(ScriptExecutionContext&, FetchRequest&, NotificationCallback&&, const String& initiator);
    static Ref<FetchResponse> createFetchResponse(ScriptExecutionContext&, FetchRequest&, NotificationCallback&&, const String&);
    static Ref<FetchResponse> createFetchResponse(ScriptExecutionContext&, const Function<UniqueRef<FetchResponseLoader>(FetchResponse&)>&);

    void startConsumingStream(unsigned);
    void consumeChunk(Ref<JSC::Uint8Array>&&);
    void finishConsumingStream(Ref<DeferredPromise>&&);

    Type type() const { return filteredResponse().type(); }
    const String& url() const;
    bool redirected() const { return filteredResponse().isRedirected(); }
    int status() const { return filteredResponse().httpStatusCode(); }
    bool ok() const { return filteredResponse().isSuccessful(); }
    const String& statusText() const { return filteredResponse().httpStatusText(); }

    const FetchHeaders& headers() const { return m_headers; }
    FetchHeaders& headers() { return m_headers; }
    ExceptionOr<Ref<FetchResponse>> clone();

    void consumeBodyAsStream() final;
    void feedStream() final;
    void cancel() final;

    using ResponseData = std::variant<std::nullptr_t, Ref<FormData>, Ref<SharedBuffer>>;
    ResponseData consumeBody();
    void setBodyData(ResponseData&&, uint64_t bodySizeWithPadding);

    bool isLoading() const { return !!m_bodyLoader; }
    bool isBodyReceivedByChunk() const { return isLoading() || hasReadableStreamBody(); }
    bool isBlobBody() const { return !isBodyNull() && body().isBlob(); }
    bool isBlobFormData() const { return !isBodyNull() && body().isFormData(); }

    using ConsumeDataByChunkCallback = Function<void(ExceptionOr<Span<const uint8_t>*>&&)>;
    void consumeBodyReceivedByChunk(ConsumeDataByChunkCallback&&);
    void cancelStream();

    WEBCORE_EXPORT ResourceResponse resourceResponse() const;
    ResourceResponse::Tainting tainting() const { return m_internalResponse.tainting(); }

    uint64_t bodySizeWithPadding() const { return m_bodySizeWithPadding; }
    void setBodySizeWithPadding(uint64_t size) { m_bodySizeWithPadding = size; }
    uint64_t opaqueLoadIdentifier() const { return m_opaqueLoadIdentifier; }

    void initializeOpaqueLoadIdentifierForTesting() { m_opaqueLoadIdentifier = 1; }

    const HTTPHeaderMap& internalResponseHeaders() const { return m_internalResponse.httpHeaderFields(); }

    bool isCORSSameOrigin() const;
    bool hasWasmMIMEType() const;

    const NetworkLoadMetrics& networkLoadMetrics() const { return m_networkLoadMetrics; }
    void setReceivedInternalResponse(const ResourceResponse&, FetchOptions::Credentials);
    void startLoader(ScriptExecutionContext&, FetchRequest&);

    void setIsNavigationPreload(bool isNavigationPreload) { m_isNavigationPreload = isNavigationPreload; }
    bool isAvailableNavigationPreload() const { return m_isNavigationPreload && m_bodyLoader && !m_bodyLoader->isActive() && !hasReadableStreamBody(); }
    void markAsUsedForPreload();
    bool isUsedForPreload() const { return m_isUsedForPreload; }

    void receivedError(Exception&&);
    void receivedError(ResourceError&&);
    void didSucceed(const NetworkLoadMetrics&);
    void receivedData(Ref<SharedBuffer>&&);

private:
    FetchResponse(ScriptExecutionContext*, std::optional<FetchBody>&&, Ref<FetchHeaders>&&, ResourceResponse&&);

    void stop() final;
    const char* activeDOMObjectName() const final;

    void sendBody() final;

    const ResourceResponse& filteredResponse() const;
    void setNetworkLoadMetrics(const NetworkLoadMetrics& metrics) { m_networkLoadMetrics = metrics; }
    void closeStream();

    void addAbortSteps(Ref<AbortSignal>&&);

    void processReceivedError();

    class BodyLoader final : public FetchLoaderClient, public FetchResponseLoader {
        WTF_MAKE_FAST_ALLOCATED;
    public:
        BodyLoader(FetchResponse&, FetchRequest&, const String&, NotificationCallback&&);
        ~BodyLoader();

    private:
        // FetchLoaderClient API
        void didSucceed(const NetworkLoadMetrics&) final;
        void didFail(const ResourceError&) final;
        void didReceiveResponse(const ResourceResponse&) final;
        void didReceiveData(const SharedBuffer&) final;

        // FetchResponseLoader
        bool start(ScriptExecutionContext&) final;
        void stop() final;
        bool isActive() const final { return !!m_loader; }
        RefPtr<FragmentedSharedBuffer> startStreamingBody() final;

        Ref<FetchRequest> m_request;
        String m_initiator;
        std::unique_ptr<FetchLoader> m_loader;
        bool m_shouldStartStreaming { false };
    };

    mutable std::optional<ResourceResponse> m_filteredResponse;
    ResourceResponse m_internalResponse;
    std::unique_ptr<FetchResponseLoader> m_bodyLoader;
    mutable String m_responseURL;
    // Opaque responses will padd their body size when used with Cache API.
    uint64_t m_bodySizeWithPadding { 0 };
    uint64_t m_opaqueLoadIdentifier { 0 };
    RefPtr<AbortSignal> m_abortSignal;
    NetworkLoadMetrics m_networkLoadMetrics;
    bool m_hasInitializedInternalResponse { false };
    bool m_isNavigationPreload { false };
    bool m_isUsedForPreload { false };
};

} // namespace WebCore
