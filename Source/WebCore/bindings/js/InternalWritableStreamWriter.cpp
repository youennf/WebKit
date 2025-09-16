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
#include "InternalWritableStreamWriter.h"

#include "WritableStream.h"

namespace WebCore {

static ExceptionOr<JSC::JSValue> invokeWritableStreamWriterFunction(JSC::JSGlobalObject& globalObject, const JSC::Identifier& identifier, const JSC::MarkedArgumentBuffer& arguments)
{
    JSC::VM& vm = globalObject.vm();
    JSC::JSLockHolder lock(vm);

    auto scope = DECLARE_CATCH_SCOPE(vm);

    auto function = globalObject.get(&globalObject, identifier);
    ASSERT(!!scope.exception() || function.isCallable());
    scope.assertNoExceptionExceptTermination();
    RETURN_IF_EXCEPTION(scope, Exception { ExceptionCode::ExistingExceptionError });

    auto callData = JSC::getCallData(function);

    auto result = call(&globalObject, function, callData, JSC::jsUndefined(), arguments);
    RETURN_IF_EXCEPTION(scope, Exception { ExceptionCode::ExistingExceptionError });

    return result;
}

ExceptionOr<Ref<InternalWritableStreamWriter>> acquireWritableStreamDefaultWriter(JSDOMGlobalObject& globalObject, WritableStream& destination)
{
    auto* clientData = static_cast<JSVMClientData*>(globalObject.vm().clientData);
    auto& privateName = clientData->builtinFunctions().writableStreamInternalsBuiltins().acquireWritableStreamDefaultWriterPrivateName();

    JSC::MarkedArgumentBuffer arguments;
    arguments.append(destination.internalWritableStream());

    auto result = invokeWritableStreamWriterFunction(globalObject, privateName, arguments);
    if (result.hasException())
        return Exception { ExceptionCode::ExistingExceptionError };

    ASSERT(result.returnValue().isObject());
    return InternalWritableStreamWriter::create(globalObject, *result.returnValue().toObject(&globalObject));
}

int writableStreamDefaultWriterGetDesiredSize(InternalWritableStreamWriter& writer)
{
    auto* globalObject = writer.globalObject();
    if (!globalObject)
        return 0;

    auto* clientData = static_cast<JSVMClientData*>(globalObject->vm().clientData);
    auto& privateName = clientData->builtinFunctions().writableStreamInternalsBuiltins().writableStreamDefaultWriterGetDesiredSizePrivateName();

    JSC::MarkedArgumentBuffer arguments;
    arguments.append(writer.guardedObject());

    auto result = invokeWritableStreamWriterFunction(*globalObject, privateName, arguments);
    return result.returnValue().toNumber(globalObject);
}

void writableStreamDefaultWriterCloseWithErrorPropagation(InternalWritableStreamWriter& writer)
{
    auto* globalObject = writer.globalObject();
    if (!globalObject)
        return;

    auto* clientData = static_cast<JSVMClientData*>(globalObject->vm().clientData);
    auto& privateName = clientData->builtinFunctions().writableStreamInternalsBuiltins().writableStreamDefaultWriterCloseWithErrorPropagationPrivateName();

    JSC::MarkedArgumentBuffer arguments;
    arguments.append(writer.guardedObject());

    invokeWritableStreamWriterFunction(*globalObject, privateName, arguments);
}

void writableStreamDefaultWriterRelease(InternalWritableStreamWriter& writer)
{
    auto* globalObject = writer.globalObject();
    if (!globalObject)
        return;

    auto* clientData = static_cast<JSVMClientData*>(globalObject->vm().clientData);
    auto& privateName = clientData->builtinFunctions().writableStreamInternalsBuiltins().writableStreamDefaultWriterReleasePrivateName();

    JSC::MarkedArgumentBuffer arguments;
    arguments.append(writer.guardedObject());

    invokeWritableStreamWriterFunction(*globalObject, privateName, arguments);
}

}
