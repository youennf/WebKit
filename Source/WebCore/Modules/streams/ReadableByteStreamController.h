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

#pragma once

#include "JSValueInWrappedObject.h"
#include <wtf/RefCounted.h>
#include <wtf/WeakPtr.h>

namespace JSC {
class ArrayBufferView;
class JSValue;
class VM;
}

namespace WebCore {

class JSDOMGlobalObject;
class ReadableStream;
class ReadableStreamBYOBRequest;
class UnderlyingSourceCancelCallback;
class UnderlyingSourcePullCallback;
class UnderlyingSourceStartCallback;

class ReadableByteStreamController : public RefCounted<ReadableByteStreamController>, public CanMakeWeakPtr<ReadableByteStreamController> {
public:
    static Ref<ReadableByteStreamController> create(ReadableStream& stream, JSC::JSValue underlyingSource, RefPtr<UnderlyingSourcePullCallback>&& pullAlgorithm, RefPtr<UnderlyingSourceCancelCallback>&& cancelAlgorithm, double highWaterMark, size_t autoAllocateChunkSize) { return adoptRef(*new ReadableByteStreamController(stream, underlyingSource, WTFMove(pullAlgorithm), WTFMove(cancelAlgorithm), highWaterMark, autoAllocateChunkSize)); }
    ~ReadableByteStreamController();

    ReadableStreamBYOBRequest* byobRequestForBindings() const;
    std::optional<double> desiredSize() const;

    ExceptionOr<void> closeForBindings();
    ExceptionOr<void> enqueueForBindings(JSDOMGlobalObject&, JSC::ArrayBufferView&);
    ExceptionOr<void> errorForBindings(JSDOMGlobalObject&, JSC::JSValue);

    void start(JSDOMGlobalObject&, UnderlyingSourceStartCallback*);

    JSValueInWrappedObject& underlyingSource() { return m_underlyingSource; }
    ReadableStream* stream();

    UnderlyingSourcePullCallback* pullAlgorithmConcurrently() { return m_pullAlgorithm.get(); }
    UnderlyingSourceCancelCallback* cancelAlgorithmConcurrently() { return m_cancelAlgorithm.get(); }

    void pullInto(JSDOMGlobalObject&, JSC::ArrayBufferView&, size_t, Ref<DeferredPromise>&&);

    void runCancelSteps(JSDOMGlobalObject&, JSC::JSValue, Function<void(std::optional<JSC::JSValue>&&)>&&);

    void storeError(JSDOMGlobalObject&, JSC::JSValue);
    JSC::JSValue storedError() const;

    ExceptionOr<void> respond(JSDOMGlobalObject&, size_t);
    ExceptionOr<void> respondWithNewView(JSDOMGlobalObject&, JSC::ArrayBufferView&);

private:
    ReadableByteStreamController(ReadableStream&, JSC::JSValue, RefPtr<UnderlyingSourcePullCallback>&&, RefPtr<UnderlyingSourceCancelCallback>&&, double highWaterMark, size_t m_autoAllocateChunkSize);

    enum ReaderType : uint8_t { None, Default, Byob };

    struct PullIntoDescriptor {
        Ref<JSC::ArrayBuffer> buffer;
        size_t bufferByteLength { 0 };
        size_t byteOffset { 0 };
        size_t byteLength { 0 };
        size_t bytesFilled { 0 };
        size_t minimumFill { 0 };
        size_t elementSize { 0 };
        JSC::TypedArrayType viewConstructor;
        ReaderType readerType;
    };

    struct Entry {
        Ref<JSC::ArrayBuffer> buffer;
        size_t byteOffset { 0 };
        size_t byteLength { 0 };
    };

    ReadableStreamBYOBRequest* getByobRequest() const;
    std::optional<double> getDesiredSize() const;
    ExceptionOr<void> enqueue(JSDOMGlobalObject&, JSC::ArrayBufferView&);
    void didStart(JSDOMGlobalObject&);

    void close();

    void invalidateByobRequest();
    void processPullIntoDescriptorsUsingQueue(JSDOMGlobalObject&);
    void enqueueDetachedPullIntoToQueue(JSDOMGlobalObject&, PullIntoDescriptor&);
    PullIntoDescriptor shiftPendingPullInto();
    void enqueueChunkToQueue(Ref<JSC::ArrayBuffer>&&, size_t byteOffset, size_t byteLength);
    void enqueueClonedChunkToQueue(JSDOMGlobalObject&, JSC::ArrayBuffer&, size_t byteOffset, size_t byteLength);
    void callPullIfNeeded(JSDOMGlobalObject&);
    bool shouldCallPull();
    bool fillPullIntoDescriptorFromQueue(PullIntoDescriptor&);
    void commitPullIntoDescriptor(JSDOMGlobalObject&, PullIntoDescriptor&);
    RefPtr<JSC::ArrayBufferView> convertPullIntoDescriptor(JSC::VM&, PullIntoDescriptor&);
    void fillHeadPullIntoDescriptor(size_t, PullIntoDescriptor&);
    void fulfillReadIntoRequest(JSDOMGlobalObject&, RefPtr<JSC::ArrayBufferView>&&, bool done);

    void error(JSDOMGlobalObject&, JSC::JSValue);
    void clearAlgorithms();
    void clearPendingPullIntos();

    void respondInternal(JSDOMGlobalObject&, size_t);
    void respondInClosedState(JSDOMGlobalObject&, PullIntoDescriptor&);
    void respondInReadableState(JSDOMGlobalObject&, size_t, PullIntoDescriptor&);

    void handleQueueDrain(JSDOMGlobalObject&);

    WeakPtr<ReadableStream> m_stream;
    bool m_pullAgain { false };
    bool m_pulling { false };
    mutable RefPtr<ReadableStreamBYOBRequest> m_byobRequest;
    bool m_closeRequested { false };
    bool m_started { false };
    double m_strategyHWM { 0 };
    RefPtr<UnderlyingSourcePullCallback> m_pullAlgorithm;
    RefPtr<UnderlyingSourceCancelCallback> m_cancelAlgorithm;
    size_t m_autoAllocateChunkSize { 0 };
    Deque<PullIntoDescriptor> m_pendingPullIntos;
    Deque<Entry> m_queue;
    size_t m_queueTotalSize { 0 };

    // FIXME: Visit these two.
    JSValueInWrappedObject m_underlyingSource;
    JSValueInWrappedObject m_storedError;
    RefPtr<DOMPromise> m_callbackPromise;
    Function<void(std::optional<JSC::JSValue>&&)> m_cancelCallback;
};

} // namespace WebCore
