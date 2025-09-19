/*
 * Copyright (C) 2024 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ReadableStreamBYOBReader.h"

#include "JSDOMPromise.h"
#include "JSDOMPromiseDeferred.h"
#include "ReadableByteStreamController.h"
#include "ReadableStream.h"
#include <JavaScriptCore/ArrayBuffer.h>
#include <JavaScriptCore/ArrayBufferView.h>

namespace WebCore {

ExceptionOr<Ref<ReadableStreamBYOBReader>> ReadableStreamBYOBReader::create(JSDOMGlobalObject& globalObject, ReadableStream& stream)
{
    auto [promise, deferred] = createPromiseAndWrapper(globalObject);
    Ref reader = adoptRef(*new ReadableStreamBYOBReader(WTFMove(promise), WTFMove(deferred)));
    auto result = reader->setupBYOBReader(globalObject, stream);
    if (result.hasException())
        return result.releaseException();
    return reader;
}

ReadableStreamBYOBReader::ReadableStreamBYOBReader(Ref<DOMPromise>&& promise, Ref<DeferredPromise>&& deferred)
     : m_closedPromise(WTFMove(promise))
    , m_closedDeferred(WTFMove(deferred))
{
}

ReadableStreamBYOBReader::~ReadableStreamBYOBReader() = default;

DOMPromise& ReadableStreamBYOBReader::closed()
{
    return m_closedPromise;
}

void ReadableStreamBYOBReader::read(JSDOMGlobalObject& globalObject, JSC::ArrayBufferView& view, ReadOptions options, Ref<DeferredPromise>&& promise)
{
    if (view.byteLength() == 0)
        return promise->reject(Exception { ExceptionCode::TypeError, "view byteLength is 0"_s });
    
    RefPtr buffer = view.possiblySharedBuffer();
    if (!buffer)
        return promise->reject(Exception { ExceptionCode::TypeError, "view's buffer is detached"_s });
    
    if (!buffer->byteLength())
        return promise->reject(Exception { ExceptionCode::TypeError, "view's buffer byteLength is 0"_s });
    
    if (!options.min)
        return promise->reject(Exception { ExceptionCode::TypeError, "options min is 0"_s });
    
    if (options.min > view.byteLength())
        return promise->reject(Exception { ExceptionCode::RangeError, "view's buffer is not long enough"_s });
    
    if (!m_stream)
        return promise->reject(Exception { ExceptionCode::TypeError, "reader has no stream"_s });
    
    read(globalObject, view, options.min, WTFMove(promise));
}

void ReadableStreamBYOBReader::releaseLock(JSDOMGlobalObject& globalObject)
{
    if (!m_stream)
        return;

    genericRelease(globalObject);

    errorReadIntoRequests(Exception { ExceptionCode::TypeError, "releasing stream"_s });
}

void ReadableStreamBYOBReader::cancel(JSDOMGlobalObject& globalObject, JSC::JSValue value, Ref<DeferredPromise>&& promise)
{
    fprintf(stderr, "ReadableStreamBYOBReader::cancel\n");

    if (!m_stream) {
        promise->reject(Exception { ExceptionCode::TypeError, "no stream"_s });
        return;
    }
    genericCancel(globalObject, value, WTFMove(promise));
}

// https://streams.spec.whatwg.org/#set-up-readable-stream-byob-reader
ExceptionOr<void> ReadableStreamBYOBReader::setupBYOBReader(JSDOMGlobalObject& globalObject, ReadableStream& stream)
{
    if (stream.isLocked())
        return Exception { ExceptionCode::TypeError, "stream is locked"_s };
    
    if (!stream.hasByteStreamController())
        return Exception { ExceptionCode::TypeError, "stream is not a byte stream"_s };
    
    initialize(globalObject, stream);
    return { };
}

// https://streams.spec.whatwg.org/#set-up-readable-stream-byob-reader
void ReadableStreamBYOBReader::initialize(JSDOMGlobalObject& globalObject, ReadableStream& stream)
{
    m_stream = &stream;

    stream.setByobReader(this);

    switch (stream.state()) {
    case ReadableStream::State::Readable:
        break;
    case ReadableStream::State::Closed:
        m_closedDeferred->resolve();
        break;
    case ReadableStream::State::Errored:
        m_closedDeferred->reject<IDLAny>(stream.storedError(globalObject));
        break;
    }
}

// https://streams.spec.whatwg.org/#readable-stream-byob-reader-read
void ReadableStreamBYOBReader::read(JSDOMGlobalObject& globalObject, JSC::ArrayBufferView& view, size_t optionMin, Ref<DeferredPromise>&& promise)
{
    ASSERT(m_stream);
    
    m_stream->setAsDisturbed();
    if (m_stream->state() == ReadableStream::State::Errored) {
        promise->reject<IDLAny>(m_stream->storedError(globalObject));
        return;
    }

    RefPtr controller = m_stream->controller();
    controller->pullInto(globalObject, view, optionMin, WTFMove(promise));
}

// https://streams.spec.whatwg.org/#readable-stream-reader-generic-release
void ReadableStreamBYOBReader::genericRelease(JSDOMGlobalObject& globalObject)
{
    ASSERT(m_stream);
    ASSERT(m_stream->byobReader() == this);

    if (m_stream->state() == ReadableStream::State::Readable)
        m_closedDeferred->reject(Exception { ExceptionCode::TypeError, "releasing stream"_s }, RejectAsHandled::Yes);
    else {
        m_closedDeferred = DeferredPromise::create(globalObject, DeferredPromise::Mode::RetainPromiseOnResolve).releaseNonNull();
        m_closedDeferred->reject(Exception { ExceptionCode::TypeError, "releasing stream"_s }, RejectAsHandled::Yes);
    }

    m_stream->setByobReader(nullptr);
    m_stream = nullptr;
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablestreambyobreadererrorreadintorequests
void ReadableStreamBYOBReader::errorReadIntoRequests(Exception&& exception)
{
    auto requests = std::exchange(m_readIntoRequests, { });
    for (auto& request : requests)
        request->reject(exception);
}

void ReadableStreamBYOBReader::errorReadIntoRequests(JSC::JSValue reason)
{
    auto requests = std::exchange(m_readIntoRequests, { });
    for (auto& request : requests)
        request->rejectWithCallback([&] (auto&) { return reason; });
}

void ReadableStreamBYOBReader::resolveClosedPromise()
{
    m_closedDeferred->resolve();
}

void ReadableStreamBYOBReader::rejectClosedPromise(JSC::JSValue reason)
{
    m_closedDeferred->reject<IDLAny>(reason, RejectAsHandled::Yes);
}

// https://streams.spec.whatwg.org/#readable-stream-reader-generic-cancel
void ReadableStreamBYOBReader::genericCancel(JSDOMGlobalObject& globalObject, JSC::JSValue value, Ref<DeferredPromise>&& promise)
{
    RefPtr stream = m_stream;
    ASSERT(stream);
    stream->cancel(globalObject, value, WTFMove(promise));
}

void ReadableStreamBYOBReader::onClosedPromiseRejection(ClosedCallback&& callback)
{
    if (m_closedCallback) {
        auto oldCallback = std::exchange(m_closedCallback, { });
        m_closedCallback = [oldCallback = WTFMove(oldCallback), callback = WTFMove(callback)](auto& globalObject, auto value) mutable {
            oldCallback(globalObject, value);
            callback(globalObject, value);
        };
        return;
    }

    m_closedCallback = WTFMove(callback);
    m_closedPromise->whenSettled([weakThis = WeakPtr { *this }]() mutable {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis || !protectedThis->m_closedPromise->globalObject() || !protectedThis->m_closedCallback || protectedThis->m_closedPromise->status() != DOMPromise::Status::Rejected)
            return;

        protectedThis->m_closedCallback(*protectedThis->m_closedPromise->globalObject(), protectedThis->m_closedPromise->result());
    });
}

} // namespace WebCore
