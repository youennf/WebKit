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
#include "ReadableByteStreamController.h"

#include "JSDOMException.h"
#include "JSDOMGlobalObject.h"
#include "JSDOMPromise.h"
#include "JSReadableByteStreamController.h"
#include "JSReadableStreamReadResult.h"
#include "ReadableStream.h"
#include "ReadableStreamBYOBReader.h"
#include "ReadableStreamBYOBRequest.h"
#include "ReadableStreamDefaultReader.h"
#include "UnderlyingSourceCancelCallback.h"
#include "UnderlyingSourcePullCallback.h"
#include "UnderlyingSourceStartCallback.h"
#include <JavaScriptCore/GenericTypedArrayViewInlines.h>
#include <JavaScriptCore/JSCJSValue.h>
#include <JavaScriptCore/JSCInlines.h>
#include <JavaScriptCore/JSGenericTypedArrayViewInlines.h>

namespace WebCore {

template<typename Algorithm, typename AlgorithmParameter>
Ref<DOMPromise> getAlgorithmPromise(JSDOMGlobalObject& globalObject, RefPtr<Algorithm> algorithm, JSC::JSValue underlyingSource, AlgorithmParameter&& parameter)
{
    RefPtr<DOMPromise> algorithmPromise;
    if (!algorithm) {
        auto* promise = JSC::JSPromise::resolvedPromise(&globalObject, JSC::jsUndefined());
        algorithmPromise = DOMPromise::create(globalObject, *promise);
    } else {
        auto algorithmResult = algorithm->invoke(underlyingSource, parameter);
        if (algorithmResult.type() != CallbackResultType::Success) {
            auto* promise = JSC::JSPromise::rejectedPromise(&globalObject, JSC::jsUndefined());
            algorithmPromise = DOMPromise::create(globalObject, *promise);
        } else
            algorithmPromise = algorithmResult.releaseReturnValue();
    }
    return algorithmPromise.releaseNonNull();
}

ReadableByteStreamController::ReadableByteStreamController(ReadableStream& stream, JSC::JSValue underlyingSource, RefPtr<UnderlyingSourcePullCallback>&& pullAlgorithm, RefPtr<UnderlyingSourceCancelCallback>&& cancelAlgorithm, double highWaterMark, size_t autoAllocateChunkSize)
    : m_stream(stream)
    , m_strategyHWM(highWaterMark)
    , m_pullAlgorithm(WTFMove(pullAlgorithm))
    , m_cancelAlgorithm(WTFMove(cancelAlgorithm))
    , m_autoAllocateChunkSize(autoAllocateChunkSize)
    , m_underlyingSource(underlyingSource)
{
    m_pullAlgorithmWrapper =  [](auto& globalObject, auto& controller) {
        return getAlgorithmPromise(globalObject, controller.m_pullAlgorithm, controller.m_underlyingSource.getValue(), controller);
    };
    m_cancelAlgorithmWrapper = [](auto& globalObject, auto& controller, auto&& reason) {
        JSC::JSValue cancelReason = reason ? *reason : JSC::jsUndefined();
        return getAlgorithmPromise(globalObject, controller.m_cancelAlgorithm, controller.m_underlyingSource.getValue(), cancelReason);
    };
}

ReadableByteStreamController::ReadableByteStreamController(ReadableStream& stream, PullAlgorithm&& pullAlgorithm, CancelAlgorithm&& cancelAlgorithm, double highWaterMark, size_t autoAllocateChunkSize)
    : m_stream(stream)
    , m_strategyHWM(highWaterMark)
    , m_autoAllocateChunkSize(autoAllocateChunkSize)
    , m_pullAlgorithmWrapper(WTFMove(pullAlgorithm))
    , m_cancelAlgorithmWrapper(WTFMove(cancelAlgorithm))
{
}

ReadableByteStreamController::~ReadableByteStreamController() = default;

void ReadableByteStreamController::ref()
{
    m_stream->ref();
}

void ReadableByteStreamController::deref()
{
    m_stream->deref();
}

ReadableStream& ReadableByteStreamController::stream()
{
    return m_stream.get();
}

ReadableStreamBYOBRequest* ReadableByteStreamController::byobRequestForBindings() const
{
    return getByobRequest();
}

std::optional<double> ReadableByteStreamController::desiredSize() const
{
    return getDesiredSize();
}

ExceptionOr<void> ReadableByteStreamController::closeForBindings()
{
    if (m_closeRequested)
        return Exception { ExceptionCode::TypeError, "controller is closed"_s };

    if (m_stream->state() != ReadableStream::State::Readable)
        return Exception { ExceptionCode::TypeError, "controller's stream is not readable"_s };

    close();
    return { };
}

ExceptionOr<void> ReadableByteStreamController::enqueueForBindings(JSDOMGlobalObject& globalObject, JSC::ArrayBufferView& chunk)
{
    if (!chunk.byteLength())
        return Exception { ExceptionCode::TypeError, "chunk's size is 0"_s };
    
    RefPtr sharedBuffer = chunk.possiblySharedBuffer();
    if (!sharedBuffer || !sharedBuffer->byteLength())
        return Exception { ExceptionCode::TypeError, "chunk's buffer size is 0"_s };
    
    if (m_closeRequested)
        return Exception { ExceptionCode::TypeError, "controller is closed"_s };
    
    if (m_stream->state() != ReadableStream::State::Readable)
        return Exception { ExceptionCode::TypeError, "controller's stream is not readable"_s };

    return enqueue(globalObject, chunk);
}

ExceptionOr<void> ReadableByteStreamController::errorForBindings(JSDOMGlobalObject& globalObject, JSC::JSValue value)
{
    error(globalObject, value);
    return { };
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollergetbyobrequest
ReadableStreamBYOBRequest* ReadableByteStreamController::getByobRequest() const
{
    fprintf(stderr, "ReadableByteStreamController::getByobRequest1 %p %d\n", this, (int)m_pendingPullIntos.size());
    if (!m_byobRequest && !m_pendingPullIntos.isEmpty()) {
        fprintf(stderr, "ReadableByteStreamController::getByobRequest2 %p\n", this);
        auto& firstDescriptor = m_pendingPullIntos.first();
        auto view = JSC::Uint8Array::create(firstDescriptor.buffer.ptr(), firstDescriptor.byteOffset + firstDescriptor.bytesFilled, firstDescriptor.byteLength - firstDescriptor.bytesFilled);
        Ref byobRequest = ReadableStreamBYOBRequest::create();

        byobRequest->setController(const_cast<ReadableByteStreamController*>(this));
        byobRequest->setView(view.ptr());

        m_byobRequest = WTFMove(byobRequest);
    }

    return m_byobRequest.get();
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-get-desired-size
std::optional<double> ReadableByteStreamController::getDesiredSize() const
{
    Ref stream = m_stream.get();
    auto state = stream->state();
    if (state == ReadableStream::State::Errored)
        return { };
    if (state == ReadableStream::State::Closed)
        return 0;

    return m_strategyHWM - m_queueTotalSize;
}

ExceptionOr<void> ReadableByteStreamController::start(JSDOMGlobalObject& globalObject, UnderlyingSourceStartCallback* startAlgorithm)
{
    RefPtr<DOMPromise> startPromise;
    if (!startAlgorithm) {
        auto* promise = JSC::JSPromise::resolvedPromise(&globalObject, JSC::jsUndefined());
        startPromise = DOMPromise::create(globalObject, *promise);
    } else {
        auto startResult = startAlgorithm->invoke(m_underlyingSource.getValue(), *this);
        if (startResult.type() != CallbackResultType::Success) {
            // FIXME: Get exception from start algorithm.
            return Exception { ExceptionCode::TypeError, "start threw"_s };
        }
        Ref vm = globalObject.vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        auto* promise = JSC::JSPromise::resolvedPromise(&globalObject, startResult.releaseReturnValue());
        if (scope.exception())
            promise = JSC::JSPromise::rejectedPromise(&globalObject, JSC::jsUndefined());
        startPromise = DOMPromise::create(globalObject, *promise);
    }

    handleSourcePromise(*startPromise, [weakThis = WeakPtr { *this }](auto& globalObject, auto&& error) {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis)
            return;

        if (error) {
            protectedThis->error(globalObject, *error);
            return;
        }

        protectedThis->didStart(globalObject);
    });
    return { };
}

void ReadableByteStreamController::didStart(JSDOMGlobalObject& globalObject)
{
    m_started = true;
    ASSERT(!m_pulling);
    ASSERT(!m_pullAgain);
    callPullIfNeeded(globalObject);
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-close
void ReadableByteStreamController::close()
{
    Ref stream = m_stream.get();

    if (m_closeRequested || stream->state() != ReadableStream::State::Readable)
        return;
    
    if (m_queueTotalSize) {
        m_closeRequested = true;
        return;
    }
    
    if (!m_pendingPullIntos.isEmpty()) {
        auto& pullInto = m_pendingPullIntos.first();
        if (pullInto.bytesFilled % pullInto.elementSize) {
            // FIXME: We should error.
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    clearAlgorithms();
    stream->close();
}

// https://streams.spec.whatwg.org/#transfer-array-buffer
static RefPtr<JSC::ArrayBuffer> transferArrayBuffer(JSC::VM& vm, JSC::ArrayBuffer& buffer)
{
    ASSERT(!buffer.isDetached());
    
    JSC::ArrayBufferContents contents;
    bool isOK = buffer.transferTo(vm, contents);
    if (!isOK)
        return nullptr;
    
    return ArrayBuffer::create(WTFMove(contents));
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-enqueue
ExceptionOr<void> ReadableByteStreamController::enqueue(JSDOMGlobalObject& globalObject, JSC::ArrayBufferView& view)
{
    if (m_closeRequested || m_stream->state() != ReadableStream::State::Readable)
        return { };
    
    RefPtr buffer = view.possiblySharedBuffer();
    if (!buffer || buffer->isDetached())
        return Exception { ExceptionCode::TypeError, "view is detached"_s };

    auto byteOffset = view.byteOffset();
    auto byteLength = view.byteLength();

    Ref vm = globalObject.vm();

    RefPtr transferredBuffer = transferArrayBuffer(vm, *buffer);
    if (!transferredBuffer)
        return Exception { ExceptionCode::TypeError, "transfer of buffer failed"_s };

    if (!m_pendingPullIntos.isEmpty()) {
        auto& firstPendingPullInto = m_pendingPullIntos.first();
        if (firstPendingPullInto.buffer->isDetached())
            return Exception { ExceptionCode::TypeError, "pendingPullInto buffer is detached"_s };

        invalidateByobRequest();

        RefPtr firstPendingPullIntoTransferredBuffer = transferArrayBuffer(vm, firstPendingPullInto.buffer.get());
        if (!firstPendingPullIntoTransferredBuffer)
            return Exception { ExceptionCode::TypeError, "transfer of buffer failed"_s };
        firstPendingPullInto.buffer = firstPendingPullIntoTransferredBuffer.releaseNonNull();

        if (firstPendingPullInto.readerType == ReaderType::None)
            enqueueDetachedPullIntoToQueue(globalObject, firstPendingPullInto);
    }

    if (m_stream->defaultReader()) {
        processReadRequestsUsingQueue(globalObject);
        if (!m_stream->getNumReadRequests()) {
            ASSERT(m_pendingPullIntos.isEmpty());
            enqueueChunkToQueue(transferredBuffer.releaseNonNull(), byteOffset, byteLength);
        } else {
            ASSERT(m_queue.isEmpty());
            if (!m_pendingPullIntos.isEmpty()) {
                ASSERT(m_pendingPullIntos.first().readerType == ReaderType::Default);
                shiftPendingPullInto();
            }
            
            Ref transferredView = Uint8Array::create(transferredBuffer.releaseNonNull(), byteOffset, byteLength);
            m_stream->fulfillReadRequest(globalObject, WTFMove(transferredView), false);
        }
    } else if (RefPtr byobReader = m_stream->byobReader()) {
        enqueueChunkToQueue(transferredBuffer.releaseNonNull(), byteOffset, byteLength);
        processPullIntoDescriptorsUsingQueue(globalObject);
    } else {
        ASSERT(!m_stream->isLocked());
        enqueueChunkToQueue(transferredBuffer.releaseNonNull(), byteOffset, byteLength);
    }

    callPullIfNeeded(globalObject);
    return { };
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerprocessreadrequestsusingqueue
void ReadableByteStreamController::processReadRequestsUsingQueue(JSDOMGlobalObject& globalObject)
{
    RefPtr reader = m_stream->defaultReader();

    ASSERT(reader);

    while (reader->getNumReadRequests()) {
        if (!m_queueTotalSize)
            return;

        auto readRequest = reader->takeFirstReadRequest();
        fillReadRequestFromQueue(globalObject, WTFMove(readRequest));
    }
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-invalidate-byob-request
void ReadableByteStreamController::invalidateByobRequest()
{
    if (!m_byobRequest)
        return;
    m_byobRequest->setController(nullptr);
    m_byobRequest->setView(nullptr);
    m_byobRequest = nullptr;
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-process-pull-into-descriptors-using-queue
void ReadableByteStreamController::processPullIntoDescriptorsUsingQueue(JSDOMGlobalObject& globalObject)
{
    ASSERT(!m_closeRequested);
    while (!m_pendingPullIntos.isEmpty()) {
        if (!m_queueTotalSize)
            return;
        fprintf(stderr, "ReadableByteStreamController::processPullIntoDescriptorsUsingQueue take first %p\n", this);

        auto pullInto = m_pendingPullIntos.takeFirst();
        if (fillPullIntoDescriptorFromQueue(pullInto)) {
            commitPullIntoDescriptor(globalObject, pullInto);
        }
    }
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerenqueuedetachedpullintotoqueue
void ReadableByteStreamController::enqueueDetachedPullIntoToQueue(JSDOMGlobalObject& globalObject, PullIntoDescriptor& pullInto)
{
    ASSERT(pullInto.readerType == ReaderType::None);

    if (pullInto.bytesFilled > 0)
        enqueueClonedChunkToQueue(globalObject, pullInto.buffer.get(), pullInto.byteOffset, pullInto.bytesFilled);
    shiftPendingPullInto();
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-shift-pending-pull-into
ReadableByteStreamController::PullIntoDescriptor ReadableByteStreamController::shiftPendingPullInto()
{
    ASSERT(!m_byobRequest);
    fprintf(stderr, "ReadableByteStreamController::shiftPendingPullInto %p\n", this);

    return m_pendingPullIntos.takeFirst();
}

void ReadableByteStreamController::enqueueChunkToQueue(Ref<JSC::ArrayBuffer>&& buffer, size_t byteOffset, size_t byteLength)
{
    m_queue.append({ WTFMove(buffer), byteOffset, byteLength });
    m_queueTotalSize += byteLength;
}

static RefPtr<JSC::ArrayBuffer> cloneArrayBuffer(JSC::ArrayBuffer& buffer, size_t byteOffset, size_t byteLength)
{
    auto span = buffer.span().subspan(byteOffset, byteLength);
    return JSC::ArrayBuffer::tryCreate(span);
}

void ReadableByteStreamController::enqueueClonedChunkToQueue(JSDOMGlobalObject& globalObject, JSC::ArrayBuffer& buffer, size_t byteOffset, size_t byteLength)
{
    auto clone = cloneArrayBuffer(buffer, byteOffset, byteLength);
    if (!clone) {
        // FIXME: Provide a good error value.
        error(globalObject, JSC::jsUndefined());
        return;
    }
    enqueueChunkToQueue(clone.releaseNonNull(), 0, byteLength);
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-call-pull-if-needed
void ReadableByteStreamController::callPullIfNeeded(JSDOMGlobalObject& globalObject)
{
    bool shouldPull = shouldCallPull();
    if (!shouldPull)
        return;
    
    if (m_pulling) {
        m_pullAgain = true;
        return;
    }
    
    ASSERT(!m_pullAgain);
    m_pulling = true;

    auto promise = m_pullAlgorithmWrapper(globalObject, *this);
    handleSourcePromise(promise, [weakThis = WeakPtr { *this }](auto& globalObject, auto&& error) {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis)
            return;

        if (error) {
            protectedThis->error(globalObject, *error);
            return;
        }

        protectedThis->m_pulling = false;
        if (protectedThis->m_pullAgain) {
            protectedThis->m_pullAgain = false;
            protectedThis->callPullIfNeeded(globalObject);
        }
    });
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-should-call-pull
bool ReadableByteStreamController::shouldCallPull()
{
    if (m_stream->state() != ReadableStream::State::Readable)
        return false;

    if (m_closeRequested)
        return false;

    if (!m_started)
        return false;

    RefPtr defaultReader = m_stream->defaultReader();
    if (defaultReader && defaultReader->getNumReadRequests() > 0)
        return true;

    RefPtr byobReader = m_stream->byobReader();
    if (byobReader && byobReader->readIntoRequestsSize() > 0)
        return true;

    return getDesiredSize() > 0;
}

static void copyDataBlockBytes(JSC::ArrayBuffer& destination, size_t destinationStart, JSC::ArrayBuffer& source, size_t sourceOffset, size_t bytesToCopy)
{
    memcpySpan(destination.mutableSpan().subspan(destinationStart, bytesToCopy), source.span().subspan(sourceOffset, bytesToCopy));
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-fill-pull-into-descriptor-from-queue
bool ReadableByteStreamController::fillPullIntoDescriptorFromQueue(PullIntoDescriptor& pullInto)
{
    size_t maxBytesToCopy = std::min(m_queueTotalSize, pullInto.byteLength - pullInto.bytesFilled);
    size_t maxBytesFilled = pullInto.bytesFilled + maxBytesToCopy;
    size_t totalBytesToCopyRemaining = maxBytesToCopy;
    bool ready = false;
    
    ASSERT(pullInto.bytesFilled < pullInto.minimumFill);
    size_t remainderBytes = maxBytesFilled % pullInto.elementSize;
    size_t maxAlignedBytes = maxBytesFilled - remainderBytes;
    
    if (maxAlignedBytes >= pullInto.minimumFill) {
        totalBytesToCopyRemaining = maxAlignedBytes - pullInto.bytesFilled;
        ready = true;
    }
    
    while (totalBytesToCopyRemaining > 0) {
        auto& headOfQueue = m_queue.first();
        size_t bytesToCopy = std::min(totalBytesToCopyRemaining, headOfQueue.byteLength);
        size_t destStart = pullInto.byteOffset + pullInto.bytesFilled;
        copyDataBlockBytes(pullInto.buffer.get(), destStart, headOfQueue.buffer.get(), headOfQueue.byteOffset, bytesToCopy);
        if (headOfQueue.byteLength == bytesToCopy)
            m_queue.takeFirst();
        else {
            headOfQueue.byteOffset = headOfQueue.byteOffset + bytesToCopy;
            headOfQueue.byteLength = headOfQueue.byteLength - bytesToCopy;
        }
        m_queueTotalSize -= bytesToCopy;
        fillHeadPullIntoDescriptor(bytesToCopy, pullInto);
        totalBytesToCopyRemaining -= bytesToCopy;
    }
    if (!ready) {
        ASSERT(!m_queueTotalSize);
        ASSERT(pullInto.bytesFilled > 0.);
        ASSERT(pullInto.bytesFilled < pullInto.minimumFill);
    }
    return ready;
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-fill-head-pull-into-descriptor
void ReadableByteStreamController::fillHeadPullIntoDescriptor(size_t size, PullIntoDescriptor& pullInto)
{
    ASSERT(m_pendingPullIntos.isEmpty() || &pullInto == &m_pendingPullIntos.first());
    ASSERT(!m_byobRequest);
    pullInto.bytesFilled += size;
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-commit-pull-into-descriptor
void ReadableByteStreamController::commitPullIntoDescriptor(JSDOMGlobalObject& globalObject, PullIntoDescriptor& pullInto)
{
    Ref stream = m_stream.get();
    ASSERT(stream->state() != ReadableStream::State::Errored);
    ASSERT(pullInto.readerType != ReaderType::None);

    bool done = false;

    if (stream->state() == ReadableStream::State::Closed) {
        ASSERT(!(pullInto.bytesFilled % pullInto.elementSize));
        done = true;
    }

    Ref vm = globalObject.vm();
    auto filledView = convertPullIntoDescriptor(vm.get(), pullInto);
    if (pullInto.readerType == ReaderType::Default) {
        // FIXME: Add support for default reading.
        RELEASE_ASSERT_NOT_REACHED();
    } else {
        ASSERT(pullInto.readerType == ReaderType::Byob);
        stream->fulfillReadIntoRequest(globalObject, WTFMove(filledView), done);
    }
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-convert-pull-into-descriptor
RefPtr<JSC::ArrayBufferView> ReadableByteStreamController::convertPullIntoDescriptor(JSC::VM& vm, PullIntoDescriptor& pullInto)
{
    auto bytesFilled = pullInto.bytesFilled;
    auto elementSize = pullInto.elementSize;
    ASSERT(bytesFilled <= pullInto.byteLength);
    ASSERT(!(bytesFilled % elementSize));

    auto buffer = transferArrayBuffer(vm, pullInto.buffer.get());
    // FIXME: Use PullIntoDescriptor.viewConstructor
    return Uint8Array::create(WTFMove(buffer), pullInto.byteOffset, bytesFilled / elementSize);
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-error
void ReadableByteStreamController::error(JSDOMGlobalObject& globalObject, JSC::JSValue value)
{
    fprintf(stderr, "ReadableByteStreamController::error %p\n", this);

    Ref stream = m_stream.get();
    if (stream->state() != ReadableStream::State::Readable)
        return;

    clearPendingPullIntos();
    
    m_queue = { };
    m_queueTotalSize = 0;
    
    clearAlgorithms();
    stream->error(globalObject, value);
}

void ReadableByteStreamController::error(JSDOMGlobalObject& globalObject, const Exception& exception)
{
    auto& vm = globalObject.vm();
    JSC::JSLockHolder lock(vm);
    auto scope = DECLARE_CATCH_SCOPE(vm);
    auto value = createDOMException(&globalObject, exception.code(), exception.message());

    if (scope.exception()) [[unlikely]] {
        ASSERT(vm.hasPendingTerminationException());
        return;
    }

    error(globalObject, value);
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-clear-pending-pull-intos
void ReadableByteStreamController::clearPendingPullIntos()
{
    fprintf(stderr, "ReadableByteStreamController::clearPendingPullIntos %p %d\n", this, (int)m_pendingPullIntos.size());

    invalidateByobRequest();
    m_pendingPullIntos = { };
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-clear-algorithms
void ReadableByteStreamController::clearAlgorithms()
{
    m_pullAlgorithm = nullptr;
    m_cancelAlgorithm = nullptr;
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-pull-into
void ReadableByteStreamController::pullInto(JSDOMGlobalObject& globalObject, JSC::ArrayBufferView& view, size_t min, Ref<DeferredPromise>&& readIntoRequest)
{
    fprintf(stderr, "ReadableByteStreamController::pullInto %p %d\n", this, (int)m_pendingPullIntos.size());

    Ref stream = m_stream.get();
    size_t elementSize = 1;
    auto viewType = view.getType();
    if (viewType != JSC::TypedArrayType::TypeDataView) {
        elementSize = JSC::elementSize(view.getType());
    }
    
    auto minimumFill = min * elementSize;
    ASSERT(minimumFill >= 0 && minimumFill <= view.byteLength());
    ASSERT(!(minimumFill & elementSize));
    
    auto byteOffset = view.byteOffset();
    auto byteLength = view.byteLength();
    if (view.isDetached()) {
        readIntoRequest->reject(Exception { ExceptionCode::TypeError, "view is detached"_s });
        return;
    }
    
    Ref vm = globalObject.vm();
    auto bufferResult = transferArrayBuffer(vm.get(), *view.possiblySharedBuffer());
    if (!bufferResult) {
        readIntoRequest->reject(Exception { ExceptionCode::TypeError, "unable to transfer view buffer"_s });
        return;
    }
    
    auto buffer = bufferResult.releaseNonNull();

    fprintf(stderr, "ReadableByteStreamController::pullInto %p buffer = %p\n", this, buffer.ptr());
    auto bufferByteLength = buffer->byteLength();
    PullIntoDescriptor pullIntoDescriptor { WTFMove(buffer), bufferByteLength, byteOffset, byteLength, 0, minimumFill, elementSize, viewType, ReaderType::Byob };
    if (!m_pendingPullIntos.isEmpty()) {
        fprintf(stderr, "ReadableByteStreamController::pullInto2 appending1 %p\n", this);
        m_pendingPullIntos.append(WTFMove(pullIntoDescriptor));
        stream->addReadIntoRequest(WTFMove(readIntoRequest));
        return;
    }
    
    if (stream->state() == ReadableStream::State::Closed) {
        // FIXME: Use request ctor.
        Ref emptyView = Uint8Array::create(WTFMove(pullIntoDescriptor.buffer), pullIntoDescriptor.byteOffset, 0);
        auto chunk = toJS<IDLArrayBufferView>(globalObject, globalObject, WTFMove(emptyView));
        readIntoRequest->resolve<IDLDictionary<ReadableStreamReadResult>>({ WTFMove(chunk), true });
        return;
    }
    
    if (m_queueTotalSize > 0) {
        if (fillPullIntoDescriptorFromQueue(pullIntoDescriptor)) {
            auto filledView = convertPullIntoDescriptor(vm, pullIntoDescriptor);
            handleQueueDrain(globalObject);

            auto chunk = toJS<IDLNullable<IDLArrayBufferView>>(globalObject, globalObject, WTFMove(filledView));
            readIntoRequest->resolve<IDLDictionary<ReadableStreamReadResult>>({ WTFMove(chunk), false });
            return;
        }
        if (m_closeRequested) {
            JSC::JSValue e = toJS(&globalObject, &globalObject, DOMException::create(ExceptionCode::TypeError, "close is requested"_s));
            error(globalObject, e);
            readIntoRequest->reject<IDLAny>(e);
            return;
        }

    }

    fprintf(stderr, "ReadableByteStreamController::pullInto2 appending2 %p\n", this);

    m_pendingPullIntos.append(WTFMove(pullIntoDescriptor));
    stream->addReadIntoRequest(WTFMove(readIntoRequest));
    callPullIfNeeded(globalObject);
    fprintf(stderr, "ReadableByteStreamController::pullInto3 %p\n", this);
}

// https://streams.spec.whatwg.org/#rbs-controller-private-cancel
void ReadableByteStreamController::runCancelSteps(JSDOMGlobalObject& globalObject, JSC::JSValue reason, Function<void(std::optional<JSC::JSValue>&&)>&& callback)
{
    fprintf(stderr, "ReadableByteStreamController::runCancelSteps %p\n", this);

    clearPendingPullIntos();

    m_queue = { };
    m_queueTotalSize = 0;

    auto promise = m_cancelAlgorithmWrapper(globalObject, *this, reason);
    handleSourcePromise(promise, [callback = WTFMove(callback)](auto&, auto&& reason) mutable {
        callback(WTFMove(reason));
    });
}

// https://streams.spec.whatwg.org/#rbs-controller-private-pull
void ReadableByteStreamController::runPullSteps(JSDOMGlobalObject& globalObject, Ref<DeferredPromise>&& readRequest)
{
    Ref stream = m_stream.get();
    ASSERT(stream->defaultReader());
    
    if (m_queueTotalSize) {
        ASSERT(!stream->getNumReadRequests());
        fillReadRequestFromQueue(globalObject, WTFMove(readRequest));
        return;
    }

    if (auto autoAllocateChunkSize = m_autoAllocateChunkSize) {
        auto buffer = JSC::ArrayBuffer::create(autoAllocateChunkSize, 1);
        fprintf(stderr, "ReadableByteStreamController::runPullSteps appending1 %p\n", this);

        m_pendingPullIntos.append({ WTFMove(buffer), autoAllocateChunkSize, 0, autoAllocateChunkSize, 0, 1, 1, JSC::TypedArrayType::TypeUint8, ReaderType::Default });
    }
    stream->addReadRequest(WTFMove(readRequest));
    callPullIfNeeded(globalObject);
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontroller-releasesteps
void ReadableByteStreamController::releaseSteps()
{
    if (!m_pendingPullIntos.isEmpty()) {
        m_pendingPullIntos.first().readerType = ReaderType::None;
        while (m_pendingPullIntos.size() > 1 )
            m_pendingPullIntos.removeLast();
    }
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerfillreadrequestfromqueue
void ReadableByteStreamController::fillReadRequestFromQueue(JSDOMGlobalObject& globalObject, Ref<DeferredPromise>&& readRequest)
{
    ASSERT(m_queueTotalSize);
    auto entry = m_queue.takeFirst();
    m_queueTotalSize -= entry.byteLength;

    handleQueueDrain(globalObject);

    Ref view = Uint8Array::create(WTFMove(entry.buffer), entry.byteOffset, entry.byteLength);
    auto chunk = toJS<IDLArrayBufferView>(globalObject, globalObject, WTFMove(view));
    readRequest->resolve<IDLDictionary<ReadableStreamReadResult>>(ReadableStreamReadResult { chunk, false });
}

void ReadableByteStreamController::storeError(JSDOMGlobalObject& globalObject, JSC::JSValue error)
{
    Ref vm = globalObject.vm();
    auto thisValue = toJS(&globalObject, &globalObject, *this);
    m_storedError.set(vm.get(), thisValue.getObject(), error);
}

JSC::JSValue ReadableByteStreamController::storedError() const
{
    return m_storedError.getValue();
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond
ExceptionOr<void> ReadableByteStreamController::respond(JSDOMGlobalObject& globalObject, size_t bytesWritten)
{
    ASSERT(!m_pendingPullIntos.isEmpty());
    auto& firstDescriptor = m_pendingPullIntos.first();
    auto state = m_stream->state();
    if (state == ReadableStream::State::Closed) {
        if (bytesWritten > 0)
            return Exception { ExceptionCode::TypeError, "stream is closed"_s };
    } else {
        ASSERT(state == ReadableStream::State::Readable);
        if (!bytesWritten)
            return Exception { ExceptionCode::TypeError, "bytesWritten is 0"_s };
        if (firstDescriptor.bytesFilled + bytesWritten > firstDescriptor.byteLength)
            return Exception { ExceptionCode::RangeError, "bytesWritten is too big"_s };
    }

    Ref vm = globalObject.vm();
    firstDescriptor.buffer = transferArrayBuffer(vm.get(), firstDescriptor.buffer.get()).releaseNonNull();

    respondInternal(globalObject, bytesWritten);
    return { };
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-with-new-view
ExceptionOr<void> ReadableByteStreamController::respondWithNewView(JSDOMGlobalObject& globalObject, JSC::ArrayBufferView& view)
{
    fprintf(stderr, "ReadableByteStreamController::respondWithNewView %p %d\n", this, (int)m_pendingPullIntos.size());

    ASSERT(!m_pendingPullIntos.isEmpty());
    ASSERT(!view.isDetached());

    auto& firstDescriptor = m_pendingPullIntos.first();
    auto state = m_stream->state();
    if (state == ReadableStream::State::Closed) {
        if (!!view.byteLength())
            return Exception { ExceptionCode::TypeError, "stream is closed"_s };
    } else {
        ASSERT(state == ReadableStream::State::Readable);
        if (!view.byteLength())
            return Exception { ExceptionCode::TypeError, "bytesWritten is 0"_s };
    }

    if (firstDescriptor.byteOffset + firstDescriptor.bytesFilled != view.byteOffset())
        return Exception { ExceptionCode::RangeError, "Wrong byte offset"_s };

    RefPtr viewedArrayBuffer = view.possiblySharedBuffer();
    auto viewedArrayBufferByteLength = viewedArrayBuffer ? viewedArrayBuffer->byteLength() : 0;
    if (firstDescriptor.bufferByteLength != viewedArrayBufferByteLength)
        return Exception { ExceptionCode::RangeError, "Wrong view buffer byte length"_s };

    if (firstDescriptor.bytesFilled + view.byteLength() > firstDescriptor.byteLength)
        return Exception { ExceptionCode::RangeError, "Wrong byte length"_s };

    auto viewByteLength = view.byteLength();

    Ref vm = globalObject.vm();
    firstDescriptor.buffer = transferArrayBuffer(vm, *view.possiblySharedBuffer()).releaseNonNull();

    respondInternal(globalObject, viewByteLength);
    return { };
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-internal
void ReadableByteStreamController::respondInternal(JSDOMGlobalObject& globalObject, size_t bytesWritten)
{
    auto& firstDescriptor = m_pendingPullIntos.first();
    ASSERT(!firstDescriptor.buffer->isDetached());
    invalidateByobRequest();

    auto state = m_stream->state();
    if (state == ReadableStream::State::Closed) {
        ASSERT(!bytesWritten);
        respondInClosedState(globalObject, firstDescriptor);
    } else {
        ASSERT(state == ReadableStream::State::Readable);
        ASSERT(bytesWritten > 0);
        respondInReadableState(globalObject, bytesWritten, firstDescriptor);
    }
    callPullIfNeeded(globalObject);
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-in-closed-state
void ReadableByteStreamController::respondInClosedState(JSDOMGlobalObject& globalObject, PullIntoDescriptor& firstDescriptor)
{
    ASSERT(!(firstDescriptor.bytesFilled % firstDescriptor.elementSize));

    if (firstDescriptor.readerType == ReaderType::None)
        shiftPendingPullInto();

    Ref stream = m_stream.get();
    if (RefPtr byobReader = stream->byobReader()) {
        while (stream->getNumReadIntoRequests() > 0) {
            auto pullIntoDescriptor = shiftPendingPullInto();
            commitPullIntoDescriptor(globalObject, pullIntoDescriptor);
        }
    }
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-respond-in-readable-state
void ReadableByteStreamController::respondInReadableState(JSDOMGlobalObject& globalObject, size_t bytesWritten, PullIntoDescriptor& pullIntoDescriptor)
{
    ASSERT(pullIntoDescriptor.bytesFilled + bytesWritten <= pullIntoDescriptor.byteLength);
    fillHeadPullIntoDescriptor(bytesWritten, pullIntoDescriptor);

    if (pullIntoDescriptor.readerType == ReaderType::None) {
        enqueueDetachedPullIntoToQueue(globalObject, pullIntoDescriptor);
        processPullIntoDescriptorsUsingQueue(globalObject);
        return;
    }
    if (pullIntoDescriptor.bytesFilled < pullIntoDescriptor.minimumFill)
        return;

    auto pullInto = shiftPendingPullInto();

    auto remainderSize = pullInto.bytesFilled % pullInto.elementSize;
    if (remainderSize > 0) {
        auto end = pullInto.byteOffset + pullInto.bytesFilled;
        enqueueClonedChunkToQueue(globalObject, pullInto.buffer.get(), end - remainderSize, remainderSize);

        pullInto.bytesFilled = pullInto.bytesFilled - remainderSize;
        commitPullIntoDescriptor(globalObject, pullInto);
        processPullIntoDescriptorsUsingQueue(globalObject);
    }
    pullInto.bytesFilled -= remainderSize;
    commitPulllIntoDescriptor(globalObject, pullInto);
    processPullIntoDescriptorsUsingQueue(globalObject);
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-commit-pull-into-descriptor
void ReadableByteStreamController::commitPulllIntoDescriptor(JSDOMGlobalObject& globalObject, PullIntoDescriptor& pullIntoDescriptor)
{
    Ref stream = m_stream.get();
    auto state = stream->state();

    ASSERT(state == ReadableStream::State::Readable);
    ASSERT(pullIntoDescriptor.readerType != ReaderType::None);
    bool done = false;

    if (state == ReadableStream::State::Closed) {
        ASSERT(!(pullIntoDescriptor.bytesFilled % pullIntoDescriptor.elementSize));
        done = true;
    }

    Ref vm = globalObject.vm();
    RefPtr filledView = convertPullIntoDescriptor(vm.get(), pullIntoDescriptor);
    if (pullIntoDescriptor.readerType == ReaderType::Default)
        stream->fulfillReadRequest(globalObject, WTFMove(filledView), done);
    else {
        ASSERT(pullIntoDescriptor.readerType != ReaderType::Byob);
        stream->fulfillReadIntoRequest(globalObject, WTFMove(filledView), done);
    }
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-handle-queue-drain
void ReadableByteStreamController::handleQueueDrain(JSDOMGlobalObject& globalObject)
{
    ASSERT(m_stream->state() == ReadableStream::State::Readable);

    if (!m_queueTotalSize && m_closeRequested) {
        clearAlgorithms();
        Ref stream = m_stream.get();
        stream->close();
    } else {
        callPullIfNeeded(globalObject);
    }
}

void ReadableByteStreamController::handleSourcePromise(DOMPromise& algorithmPromise, Callback&& callback)
{
    algorithmPromise.whenSettled([promise = Ref { algorithmPromise }, callback = WTFMove(callback)]() mutable {
        auto* globalObject = promise->globalObject();
        if (!globalObject)
            return;

        switch (promise->status()) {
        case DOMPromise::Status::Fulfilled:
            callback(*globalObject, { });
            break;
        case DOMPromise::Status::Rejected:
            callback(*globalObject, promise->result());
            break;
        case DOMPromise::Status::Pending:
            ASSERT_NOT_REACHED();
            break;
        }
    });
}

template<typename Visitor>
void JSReadableByteStreamController::visitAdditionalChildren(Visitor& visitor)
{
    Ref controller = wrapped();

    controller->underlyingSource().visit(visitor);
    controller->storedErrorObject().visit(visitor);

    if (auto* callback = controller->pullAlgorithmConcurrently())
        callback->visitJSFunction(visitor);
    if (auto* callback = controller->cancelAlgorithmConcurrently())
        callback->visitJSFunction(visitor);
}

DEFINE_VISIT_ADDITIONAL_CHILDREN(JSReadableByteStreamController);

} // namespace WebCore
