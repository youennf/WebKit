/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY CANON INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL CANON INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <JavaScriptCore/JSCJSValue.h>
#include <wtf/RefCountedAndCanMakeWeakPtr.h>

namespace WebCore {

class DeferredPromise;
class InternalWritableStreamWriter;
class ReadableStream;
class ReadableStreamDefaultReader;
class WritableStream;

struct StreamPipeOptions;

class PipeToState : public RefCountedAndCanMakeWeakPtr<PipeToState> {
public:
    static void readableStreamPipeTo(JSDOMGlobalObject&, Ref<ReadableStream>&&, Ref<WritableStream>&&, Ref<ReadableStreamDefaultReader>&&, Ref<InternalWritableStreamWriter>&&, StreamPipeOptions&&, RefPtr<DeferredPromise>&&);
    ~PipeToState();

private:
    static Ref<PipeToState> create(Ref<ReadableStream>&& source, Ref<WritableStream>&& destination, Ref<ReadableStreamDefaultReader>&& reader, Ref<InternalWritableStreamWriter>&& writer, StreamPipeOptions&& options, RefPtr<DeferredPromise>&& promise)
    {
        return adoptRef(*new PipeToState(WTFMove(source), WTFMove(destination), WTFMove(reader), WTFMove(writer), WTFMove(options), WTFMove(promise)));
    }

    PipeToState(Ref<ReadableStream>&&, Ref<WritableStream>&&, Ref<ReadableStreamDefaultReader>&&, Ref<InternalWritableStreamWriter>&&, StreamPipeOptions&&, RefPtr<DeferredPromise>&&);

    void handleSignal();

    void loop();
    void doReadWrite();

    void errorsMustBePropagatedForward(JSDOMGlobalObject&);
    void errorsMustBePropagatedBackward();
    void closingMustBePropagatedForward();
    void closingMustBePropagatedBackward();

    using Action = Function<RefPtr<DOMPromise>()>;
    using GetError = Function<JSC::JSValue(JSDOMGlobalObject&)>&&;
    void shutdownWithAction(Action&&, GetError&& = { });
    void shutdown(GetError&& = { });
    void finalize(GetError&&);

    RefPtr<DOMPromise> waitForPendingReadAndWrite(Action&&);

    const Ref<ReadableStream> m_source;
    const Ref<WritableStream> m_destination;
    const Ref<ReadableStreamDefaultReader> m_reader;
    const Ref<InternalWritableStreamWriter> m_writer;
    const StreamPipeOptions m_options;
    const RefPtr<DeferredPromise> m_promise;

    bool m_shuttingDown { false };
    RefPtr<DOMPromise> m_pendingReadPromise;
    RefPtr<DOMPromise> m_pendingWritePromise;
};

}
