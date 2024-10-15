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
}

namespace WebCore {

class ReadableStream;
class ReadableStreamBYOBRequest;
class UnderlyingSourceCancelCallback;
class UnderlyingSourcePullCallback;
class UnderlyingSourceStartCallback;

class ReadableByteStreamController : public RefCounted<ReadableByteStreamController>, public CanMakeWeakPtr<ReadableByteStreamController> {
public:
    static Ref<ReadableByteStreamController> create(ReadableStream& stream, JSC::JSValue underlyingSource, RefPtr<UnderlyingSourcePullCallback>&& pullAlgorithm, RefPtr<UnderlyingSourceCancelCallback>&& cancelAlgorithm, double highWaterMark, size_t autoAllocateChunkSize) { return adoptRef(*new ReadableByteStreamController(stream, underlyingSource, WTFMove(pullAlgorithm), WTFMove(cancelAlgorithm), highWaterMark, autoAllocateChunkSize)); }
    ~ReadableByteStreamController();

    RefPtr<ReadableStreamBYOBRequest> byobRequestForBindings() const;
    std::optional<double> desiredSize() const;

    ExceptionOr<void> closeForBindings();
    ExceptionOr<void> enqueueForBindings(JSC::ArrayBufferView&);
    ExceptionOr<void> errorForBindings(JSC::JSValue);

    void start(UnderlyingSourceStartCallback*);

    JSValueInWrappedObject& underlyingSourceWrapper() { return m_underlyingSourceWrapper; }
    ReadableStream* stream();

    UnderlyingSourcePullCallback* pullAlgorithmConcurrently() { return m_pullAlgorithm.get(); }
    UnderlyingSourceCancelCallback* cancelAlgorithmConcurrently() { return m_cancelAlgorithm.get(); }

    void pullInto(JSC::ArrayBufferView&, size_t, Ref<DeferredPromise>&&);

private:
    ReadableByteStreamController(ReadableStream&, JSC::JSValue, RefPtr<UnderlyingSourcePullCallback>&&, RefPtr<UnderlyingSourceCancelCallback>&&, double highWaterMark, size_t m_autoAllocateChunkSize);

    std::optional<double> getDesiredSize() const;
    void didStart();
    void error(JSC::JSValue);
    void close();
    void pullIfNeeded();
    void clearAlgorithms();
    void clearPendingPullIntos();
    bool shouldCallPull();

    JSValueInWrappedObject m_underlyingSourceWrapper;
    
    WeakPtr<ReadableStream> m_stream;
    bool m_pullAgain { false };
    bool m_pulling { false };
    RefPtr<ReadableStreamBYOBRequest> m_byobRequest;
    bool m_closeRequested { false };
    bool m_started { false };
    double m_strategyHWM { 0 };
    RefPtr<UnderlyingSourcePullCallback> m_pullAlgorithm;
    RefPtr<UnderlyingSourceCancelCallback> m_cancelAlgorithm;
    size_t m_autoAllocateChunkSize { 0 };
    Deque<int> m_pendingPullIntos;
    Deque<int> m_queue;
    size_t m_queueTotalSize { 0 };

    JSValueInWrappedObject m_underlyingSource;
    RefPtr<DOMPromise> m_callbackPromise;
};

} // namespace WebCore
