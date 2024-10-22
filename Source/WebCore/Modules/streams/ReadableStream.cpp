/*
 * Copyright (C) 2022 Apple Inc. All rights reserved.
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
#include "ReadableStream.h"

#include "JSDOMPromiseDeferred.h"
#include "JSReadableStream.h"
#include "JSReadableStreamBYOBReader.h"
#include "JSReadableStreamDefaultReader.h"
#include "JSReadableStreamReadResult.h"
#include "JSReadableStreamSource.h"
#include "JSUnderlyingSource.h"
#include "QueuingStrategy.h"
#include "ReadableByteStreamController.h"
#include "ReadableStreamBYOBReader.h"
#include "ScriptExecutionContext.h"

namespace WebCore {

static inline ExceptionOr<double> extractHighWaterMark(const QueuingStrategy& strategy, double defaultValue)
{
    if (!strategy.highWaterMark)
        return defaultValue;
    auto highWaterMark = *strategy.highWaterMark;
    if (std::isnan(highWaterMark) || highWaterMark < 0)
        return Exception { ExceptionCode::RangeError, "highWaterMark value is invalid"_s };
    return highWaterMark;
}

ExceptionOr<Ref<ReadableStream>> ReadableStream::create(JSDOMGlobalObject& globalObject, std::optional<JSC::Strong<JSC::JSObject>>&& underlyingSourceValue, std::optional<JSC::Strong<JSC::JSObject>>&& strategyValue)
{
    JSC::JSValue underlyingSource = JSC::jsUndefined();
    if (underlyingSourceValue)
        underlyingSource = underlyingSourceValue->get();
    
    JSC::JSValue strategy = JSC::jsUndefined();
    if (strategyValue)
        strategy = strategyValue->get();

    // FIXME: We convert twice underlyingSource for regular streams, we should fix this.
    auto throwScope = DECLARE_THROW_SCOPE(globalObject.vm());
    auto underlyingSourceDictOrException = convertDictionary<UnderlyingSource>(globalObject, underlyingSource);
    if (underlyingSourceDictOrException.hasException(throwScope))
        return Exception { ExceptionCode::ExistingExceptionError };

    auto strategyDictOrException = convertDictionary<QueuingStrategy>(globalObject, strategy);
    if (strategyDictOrException.hasException(throwScope))
        return Exception { ExceptionCode::ExistingExceptionError };

    auto strategyDict = strategyDictOrException.releaseReturnValue();
    // ASSERT that underlyingSourceDict.type is 'bytes'
    if (strategyDict.size)
        return Exception { ExceptionCode::RangeError, "size should not be present"_s };
    
    auto highWaterMarkOrException = extractHighWaterMark(strategyDict, 0);
    if (highWaterMarkOrException.hasException())
        return highWaterMarkOrException.releaseException();
    auto highWatermark = highWaterMarkOrException.releaseReturnValue();

    auto underlyingSourceDict = underlyingSourceDictOrException.releaseReturnValue();
    if (underlyingSourceDict.type) {
        return createFromByteUnderlyingSource(globalObject, underlyingSource, WTFMove(underlyingSourceDict), highWatermark);
    }
    
    ASSERT(!underlyingSourceDict.type);
    // Convert source dict and pass the converted dictionary to createFromJSValues.
    return createFromJSValues(globalObject, underlyingSource, strategy);
}

ExceptionOr<Ref<ReadableStream>> ReadableStream::createFromJSValues(JSC::JSGlobalObject& globalObject, JSC::JSValue underlyingSource, JSC::JSValue strategy)
{
    auto& jsDOMGlobalObject = *JSC::jsCast<JSDOMGlobalObject*>(&globalObject);
    RefPtr protectedContext { jsDOMGlobalObject.scriptExecutionContext() };
    auto result = InternalReadableStream::createFromUnderlyingSource(jsDOMGlobalObject, underlyingSource, strategy);
    if (result.hasException())
        return result.releaseException();
    
    return adoptRef(*new ReadableStream(result.releaseReturnValue()));
}

ExceptionOr<Ref<ReadableStream>> ReadableStream::createFromByteUnderlyingSource(JSDOMGlobalObject& globalObject, JSC::JSValue underlyingSource, UnderlyingSource&& underlyingSourceDict, double highWaterMark)
{
    auto readableStream = adoptRef(*new ReadableStream());
    
    readableStream->setupReadableByteStreamControllerFromUnderlyingSource(globalObject, underlyingSource, WTFMove(underlyingSourceDict), highWaterMark);
    return readableStream;
}

ExceptionOr<Ref<InternalReadableStream>> ReadableStream::createInternalReadableStream(JSDOMGlobalObject& globalObject, Ref<ReadableStreamSource>&& source)
{
    return InternalReadableStream::createFromUnderlyingSource(globalObject, toJSNewlyCreated(&globalObject, &globalObject, WTFMove(source)), JSC::jsUndefined());
}

ExceptionOr<Ref<ReadableStream>> ReadableStream::create(JSDOMGlobalObject& globalObject, Ref<ReadableStreamSource>&& source)
{
    return createFromJSValues(globalObject, toJSNewlyCreated(&globalObject, &globalObject, WTFMove(source)), JSC::jsUndefined());
}

Ref<ReadableStream> ReadableStream::create(Ref<InternalReadableStream>&& internalReadableStream)
{
    return adoptRef(*new ReadableStream(WTFMove(internalReadableStream)));
}

ReadableStream::ReadableStream(RefPtr<InternalReadableStream>&& internalReadableStream)
    : m_internalReadableStream(WTFMove(internalReadableStream))
{
}

ReadableStream::~ReadableStream() = default;

void ReadableStream::lock()
{
    if (RefPtr internalReadableStream = m_internalReadableStream)
        internalReadableStream->lock();
}

bool ReadableStream::isLocked() const
{
    return !!m_byobReader || (m_internalReadableStream && m_internalReadableStream->isLocked());
}

bool ReadableStream::isDisturbed() const
{
    return m_disturbed || (m_internalReadableStream && m_internalReadableStream->isDisturbed());
}

void ReadableStream::cancel(Exception&& exception)
{
    // FIXME: support byte stream.
    if (RefPtr internalReadableStream = m_internalReadableStream)
        internalReadableStream->cancel(WTFMove(exception));
}

void ReadableStream::pipeTo(ReadableStreamSink& sink)
{
    // FIXME: support byte stream.
    if (RefPtr internalReadableStream = m_internalReadableStream)
        internalReadableStream->pipeTo(sink);
}

ExceptionOr<Vector<Ref<ReadableStream>>> ReadableStream::tee(bool shouldClone)
{
    // FIXME: support byte stream.
    if (RefPtr internalReadableStream = m_internalReadableStream) {
        auto result = internalReadableStream->tee(shouldClone);
        if (result.hasException())
            return result.releaseException();
        
        auto pair = result.releaseReturnValue();
        
        return Vector {
            ReadableStream::create(WTFMove(pair.first)),
            ReadableStream::create(WTFMove(pair.second))
        };
    }
    return Exception { ExceptionCode::NotSupportedError };
}

ExceptionOr<JSC::Strong<JSC::JSObject>> ReadableStream::getReader(JSDOMGlobalObject& jsDOMGlobalObject, const GetReaderOptions& options)
{
    if (!m_internalReadableStream) {
        ASSERT(m_controller);
        if (options.mode) {
            auto readerOrException = ReadableStreamBYOBReader::create(jsDOMGlobalObject, *this);
            if (readerOrException.hasException())
                return readerOrException.releaseException();
            auto newReaderValue = toJSNewlyCreated<IDLInterface<ReadableStreamBYOBReader>>(jsDOMGlobalObject, jsDOMGlobalObject, readerOrException.releaseReturnValue());
            Ref vm = jsDOMGlobalObject.vm();
            return JSC::Strong<JSC::JSObject> { vm.get(), newReaderValue.toObject(&jsDOMGlobalObject) };
        }
        auto readerOrException = ReadableStreamDefaultReader::create(jsDOMGlobalObject, *this);
        if (readerOrException.hasException())
            return readerOrException.releaseException();
        auto newReaderValue = toJSNewlyCreated<IDLInterface<ReadableStreamDefaultReader>>(jsDOMGlobalObject, jsDOMGlobalObject, readerOrException.releaseReturnValue());
        Ref vm = jsDOMGlobalObject.vm();
        return JSC::Strong<JSC::JSObject> { vm.get(), newReaderValue.toObject(&jsDOMGlobalObject) };
    }

    if (options.mode)
        return m_internalReadableStream->getByobReader();

    // FIXME: Do we need this one.
    auto* globalObject = JSC::jsCast<JSDOMGlobalObject*>(m_internalReadableStream->globalObject());

    auto readerOrException = ReadableStreamDefaultReader::create(*globalObject, *m_internalReadableStream);
    if (readerOrException.hasException())
        return readerOrException.releaseException();

    auto newReaderValue = toJSNewlyCreated<IDLInterface<ReadableStreamDefaultReader>>(*globalObject, *globalObject, readerOrException.releaseReturnValue());
    return JSC::Strong<JSC::JSObject> { globalObject->vm(), newReaderValue.toObject(globalObject) };
}

void ReadableStream::setDefaultReader(ReadableStreamDefaultReader* reader)
{
    ASSERT(!m_defaultReader || !reader);
    ASSERT(!m_byobReader);
    m_defaultReader = WeakPtr { reader };
}

void ReadableStream::setByobReader(ReadableStreamBYOBReader* reader)
{
    ASSERT(!m_byobReader || !reader);
    ASSERT(!m_defaultReader);
    m_byobReader = WeakPtr { reader };
}

ReadableStreamBYOBReader* ReadableStream::byobReader()
{
    return m_byobReader.get();
}

JSC::JSValue JSReadableStream::cancel(JSC::JSGlobalObject& globalObject, JSC::CallFrame& callFrame)
{
    RefPtr internalReadableStream = wrapped().internalReadableStream();
    if (!internalReadableStream) {
        // TODO
        return { };
    }
    return internalReadableStream->cancelForBindings(globalObject, callFrame.argument(0));
}

JSC::JSValue JSReadableStream::pipeTo(JSC::JSGlobalObject& globalObject, JSC::CallFrame& callFrame)
{
    RefPtr internalReadableStream = wrapped().internalReadableStream();
    if (!internalReadableStream) {
        // TODO
        return { };
    }
    return internalReadableStream->pipeTo(globalObject, callFrame.argument(0), callFrame.argument(1));
}

JSC::JSValue JSReadableStream::pipeThrough(JSC::JSGlobalObject& globalObject, JSC::CallFrame& callFrame)
{
    RefPtr internalReadableStream = wrapped().internalReadableStream();
    if (!internalReadableStream) {
        // TODO
        return { };
    }
    return internalReadableStream->pipeThrough(globalObject, callFrame.argument(0), callFrame.argument(1));
}

ExceptionOr<void> ReadableStream::setupReadableByteStreamControllerFromUnderlyingSource(JSDOMGlobalObject& globalObject, JSC::JSValue underlyingSource, UnderlyingSource&& underlyingSourceDict, double highWaterMark)
{
    // handle start, pull, cancel algorithms.
    if (underlyingSourceDict.autoAllocateChunkSize && !*underlyingSourceDict.autoAllocateChunkSize)
        return Exception { ExceptionCode::TypeError, "autoAllocateChunkSize is zero"_s };

    // https://streams.spec.whatwg.org/#set-up-readable-byte-stream-controller
    m_controller = ReadableByteStreamController::create(*this, underlyingSource, WTFMove(underlyingSourceDict.pull), WTFMove(underlyingSourceDict.cancel), highWaterMark, underlyingSourceDict.autoAllocateChunkSize.value_or(0));

    m_controller->start(globalObject, underlyingSourceDict.start.get());
    return { };
}

// https://streams.spec.whatwg.org/#readable-stream-close
void ReadableStream::close()
{
    ASSERT(m_state == ReadableStream::State::Readable);
    m_state = ReadableStream::State::Closed;

    if (RefPtr byobReader = m_byobReader.get())
        byobReader->resolveClosedPromise();
}

// https://streams.spec.whatwg.org/#readable-stream-error
void ReadableStream::error(JSDOMGlobalObject& globalObject, JSC::JSValue reason)
{
    ASSERT(m_state == ReadableStream::State::Readable);
    m_state = ReadableStream::State::Errored;

    RefPtr controller = m_controller;
    controller->storeError(globalObject, reason);

    RefPtr byobReader = m_byobReader.get();
    if (!byobReader)
        return;

    byobReader->rejectClosedPromise(reason);
    byobReader->errorReadIntoRequests(reason);
}

// https://streams.spec.whatwg.org/#readable-stream-cancel
void ReadableStream::cancel(JSDOMGlobalObject& globalObject, JSC::JSValue reason, Ref<DeferredPromise>&& promise)
{
    ASSERT(!m_internalReadableStream);
    
    m_disturbed = true;
    if (m_state == State::Closed) {
        promise->resolve();
        return;
    }
    
    if (m_state == State::Errored) {
        promise->rejectWithCallback([&] (auto&) {
            return m_controller->storedError();
        });
        return;
    }
    
    close();
    
    RefPtr byobReader = m_byobReader.get();
    if (byobReader) {
        // FIXME: Check whether using an empty view.
        while (byobReader->readIntoRequestsSize())
            byobReader->takeFirstReadIntoRequest()->resolve<IDLDictionary<ReadableStreamReadResult>>({ JSC::jsUndefined(), true });
    }

    m_controller->runCancelSteps(globalObject, reason, [promise = WTFMove(promise)] (auto&& error) mutable {
        if (error) {
            promise->rejectWithCallback([&] (auto&) {
                return *error;
            });
            return;
        }
        promise->resolve();
    });
}

// https://streams.spec.whatwg.org/#readable-stream-get-num-read-into-requests
size_t ReadableStream::getNumReadIntoRequests() const
{
    ASSERT(m_byobReader);
    RefPtr byobReader = m_byobReader.get();
    return byobReader->readIntoRequestsSize();
}

// https://streams.spec.whatwg.org/#readable-stream-get-num-read-requests
size_t ReadableStream::getNumReadRequests() const
{
    ASSERT(m_defaultReader);
    RefPtr defaultReader = m_defaultReader.get();
    return defaultReader->getNumReadRequests();
}

// https://streams.spec.whatwg.org/#readable-stream-add-read-into-request
void ReadableStream::addReadIntoRequest(Ref<DeferredPromise>&& promise)
{
    ASSERT(m_byobReader);
    RefPtr byobReader = m_byobReader.get();
    return byobReader->addReadIntoRequest(WTFMove(promise));
}

// https://streams.spec.whatwg.org/#readable-stream-add-read-request
void ReadableStream::addReadRequest(Ref<DeferredPromise>&& promise)
{
    ASSERT(m_defaultReader);
    RefPtr defaultReader = m_defaultReader.get();
    return defaultReader->addReadRequest(WTFMove(promise));
}

JSC::JSValue ReadableStream::storedError() const
{
    ASSERT(m_controller);
    return m_controller ? m_controller->storedError() : JSC::jsUndefined();
}

} // namespace WebCore
