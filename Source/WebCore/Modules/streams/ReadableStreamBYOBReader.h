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

#include "ExceptionOr.h"
#include "IDLTypes.h"
#include "JSValueInWrappedObject.h"
#include <wtf/Deque.h>
#include <wtf/RefCounted.h>
#include <wtf/UniqueRef.h>
#include <wtf/WeakPtr.h>

namespace WebCore {

class DeferredPromise;
class ReadableStream;

template<typename IDLType> class DOMPromiseProxy;

class ReadableStreamBYOBReader : public RefCounted<ReadableStreamBYOBReader>, public CanMakeWeakPtr<ReadableStreamBYOBReader> {
public:
    static ExceptionOr<Ref<ReadableStreamBYOBReader>> create(JSDOMGlobalObject&, ReadableStream&);
    ~ReadableStreamBYOBReader();
    
    struct ReadOptions {
        size_t min { 1 };
    };

    void read(JSDOMGlobalObject&, JSC::ArrayBufferView&, ReadOptions, Ref<DeferredPromise>&&);
    void releaseLock(JSDOMGlobalObject&);

    JSC::JSValue closed();

    void cancel(JSDOMGlobalObject& globalObject, JSC::JSValue, Ref<DeferredPromise>&&);

    void resolveClosedPromise();

    Ref<DeferredPromise> takeFirstReadIntoRequest() { return m_readIntoRequests.takeFirst(); }
    size_t readIntoRequestsSize() const { return m_readIntoRequests.size(); }
    void addReadIntoRequest(Ref<DeferredPromise>&& promise) { m_readIntoRequests.append(WTFMove(promise)); }

    void rejectClosedPromise(JSC::JSValue);
    void errorReadIntoRequests(JSC::JSValue);

private:
    explicit ReadableStreamBYOBReader(JSDOMGlobalObject&);

    ExceptionOr<void> setupBYOBReader(ReadableStream&);
    void initialize(ReadableStream&);
    void read(JSDOMGlobalObject&, JSC::ArrayBufferView&, size_t, Ref<DeferredPromise>&&);
    void genericRelease(JSDOMGlobalObject&);
    void errorReadIntoRequests(Exception&&);

    void genericCancel(JSDOMGlobalObject&, JSC::JSValue, Ref<DeferredPromise>&&);

    Ref<DeferredPromise> m_closedPromise;
    RefPtr<ReadableStream> m_stream;
    Deque<Ref<DeferredPromise>> m_readIntoRequests;
};

} // namespace WebCore
