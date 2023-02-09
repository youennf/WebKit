/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "BackgroundFetchRecordResponseBodyLoader.h"

#if ENABLE(SERVICE_WORKER)

#include "FetchBody.h"
#include "FetchResponse.h"
#include "SWClientConnection.h"

namespace WebCore {

BackgroundFetchRecordResponseBodyLoader::BackgroundFetchRecordResponseBodyLoader(FetchResponse& response, FetchOptions::Credentials credentials, NotificationCallback&& callback, BackgroundFetchRecordIdentifier recordIdentifier)
    : FetchResponseLoader(response, credentials, WTFMove(callback))
    , m_recordIdentifier(recordIdentifier)
{
}

bool BackgroundFetchRecordResponseBodyLoader::start(ScriptExecutionContext& context)
{
    fprintf(stderr, "BackgroundFetchRecordResponseBodyLoader::start\n");

    m_context = context;
    SWClientConnection::fromScriptExecutionContext(context)->retrieveRecordResponse(m_recordIdentifier, [weakThis = WeakPtr { *this }](auto&& result) {
        if (!weakThis || !weakThis->m_isActive)
            return;

        if (result.hasException()) {
            weakThis->receivedError(result.releaseException());
            return;
        }

        fprintf(stderr, "BackgroundFetchRecordResponseBodyLoader::start 1\n");
        weakThis->receivedResponse(result.returnValue());
        if (weakThis->m_isActive)
            weakThis->startStreamingBody();
    });
    return true;
}

void BackgroundFetchRecordResponseBodyLoader::stop()
{
    m_isActive = false;
}

bool BackgroundFetchRecordResponseBodyLoader::isActive() const
{
    return m_isActive;
}

RefPtr<FragmentedSharedBuffer> BackgroundFetchRecordResponseBodyLoader::startStreamingBody()
{
    fprintf(stderr, "BackgroundFetchRecordResponseBodyLoader::startStreamingBody 1\n");

    ASSERT(m_isActive);
    if (m_hasStartedStreamingBody)
        return nullptr;

    m_hasStartedStreamingBody = true;

    fprintf(stderr, "BackgroundFetchRecordResponseBodyLoader::startStreamingBody 2\n");
    if (!m_context) {
        if (auto callback = takeConsumeDataCallback())
            callback(Exception { TypeError });
        return nullptr;
    }

    fprintf(stderr, "BackgroundFetchRecordResponseBodyLoader::startStreamingBody 3\n");
    SWClientConnection::fromScriptExecutionContext(*m_context)->retrieveRecordResponseBody(m_recordIdentifier, [weakThis = WeakPtr { *this }](auto&& result) {
        fprintf(stderr, "BackgroundFetchRecordResponseBodyLoader::startStreamingBody 4\n");
        if (!weakThis)
            return;
        fprintf(stderr, "BackgroundFetchRecordResponseBodyLoader::startStreamingBody 5\n");

        if (!result.has_value()) {
            weakThis->receivedError(Exception { TypeError, result.error().sanitizedDescription() });
            return;
        }
        
        if (auto buffer = WTFMove(result.value())) {
            weakThis->response()->receivedData(buffer.releaseNonNull());
            return;
        }

        weakThis->response()->didSucceed({ });
    });

    return nullptr;
}

} // namespace WebCore

#endif // ENABLE(SERVICE_WORKER)


