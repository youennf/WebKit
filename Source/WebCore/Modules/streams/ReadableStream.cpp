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

#include "JSDOMPromise.h"
#include "JSDOMPromiseDeferred.h"
#include "JSReadableStream.h"
#include "JSReadableStreamBYOBReader.h"
#include "JSReadableStreamDefaultReader.h"
#include "JSReadableStreamReadResult.h"
#include "JSReadableStreamSource.h"
#include "JSUnderlyingSource.h"
#include "QueuingStrategy.h"
#include "ReadableByteStreamController.h"
#include "ReadableStreamBYOBReader.h"
#include "ScriptExecutionContext.h"

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
    void setReadAgainForBranch1() { m_readAgainForBranch1 = true; }

    bool canceled1() const { return m_canceled1;}
    bool canceled2() const { return m_canceled2;}
    void setCanceled1() { m_canceled1 = true; }
    void setCanceled2() { m_canceled2 = true; }

    ReadableStream& stream() const { return m_stream; }
    ReadableStream* branch1() const { return m_branch1.get(); }
    ReadableStream* branch2() const { return m_branch2.get(); }

    DOMPromise* readPromise() const {return m_readPromise.get(); }
    void setReadPromise(Ref<DOMPromise>&& promise)
    {
        ASSERT(!m_readPromise);
        m_readPromise = WTFMove(promise);
    }

    RefPtr<ReadableStreamBYOBReader> takeBYOBReader() { return std::exchange(m_byobReader, { }); }

    ReadableStreamDefaultReader* defaultReader() const { return m_defaultReader.get(); }
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

    RefPtr<DOMPromise> m_readPromise;
};
/*
template<typename Reader>
void forwardReadError(TeeState& state, Reader& reader)
{
    reader.onClosedPromiseRejection([state = Ref { state }, reader = &thisReader](auto& globalObject, auto&& reason) {
        if (!state->isReader(thisReader))
            return;
        if (RefPtr branch1 = state->branch1())
            branch1->controller()->storeError(globalObject, reason);
        if (RefPtr branch2 = state->branch2())
            branch2->controller()->storeError(globalObject, reason);
    });
}
*/
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
        state.setReader(readerOrException.releaseReturnValue());
    }
    RefPtr reader = state.defaultReader();
    RefPtr promise = DeferredPromise::create(globalObject);
    reader->read(globalObject, *promise);
    promise->whenSettled([state = Ref {state }, weakReader = WeakPtr { *reader }] {
        RefPtr readPromise = state->readPromise();
        RefPtr reader = weakReader.get();
        if (!readPromise || !reader)
            return;

        switch (readPromise->status()) {
        case DOMPromise::Status::Fulfilled:
            // close steps or chunk steps.
            // Convert result.
            // If done is not false, apply read requests.
            // If done is false, apply close steps.
            //callback(*globalObject, { });
            break;
        case DOMPromise::Status::Rejected:
            // error steps.
            state->setReading(false);
            break;
        case DOMPromise::Status::Pending:
            ASSERT_NOT_REACHED();
            break;
        }
    });
    state.setReadPromise(domPromiseFromDeferred(globalObject, *promise).releaseNonNull());
}

// https://streams.spec.whatwg.org/#abstract-opdef-readablebytestreamtee
ExceptionOr<Vector<Ref<ReadableStream>>> ReadableStream::byteStreamTee(JSDOMGlobalObject& globalObject)
{
    ASSERT(!!m_controller);

    auto readerOrException = ReadableStreamDefaultReader::create(globalObject, *this);
    if (readerOrException.hasException())
        return readerOrException.releaseException();

    Ref state = TeeState::create(globalObject, *this, readerOrException.releaseReturnValue());

/*
    RefPtr pull1Algorithm;
    RefPtr pull2Algorithm;
    RefPtr cancel1Algorithm;
    RefPtr cancel2Algorithm;
*/
    auto forwardReadError = [state](auto& thisReader) {
        thisReader.onClosedPromiseRejection([state, &thisReader](auto& globalObject, auto&& reason) {
            if (state->defaultReader() != &thisReader)
                return;
            if (RefPtr branch1 = state->branch1())
                branch1->controller()->error(globalObject, reason);
            if (RefPtr branch2 = state->branch2())
                branch2->controller()->error(globalObject, reason);
        });
    };

    ReadableByteStreamController::PullAlgorithm pull1Algorithm = [state = Ref { state }](auto& globalObject, auto&& controller) {
        if (state->reading()) {
            state->setReadAgainForBranch1();
            // FIXME: We can do better;
            RefPtr promise = DeferredPromise::create(globalObject);
            promise->resolve();
            return domPromiseFromDeferred(globalObject, *promise).releaseNonNull();
        }
        state->setReading(true);

        RefPtr byobRequest = controller.getByobRequest();
        if (!byobRequest)
            pullWithDefaultReader(globalObject, state);
        //else
          //  pullWithBYOBReader(*biobyRequest, false);

        // FIXME: We can do better;
        RefPtr promise = DeferredPromise::create(globalObject);
        promise->resolve();
        return domPromiseFromDeferred(globalObject, *promise).releaseNonNull();
    };

    ReadableByteStreamController::CancelAlgorithm cancel1Algorithm = [state = Ref { state }](auto& globalObject, auto&&, auto&&) {
        state->setCanceled1();
        // set reason1;
        if (state->canceled2()) {
            // Create the array of reason1 and reason2.
            //JSC::VM& vm = JSC::getVM(&globalObject);
            //auto scope = DECLARE_THROW_SCOPE(vm);
            JSC::MarkedArgumentBuffer list;
            list.ensureCapacity(2);
            // FIXME
//            list.append(state->reason1());
  //          list.append(state->reason2());

            RefPtr promise = DeferredPromise::create(globalObject);
            state->stream().cancel(globalObject, JSC::constructArray(&globalObject, static_cast<JSC::ArrayAllocationProfile*>(nullptr), list), *promise);
            // Chain cancel returned promise to call resolveCancelPromise
            promise->whenSettled([state] {
                state->resolveCancelPromise();
            });
        }
        return Ref { state->cancelPromise() };
    };
    Vector<Ref<ReadableStream>> branches;
    branches.append(createReadableByteStream(WTFMove(pull1Algorithm), WTFMove(cancel1Algorithm)));
//    branches.append(createReadableByteStream());
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
    }

    if (RefPtr byobReader = m_byobReader.get())
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

JSC::JSValue ReadableStream::storedError() const
{
    ASSERT(m_controller);
    return m_controller ? m_controller->storedError() : JSC::jsUndefined();
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
    if (!internalReadableStream) {
        // TODO
        return { };
    }
    return internalReadableStream->pipeTo(globalObject, callFrame.argument(0), callFrame.argument(1));
}

JSC::JSValue JSReadableStream::pipeThrough(JSC::JSGlobalObject& globalObject, JSC::CallFrame& callFrame)
{
    RefPtr internalReadableStream = wrapped().internalReadableStream();
    if (!internalReadableStream) {
        // TODO
        return { };
    }
    return internalReadableStream->pipeThrough(globalObject, callFrame.argument(0), callFrame.argument(1));
}

} // namespace WebCore
