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

#include "InternalWritableStreamWriter.h"
#include "JSDOMPromise.h"
#include "JSDOMPromiseDeferred.h"
#include "JSReadableStream.h"
#include "JSReadableStreamBYOBReader.h"
#include "JSReadableStreamDefaultReader.h"
#include "JSReadableStreamReadResult.h"
#include "JSReadableStreamSource.h"
#include "JSStreamPipeOptions.h"
#include "JSUnderlyingSource.h"
#include "QueuingStrategy.h"
#include "ReadableByteStreamController.h"
#include "ReadableStreamBYOBReader.h"
#include "ReadableStreamBYOBRequest.h"
#include "ScriptExecutionContext.h"
#include "StreamPipeToUtilities.h"
#include "WritableStream.h"

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

static std::optional<bool> isReadableByteStream(JSDOMGlobalObject& globalObject, JSC::JSValue underlyingSource)
{
    bool isNullOrUndefined = underlyingSource.isUndefinedOrNull();
    auto* object = isNullOrUndefined ? nullptr : underlyingSource.getObject();
    if (!object)
        return false;

    Ref vm = globalObject.vm();
    auto throwScope = DECLARE_THROW_SCOPE(vm);
    auto typeValue = object->get(&globalObject, JSC::Identifier::fromString(vm, "type"_s));
    if (throwScope.exception())
        return { };

    if (typeValue.isUndefined())
        return false;;

    convert<IDLEnumeration<ReadableStreamType>>(globalObject, typeValue);
    if (throwScope.exception())
        return { };

    return true;
}

ExceptionOr<Ref<ReadableStream>> ReadableStream::create(JSDOMGlobalObject& globalObject, std::optional<JSC::Strong<JSC::JSObject>>&& underlyingSourceValue, std::optional<JSC::Strong<JSC::JSObject>>&& strategyValue)
{
    JSC::JSValue underlyingSource = JSC::jsUndefined();
    if (underlyingSourceValue)
        underlyingSource = underlyingSourceValue->get();

    JSC::JSValue strategy = JSC::jsUndefined();
    if (strategyValue)
        strategy = strategyValue->get();

    // FIXME: We convert strategy twice for regular readable streams.
    auto strategyDictOrException = convertDictionary<QueuingStrategy>(globalObject, strategy);
    {
        auto throwScope = DECLARE_THROW_SCOPE(globalObject.vm());
        if (strategyDictOrException.hasException(throwScope))
            return Exception { ExceptionCode::ExistingExceptionError };
    }

    auto isReadableByteStreamResult = isReadableByteStream(globalObject, underlyingSource);
    if (!isReadableByteStreamResult)
        return Exception { ExceptionCode::ExistingExceptionError };

    if (*isReadableByteStreamResult) {
        auto underlyingSourceDictOrException = convertDictionary<UnderlyingSource>(globalObject, underlyingSource);
        {
            auto throwScope = DECLARE_THROW_SCOPE(globalObject.vm());
            if (underlyingSourceDictOrException.hasException(throwScope))
                return Exception { ExceptionCode::ExistingExceptionError };
        }

        auto underlyingSourceDict = underlyingSourceDictOrException.releaseReturnValue();
        auto strategyDict = strategyDictOrException.releaseReturnValue();

        if (strategyDict.size)
            return Exception { ExceptionCode::RangeError, "size should not be present"_s };

        auto highWaterMarkOrException = extractHighWaterMark(strategyDict, 0);
        if (highWaterMarkOrException.hasException())
            return highWaterMarkOrException.releaseException();
        auto highWatermark = highWaterMarkOrException.releaseReturnValue();

        return createFromByteUnderlyingSource(globalObject, underlyingSource, WTFMove(underlyingSourceDict), highWatermark);
    }

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

ExceptionOr<Ref<ReadableStream>> ReadableStream::createFromByteUnderlyingSource(JSDOMGlobalObject& globalObject, JSC::JSValue underlyingSource, UnderlyingSource&& underlyingSourceDict, double highWaterMark)
{
    auto readableStream = adoptRef(*new ReadableStream());
    
    readableStream->setupReadableByteStreamControllerFromUnderlyingSource(globalObject, underlyingSource, WTFMove(underlyingSourceDict), highWaterMark);
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

void ReadableStream::lock()
{
    if (RefPtr internalReadableStream = m_internalReadableStream)
        internalReadableStream->lock();
}

bool ReadableStream::isLocked() const
{
    return !!m_byobReader || (m_internalReadableStream && m_internalReadableStream->isLocked());
}

bool ReadableStream::isDisturbed() const
{
    return m_disturbed || (m_internalReadableStream && m_internalReadableStream->isDisturbed());
}

void ReadableStream::cancel(Exception&& exception)
{
    // FIXME: support byte stream.
    if (RefPtr internalReadableStream = m_internalReadableStream)
        internalReadableStream->cancel(WTFMove(exception));
}

void ReadableStream::pipeTo(ReadableStreamSink& sink)
{
    // FIXME: support byte stream.
    if (RefPtr internalReadableStream = m_internalReadableStream)
        internalReadableStream->pipeTo(sink);
}

ReadableStream::State ReadableStream::state() const
{
    if (RefPtr internalReadableStream = m_internalReadableStream)
        return internalReadableStream->state();

    return m_state;
}

ExceptionOr<Vector<Ref<ReadableStream>>> ReadableStream::tee(JSDOMGlobalObject& globalObject, bool shouldClone)
{
    if (!m_internalReadableStream) {
        ASSERT(m_controller);
        return byteStreamTee(globalObject);
    }

    Ref internalReadableStream = *m_internalReadableStream;
    auto result = internalReadableStream->tee(shouldClone);
    if (result.hasException())
        return result.releaseException();
    
    auto pair = result.releaseReturnValue();
    
    return Vector {
        ReadableStream::create(WTFMove(pair.first)),
        ReadableStream::create(WTFMove(pair.second))
    };
}

ExceptionOr<JSC::Strong<JSC::JSObject>> ReadableStream::getReader(JSDOMGlobalObject& jsDOMGlobalObject, const GetReaderOptions& options)
{
    if (!m_internalReadableStream) {
        ASSERT(m_controller);
        if (options.mode) {
            auto readerOrException = ReadableStreamBYOBReader::create(jsDOMGlobalObject, *this);
            if (readerOrException.hasException())
                return readerOrException.releaseException();
            auto newReaderValue = toJSNewlyCreated<IDLInterface<ReadableStreamBYOBReader>>(jsDOMGlobalObject, jsDOMGlobalObject, readerOrException.releaseReturnValue());
            Ref vm = jsDOMGlobalObject.vm();
            return JSC::Strong<JSC::JSObject> { vm.get(), newReaderValue.toObject(&jsDOMGlobalObject) };
        }
        auto readerOrException = ReadableStreamDefaultReader::create(jsDOMGlobalObject, *this);
        if (readerOrException.hasException())
            return readerOrException.releaseException();
        auto newReaderValue = toJSNewlyCreated<IDLInterface<ReadableStreamDefaultReader>>(jsDOMGlobalObject, jsDOMGlobalObject, readerOrException.releaseReturnValue());
        Ref vm = jsDOMGlobalObject.vm();
        return JSC::Strong<JSC::JSObject> { vm.get(), newReaderValue.toObject(&jsDOMGlobalObject) };
    }

    if (options.mode)
        return m_internalReadableStream->getByobReader();

    // FIXME: Do we need this one.
    auto* globalObject = JSC::jsCast<JSDOMGlobalObject*>(m_internalReadableStream->globalObject());

    auto readerOrException = ReadableStreamDefaultReader::create(*globalObject, *m_internalReadableStream);
    if (readerOrException.hasException())
        return readerOrException.releaseException();

    auto newReaderValue = toJSNewlyCreated<IDLInterface<ReadableStreamDefaultReader>>(*globalObject, *globalObject, readerOrException.releaseReturnValue());
    return JSC::Strong<JSC::JSObject> { globalObject->vm(), newReaderValue.toObject(globalObject) };
}

void ReadableStream::setDefaultReader(ReadableStreamDefaultReader* reader)
{
    ASSERT(!m_defaultReader || !reader);
    ASSERT(!m_byobReader);
    m_defaultReader = WeakPtr { reader };
}

ReadableStreamDefaultReader* ReadableStream::defaultReader()
{
    return m_defaultReader.get();
}

// https://streams.spec.whatwg.org/#abstract-opdef-createreadablebytestream
Ref<ReadableStream> ReadableStream::createReadableByteStream(ReadableByteStreamController::PullAlgorithm&& pullAlgorithm, ReadableByteStreamController::CancelAlgorithm&& cancelAlgorithm)
{
    Ref readableStream = adoptRef(*new ReadableStream());
    readableStream->setupReadableByteStreamController(WTFMove(pullAlgorithm), WTFMove(cancelAlgorithm), 0);
    return readableStream;
}

static RefPtr<DOMPromise> domPromiseFromDeferred(JSDOMGlobalObject& globalObject, DeferredPromise& deferred)
{
    auto* promise = jsCast<JSC::JSPromise*>(deferred.promise());
    if (!promise)
        return nullptr;
    return DOMPromise::create(globalObject, *promise);
}

class TeeState : public RefCounted<TeeState> {
public:
    static Ref<TeeState> create(JSDOMGlobalObject& globalObject, Ref<ReadableStream>&& stream, Ref<ReadableStreamDefaultReader>&& reader) { return adoptRef(*new TeeState(globalObject, WTFMove(stream), WTFMove(reader))); }
    
    bool isReader(const ReadableStreamDefaultReader* thisReader) const { return m_defaultReader && m_defaultReader.get() == thisReader; }
    bool isReader(const ReadableStreamBYOBReader* thisReader) const { return m_byobReader && m_byobReader.get() == thisReader; }
    
    bool reading() const { return m_reading;}
    void setReading(bool value) { m_reading = value; }
    
    bool readAgainForBranch1() const { return m_readAgainForBranch1; }
    void setReadAgainForBranch1(bool value) { m_readAgainForBranch1 = value; }
    
    bool readAgainForBranch2() const { return m_readAgainForBranch2; }
    void setReadAgainForBranch2(bool value) { m_readAgainForBranch2 = value; }
    
    bool canceled1() const { return m_canceled1;}
    bool canceled2() const { return m_canceled2;}
    void setCanceled1() { m_canceled1 = true; }
    void setCanceled2() { m_canceled2 = true; }
    JSC::JSValue reason1() { return m_branch1Reason.get(); }
    JSC::JSValue reason2() { return m_branch2Reason.get(); }
    void setReason1(JSDOMGlobalObject& globalObject, JSC::JSValue value)
    {
        Ref vm = globalObject.vm();
        m_branch1Reason = {vm, value };
    }
    void setReason2(JSDOMGlobalObject& globalObject, JSC::JSValue value)
    {
        Ref vm = globalObject.vm();
        m_branch2Reason = {vm, value };
    }

    ReadableStream& stream() const { return m_stream; }
    ReadableStream* branch1() const { return m_branch1.get(); }
    ReadableStream* branch2() const { return m_branch2.get(); }
    void setBranch1(Ref<ReadableStream>&& stream) { m_branch1 = WTFMove(stream); }
    void setBranch2(Ref<ReadableStream>&& stream) { m_branch2 = WTFMove(stream); }

    DOMPromise* readPromise() const {return m_readPromise.get(); }
    void setReadPromise(Ref<DOMPromise>&& promise)
    {
        ASSERT(!m_readPromise);
        m_readPromise = WTFMove(promise);
    }

    ReadableStreamBYOBReader* byobReader() const { return m_byobReader.get(); }
    RefPtr<ReadableStreamBYOBReader> takeBYOBReader() { return std::exchange(m_byobReader, { }); }
    void setReader(Ref<ReadableStreamBYOBReader>&& reader)
    {
        ASSERT(!m_defaultReader);
        ASSERT(!m_byobReader);
        m_byobReader = WTFMove(reader);
    }

    ReadableStreamDefaultReader* defaultReader() const { return m_defaultReader.get(); }
    RefPtr<ReadableStreamDefaultReader> takeDefaultReader() { return std::exchange(m_defaultReader, { }); }
    void setReader(Ref<ReadableStreamDefaultReader>&& reader)
    {
        ASSERT(!m_defaultReader);
        ASSERT(!m_byobReader);
        m_defaultReader = WTFMove(reader);
    }

    DOMPromise& cancelPromise() { return m_cancelPromise; }
    void resolveCancelPromise()
    {
        m_cancelDeferredPromise->resolve();
    }

private:
    explicit TeeState(JSDOMGlobalObject& globalObject, Ref<ReadableStream>&& stream, RefPtr<ReadableStreamDefaultReader>&& reader)
        : m_stream(WTFMove(stream))
        , m_defaultReader(WTFMove(reader))
        , m_cancelDeferredPromise(DeferredPromise::create(globalObject).releaseNonNull())
        , m_cancelPromise(domPromiseFromDeferred(globalObject, m_cancelDeferredPromise).releaseNonNull())
    {
    }

    const Ref<ReadableStream> m_stream;
    RefPtr<ReadableStreamDefaultReader> m_defaultReader;
    RefPtr<ReadableStreamBYOBReader> m_byobReader;
    bool m_reading = false;
    bool m_readAgainForBranch1 = false;
    bool m_readAgainForBranch2 = false;
    bool m_canceled1 = false;
    bool m_canceled2 = false;
    Ref<DeferredPromise> m_cancelDeferredPromise;
    Ref<DOMPromise> m_cancelPromise;
    RefPtr<ReadableStream> m_branch1;
    RefPtr<ReadableStream> m_branch2;

    JSC::Strong<JSC::Unknown> m_branch1Reason;
    JSC::Strong<JSC::Unknown> m_branch2Reason;

    RefPtr<DOMPromise> m_readPromise;
};

template<typename Reader>
static void forwardReadError(TeeState& state, Reader& thisReader)
{
    thisReader.onClosedPromiseRejection([state = Ref { state }, thisReader = WeakPtr { thisReader }](auto& globalObject, auto&& reason) {
        if (!state->isReader(thisReader.get()))
            return;
        if (RefPtr branch1 = state->branch1())
            branch1->controller()->error(globalObject, reason);
        if (RefPtr branch2 = state->branch2())
            branch2->controller()->error(globalObject, reason);
    });
}

static ExceptionOr<Ref<JSC::ArrayBufferView>> cloneAsUInt8Array(JSC::ArrayBufferView& o)
{
    RefPtr buffer = JSC::ArrayBuffer::tryCreate(o.span());
    if (!buffer)
        return Exception { ExceptionCode::OutOfMemoryError };

    Ref<JSC::ArrayBufferView> clone = JSC::Uint8Array::create(WTFMove(buffer), 0, o.byteLength());

    return clone;
}

static void pullWithBYOBReader(JSDOMGlobalObject&, TeeState&, ReadableStreamBYOBRequest&, bool);
static void pullWithDefaultReader(JSDOMGlobalObject&, TeeState&);

static Ref<DOMPromise> pull1Steps(TeeState& state, JSDOMGlobalObject& globalObject)
{
    if (state.reading()) {
        state.setReadAgainForBranch1(true);
        auto [promise, deferred] = createPromiseAndWrapper(globalObject);
        deferred->resolve();
        return promise;
    }

    state.setReading(true);
    
    RefPtr byobRequest = state.branch1()->protectedController()->getByobRequest();
    if (!byobRequest)
        pullWithDefaultReader(globalObject, state);
    else
        pullWithBYOBReader(globalObject, state, *byobRequest, false);

    auto [promise, deferred] = createPromiseAndWrapper(globalObject);
    deferred->resolve();
    return promise;
};

static Ref<DOMPromise> pull2Steps(TeeState& state, JSDOMGlobalObject& globalObject)
{
    if (state.reading()) {
        state.setReadAgainForBranch2(true);
        auto [promise, deferred] = createPromiseAndWrapper(globalObject);
        deferred->resolve();
        return promise;
    }

    state.setReading(true);
    
    RefPtr byobRequest = state.branch2()->protectedController()->getByobRequest();
    if (!byobRequest)
        pullWithDefaultReader(globalObject, state);
    else
        pullWithBYOBReader(globalObject, state, *byobRequest, false);

    auto [promise, deferred] = createPromiseAndWrapper(globalObject);
    deferred->resolve();
    return promise;
};

static void pullWithDefaultReader(JSDOMGlobalObject& globalObject, TeeState& state)
{
    if (RefPtr byobReader = state.takeBYOBReader()) {
        ASSERT(!byobReader->readIntoRequestsSize());
        byobReader->releaseLock(globalObject);

        auto readerOrException = ReadableStreamDefaultReader::create(globalObject, Ref { state.stream() }.get());
        if (readerOrException.hasException()) {
            ASSERT_NOT_REACHED();
            return;
        }
        Ref reader = readerOrException.releaseReturnValue();
        state.setReader(reader.get());
        forwardReadError(state, reader.get());
    }

    RefPtr reader = state.defaultReader();

    auto [promise, deferred] = createPromiseAndWrapper(globalObject);
    reader->read(globalObject, WTFMove(deferred));
    promise->whenSettled([state = Ref {state }, weakReader = WeakPtr { *reader }] {
        RefPtr readPromise = state->readPromise();
        RefPtr reader = weakReader.get();
        if (!readPromise || !reader)
            return;

        switch (readPromise->status()) {
        case DOMPromise::Status::Fulfilled: {
            auto& globalObject = *readPromise->globalObject();

            Ref vm = globalObject.vm();
            auto scope = DECLARE_THROW_SCOPE(vm);
            auto resultOrException = convertDictionary<ReadableStreamReadResult>(globalObject, readPromise->result());
            ASSERT(!resultOrException.hasException(scope));
            if (resultOrException.hasException(scope))
                return;
            auto result = resultOrException.releaseReturnValue();
            if (!result.done) {
                // chunk steps.
                state->setReadAgainForBranch1(false);
                state->setReadAgainForBranch2(false);

                auto chunkResult = convert<IDLArrayBufferView>(globalObject, result.value);
                if (chunkResult.hasException(scope)) [[unlikely]]
                    return;

                Ref chunk1 = chunkResult.releaseReturnValue();
                Ref chunk2 = chunk1;

                if (!state->canceled1() && !state->canceled2()) {
                    auto resultOrException = cloneAsUInt8Array(chunk1);
                    if (resultOrException.hasException()) {
                        if (RefPtr branch1 = state->branch1())
                            branch1->controller()->error(globalObject, resultOrException.exception());
                        if (RefPtr branch2 = state->branch2())
                            branch2->controller()->error(globalObject, resultOrException.exception());

                        state->stream().cancel(resultOrException.releaseException());
                        return;
                    }
                    chunk2 = resultOrException.releaseReturnValue();
                }
                if (!state->canceled1()) {
                    if (RefPtr branch1 = state->branch1())
                        branch1->protectedController()->enqueue(globalObject, chunk1);
                }
                if (!state->canceled2()) {
                    if (RefPtr branch2 = state->branch2())
                        branch2->protectedController()->enqueue(globalObject, chunk2);
                }

                state->setReading(false);
                if (state->readAgainForBranch1())
                    pull1Steps(state, globalObject);
                else if (state->readAgainForBranch2())
                    pull2Steps(state, globalObject);
                return;
            }
            //close steps.
            state->setReading(false);
            if (!state->canceled1()) {
                if (RefPtr branch1 = state->branch1())
                    branch1->controller()->close();
            }
            if (!state->canceled2()) {
                if (RefPtr branch2 = state->branch2())
                    branch2->controller()->close();
            }
            if (RefPtr branch1 = state->branch1(); branch1->protectedController()->hasPendingPullIntos())
                branch1->protectedController()->respond(globalObject, 0);
            if (RefPtr branch2 = state->branch2(); branch2->protectedController()->hasPendingPullIntos())
                branch2->protectedController()->respond(globalObject, 0);
            return;
        }
        case DOMPromise::Status::Rejected:
            // error steps.
            state->setReading(false);
            return;
        case DOMPromise::Status::Pending:
            ASSERT_NOT_REACHED();
            break;
        }
    });
    state.setReadPromise(WTFMove(promise));
}

static void pullWithBYOBReader(JSDOMGlobalObject& globalObject, TeeState& state, ReadableStreamBYOBRequest& request, bool forBranch2)
{
    if (RefPtr defaultReader = state.takeDefaultReader()) {
        ASSERT(!defaultReader->getNumReadRequests());
        defaultReader->releaseLock(globalObject);
        
        auto readerOrException = ReadableStreamBYOBReader::create(globalObject, Ref { state.stream() }.get());
        if (readerOrException.hasException()) {
            ASSERT_NOT_REACHED();
            return;
        }
        Ref reader = readerOrException.releaseReturnValue();
        state.setReader(reader.get());
        forwardReadError(state, reader.get());
    }
    
    RefPtr reader = state.byobReader();
    RefPtr byobBranch = forBranch2 ? state.branch2() : state.branch1();
    RefPtr otherBranch = forBranch2 ? state.branch1() : state.branch2();
    
    auto [promise, deferred] = createPromiseAndWrapper(globalObject);

    reader->read(globalObject, *request.view(), 1, WTFMove(deferred));
    promise->whenSettled([state = Ref { state }, weakReader = WeakPtr { *reader }, forBranch2] {
        RefPtr readPromise = state->readPromise();
        RefPtr reader = weakReader.get();
        if (!readPromise || !reader)
            return;

        switch (readPromise->status()) {
        case DOMPromise::Status::Fulfilled: {
            auto& globalObject = *readPromise->globalObject();
            auto resultOrException = convertDictionary<ReadableStreamReadResult>(globalObject, readPromise->result());

            Ref vm = globalObject.vm();
            auto scope = DECLARE_THROW_SCOPE(vm);
            ASSERT(!resultOrException.hasException(scope));
            if (resultOrException.hasException(scope))
                return;

            auto result = resultOrException.releaseReturnValue();
            if (!result.done) {
                // chunk steps.
                auto chunkResult = convert<IDLArrayBufferView>(globalObject, result.value);
                if (chunkResult.hasException(scope)) [[unlikely]]
                    return;

                Ref chunk = chunkResult.releaseReturnValue();
                
                state->setReadAgainForBranch1(false);
                state->setReadAgainForBranch2(false);

                bool byobCanceled = forBranch2 ? state->canceled2() : state->canceled1();
                bool otherCanceled = forBranch2 ? state->canceled1() : state->canceled2();

                RefPtr byobBranch = forBranch2 ? state->branch2() : state->branch1();
                RefPtr otherBranch = forBranch2 ? state->branch1() : state->branch2();

                if (!otherCanceled) {
                    auto resultOrException = cloneAsUInt8Array(chunk);
                    if (resultOrException.hasException()) {
                        if (byobBranch)
                            byobBranch->controller()->error(globalObject, resultOrException.exception());
                        if (otherBranch)
                            otherBranch->controller()->error(globalObject, resultOrException.exception());

                        state->stream().cancel(resultOrException.releaseException());
                        return;
                    }
                    Ref clonedChunk = resultOrException.releaseReturnValue();
                    if (!byobCanceled)
                        byobBranch->protectedController()->respondWithNewView(globalObject, chunk);
                    otherBranch->protectedController()->respondWithNewView(globalObject, clonedChunk);
                } else if (!byobCanceled)
                    byobBranch->protectedController()->respondWithNewView(globalObject, chunk);

                state->setReading(false);
                if (state->readAgainForBranch1())
                    pull1Steps(state, globalObject);
                else if (state->readAgainForBranch2())
                    pull2Steps(state, globalObject);
                return;
            }

            //close steps.
            state->setReading(false);
            bool byobCanceled = forBranch2 ? state->canceled2() : state->canceled1();
            bool otherCanceled = forBranch2 ? state->canceled1() : state->canceled2();
            if (!byobCanceled) {
                if (RefPtr branch1 = state->branch1())
                    branch1->controller()->close();
            }
            if (!otherCanceled) {
                if (RefPtr branch2 = state->branch2())
                    branch2->controller()->close();
            }
            if (RefPtr branch1 = state->branch1(); branch1->controller()->hasPendingPullIntos())
                branch1->protectedController()->respond(globalObject, 0);
            if (RefPtr branch2 = state->branch2(); branch2->controller()->hasPendingPullIntos())
                branch2->protectedController()->respond(globalObject, 0);

            if (result.value) {
                auto chunkResult = convert<IDLArrayBufferView>(globalObject, result.value);
                if (chunkResult.hasException(scope)) [[unlikely]]
                    return;

                Ref chunk = chunkResult.releaseReturnValue();
                ASSERT(!chunk->byteLength());
                if (RefPtr branch1 = state->branch1())
                    branch1->protectedController()->respondWithNewView(globalObject, chunk);
                if (RefPtr branch2 = state->branch1())
                    branch2->protectedController()->respondWithNewView(globalObject, chunk);

            }
            if (!byobCanceled || !otherCanceled)
                state->resolveCancelPromise();
            return;
        }
        case DOMPromise::Status::Rejected:
            // error steps.
            state->setReading(false);
            return;
        case DOMPromise::Status::Pending:
            ASSERT_NOT_REACHED();
            break;
        }
    });
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamtee
ExceptionOr<Vector<Ref<ReadableStream>>> ReadableStream::byteStreamTee(JSDOMGlobalObject& globalObject)
{
    ASSERT(!!m_controller);

    auto readerOrException = ReadableStreamDefaultReader::create(globalObject, *this);
    if (readerOrException.hasException())
        return readerOrException.releaseException();

    Ref reader = readerOrException.releaseReturnValue();
    Ref state = TeeState::create(globalObject, *this, reader.get());

    ReadableByteStreamController::PullAlgorithm pull1Algorithm = [state = Ref { state }](auto& globalObject, auto&&) {
        return pull1Steps(state, globalObject);
    };

    ReadableByteStreamController::PullAlgorithm pull2Algorithm = [state = Ref { state }](auto& globalObject, auto&&) {
        return pull2Steps(state, globalObject);
    };

    ReadableByteStreamController::CancelAlgorithm cancel1Algorithm = [state = Ref { state }](auto& globalObject, auto&&, auto&& reason) {
        state->setCanceled1();
        state->setReason1(globalObject, reason.value_or(JSC::jsUndefined()));

        if (state->canceled2()) {
            // Create the array of reason1 and reason2.
            JSC::MarkedArgumentBuffer list;
            list.ensureCapacity(2);
            list.append(state->reason1());
            list.append(state->reason2());
            JSC::JSValue reason = JSC::constructArray(&globalObject, static_cast<JSC::ArrayAllocationProfile*>(nullptr), list);

            auto [promise, deferred] = createPromiseAndWrapper(globalObject);
            state->stream().cancel(globalObject, reason, WTFMove(deferred));
            promise->whenSettled([state] {
                state->resolveCancelPromise();
            });
        }
        return Ref { state->cancelPromise() };
    };

    ReadableByteStreamController::CancelAlgorithm cancel2Algorithm = [state = Ref { state }](auto& globalObject, auto&&, auto&& reason) {
        state->setCanceled2();
        state->setReason2(globalObject, reason.value_or(JSC::jsUndefined()));

        if (state->canceled1()) {
            // Create the array of reason1 and reason2.
            JSC::MarkedArgumentBuffer list;
            list.ensureCapacity(2);
            list.append(state->reason1());
            list.append(state->reason2());
            JSC::JSValue reason = JSC::constructArray(&globalObject, static_cast<JSC::ArrayAllocationProfile*>(nullptr), list);

            auto [promise, deferred] = createPromiseAndWrapper(globalObject);
            state->stream().cancel(globalObject, reason, WTFMove(deferred));
            promise->whenSettled([state] {
                state->resolveCancelPromise();
            });
        }
        return Ref { state->cancelPromise() };
    };

    Vector<Ref<ReadableStream>> branches;
    branches.append(createReadableByteStream(WTFMove(pull1Algorithm), WTFMove(cancel1Algorithm)));
    branches.append(createReadableByteStream(WTFMove(pull2Algorithm), WTFMove(cancel2Algorithm)));

    state->setBranch1(branches[0].get());
    state->setBranch2(branches[1].get());

    forwardReadError(state, reader.get());

    return branches;
}

// https://streams.spec.whatwg.org/#readable-stream-fulfill-read-request
void ReadableStream::fulfillReadRequest(JSDOMGlobalObject& globalObject, RefPtr<JSC::ArrayBufferView>&& filledView, bool done)
{
    RefPtr defaultReader = this->defaultReader();
    ASSERT(defaultReader);
    ASSERT(defaultReader->getNumReadRequests());

    auto chunk = toJS<IDLNullable<IDLArrayBufferView>>(globalObject, globalObject, WTFMove(filledView));

    defaultReader->takeFirstReadRequest()->resolve<IDLDictionary<ReadableStreamReadResult>>({ chunk, done });
}

void ReadableStream::setByobReader(ReadableStreamBYOBReader* reader)
{
    ASSERT(!m_byobReader || !reader);
    ASSERT(!m_defaultReader);
    m_byobReader = WeakPtr { reader };
}

ReadableStreamBYOBReader* ReadableStream::byobReader()
{
    return m_byobReader.get();
}

// https://streams.spec.whatwg.org/#readable-stream-fulfill-read-into-request
void ReadableStream::fulfillReadIntoRequest(JSDOMGlobalObject& globalObject, RefPtr<JSC::ArrayBufferView>&& filledView, bool done)
{
    RefPtr byobReader = this->byobReader();
    ASSERT(byobReader);
    ASSERT(byobReader->readIntoRequestsSize());

    auto chunk = toJS<IDLNullable<IDLArrayBufferView>>(globalObject, globalObject, WTFMove(filledView));

    byobReader->takeFirstReadIntoRequest()->resolve<IDLDictionary<ReadableStreamReadResult>>({ chunk, done });
}

ExceptionOr<void> ReadableStream::setupReadableByteStreamControllerFromUnderlyingSource(JSDOMGlobalObject& globalObject, JSC::JSValue underlyingSource, UnderlyingSource&& underlyingSourceDict, double highWaterMark)
{
    // handle start, pull, cancel algorithms.
    if (underlyingSourceDict.autoAllocateChunkSize && !*underlyingSourceDict.autoAllocateChunkSize)
        return Exception { ExceptionCode::TypeError, "autoAllocateChunkSize is zero"_s };

    // https://streams.spec.whatwg.org/#set-up-readable-byte-stream-controller
    m_controller = std::unique_ptr<ReadableByteStreamController>(new ReadableByteStreamController(*this, underlyingSource, WTFMove(underlyingSourceDict.pull), WTFMove(underlyingSourceDict.cancel), highWaterMark, underlyingSourceDict.autoAllocateChunkSize.value_or(0)));

    return m_controller->start(globalObject, underlyingSourceDict.start.get());
}

void ReadableStream::setupReadableByteStreamController(ReadableByteStreamController::PullAlgorithm&& pullAlgorithm, ReadableByteStreamController::CancelAlgorithm&& cancelAlgorithm, double highWaterMark)
{
    m_controller = std::unique_ptr<ReadableByteStreamController>(new ReadableByteStreamController(*this, WTFMove(pullAlgorithm), WTFMove(cancelAlgorithm), highWaterMark, 0));
}

// https://streams.spec.whatwg.org/#readable-stream-close
void ReadableStream::close()
{
    ASSERT(m_state == ReadableStream::State::Readable);
    m_state = ReadableStream::State::Closed;
    
    if (RefPtr defaultReader = m_defaultReader.get()) {
        defaultReader->resolveClosedPromise();
        return;
    } else if (RefPtr byobReader = m_byobReader.get())
        byobReader->resolveClosedPromise();
}

// https://streams.spec.whatwg.org/#readable-stream-error
void ReadableStream::error(JSDOMGlobalObject& globalObject, JSC::JSValue reason)
{
    ASSERT(m_state == ReadableStream::State::Readable);
    m_state = ReadableStream::State::Errored;

    m_controller->storeError(globalObject, reason);

    if (RefPtr defaultReader = m_defaultReader.get()) {
        defaultReader->rejectClosedPromise(reason);
        defaultReader->errorReadRequests(reason);
        return;
    }

    RefPtr byobReader = m_byobReader.get();
    if (!byobReader)
        return;

    byobReader->rejectClosedPromise(reason);
    byobReader->errorReadIntoRequests(reason);
}

// https://streams.spec.whatwg.org/#readable-stream-cancel
void ReadableStream::cancel(JSDOMGlobalObject& globalObject, JSC::JSValue reason, Ref<DeferredPromise>&& promise)
{
    ASSERT(!m_internalReadableStream);
    
    m_disturbed = true;
    if (m_state == State::Closed) {
        promise->resolve();
        return;
    }
    
    if (m_state == State::Errored) {
        promise->rejectWithCallback([&] (auto&) {
            return m_controller->storedError();
        });
        return;
    }
    
    close();
    
    RefPtr byobReader = m_byobReader.get();
    if (byobReader) {
        // FIXME: Check whether using an empty view.
        while (byobReader->readIntoRequestsSize())
            byobReader->takeFirstReadIntoRequest()->resolve<IDLDictionary<ReadableStreamReadResult>>({ JSC::jsUndefined(), true });
    }

    m_controller->runCancelSteps(globalObject, reason, [promise = WTFMove(promise)] (auto&& error) mutable {
        if (error) {
            promise->rejectWithCallback([&] (auto&) {
                return *error;
            });
            return;
        }
        promise->resolve();
    });
}

// https://streams.spec.whatwg.org/#readable-stream-get-num-read-into-requests
size_t ReadableStream::getNumReadIntoRequests() const
{
    ASSERT(m_byobReader);
    RefPtr byobReader = m_byobReader.get();
    return byobReader->readIntoRequestsSize();
}

// https://streams.spec.whatwg.org/#readable-stream-get-num-read-requests
size_t ReadableStream::getNumReadRequests() const
{
    ASSERT(m_defaultReader);
    RefPtr defaultReader = m_defaultReader.get();
    return defaultReader->getNumReadRequests();
}

// https://streams.spec.whatwg.org/#readable-stream-add-read-into-request
void ReadableStream::addReadIntoRequest(Ref<DeferredPromise>&& promise)
{
    ASSERT(m_byobReader);
    RefPtr byobReader = m_byobReader.get();
    return byobReader->addReadIntoRequest(WTFMove(promise));
}

// https://streams.spec.whatwg.org/#readable-stream-add-read-request
void ReadableStream::addReadRequest(Ref<DeferredPromise>&& promise)
{
    ASSERT(m_defaultReader);
    RefPtr defaultReader = m_defaultReader.get();
    return defaultReader->addReadRequest(WTFMove(promise));
}

// https://streams.spec.whatwg.org/#readable-stream-pipe-to
static void pipeToInternal(JSDOMGlobalObject& globalObject, ReadableStream& source, WritableStream& destination, StreamPipeOptions&& options, RefPtr<DeferredPromise>&& promise)
{
    auto readerOrException = ReadableStreamDefaultReader::create(globalObject, source);
    if (readerOrException.hasException())
        return;
    
    auto writerOrException = acquireWritableStreamDefaultWriter(globalObject, destination);
    if (writerOrException.hasException())
        return;
    
    source.setAsDisturbed();

    PipeToState::readableStreamPipeTo(globalObject, source, destination, readerOrException.releaseReturnValue(), writerOrException.releaseReturnValue(), WTFMove(options), WTFMove(promise));
}

void ReadableStream::pipeTo(JSDOMGlobalObject& globalObject, WritableStream& destination, StreamPipeOptions&& options, Ref<DeferredPromise>&& promise)
{
    if (isLocked()) {
        promise->reject(Exception { ExceptionCode::TypeError, "stream is locked"_s }, RejectAsHandled::Yes);
        return;
    }

    if (destination.locked()) {
        promise->reject(Exception { ExceptionCode::TypeError, "destination is locked"_s }, RejectAsHandled::Yes);
        return;
    }
    
    pipeToInternal(globalObject, *this, destination, WTFMove(options), WTFMove(promise));
}

ExceptionOr<Ref<ReadableStream>> ReadableStream::pipeThrough(JSDOMGlobalObject& globalObject, WritablePair&& transform, StreamPipeOptions&& options)
{
    if (isLocked())
        return Exception { ExceptionCode::TypeError, "stream is locked"_s };
    
    if (transform.writable->locked())
        return Exception { ExceptionCode::TypeError, "transform writable is locked"_s };
    
    pipeToInternal(globalObject, *this, *transform.writable, WTFMove(options), nullptr);
    
    return Ref { *transform.readable };
}

JSC::JSValue ReadableStream::storedError(JSDOMGlobalObject& globalObject) const
{
    if (RefPtr internalReadableStream = m_internalReadableStream)
        return internalReadableStream->storedError(globalObject);

    return m_controller->storedError();
}

JSC::JSValue JSReadableStream::cancel(JSC::JSGlobalObject& globalObject, JSC::CallFrame& callFrame)
{
    RefPtr internalReadableStream = wrapped().internalReadableStream();
    if (!internalReadableStream) {
        Ref vm = globalObject.vm();
        auto& jsDOMGlobalObject = *JSC::jsCast<JSDOMGlobalObject*>(&globalObject);

        auto* promise = JSC::JSPromise::create(vm.get(), globalObject.promiseStructure());

        Ref { wrapped() }->cancel(jsDOMGlobalObject, callFrame.argument(0), DeferredPromise::create(jsDOMGlobalObject, *promise));
        return promise;
    }
    return internalReadableStream->cancelForBindings(globalObject, callFrame.argument(0));
}

JSC::JSValue JSReadableStream::pipeTo(JSC::JSGlobalObject& globalObject, JSC::CallFrame& callFrame)
{
    RefPtr internalReadableStream = wrapped().internalReadableStream();
//    if (!internalReadableStream) {
    {
        auto& jsDOMGlobalObject = *JSC::jsCast<JSDOMGlobalObject*>(&globalObject);

        Ref vm = globalObject.vm();
        auto throwScope = DECLARE_THROW_SCOPE(vm);

        auto* promise = JSC::JSPromise::create(vm.get(), globalObject.promiseStructure());
        Ref domPromise = DeferredPromise::create(*JSC::jsCast<JSDOMGlobalObject*>(&globalObject), *promise);

        if (callFrame.argumentCount() < 1) {
            domPromise->rejectWithCallback([](auto& globalObject) {
                return createNotEnoughArgumentsError(&globalObject);
            });
            return promise;
        }

        JSC::EnsureStillAliveScope argument0 = callFrame.uncheckedArgument(0);
        auto destinationConversionResult = convert<IDLInterface<WritableStream>>(globalObject, argument0.value(), [](JSC::JSGlobalObject& lexicalGlobalObject, JSC::ThrowScope& scope) { throwArgumentTypeError(lexicalGlobalObject, scope, 0, "destination"_s, "ReadableStream"_s, "pipeTo"_s, "WritableStream"_s); });
        if (destinationConversionResult.hasException(throwScope)) [[unlikely]] {
            domPromise->reject(Exception { ExceptionCode::ExistingExceptionError });
            return promise;
        }

        JSC::EnsureStillAliveScope argument1 = callFrame.argument(1);
        auto optionsConversionResult = convert<IDLDictionary<StreamPipeOptions>>(globalObject, argument1.value());
        if (optionsConversionResult.hasException(throwScope)) [[unlikely]] {
            domPromise->reject(Exception { ExceptionCode::ExistingExceptionError });
            return promise;
        }

        Ref { wrapped() }->pipeTo(jsDOMGlobalObject, *destinationConversionResult.releaseReturnValue(), optionsConversionResult.releaseReturnValue(), WTFMove(domPromise));
        return promise;
    }

//    return internalReadableStream->pipeTo(globalObject, callFrame.argument(0), callFrame.argument(1));
}

JSC::JSValue JSReadableStream::pipeThrough(JSC::JSGlobalObject& globalObject, JSC::CallFrame& callFrame)
{
    RefPtr internalReadableStream = wrapped().internalReadableStream();
    if (!internalReadableStream) {
        auto& jsDOMGlobalObject = *JSC::jsCast<JSDOMGlobalObject*>(&globalObject);

        Ref vm = globalObject.vm();
        auto throwScope = DECLARE_THROW_SCOPE(vm);
        if (callFrame.argumentCount() < 1)
            return throwException(&globalObject, throwScope, createNotEnoughArgumentsError(&globalObject));

        JSC::EnsureStillAliveScope argument0 = callFrame.uncheckedArgument(0);
        auto transformConversionResult = convert<IDLDictionary<ReadableStream::WritablePair>>(globalObject, argument0.value());
        if (transformConversionResult.hasException(throwScope)) [[unlikely]]
            return { };

        JSC::EnsureStillAliveScope argument1 = callFrame.argument(1);
        auto optionsConversionResult = convert<IDLDictionary<StreamPipeOptions>>(globalObject, argument1.value());
        if (optionsConversionResult.hasException(throwScope)) [[unlikely]]
            return { };
        
        auto readableStreamOrException = Ref { wrapped() }->pipeThrough(jsDOMGlobalObject, transformConversionResult.releaseReturnValue(), optionsConversionResult.releaseReturnValue());

        if (readableStreamOrException.hasException()) {
            throwException(&globalObject, throwScope, createDOMException(globalObject, readableStreamOrException.releaseException()));
            return { };
        }

        return toJS(&globalObject, &jsDOMGlobalObject, readableStreamOrException.releaseReturnValue());
    }

    return internalReadableStream->pipeThrough(globalObject, callFrame.argument(0), callFrame.argument(1));
}

} // namespace WebCore
