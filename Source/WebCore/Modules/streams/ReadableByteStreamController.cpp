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
#include "UnderlyingSourceCancelCallback.h"
#include "UnderlyingSourcePullCallback.h"
#include "UnderlyingSourceStartCallback.h"
#include <JavaScriptCore/GenericTypedArrayViewInlines.h>
#include <JavaScriptCore/JSCInlines.h>
#include <JavaScriptCore/JSGenericTypedArrayViewInlines.h>

namespace WebCore {

ReadableByteStreamController::ReadableByteStreamController(ReadableStream& stream, JSC::JSValue underlyingSource, RefPtr<UnderlyingSourcePullCallback>&& pullAlgorithm, RefPtr<UnderlyingSourceCancelCallback>&& cancelAlgorithm, double highWaterMark, size_t autoAllocateChunkSize)
    : m_stream(stream)
    , m_strategyHWM(highWaterMark)
    , m_pullAlgorithm(WTFMove(pullAlgorithm))
    , m_cancelAlgorithm(WTFMove(cancelAlgorithm))
    , m_autoAllocateChunkSize(autoAllocateChunkSize)
    , m_underlyingSource(underlyingSource)
{
}

ReadableByteStreamController::~ReadableByteStreamController() = default;

ReadableStream* ReadableByteStreamController::stream()
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
    if (!m_byobRequest && !m_pendingPullIntos.isEmpty()) {
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
    RefPtr stream = m_stream.get();
    auto state = stream->state();
    if (state == ReadableStream::State::Errored)
        return { };
    if (state == ReadableStream::State::Closed)
        return 0;

    return m_strategyHWM - m_queueTotalSize;
}

void ReadableByteStreamController::start(JSDOMGlobalObject& globalObject, UnderlyingSourceStartCallback* startAlgorithm)
{
    RefPtr<DOMPromise> startPromise;
    if (!startAlgorithm) {
        auto* promise = JSC::JSPromise::resolvedPromise(&globalObject, JSC::jsUndefined());
        startPromise = DOMPromise::create(globalObject, *promise);
    } else {
        auto startResult = startAlgorithm->handleEvent(m_underlyingSource.getValue(), *this);
        if (startResult.type() != CallbackResultType::Success) {
            auto* promise = JSC::JSPromise::rejectedPromise(&globalObject, JSC::jsUndefined());
            startPromise = DOMPromise::create(globalObject, *promise);
        } else {
            Ref vm = globalObject.vm();
            auto scope = DECLARE_CATCH_SCOPE(vm);
            auto* promise = JSC::JSPromise::resolvedPromise(&globalObject, startResult.releaseReturnValue());
            if (scope.exception())
                promise = JSC::JSPromise::rejectedPromise(&globalObject, JSC::jsUndefined());
            startPromise = DOMPromise::create(globalObject, *promise);
        }
    }

    m_callbackPromise = WTFMove(startPromise);
    m_callbackPromise->whenSettled([weakThis = WeakPtr { *this }] {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis || !protectedThis->m_callbackPromise)
            return;

        auto promise = std::exchange(protectedThis->m_callbackPromise, { });
        auto* globalObject = promise->globalObject();
        if (!globalObject)
            return;

        switch (promise->status()) {
        case DOMPromise::Status::Fulfilled:
            protectedThis->didStart(*globalObject);
            break;
        case DOMPromise::Status::Rejected:
            protectedThis->error(*globalObject, promise->result());
            break;
        case DOMPromise::Status::Pending:
            ASSERT_NOT_REACHED();
            break;
        }
    });
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
    RefPtr stream = m_stream.get();

    if (m_closeRequested || stream->state() != ReadableStream::State::Readable)
        return;
    
    if (m_queueTotalSize) {
        m_closeRequested = true;
        return;
    }
    
    if (!m_pendingPullIntos.isEmpty()) {
        // Feed the pending pulls.
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

    RefPtr byobReader = m_stream->byobReader();

    if (!byobReader && m_stream->isLocked()) {
        // FIXME: Implement default reader reading.
        return { };
    }

    if (byobReader) {
        enqueueChunkToQueue(transferredBuffer.releaseNonNull(), byteOffset, byteLength);
        processPullIntoDescriptorsUsingQueue(globalObject);
    } else {
        ASSERT(!m_stream->isLocked());
        enqueueChunkToQueue(transferredBuffer.releaseNonNull(), byteOffset, byteLength);
    }

    callPullIfNeeded(globalObject);
    return { };
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
        if (m_queueTotalSize > 0)
            return;
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

    ASSERT(!m_callbackPromise);

    auto pullResult = m_pullAlgorithm->handleEvent(m_underlyingSource.getValue(), *this);
    if (pullResult.type() != CallbackResultType::Success) {
        error(globalObject, JSC::jsUndefined());
        return;
    }

    m_callbackPromise = pullResult.releaseReturnValue();
    m_callbackPromise->whenSettled([weakThis = WeakPtr { *this }] {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis || !protectedThis->m_callbackPromise)
            return;

        auto promise = std::exchange(protectedThis->m_callbackPromise, { });
        auto* globalObject = promise->globalObject();
        if (!globalObject)
            return;

        switch (promise->status()) {
        case DOMPromise::Status::Fulfilled:
            protectedThis->m_pulling = false;
            if (protectedThis->m_pullAgain) {
                protectedThis->m_pullAgain = false;
                protectedThis->callPullIfNeeded(*globalObject);
            }
            break;
        case DOMPromise::Status::Rejected:
            protectedThis->error(*globalObject, promise->result());
            break;
        case DOMPromise::Status::Pending:
            ASSERT_NOT_REACHED();
            break;
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

    // FIXME: handle default reader case.

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
    RefPtr stream = m_stream.get();
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
    } else {
        ASSERT(pullInto.readerType == ReaderType::Byob);
        fulfillReadIntoRequest(globalObject, WTFMove(filledView), done);
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

// https://streams.spec.whatwg.org/#readable-stream-fulfill-read-into-request
void ReadableByteStreamController::fulfillReadIntoRequest(JSDOMGlobalObject& globalObject, RefPtr<JSC::ArrayBufferView>&& filledView, bool done)
{
    RefPtr byobReader = m_stream->byobReader();
    ASSERT(byobReader);
    ASSERT(byobReader->readIntoRequestsSize());

    auto chunk = toJS<IDLNullable<IDLArrayBufferView>>(globalObject, globalObject, WTFMove(filledView));

    byobReader->takeFirstReadIntoRequest()->resolve<IDLDictionary<ReadableStreamReadResult>>({ chunk, done });
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-error
void ReadableByteStreamController::error(JSDOMGlobalObject& globalObject, JSC::JSValue value)
{
    RefPtr stream = m_stream.get();
    if (stream->state() != ReadableStream::State::Readable)
        return;

    clearPendingPullIntos();
    
    m_queue = { };
    m_queueTotalSize = 0;
    
    clearAlgorithms();
    stream->error(globalObject, value);
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-clear-pending-pull-intos
void ReadableByteStreamController::clearPendingPullIntos()
{
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
    RefPtr stream = m_stream.get();
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
    
    auto bufferByteLength = buffer->byteLength();
    PullIntoDescriptor pullIntoDescriptor { WTFMove(buffer), bufferByteLength, byteOffset, byteLength, 0, minimumFill, elementSize, viewType, ReaderType::Byob };
    if (!m_pendingPullIntos.isEmpty()) {
        m_pendingPullIntos.append(WTFMove(pullIntoDescriptor));
        stream->addReadIntoRequest(WTFMove(readIntoRequest));
        return;
    }
    
    if (stream->state() == ReadableStream::State::Closed) {
        // FIXME: Use an empty view.
        readIntoRequest->resolve<IDLDictionary<ReadableStreamReadResult>>({ JSC::jsUndefined(), true });
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
    
    m_pendingPullIntos.append(WTFMove(pullIntoDescriptor));
    stream->addReadIntoRequest(WTFMove(readIntoRequest));
    callPullIfNeeded(globalObject);
}

// https://streams.spec.whatwg.org/#rbs-controller-private-cancel
void ReadableByteStreamController::runCancelSteps(JSDOMGlobalObject& globalObject, JSC::JSValue reason, Function<void(std::optional<JSC::JSValue>&&)>&& callback)
{
    clearPendingPullIntos();

    m_queue = { };
    m_queueTotalSize = 0;

    RefPtr<DOMPromise> cancelPromise;
    if (!m_cancelAlgorithm) {
        auto* promise = JSC::JSPromise::resolvedPromise(&globalObject, JSC::jsUndefined());
        m_callbackPromise = DOMPromise::create(globalObject, *promise);
    } else {
        Ref cancelAlgorithm = *m_cancelAlgorithm;
        auto cancelResult = cancelAlgorithm->handleEvent(m_underlyingSource.getValue(), reason);
        if (cancelResult.type() != CallbackResultType::Success) {
            auto* promise = JSC::JSPromise::rejectedPromise(&globalObject, JSC::jsUndefined());
            m_callbackPromise = DOMPromise::create(globalObject, *promise);
        } else
            m_callbackPromise = cancelResult.releaseReturnValue();
    }

    // FIXME: Determine what to do if there is a pending pull promise, this ASSERT is wrong;
    ASSERT(!m_callbackPromise);
    m_cancelCallback = WTFMove(callback);
    m_callbackPromise->whenSettled([weakThis = WeakPtr { *this }] {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis || !protectedThis->m_cancelCallback || !protectedThis->m_callbackPromise)
            return;

        auto callback = std::exchange(protectedThis->m_cancelCallback, { });
        auto promise = std::exchange(protectedThis->m_callbackPromise, { });
        switch (promise->status()) {
        case DOMPromise::Status::Fulfilled:
            callback({ });
            break;
        case DOMPromise::Status::Rejected:
            callback(std::make_optional(promise->result()));
            break;
        case DOMPromise::Status::Pending:
            ASSERT_NOT_REACHED();
            break;
        }
    });
}

// https://streams.spec.whatwg.org/#rbs-controller-private-pull
void ReadableByteStreamController::runPullSteps(JSDOMGlobalObject& globalObject, Ref<DeferredPromise>&& readRequest)
{
    RefPtr stream = m_stream.get();
    ASSERT(stream->defaultReader());
    
    if (m_queueTotalSize) {
        ASSERT(!stream->getNumReadRequests());
        fillReadRequestFromQueue(globalObject, WTFMove(readRequest));
        return;
    }

    if (auto autoAllocateChunkSize = m_autoAllocateChunkSize) {
        auto buffer = JSC::ArrayBuffer::create(autoAllocateChunkSize, 1);
        m_pendingPullIntos.append({ WTFMove(buffer), autoAllocateChunkSize, 0, autoAllocateChunkSize, 0, 1, 1, JSC::TypedArrayType::TypeUint8, ReaderType::Default });
    }
    stream->addReadRequest(WTFMove(readRequest));
    callPullIfNeeded(globalObject);
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamcontrollerfillreadrequestfromqueue
void ReadableByteStreamController::fillReadRequestFromQueue(JSDOMGlobalObject& globalObject, Ref<DeferredPromise>&& readRequest)
{
    ASSERT(m_queueTotalSize);
    auto entry = m_queue.takeFirst();
    m_queueTotalSize -= entry.byteLength;

    handleQueueDrain(globalObject);

    Ref view = Uint8Array::create(WTFMove(entry.buffer), entry.byteOffset, entry.byteLength);
    readRequest->resolve<IDLUint8Array>(WTFMove(view));
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

    // FIXME: We should use  view.[[ViewedArrayBuffer]].[[ByteLength]].
    if (firstDescriptor.bufferByteLength != view.byteLength())
        return Exception { ExceptionCode::RangeError, "Wrong view byte length"_s };

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

    RefPtr stream = m_stream.get();
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
    shiftPendingPullInto();

    auto remainderSize = pullIntoDescriptor.bytesFilled % pullIntoDescriptor.elementSize;
    if (remainderSize > 0) {
        auto end = pullIntoDescriptor.byteOffset + pullIntoDescriptor.bytesFilled;
        enqueueClonedChunkToQueue(globalObject, pullIntoDescriptor.buffer.get(), end - remainderSize, remainderSize);

        pullIntoDescriptor.bytesFilled = pullIntoDescriptor.bytesFilled - remainderSize;
        commitPullIntoDescriptor(globalObject, pullIntoDescriptor);
        processPullIntoDescriptorsUsingQueue(globalObject);
    }
}

// https://streams.spec.whatwg.org/#readable-byte-stream-controller-handle-queue-drain
void ReadableByteStreamController::handleQueueDrain(JSDOMGlobalObject& globalObject)
{
    ASSERT(m_stream->state() == ReadableStream::State::Readable);

    if (!m_queueTotalSize && m_closeRequested) {
        clearAlgorithms();
        RefPtr stream = m_stream.get();
        stream->close();
    } else {
        callPullIfNeeded(globalObject);
    }
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
