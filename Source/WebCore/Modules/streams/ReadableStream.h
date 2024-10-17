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

#pragma once

#include "InternalReadableStream.h"
#include "JSValueInWrappedObject.h"
#include <JavaScriptCore/Strong.h>
#include <wtf/RefCounted.h>
#include <wtf/WeakPtr.h>

namespace WebCore {

class InternalReadableStream;
class JSDOMGlobalObject;
class ReadableByteStreamController;
class ReadableStreamBYOBReader;
class ReadableStreamDefaultReader;
class ReadableStreamSource;

struct UnderlyingSource;

class ReadableStream : public RefCounted<ReadableStream>, public CanMakeWeakPtr<ReadableStream> {
public:
    enum class ReaderMode { Byob };
    struct GetReaderOptions {
        std::optional<ReaderMode> mode;
    };

    static ExceptionOr<Ref<ReadableStream>> create(JSDOMGlobalObject&, std::optional<JSC::Strong<JSC::JSObject>>&&, std::optional<JSC::Strong<JSC::JSObject>>&&);
    static ExceptionOr<Ref<ReadableStream>> create(JSDOMGlobalObject&, Ref<ReadableStreamSource>&&);
    static ExceptionOr<Ref<ReadableStream>> createFromByteUnderlyingSource(JSDOMGlobalObject&, JSC::JSValue underlyingSource, UnderlyingSource&&, double highWaterMark);
    static Ref<ReadableStream> create(Ref<InternalReadableStream>&&);

    ~ReadableStream();

    enum class State : uint8_t { Readable, Closed, Errored };
    State state() const { return m_state; }

    ExceptionOr<JSC::Strong<JSC::JSObject>> getReader(JSDOMGlobalObject&, const GetReaderOptions&);

    void lock();
    bool isLocked() const;
    WEBCORE_EXPORT bool isDisturbed() const;
    void cancel(Exception&&);
    void pipeTo(ReadableStreamSink&);
    ExceptionOr<Vector<Ref<ReadableStream>>> tee(bool shouldClone = false);

    InternalReadableStream* internalReadableStream() { return m_internalReadableStream.get(); }

    bool hasByteStreamController() { return !!m_controller; }
    ReadableByteStreamController* controller() { return m_controller.get(); }

    void setByobReader(ReadableStreamBYOBReader*);
    ReadableStreamBYOBReader* byobReader();

    void setAsDisturbed() { m_disturbed = true; }

    void close();
    void error(JSDOMGlobalObject&, JSC::JSValue);
    void cancel(JSDOMGlobalObject&, JSC::JSValue, Ref<DeferredPromise>&&);

    JSC::JSValue storedError() const;

    size_t getNumReadIntoRequests();
    void addReadIntoRequest(Ref<DeferredPromise>&&);

protected:
    static ExceptionOr<Ref<ReadableStream>> createFromJSValues(JSC::JSGlobalObject&, JSC::JSValue, JSC::JSValue);
    static ExceptionOr<Ref<InternalReadableStream>> createInternalReadableStream(JSDOMGlobalObject&, Ref<ReadableStreamSource>&&);
    explicit ReadableStream(RefPtr<InternalReadableStream>&& = { });
    
private:
    ExceptionOr<void> setupReadableByteStreamControllerFromUnderlyingSource(JSDOMGlobalObject&, JSC::JSValue, UnderlyingSource&&, double);

    RefPtr<InternalReadableStream> m_internalReadableStream;

    State m_state { State::Readable };
    bool m_disturbed { false };

    RefPtr<ReadableByteStreamController> m_controller;
    WeakPtr<ReadableStreamBYOBReader> m_byobReader;
};

} // namespace WebCore
