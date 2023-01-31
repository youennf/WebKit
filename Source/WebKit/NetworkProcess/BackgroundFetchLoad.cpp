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
#include "BackgroundFetchLoad.h"

#include "AuthenticationChallengeDisposition.h"
#include "AuthenticationManager.h"
#include "Logging.h"
#include "NetworkConnectionToWebProcess.h"
#include "NetworkLoadChecker.h"
#include "NetworkProcess.h"
#include "WebErrors.h"

#define PING_RELEASE_LOG(fmt, ...) RELEASE_LOG(Network, "%p - BackgroundFetchLoad::" fmt, this, ##__VA_ARGS__)

namespace WebKit {

using namespace WebCore;

BackgroundFetchLoad::BackgroundFetchLoad(NetworkProcess& networkProcess, PAL::SessionID sessionID, NetworkResourceLoadParameters&& parameters, CompletionHandler<void(const ResourceError&, const ResourceResponse&)>&& completionHandler)
    : m_sessionID(sessionID)
    , m_parameters(WTFMove(parameters))
    , m_completionHandler(WTFMove(completionHandler))
    , m_timeoutTimer(*this, &BackgroundFetchLoad::timeoutTimerFired)
    , m_networkLoadChecker(makeUniqueRef<NetworkLoadChecker>(networkProcess, nullptr, nullptr, FetchOptions { m_parameters.options }, m_sessionID, m_parameters.webPageProxyID, WTFMove(m_parameters.originalRequestHeaders), URL { m_parameters.request.url() }, URL { m_parameters.documentURL }, m_parameters.sourceOrigin.copyRef(), m_parameters.topOrigin.copyRef(), m_parameters.parentOrigin(), m_parameters.preflightPolicy, m_parameters.request.httpReferrer(), m_parameters.allowPrivacyProxy, m_parameters.networkConnectionIntegrityPolicy))
{
    initialize(networkProcess);
}

BackgroundFetchLoad::BackgroundFetchLoad(NetworkConnectionToWebProcess& connection, NetworkResourceLoadParameters&& parameters, CompletionHandler<void(const ResourceError&, const ResourceResponse&)>&& completionHandler)
    : m_sessionID(connection.sessionID())
    , m_parameters(WTFMove(parameters))
    , m_completionHandler(WTFMove(completionHandler))
    , m_timeoutTimer(*this, &BackgroundFetchLoad::timeoutTimerFired)
    , m_networkLoadChecker(makeUniqueRef<NetworkLoadChecker>(connection.networkProcess(), nullptr,  &connection.schemeRegistry(), FetchOptions { m_parameters.options }, m_sessionID, m_parameters.webPageProxyID, WTFMove(m_parameters.originalRequestHeaders), URL { m_parameters.request.url() }, URL { m_parameters.documentURL }, m_parameters.sourceOrigin.copyRef(), m_parameters.topOrigin.copyRef(), m_parameters.parentOrigin(), m_parameters.preflightPolicy, m_parameters.request.httpReferrer(), m_parameters.allowPrivacyProxy, m_parameters.networkConnectionIntegrityPolicy))
    , m_blobFiles(connection.resolveBlobReferences(m_parameters))
{
    for (auto& file : m_blobFiles) {
        if (file)
            file->prepareForFileAccess();
    }

    initialize(connection.networkProcess());
}

void BackgroundFetchLoad::initialize(NetworkProcess& networkProcess)
{
    m_networkLoadChecker->enableContentExtensionsCheck();
    if (m_parameters.cspResponseHeaders)
        m_networkLoadChecker->setCSPResponseHeaders(WTFMove(m_parameters.cspResponseHeaders.value()));
    m_networkLoadChecker->setParentCrossOriginEmbedderPolicy(m_parameters.parentCrossOriginEmbedderPolicy);
    m_networkLoadChecker->setCrossOriginEmbedderPolicy(m_parameters.crossOriginEmbedderPolicy);
#if ENABLE(CONTENT_EXTENSIONS)
    m_networkLoadChecker->setContentExtensionController(WTFMove(m_parameters.mainDocumentURL), WTFMove(m_parameters.frameURL), m_parameters.userContentControllerIdentifier);
#endif

    // If the server never responds, this object will hang around forever.
    // Set a very generous timeout, just in case.
    m_timeoutTimer.startOneShot(60000_s);

    m_networkLoadChecker->check(ResourceRequest { m_parameters.request }, nullptr, [this, weakThis = WeakPtr { *this }, networkProcess = Ref { networkProcess }] (auto&& result) {
        if (!weakThis)
            return;
        WTF::switchOn(result,
            [this] (ResourceError& error) {
                this->didFinish(error);
            },
            [] (NetworkLoadChecker::RedirectionTriplet& triplet) {
                // We should never send a synthetic redirect for BackgroundFetchLoads.
                ASSERT_NOT_REACHED();
            },
            [&] (ResourceRequest& request) {
                this->loadRequest(networkProcess, WTFMove(request));
            }
        );
    });
}

BackgroundFetchLoad::~BackgroundFetchLoad()
{
    if (m_task) {
        ASSERT(m_task->client() == this);
        m_task->clearClient();
        m_task->cancel();
    }
    for (auto& file : m_blobFiles) {
        if (file)
            file->revokeFileAccess();
    }
}

void BackgroundFetchLoad::didFinish(const ResourceError& error, const ResourceResponse& response)
{
    m_completionHandler(error, response);
    delete this;
}

void BackgroundFetchLoad::loadRequest(NetworkProcess& networkProcess, ResourceRequest&& request)
{
    PING_RELEASE_LOG("startNetworkLoad");
    if (auto* networkSession = networkProcess.networkSession(m_sessionID)) {
        auto loadParameters = m_parameters;
        loadParameters.request = WTFMove(request);
        m_task = NetworkDataTask::create(*networkSession, *this, WTFMove(loadParameters));
        m_task->resume();
    } else
        ASSERT_NOT_REACHED();
}

void BackgroundFetchLoad::willPerformHTTPRedirection(ResourceResponse&& redirectResponse, ResourceRequest&& request, RedirectCompletionHandler&& completionHandler)
{
    m_networkLoadChecker->checkRedirection(ResourceRequest { }, WTFMove(request), WTFMove(redirectResponse), nullptr, [this, completionHandler = WTFMove(completionHandler)] (auto&& result) mutable {
        if (!result.has_value()) {
            this->didFinish(result.error());
            completionHandler({ });
            return;
        }
        auto request = WTFMove(result->redirectRequest);
        if (!request.url().protocolIsInHTTPFamily()) {
            this->didFinish(ResourceError { String { }, 0, request.url(), "Redirection to URL with a scheme that is not HTTP(S)"_s, ResourceError::Type::AccessControl });
            completionHandler({ });
            return;
        }

        completionHandler(WTFMove(request));
    });
}

void BackgroundFetchLoad::didReceiveChallenge(AuthenticationChallenge&& challenge, NegotiatedLegacyTLS negotiatedLegacyTLS, ChallengeCompletionHandler&& completionHandler)
{
    PING_RELEASE_LOG("didReceiveChallenge");
    if (challenge.protectionSpace().authenticationScheme() == ProtectionSpace::AuthenticationScheme::ServerTrustEvaluationRequested) {
        m_networkLoadChecker->networkProcess().authenticationManager().didReceiveAuthenticationChallenge(m_sessionID, m_parameters.webPageProxyID,  m_parameters.topOrigin ? &m_parameters.topOrigin->data() : nullptr, challenge, negotiatedLegacyTLS, WTFMove(completionHandler));
        return;
    }
    WeakPtr weakThis { *this };
    completionHandler(AuthenticationChallengeDisposition::Cancel, { });
    if (!weakThis)
        return;
    didFinish(ResourceError { String(), 0, currentURL(), "Failed HTTP authentication"_s, ResourceError::Type::AccessControl });
}

void BackgroundFetchLoad::didReceiveResponse(ResourceResponse&& response, NegotiatedLegacyTLS, PrivateRelayed, ResponseCompletionHandler&& completionHandler)
{
    PING_RELEASE_LOG("didReceiveResponse - httpStatusCode=%d", response.httpStatusCode());
    WeakPtr weakThis { *this };
    completionHandler(PolicyAction::Ignore);
    if (!weakThis)
        return;
    didFinish({ }, response);
}

void BackgroundFetchLoad::didReceiveData(const SharedBuffer&)
{
    PING_RELEASE_LOG("didReceiveData");
    ASSERT_NOT_REACHED();
}

void BackgroundFetchLoad::didCompleteWithError(const ResourceError& error, const NetworkLoadMetrics&)
{
    if (error.isNull())
        PING_RELEASE_LOG("didComplete");
    else
        PING_RELEASE_LOG("didCompleteWithError, error_code=%d", error.errorCode());

    didFinish(error);
}

void BackgroundFetchLoad::didSendData(uint64_t totalBytesSent, uint64_t totalBytesExpectedToSend)
{
}

void BackgroundFetchLoad::wasBlocked()
{
    PING_RELEASE_LOG("wasBlocked");
    didFinish(blockedError(ResourceRequest { currentURL() }));
}

void BackgroundFetchLoad::cannotShowURL()
{
    PING_RELEASE_LOG("cannotShowURL");
    didFinish(cannotShowURLError(ResourceRequest { currentURL() }));
}

void BackgroundFetchLoad::wasBlockedByRestrictions()
{
    PING_RELEASE_LOG("wasBlockedByRestrictions");
    didFinish(wasBlockedByRestrictionsError(ResourceRequest { currentURL() }));
}

void BackgroundFetchLoad::wasBlockedByDisabledFTP()
{
    PING_RELEASE_LOG("wasBlockedByDisabledFTP");
    didFinish(ftpDisabledError(ResourceRequest(currentURL())));
}

void BackgroundFetchLoad::timeoutTimerFired()
{
    PING_RELEASE_LOG("timeoutTimerFired");
    didFinish(ResourceError { String(), 0, currentURL(), "Load timed out"_s, ResourceError::Type::Timeout });
}

const URL& BackgroundFetchLoad::currentURL() const
{
    return m_networkLoadChecker->url();
}

} // namespace WebKit

#undef PING_RELEASE_LOG
