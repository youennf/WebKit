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

#include "config.h"
#include "ReadableStreamBYOBReader.h"

#include "DOMPromiseProxy.h"
#include "ReadableByteStreamController.h"
#include "ReadableStream.h"
#include <JavaScriptCore/ArrayBuffer.h>
#include <JavaScriptCore/ArrayBufferView.h>

namespace WebCore {

ExceptionOr<Ref<ReadableStreamBYOBReader>> ReadableStreamBYOBReader::create(ReadableStream& stream)
{
    Ref reader = adoptRef(*new ReadableStreamBYOBReader);
    auto result = reader->setupBYOBReader(stream);
    if (result.hasException())
        return result.releaseException();
    return reader;
}

ReadableStreamBYOBReader::ReadableStreamBYOBReader()
    : m_closedPromise(makeUniqueRef<ClosedPromise>())
{
}

ReadableStreamBYOBReader::~ReadableStreamBYOBReader() = default;

void ReadableStreamBYOBReader::read(JSC::ArrayBufferView& view, ReadOptions options, Ref<DeferredPromise>&& promise)
{
    if (view.byteLength() == 0)
        return promise->reject(Exception { ExceptionCode::TypeError, "view byteLength is 0"_s });
    
    RefPtr buffer = view.possiblySharedBuffer();
    if (!buffer)
        return promise->reject(Exception { ExceptionCode::TypeError, "view's buffer is detached"_s });
    
    if (!buffer->byteLength())
        return promise->reject(Exception { ExceptionCode::TypeError, "view's buffer byteLength is 0"_s });
    
    if (!options.min)
        return promise->reject(Exception { ExceptionCode::TypeError, "options min is 0"_s });
    
    if (options.min > view.byteLength())
        return promise->reject(Exception { ExceptionCode::RangeError, "view's buffer is not long enough"_s });
    
    if (!m_stream)
        return promise->reject(Exception { ExceptionCode::TypeError, "reader has no stream"_s });
    
    read(view, options.min, WTFMove(promise));
}

void ReadableStreamBYOBReader::releaseLock()
{
    if (!m_stream)
        return;

    genericRelease();

    errorReadIntoRequests(Exception { ExceptionCode::TypeError, "releasing stream"_s });
}

void ReadableStreamBYOBReader::cancel(JSC::JSValue, Ref<DeferredPromise>&&)
{
}

// https://streams.spec.whatwg.org/#set-up-readable-stream-byob-reader
ExceptionOr<void> ReadableStreamBYOBReader::setupBYOBReader(ReadableStream& stream)
{
    if (stream.isLocked())
        return Exception { ExceptionCode::TypeError, "stream is locked"_s };
    
    if (!stream.hasByteStreamController())
        return Exception { ExceptionCode::TypeError, "stream is not a byte stream"_s };
    
    initialize(stream);
    return { };
}

// https://streams.spec.whatwg.org/#set-up-readable-stream-byob-reader
void ReadableStreamBYOBReader::initialize(ReadableStream& stream)
{
    m_stream = &stream;

    stream.setReader(this);

    switch (stream.state()) {
    case ReadableStream::State::Readable:
        break;
    case ReadableStream::State::Closed:
        m_closedPromise->resolve();
        break;
    case ReadableStream::State::Errored:
//        m_closedPromise->reject<IDLAny>(stream.storedError().getValue(), RejectAsHandled::Yes);
        // FIXME: reject with stored error.
        m_closedPromise->reject(Exception { ExceptionCode::TypeError }, RejectAsHandled::Yes);
        break;
    }
}

// https://streams.spec.whatwg.org/#readable-stream-byob-reader-read
void ReadableStreamBYOBReader::read(JSC::ArrayBufferView& view, size_t optionMin, Ref<DeferredPromise>&& promise)
{
    ASSERT(m_stream);
    
    m_stream->setAsDisturbed();
    if (m_stream->state() == ReadableStream::State::Errored) {
        promise->reject<IDLAny>(m_stream->storedError().getValue());
        return;
    }

    RefPtr controller = m_stream->controller();
    controller->pullInto(view, optionMin, WTFMove(promise));
}

// https://streams.spec.whatwg.org/#readable-stream-reader-generic-release
void ReadableStreamBYOBReader::genericRelease()
{
    ASSERT(m_stream);
    ASSERT(m_stream->reader() == this);

    if (m_stream->state() == ReadableStream::State::Readable)
        m_closedPromise->reject(Exception { ExceptionCode::TypeError, "releasing stream"_s }, RejectAsHandled::Yes);
    else {
        m_closedPromise = makeUniqueRef<ClosedPromise>();
        m_closedPromise->reject(Exception { ExceptionCode::TypeError, "releasing stream"_s }, RejectAsHandled::Yes);
    }

    m_stream->setReader(nullptr);
    m_stream = nullptr;
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablestreambyobreadererrorreadintorequests
void ReadableStreamBYOBReader::errorReadIntoRequests(Exception&& exception)
{
    auto requests = std::exchange(m_readIntoRequests, { });
    for (auto& request : requests)
        request->reject(exception);
}

} // namespace WebCore
