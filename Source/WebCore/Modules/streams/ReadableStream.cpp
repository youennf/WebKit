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

#include "JSReadableStream.h"
#include "JSReadableStreamDefaultReader.h"
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

ExceptionOr<Ref<ReadableStream>> ReadableStream::create(JSC::JSGlobalObject& globalObject, std::optional<JSC::Strong<JSC::JSObject>>&& underlyingSourceValue, std::optional<JSC::Strong<JSC::JSObject>>&& strategyValue)
{
    JSC::JSValue underlyingSource = JSC::jsUndefined();
    if (underlyingSourceValue)
        underlyingSource = underlyingSourceValue->get();
    
    JSC::JSValue strategy = JSC::jsUndefined();
    if (strategyValue)
        strategy = strategyValue->get();

    // FIXME: We converting twice underlyingSource for regular streams, we should fix this.
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
        return createFromByteUnderlyingSource(underlyingSource, WTFMove(underlyingSourceDict), highWatermark);
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

ExceptionOr<Ref<ReadableStream>> ReadableStream::createFromByteUnderlyingSource(JSC::JSValue underlyingSource, UnderlyingSource&& underlyingSourceDict, double highWaterMark)
{
    auto readableStream = adoptRef(*new ReadableStream());
    
    readableStream->setUpReadableByteStreamControllerFromUnderlyingSource(underlyingSource, WTFMove(underlyingSourceDict), highWaterMark);
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

ExceptionOr<Vector<Ref<ReadableStream>>> ReadableStream::tee(bool shouldClone)
{
    auto result = m_internalReadableStream->tee(shouldClone);
    if (result.hasException())
        return result.releaseException();
    
    auto pair = result.releaseReturnValue();
    
    return Vector {
        ReadableStream::create(WTFMove(pair.first)),
        ReadableStream::create(WTFMove(pair.second))
    };
}

ExceptionOr<JSC::Strong<JSC::JSObject>> ReadableStream::getReader(const GetReaderOptions& options)
{
    if (!m_internalReadableStream) {
        // TODO
    }

    if (options.mode)
        return m_internalReadableStream->getByobReader();

    auto* globalObject = JSC::jsCast<JSDOMGlobalObject*>(m_internalReadableStream->globalObject());

    auto readerOrException = ReadableStreamDefaultReader::create(*globalObject, *m_internalReadableStream);
    if (readerOrException.hasException())
        return readerOrException.releaseException();

    auto newReaderValue = toJSNewlyCreated<IDLInterface<ReadableStreamDefaultReader>>(*globalObject, *globalObject, readerOrException.releaseReturnValue());
    return JSC::Strong<JSC::JSObject> { globalObject->vm(), newReaderValue.toObject(globalObject) };
}

void ReadableStream::setReader(ReadableStreamBYOBReader* reader)
{
    ASSERT(!m_reader);
    m_reader = reader;
}

ReadableStreamBYOBReader* ReadableStream::reader()
{
    return m_reader.get();
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

ExceptionOr<void> ReadableStream::setUpReadableByteStreamControllerFromUnderlyingSource(JSC::JSValue underlyingSource, UnderlyingSource&& underlyingSourceDict, double highWaterMark)
{
    // handle start, pull, cancel algorithms.
    if (underlyingSourceDict.autoAllocateChunkSize && !*underlyingSourceDict.autoAllocateChunkSize)
        return Exception { ExceptionCode::TypeError, "autoAllocateChunkSize is zero"_s };

    // https://streams.spec.whatwg.org/#set-up-readable-byte-stream-controller
    m_controller = ReadableByteStreamController::create(*this, underlyingSource, WTFMove(underlyingSourceDict.pull), WTFMove(underlyingSourceDict.cancel), highWaterMark, underlyingSourceDict.autoAllocateChunkSize.value_or(0));

    m_controller->start(underlyingSourceDict.start.get());
    return { };
}

} // namespace WebCore
