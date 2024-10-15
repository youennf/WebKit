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

#include "JSDOMPromise.h"
#include "ReadableStream.h"
#include "ReadableStreamBYOBRequest.h"
#include "UnderlyingSourceCancelCallback.h"
#include "UnderlyingSourcePullCallback.h"
#include "UnderlyingSourceStartCallback.h"

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

RefPtr<ReadableStreamBYOBRequest> ReadableByteStreamController::byobRequestForBindings() const
{
    return { };
//    return byobRequest();
}

ReadableStream* ReadableByteStreamController::stream()
{
    return m_stream.get();
}

std::optional<double> ReadableByteStreamController::desiredSize() const
{
    return getDesiredSize();
}

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

ExceptionOr<void> ReadableByteStreamController::closeForBindings()
{
    if (m_closeRequested)
        return Exception { ExceptionCode::TypeError, "controller is closed"_s };

    if (m_stream->state() != ReadableStream::State::Readable)
        return Exception { ExceptionCode::TypeError, "controller's stream is not readable"_s };

//    close();
    return { };
}

ExceptionOr<void> ReadableByteStreamController::enqueueForBindings(JSC::ArrayBufferView& chunk)
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

//    enqueue(chunk);
    return { };
}

ExceptionOr<void> ReadableByteStreamController::errorForBindings(JSC::JSValue value)
{
    error(value);
    return { };
}

void ReadableByteStreamController::start(UnderlyingSourceStartCallback* startAlgorithm)
{
    if (!startAlgorithm) {
        // FIXME: this should be done in a microtask.
        didStart();
        return;
    }

    auto startResult = startAlgorithm->handleEvent(m_underlyingSource.getValue(), *this);
    if (startResult.type() != CallbackResultType::Success) {
        // FIXME: Use an exception?
        return;
    }
/*
    m_callbackPromise = startResult.releaseReturnValue();
    m_callbackPromise->whenSettled([weakThis = WeakPtr { *this }] {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis || !protectedThis->m_callbackPromise)
            return;

        auto promise = std::exchange(protectedThis->m_callbackPromise, { });
        switch (promise->status()) {
        case DOMPromise::Status::Fulfilled:
            protectedThis->didStart();
            break;
        case DOMPromise::Status::Rejected:
            protectedThis->error(promise->result());
            break;
        case DOMPromise::Status::Pending:
            ASSERT_NOT_REACHED();
            break;
        }
    });
*/
}

void ReadableByteStreamController::didStart()
{
    m_started = true;
    ASSERT(!m_pulling);
    ASSERT(!m_pullAgain);
    pullIfNeeded();
}

void ReadableByteStreamController::error(JSC::JSValue)
{
    RefPtr stream = m_stream.get();
    if (stream->state() != ReadableStream::State::Readable)
        return;

    clearPendingPullIntos();
    
    m_queue = { };
    m_queueTotalSize = 0;
    
    clearAlgorithms();
//    stream->error();
}

void ReadableByteStreamController::pullIfNeeded()
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
        // Report error.
        return;
    }

    m_callbackPromise = pullResult.releaseReturnValue();
    m_callbackPromise->whenSettled([weakThis = WeakPtr { *this }] {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis || !protectedThis->m_callbackPromise)
            return;

        auto promise = std::exchange(protectedThis->m_callbackPromise, { });
        switch (promise->status()) {
        case DOMPromise::Status::Fulfilled:
            protectedThis->m_pulling = false;
            if (protectedThis->m_pullAgain) {
                protectedThis->m_pullAgain = false;
                protectedThis->pullIfNeeded();
            }
            break;
        case DOMPromise::Status::Rejected:
            protectedThis->error(promise->result());
            break;
        case DOMPromise::Status::Pending:
            ASSERT_NOT_REACHED();
            break;
        }
    });
}

void ReadableByteStreamController::clearAlgorithms()
{
    m_pullAlgorithm = nullptr;
    m_cancelAlgorithm = nullptr;
}

void ReadableByteStreamController::clearPendingPullIntos()
{
//    invalidateBYOBRequest();
    m_pendingPullIntos = { };
}

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
        
    }
    
    clearAlgorithms();
//    stream->close();
}
/*
void ReadableByteStreamController::commitPullIntoDescriptor(pullIntoDescriptor)
{
    RefPtr stream = m_stream.get();
    ASSERT(stream->state() != ReadableStream::State::Errored);
    ASSERT(pullIntoDescriptor.readerType != ReaderType::None);
    
    bool done = false;
    if (stream->state() != ReadableStream::State::Closed) {
        // Implement ASSERT.
        done = true;
    }
    
    auto filledView = convertPullIntoDescriptor(pullIntoDescriptor);
    
    if (pullIntoDescriptor.readerType == ReaderType::Default) {
        stream->fulfillReadRequest(filledView, done);
        return;
    }
    
    ASSERT(pullIntoDescriptor.readerType != ReaderType::Byob);
    stream->fulfillReadIntoRequest(filledView, done);
}

RefPtr<> ReadableByteStreamController::convertPullIntoDescriptor(pullIntoDescriptor)
{
    // FIXME: Implement this.
}

void ReadableByteStreamController::enqueue(JSC::ArrayBufferView& value)
{
    RefPtr stream = m_stream.get();
    if (m_closeRequested || stream->state() != ReadableStream::State::Readable)
        return;

}
 */

bool ReadableByteStreamController::shouldCallPull()
{
    return false;
}

// https://streams.spec.whatwg.org/#readable-stream-byob-reader-read
void ReadableByteStreamController::pullInto(JSC::ArrayBufferView&, size_t, Ref<DeferredPromise>&&)
{
}

} // namespace WebCore
