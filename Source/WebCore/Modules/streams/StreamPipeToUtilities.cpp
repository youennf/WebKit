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

#include "config.h"
#include "StreamPipeToUtilities.h"

#include "InternalWritableStream.h"
#include "InternalWritableStreamWriter.h"
#include "JSDOMPromise.h"
#include "WritableStream.h"

namespace WebCore {

// https://streams.spec.whatwg.org/#readable-stream-pipe-to
void PipeToState::readableStreamPipeTo(JSDOMGlobalObject& globalObject, Ref<ReadableStream>&& source, Ref<WritableStream>&& destination, Ref<ReadableStreamDefaultReader>&& reader, Ref<InternalWritableStreamWriter>&& writer, StreamPipeOptions&& options, RefPtr<DeferredPromise>&& promise)
{
    Ref pipeState = create(WTFMove(source), WTFMove(destination), WTFMove(reader), WTFMove(writer), WTFMove(options), WTFMove(promise));
    // FIXME: Handle signal.

    pipeState->handleSignal();

    pipeState->errorsMustBePropagatedForward(globalObject);
    pipeState->errorsMustBePropagatedBackward();
    pipeState->closingMustBePropagatedForward();
    pipeState->closingMustBePropagatedBackward();

    pipeState->loop();
}

PipeToState::PipeToState(Ref<ReadableStream>&& source, Ref<WritableStream>&& destination, Ref<ReadableStreamDefaultReader>&& reader, Ref<InternalWritableStreamWriter>&& writer, StreamPipeOptions&& options, RefPtr<DeferredPromise>&& promise)
    : m_source(WTFMove(source))
    , m_destination(WTFMove(destination))
    , m_reader(WTFMove(reader))
    , m_writer(WTFMove(writer))
    , m_options(WTFMove(options))
    , m_promise(WTFMove(promise))
{
    fprintf(stderr, "PipeToState::PipeToState %p\n", this);
}

PipeToState::~PipeToState()
{
    fprintf(stderr, "PipeToState::~PipeToState %p\n", this);
}

void PipeToState::handleSignal()
{
    if (!m_options.signal)
        return;
    fprintf(stderr, "PipeToState::handleSignal1 %p\n", this);

    auto algorithmSteps = [weakThis = WeakPtr { *this }, signal = m_options.signal]() mutable {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis)
            return;

        fprintf(stderr, "PipeToState::handleSignal1  within algo %p\n", protectedThis.get());

        protectedThis->shutdownWithAction([protectedThis, signal] -> RefPtr<DOMPromise> {
            fprintf(stderr, "PipeToState::handleSignal2\n");
            RefPtr promiseDestination = [&] -> RefPtr<DOMPromise> {
                fprintf(stderr, "PipeToState::handleSignal1  abort writable ?\n");
                bool shouldAbortDestination = !protectedThis->m_options.preventAbort && protectedThis->m_destination->state() == WritableStream::State::Writable;
                if (!shouldAbortDestination)
                    return nullptr;
                fprintf(stderr, "PipeToState::handleSignal1  abort writable y\n");

                Ref internalWritableStream = protectedThis->m_destination->internalWritableStream();
                auto* globalObject = internalWritableStream->globalObject();
                if (!globalObject)
                    return nullptr;
                
                fprintf(stderr, "PipeToState::handleSignal1  abort writable\n");
                auto value = internalWritableStream->abort(*globalObject, signal->reason().getValue());
                auto* promise = jsCast<JSC::JSPromise*>(value);
                if (!promise)
                    return nullptr;
                
                fprintf(stderr, "PipeToState::handleSignal1  abort writabl with promisee\n");
                return DOMPromise::create(*globalObject, *promise);
            }();
            
            RefPtr promiseSource = [&] -> RefPtr<DOMPromise> {
                bool shouldAbortSource = !protectedThis->m_options.preventCancel && protectedThis->m_source->state() == ReadableStream::State::Readable;
                fprintf(stderr, "PipeToState::handleSignal1  cancel readable ? \n");
                if (!shouldAbortSource)
                    return nullptr;
                
                fprintf(stderr, "PipeToState::handleSignal1  cancel readable y\n");
                // FIXME: We should use ReadableStream::cancel()
                RefPtr internalReadableStream = protectedThis->m_source->internalReadableStream();
                if (!internalReadableStream)
                    return nullptr;

                auto* globalObject = internalReadableStream->globalObject();
                if (!globalObject)
                    return nullptr;
                
                fprintf(stderr, "PipeToState::handleSignal1  cancel readable\n");
                auto value = internalReadableStream->cancel(*globalObject, signal->reason().getValue(), InternalReadableStream::Use::Private);
                auto* promise = jsCast<JSC::JSPromise*>(value);
                if (!promise)
                    return nullptr;
                
                fprintf(stderr, "PipeToState::handleSignal1  cancel readable with promise\n");
                return DOMPromise::create(*globalObject, *promise);
            }();

            fprintf(stderr, "PipeToState::handleSignal4\n");
            if (!promiseSource && !promiseDestination) {
                fprintf(stderr, "PipeToState::handleSignal5\n");
                return nullptr;
            }
            
            fprintf(stderr, "PipeToState::handleSignal3\n");
            auto* globalObject = promiseSource ? promiseSource->globalObject() : promiseDestination->globalObject();
            if (!globalObject) {
                fprintf(stderr, "PipeToState::handleSignal3.5\n");
                return nullptr;
            }

            auto [result, deferred] = createPromiseAndWrapper(*globalObject);
            if (promiseDestination) {
                promiseDestination->whenSettled([promiseDestination, promiseSource, deferred] {
                    fprintf(stderr, "PipeToState::handleSignal6\n");
                    if (promiseDestination->status() == DOMPromise::Status::Rejected) {
                        fprintf(stderr, "PipeToState::handleSignal6.1\n");
                        deferred->rejectWithCallback([&](auto&) {
                            return promiseDestination->result();
                        }, RejectAsHandled::Yes);
                        return;
                    }
                    fprintf(stderr, "PipeToState::handleSignal6.5\n");
                    if (promiseSource && promiseSource->status() != DOMPromise::Status::Fulfilled)
                        return;
                    fprintf(stderr, "PipeToState::handleSignal6.6\n");
                    deferred->resolve();
                });
            }
            if (promiseSource) {
                promiseSource->whenSettled([promiseDestination, promiseSource, deferred] {
                    fprintf(stderr, "PipeToState::handleSignal7\n");
                    if (promiseSource->status() == DOMPromise::Status::Rejected) {
                        fprintf(stderr, "PipeToState::handleSignal7.1\n");
                        deferred->rejectWithCallback([&](auto&) {
                            return promiseSource->result();
                        }, RejectAsHandled::Yes);
                        return;
                    }
                    fprintf(stderr, "PipeToState::handleSignal7.5\n");
                    if (promiseDestination && promiseDestination->status() != DOMPromise::Status::Fulfilled)
                        return;
                    fprintf(stderr, "PipeToState::handleSignal7.6\n");
                    deferred->resolve();
                });
            }

            return RefPtr { WTFMove(result) };
        }, [signal](auto&) { return signal->reason().getValue(); });
    };

    if (m_options.signal->aborted()) {
        algorithmSteps();
        return;
    }

    m_options.signal->addAlgorithm([algorithmSteps = WTFMove(algorithmSteps)](auto&&) mutable {
        algorithmSteps();
    });
}

void PipeToState::loop()
{
    fprintf(stderr, "PipeToState::loop\n");
    if (m_shuttingDown)
        return;
    
    doReadWrite();
}

void PipeToState::doReadWrite()
{
    fprintf(stderr, "PipeToState::doReadWrite1\n");
    ASSERT(!m_shuttingDown);

    m_writer->whenReady([protectedThis = Ref { *this }]() mutable {
        fprintf(stderr, "PipeToState::doReadWrite2\n");
        if (protectedThis->m_shuttingDown)
            return;

        RefPtr internalDefaultReader = protectedThis->m_reader->internalDefaultReader();
        if (!internalDefaultReader)
            return;

        auto* globalObject = internalDefaultReader->globalObject();
        if (!globalObject)
            return;

        auto value = internalDefaultReader->readForBindings(*globalObject);
        auto* promise = jsCast<JSC::JSPromise*>(value);
        if (!promise)
            return;

        fprintf(stderr, "PipeToState::doReadWrite2.5\n");

        Ref domPromise = DOMPromise::create(*globalObject, *promise);
        protectedThis->m_pendingReadPromise = domPromise.ptr();
        protectedThis->m_pendingReadPromise->markAsHandled();
        domPromise->whenSettled([domPromise, protectedThis = WTFMove(protectedThis)] {
            if (domPromise->status() != DOMPromise::Status::Fulfilled)
                return;

            fprintf(stderr, "PipeToState::doReadWrite 4\n");
            auto* globalObject = domPromise->globalObject();
            if (!globalObject)
                return;
            fprintf(stderr, "PipeToState::doReadWrite 5\n");

            Ref vm = globalObject->vm();
            auto scope = DECLARE_THROW_SCOPE(vm);

            auto* chunkObject = domPromise->result().toObject(globalObject);
            if (vm->traps().maybeNeedHandling() && vm->hasExceptionsAfterHandlingTraps())
                return;

            fprintf(stderr, "PipeToState::doReadWrite 6\n");
            auto doneValue = chunkObject->get(globalObject, JSC::Identifier::fromString(vm, "done"_s));
            if (scope.exception()) [[unlikely]]
                return;

            fprintf(stderr, "PipeToState::doReadWrite 7\n");
            if (doneValue.toBoolean(globalObject))
                return;
            fprintf(stderr, "PipeToState::doReadWrite 8\n");

            protectedThis->m_pendingWritePromise = writableStreamDefaultWriterWrite(protectedThis->m_writer, chunkObject->get(globalObject, JSC::Identifier::fromString(vm, "value"_s)));
            protectedThis->m_pendingWritePromise->markAsHandled();
            protectedThis->doReadWrite();
        });
    });
}

void PipeToState::errorsMustBePropagatedForward(JSDOMGlobalObject& globalObject)
{
    fprintf(stderr, "PipeToState::errorsMustBePropagatedForward\n");

    auto propagateErrorSteps = [weakThis = WeakPtr { *this }](auto&& error) {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis)
            return;
        if (!protectedThis->m_options.preventAbort) {
            fprintf(stderr, "PipeToState::errorsMustBePropagatedForward3\n");
            protectedThis->shutdownWithAction([protectedThis, error] -> RefPtr<DOMPromise> {
                fprintf(stderr, "PipeToState::errorsMustBePropagatedForward5\n");
                Ref internalWritableStream = protectedThis->m_destination->internalWritableStream();
                auto* globalObject = internalWritableStream->globalObject();
                if (!globalObject)
                    return nullptr;
                auto value = internalWritableStream->abort(*globalObject, error.get());
                auto* promise = jsCast<JSC::JSPromise*>(value);
                if (!promise) {
                    fprintf(stderr, "PipeToState::errorsMustBePropagatedForward6\n");
                    auto [result, deferred] = createPromiseAndWrapper(*globalObject);
                    deferred->resolve();
                    return RefPtr { WTFMove(result) };
                }
                fprintf(stderr, "PipeToState::errorsMustBePropagatedForward7\n");
                return DOMPromise::create(*globalObject, *promise);
            }, [error](auto&) { return error.get(); });
            return;
        }
        fprintf(stderr, "PipeToState::errorsMustBePropagatedForward4\n");
        protectedThis->shutdown([error](auto&) { return error.get(); });
    };

    if (m_source->state() == ReadableStream::State::Errored) {
        // FIXME: Check whether ok to take a strong
        fprintf(stderr, "PipeToState::errorsMustBePropagatedForward source is errored\n");
        propagateErrorSteps(JSC::Strong<JSC::Unknown> { Ref { m_destination->internalWritableStream().globalObject()->vm() }, m_source->storedError(globalObject) });
        return;
    }

    m_reader->onClosedPromiseRejection([propagateErrorSteps = WTFMove(propagateErrorSteps)](auto& globalObject, auto&& error) mutable {
        // FIXME: Check whether ok to take a strong
        fprintf(stderr, "PipeToState::errorsMustBePropagatedForward2\n");

        propagateErrorSteps(JSC::Strong<JSC::Unknown> { Ref { globalObject.vm() }, error });
    });
}

void PipeToState::errorsMustBePropagatedBackward()
{
    auto propagateErrorSteps = [weakThis = WeakPtr { *this }](auto&& error) {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis)
            return;
        if (!protectedThis->m_options.preventCancel) {
            protectedThis->shutdownWithAction([protectedThis, error] -> RefPtr<DOMPromise> {
                fprintf(stderr, "PipeToState::errorsMustBePropagatedBackward 30\n");
                RefPtr internalReadableStream = protectedThis->m_source->internalReadableStream();
                if (!internalReadableStream)
                    return nullptr;

                auto* globalObject = internalReadableStream->globalObject();
                if (!globalObject)
                    return nullptr;

                fprintf(stderr, "PipeToState::errorsMustBePropagatedBackward 31\n");

                auto getError2 = [error = WTFMove(error)](auto&) {
                    return error.get();
                };

                auto value = internalReadableStream->cancel(*globalObject, error.get(), InternalReadableStream::Use::Private);
                auto [result, deferred] = createPromiseAndWrapper(*globalObject);
                auto* promise = jsCast<JSC::JSPromise*>(value);
                if (!promise) {
                    fprintf(stderr, "PipeToState::errorsMustBePropagatedBackward 32\n");
                    deferred->rejectWithCallback(WTFMove(getError2), RejectAsHandled::Yes);
                } else {
                    fprintf(stderr, "PipeToState::errorsMustBePropagatedBackward 33\n");
                    Ref cancelPromise = DOMPromise::create(*globalObject, *promise);
                    cancelPromise->whenSettled([deferred = WTFMove(deferred), cancelPromise, getError2 = WTFMove(getError2)] {
                        if (cancelPromise->status() == DOMPromise::Status::Rejected) {
                            fprintf(stderr, "PipeToState::errorsMustBePropagatedBackward 34\n");
                            deferred->rejectWithCallback([&](auto&) {
                                return cancelPromise->result();
                            }, RejectAsHandled::Yes);
                            return;
                        }
                        fprintf(stderr, "PipeToState::errorsMustBePropagatedBackward 35\n");
                        deferred->rejectWithCallback(WTFMove(getError2), RejectAsHandled::Yes);
                    });
                }
                return RefPtr { WTFMove(result) };
            }, [error](auto&) { return error.get(); });
            return;
        }
        protectedThis->shutdown([error](auto&) { return error.get(); });
    };

    // Check whether actually needed.
    if (m_destination->state() == WritableStream::State::Errored) {
        // FIXME: Check whether ok to take a strong
        fprintf(stderr, "PipeToState::errorsMustBePropagatedBackward 2\n");
        auto errorOrException = m_destination->internalWritableStream().storedError();
        if (errorOrException.hasException())
            return;
        fprintf(stderr, "PipeToState::errorsMustBePropagatedBackward 3\n");
        propagateErrorSteps(JSC::Strong<JSC::Unknown> { Ref { m_destination->internalWritableStream().globalObject()->vm() }, errorOrException.releaseReturnValue() });
        return;
    }

    m_writer->onClosedPromiseRejection([propagateErrorSteps = WTFMove(propagateErrorSteps)](auto& globalObject, auto&& error) mutable {
        fprintf(stderr, "PipeToState::errorsMustBePropagatedBackward 3\n");
        // FIXME: Check whether ok to take a strong
        propagateErrorSteps(JSC::Strong<JSC::Unknown> { Ref { globalObject.vm() }, error });
    });
}

void PipeToState::closingMustBePropagatedForward()
{
    fprintf(stderr, "PipeToState::closingMustBePropagatedForward\n");

    auto propagateClosedSteps = [weakThis = WeakPtr { *this }]() {
        fprintf(stderr, "PipeToState::closingMustBePropagatedForward propagateClosedSteps\n");
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis)
            return;
        if (!protectedThis->m_options.preventClose) {
            protectedThis->shutdownWithAction([protectedThis] -> RefPtr<DOMPromise> {
                fprintf(stderr, "PipeToState::closingMustBePropagatedForward writableStreamDefaultWriterCloseWithErrorPropagation\n");
                return writableStreamDefaultWriterCloseWithErrorPropagation(protectedThis->m_writer);
            });
            return;
        }
        fprintf(stderr, "PipeToState::closingMustBePropagatedForward propagateClosedSteps shutting down\n");
        protectedThis->shutdown();
    };

    if (m_source->state() == ReadableStream::State::Closed) {
        fprintf(stderr, "PipeToState::closingMustBePropagatedForward 2\n");
        propagateClosedSteps();
        return;
    }

    m_reader->onClosedPromiseResolution([propagateClosedSteps = WTFMove(propagateClosedSteps)]() mutable {
        fprintf(stderr, "PipeToState::closingMustBePropagatedForward 3\n");
        propagateClosedSteps();
    });
}

void PipeToState::closingMustBePropagatedBackward()
{
    fprintf(stderr, "PipeToState::closingMustBePropagatedBackward1\n");

    if (!m_destination->internalWritableStream().closeQueuedOrInFlight() && m_destination->state() != WritableStream::State::Closed)
        return;

    fprintf(stderr, "PipeToState::closingMustBePropagatedBackward2\n");
    // @assert no chunks have been read/written

    auto getError = [](auto& globalObject) {
        return createDOMException(globalObject, Exception { ExceptionCode::TypeError, "closing is propagated backward"_s });
    };

    if (!m_options.preventCancel) {
        shutdownWithAction([protectedThis = Ref { *this }, getError = WTFMove(getError)]() mutable -> RefPtr<DOMPromise> {
            fprintf(stderr, "PipeToState::closingMustBePropagatedBackward3\n");
            RefPtr internalReadableStream = protectedThis->m_source->internalReadableStream();
            if (!internalReadableStream)
                return nullptr;

            auto* globalObject = internalReadableStream->globalObject();
            if (!globalObject)
                return nullptr;

            Ref vm = globalObject->vm();
            // FIXME: Verify it is ok to take a Strong.
            JSC::Strong<JSC::Unknown> error { vm, getError(*globalObject) };
            auto value = internalReadableStream->cancel(*globalObject, error.get(), InternalReadableStream::Use::Private);

            auto getError2 = [error = WTFMove(error)](auto&) {
                return error.get();
            };

            auto [result, deferred] = createPromiseAndWrapper(*globalObject);
            auto* promise = jsCast<JSC::JSPromise*>(value);
            if (!promise)
                deferred->rejectWithCallback(WTFMove(getError2), RejectAsHandled::Yes);
            else {
                Ref cancelPromise = DOMPromise::create(*globalObject, *promise);
                cancelPromise->whenSettled([deferred = WTFMove(deferred), cancelPromise, getError2 = WTFMove(getError2)] {
                    if (cancelPromise->status() == DOMPromise::Status::Rejected) {
                        deferred->rejectWithCallback([&](auto&) {
                            return cancelPromise->result();
                        }, RejectAsHandled::Yes);
                        return;
                    }
                    deferred->rejectWithCallback(WTFMove(getError2), RejectAsHandled::Yes);
                });
            }
            return RefPtr { WTFMove(result) };
        });
        return;
    }

    auto* globalObject = m_destination->internalWritableStream().globalObject();
    if (!globalObject)
        return;
    shutdown(WTFMove(getError));
}

RefPtr<DOMPromise> PipeToState::waitForPendingReadAndWrite(Action&& action)
{
    RefPtr<DOMPromise> finalizePromise;
    if (m_destination->state() == WritableStream::State::Writable && !m_destination->internalWritableStream().closeQueuedOrInFlight()) {
        fprintf(stderr, "PipeToState::waitForPendingReadAndWrite1\n");
        if (m_pendingReadPromise || m_pendingWritePromise) {
            fprintf(stderr, "PipeToState::waitForPendingReadAndWrite2\n");
            auto handlePendingWritePromise = [this, protectedThis = Ref { *this }, action = WTFMove(action)](auto&& deferred) mutable {
                fprintf(stderr, "PipeToState::waitForPendingReadAndWrite handlePendingWritePromise 1\n");
                auto waitForAction = [action = WTFMove(action)](auto&& deferred) {
                    RefPtr promise = action();
                    if (!promise) {
                        fprintf(stderr, "no action promise, resolving\n");
                        deferred->resolve();
                        return;
                    }
                    promise->whenSettled([deferred = WTFMove(deferred), promise] {
                        // FIXME: use switch
                        if (promise->status() == DOMPromise::Status::Rejected) {
                            fprintf(stderr, "action promise rejected\n");
                            deferred->rejectWithCallback([&](auto&) { return promise->result(); }, RejectAsHandled::Yes);
                            return;
                        }
                        fprintf(stderr, " action promise resolved\n");
                        deferred->resolve();
                    });
                };

                if (!m_pendingWritePromise) {
                    waitForAction(WTFMove(deferred));
                    return;
                }

                auto* globalObject = m_pendingWritePromise->globalObject();
                if (!globalObject) {
                    waitForAction(WTFMove(deferred));
                    return;
                }

                fprintf(stderr, "PipeToState::waitForPendingReadAndWrite handlePendingWritePromise 2\n");

                m_pendingWritePromise->whenSettled([waitForAction = WTFMove(waitForAction), pendingWritePromise = m_pendingWritePromise, deferred = WTFMove(deferred)]() mutable {
                    fprintf(stderr, "PipeToState::waitForPendingReadAndWrite2.5\n");
                    // FIXME: use switch
                    if (pendingWritePromise->status() == DOMPromise::Status::Rejected) {
                        fprintf(stderr, "PipeToState::waitForPendingReadAndWrite2.6\n");
                        //deferred->rejectWithCallback([&](auto&) { return pendingWritePromise->result(); }, RejectAsHandled::Yes);
                        //return;
                    }
                    fprintf(stderr, "PipeToState::waitForPendingReadAndWrite2.7\n");
                    waitForAction(WTFMove(deferred));
                });
            };

            fprintf(stderr, "PipeToState::waitForPendingReadAndWrite10\n");
            auto* globalObject = m_pendingReadPromise ? m_pendingReadPromise->globalObject() : m_pendingWritePromise->globalObject();
            if (globalObject) {
                fprintf(stderr, "PipeToState::waitForPendingReadAndWrite11\n");
                auto [promise, deferred] = createPromiseAndWrapper(*globalObject);
                if (m_pendingReadPromise) {
                    fprintf(stderr, "PipeToState::waitForPendingReadAndWrite12\n");
                    m_pendingReadPromise->whenSettled([handlePendingWritePromise = WTFMove(handlePendingWritePromise), pendingReadPromise = m_pendingReadPromise, deferred = WTFMove(deferred)]() mutable {
                        // FIXME: use switch
                        if (pendingReadPromise->status() == DOMPromise::Status::Rejected) {
                            fprintf(stderr, "PipeToState::waitForPendingReadAndWrite12.5\n");
                            //deferred->rejectWithCallback([&](auto&) { return pendingReadPromise->result(); }, RejectAsHandled::Yes);
                            //return;
                        }

                        fprintf(stderr, "PipeToState::waitForPendingReadAndWrite13\n");
                        handlePendingWritePromise(WTFMove(deferred));
                    });
                } else
                    handlePendingWritePromise(WTFMove(deferred));
                finalizePromise = WTFMove(promise);
            }
        }
    }

    fprintf(stderr, "PipeToState::waitForPendingReadAndWrite20\n");
    if (!finalizePromise) {
        fprintf(stderr, "PipeToState::waitForPendingReadAndWrite21\n");
        finalizePromise = action();
    }
    return finalizePromise;
}

void PipeToState::shutdownWithAction(Action&& action, GetError&& getError)
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
 
    fprintf(stderr, "PipeToState::shutdownWithAction1\n");

    RefPtr finalizePromise = waitForPendingReadAndWrite(WTFMove(action));
    if (!finalizePromise) {
        finalize(WTFMove(getError));
        return;
    }
    
    finalizePromise->whenSettled([protectedThis = Ref { *this }, finalizePromise, getError = WTFMove(getError)]() mutable {
        switch (finalizePromise->status()) {
        case DOMPromise::Status::Fulfilled:
            fprintf(stderr, "PipeToState::shutdownWithAction fulfilled\n");
            protectedThis->finalize(WTFMove(getError));
            return;
        case DOMPromise::Status::Rejected:
            fprintf(stderr, "PipeToState::shutdownWithAction rejected\n");
            protectedThis->finalize([&](auto&) {
//                if (getError)
  //                  return getError(globalObject);
                return finalizePromise->result();
            });
            return;
        case DOMPromise::Status::Pending:
            ASSERT_NOT_REACHED();
            break;
        }
    });
}

void PipeToState::shutdown(GetError&& getError)
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;

    RefPtr finalizePromise = waitForPendingReadAndWrite([] { return nullptr; });
    if (!finalizePromise) {
        finalize(WTFMove(getError));
        return;
    }
    
    finalizePromise->whenSettled([protectedThis = Ref { *this }, finalizePromise, getError = WTFMove(getError)]() mutable {
        fprintf(stderr, "PipeToState::shutdown after promise settled\n");
        protectedThis->finalize(WTFMove(getError));
        ASSERT(finalizePromise->status() != DOMPromise::Status::Pending);
    });
}
void PipeToState::finalize(GetError&& getError)
{
    fprintf(stderr, "PipeToState::finalize %p\n", this);
    auto* globalObject = m_writer->globalObject();
    if (!globalObject) {
        fprintf(stderr, "PipeToState::finalize no globaobject\n");
        return;
    }

    fprintf(stderr, "PipeToState::finalize00\n");
    writableStreamDefaultWriterRelease(m_writer);
    fprintf(stderr, "PipeToState::finalize1\n");
    m_reader->releaseLock(*globalObject);

    fprintf(stderr, "PipeToState::finalize2\n");
    if (!m_promise)
        return;

    fprintf(stderr, "PipeToState::finalize3\n");
    if (getError) {
        m_promise->rejectWithCallback(WTFMove(getError), RejectAsHandled::No);
        return;
    }

    m_promise->resolve();
}


}
