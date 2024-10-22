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
#include "ReadableStreamDefaultReader.h"

#include "JSDOMPromiseDeferred.h"
#include "JSReadableStreamDefaultReader.h"
#include "JSReadableStreamReadResult.h"
#include "ReadableStream.h"

namespace WebCore {

ExceptionOr<Ref<ReadableStreamDefaultReader>> ReadableStreamDefaultReader::create(JSDOMGlobalObject& globalObject, ReadableStream& stream)
{
    RefPtr internalReadableStream = stream.internalReadableStream();
    if (!internalReadableStream) {
        ASSERT(stream.hasByteStreamController());
        return adoptRef(*new ReadableStreamDefaultReader(globalObject, stream));
    }

    return create(globalObject, *internalReadableStream);
}

ExceptionOr<Ref<ReadableStreamDefaultReader>> ReadableStreamDefaultReader::create(JSDOMGlobalObject& globalObject, InternalReadableStream& stream)
{
    auto internalReaderOrException = InternalReadableStreamDefaultReader::create(globalObject, stream);
    if (internalReaderOrException.hasException())
        return internalReaderOrException.releaseException();

    return create(globalObject, internalReaderOrException.releaseReturnValue());
}

Ref<ReadableStreamDefaultReader> ReadableStreamDefaultReader::create(JSDOMGlobalObject& globalObject, Ref<InternalReadableStreamDefaultReader>&& internalDefaultReader)
{
    return adoptRef(*new ReadableStreamDefaultReader(globalObject, WTFMove(internalDefaultReader)));
}

// FIXME: We use a DeferredPromise as we want to reject with a JSValue. We should instead improve DOMPromiseProxy to allow rejecting with a JSValue.
ReadableStreamDefaultReader::ReadableStreamDefaultReader(JSDOMGlobalObject& globalObject, Ref<InternalReadableStreamDefaultReader>&& internalDefaultReader)
    : m_closedPromise(DeferredPromise::create(globalObject, DeferredPromise::Mode::RetainPromiseOnResolve).releaseNonNull())
    , m_internalDefaultReader(WTFMove(internalDefaultReader))
{
}

ReadableStreamDefaultReader::ReadableStreamDefaultReader(JSDOMGlobalObject& globalObject, Ref<ReadableStream>&& stream)
    : m_closedPromise(DeferredPromise::create(globalObject, DeferredPromise::Mode::RetainPromiseOnResolve).releaseNonNull())
    , m_stream(WTFMove(stream))
{
    ASSERT(m_stream->hasByteStreamController());
}

ExceptionOr<void> ReadableStreamDefaultReader::releaseLock(JSDOMGlobalObject& globalObject)
{
    if (RefPtr internalDefaultReader = this->internalDefaultReader())
        return internalDefaultReader->releaseLock();
    
    genericRelease(globalObject);
    errorReadRequests(globalObject, Exception { ExceptionCode::TypeError, "lock released"_s });
    return { };
}

// https://streams.spec.whatwg.org/#readable-stream-default-reader-read
void ReadableStreamDefaultReader::read(JSDOMGlobalObject& globalObject, Ref<DeferredPromise>&& readRequest)
{
    RefPtr stream = m_stream;
    if (!stream) {
        readRequest->reject(Exception { ExceptionCode::TypeError, "stream is undefined"_s });
        return;
    }

    ASSERT(stream->defaultReader() == this);
    ASSERT(stream->hasByteStreamController());

    stream->setAsDisturbed();
    switch (stream->state()) {
    case ReadableStream::State::Closed:
        readRequest->resolve<IDLDictionary<ReadableStreamReadResult>>({ JSC::jsUndefined(), true });
        break;
    case ReadableStream::State::Errored:
        readRequest->reject<IDLAny>(stream->storedError());
        break;
    case ReadableStream::State::Readable:
        RefPtr { stream->controller() }->runPullSteps(globalObject, WTFMove(readRequest));
    }
}

// https://streams.spec.whatwg.org/#readable-stream-reader-generic-release
void ReadableStreamDefaultReader::genericRelease(JSDOMGlobalObject& globalObject)
{
    ASSERT(m_stream);
    ASSERT(m_stream->defaultReader() == this);

    if (m_stream->state() == ReadableStream::State::Readable)
        m_closedPromise->reject(Exception { ExceptionCode::TypeError, "releasing stream"_s }, RejectAsHandled::Yes);
    else {
        m_closedPromise = DeferredPromise::create(globalObject, DeferredPromise::Mode::RetainPromiseOnResolve).releaseNonNull();
        m_closedPromise->reject(Exception { ExceptionCode::TypeError, "releasing stream"_s }, RejectAsHandled::Yes);
    }

    m_stream->setDefaultReader(nullptr);
    m_stream = nullptr;
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablestreamdefaultreadererrorreadrequests
void ReadableStreamDefaultReader::errorReadRequests(JSDOMGlobalObject& globalObject, const Exception& exception)
{
    UNUSED_PARAM(globalObject);
    auto readRequests = std::exchange(m_readRequests, { });
    for (auto& readRequest : readRequests)
        readRequest->reject(exception);
}

// https://streams.spec.whatwg.org/#readable-stream-reader-generic-cancel
void ReadableStreamDefaultReader::genericCancel(JSDOMGlobalObject& globalObject, JSC::JSValue value, Ref<DeferredPromise>&& promise)
{
    ASSERT(m_stream);
    ASSERT(m_stream->defaultReader() == this);
    RefPtr stream = m_stream;
    stream->cancel(globalObject, value, WTFMove(promise));
}

JSC::JSValue ReadableStreamDefaultReader::closedPromise() const
{
    return m_closedPromise->promise();
}

JSC::JSValue JSReadableStreamDefaultReader::read(JSC::JSGlobalObject& globalObject, JSC::CallFrame&)
{
    if (RefPtr internalDefaultReader = wrapped().internalDefaultReader())
        return internalDefaultReader->readForBindings(globalObject);
    
    Ref vm = globalObject.vm();
    auto& jsDOMGlobalObject = *JSC::jsCast<JSDOMGlobalObject*>(&globalObject);
    auto* promise = JSC::JSPromise::create(vm.get(), globalObject.promiseStructure());
    Ref { wrapped() }->read(jsDOMGlobalObject, DeferredPromise::create(jsDOMGlobalObject, *promise));
    return promise;
}

JSC::JSValue JSReadableStreamDefaultReader::closed(JSC::JSGlobalObject& globalObject) const
{
    if (RefPtr internalDefaultReader = wrapped().internalDefaultReader())
        return internalDefaultReader->closedForBindings(globalObject);
    return  Ref { wrapped() }->closedPromise();
}

// https://streams.spec.whatwg.org/#generic-reader-cancel
JSC::JSValue JSReadableStreamDefaultReader::cancel(JSC::JSGlobalObject& globalObject, JSC::CallFrame& callFrame)
{
    if (RefPtr internalDefaultReader = wrapped().internalDefaultReader())
        return internalDefaultReader->cancelForBindings(globalObject, callFrame.argument(0));

    Ref vm = globalObject.vm();
    auto& jsDOMGlobalObject = *JSC::jsCast<JSDOMGlobalObject*>(&globalObject);
    auto* promise = JSC::JSPromise::create(vm.get(), globalObject.promiseStructure());
    Ref { wrapped() }->genericCancel(jsDOMGlobalObject, callFrame.argument(0), DeferredPromise::create(jsDOMGlobalObject, *promise));
    return promise;
}

} // namespace WebCore
