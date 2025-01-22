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
#include "ServiceWorkerRoute.h"

#include "URLPatternCanonical.h"
#include "URLPatternParser.h"
#include <wtf/CrossThreadCopier.h>

namespace WebCore {

static std::optional<ExceptionData> validateURLPatternComponent(StringView component, EncodingCallbackType type)
{
    auto result = URLPatternUtilities::URLPatternParser::parse(component, { .ignoreCase = true }, type);
    if (result.hasException())
        return ExceptionData { result.exception().code(), result.releaseException().releaseMessage() };

    auto parts = result.releaseReturnValue();
    for (auto& part : parts) {
        // FIXME: We should only reject for regexp group and support all other values.
        if (part.type != URLPatternUtilities::PartType::FixedText && part.type != URLPatternUtilities::PartType::FullWildcard)
            return ExceptionData { ExceptionCode::NotSupportedError, "URLPattern component value not supported"_s };
    }

    return { };
}

static inline std::optional<ExceptionData> validateServiceWorkerRouteCondition(ServiceWorkerRouteCondition& condition, size_t maxRouteConditionDepth, size_t depth = 0)
{
    if (++depth > maxRouteConditionDepth)
        return ExceptionData { ExceptionCode::TypeError, "Service Worker route condition depth is too high"_s };

    if (condition.urlPattern) {
        if (auto exception = validateURLPatternComponent(condition.urlPattern->protocol, EncodingCallbackType::Protocol))
            return exception;
        if (auto exception = validateURLPatternComponent(condition.urlPattern->username, EncodingCallbackType::Username))
            return exception;
        if (auto exception = validateURLPatternComponent(condition.urlPattern->password, EncodingCallbackType::Password))
            return exception;
        if (auto exception = validateURLPatternComponent(condition.urlPattern->hostname, EncodingCallbackType::Host))
            return exception;
        if (auto exception = validateURLPatternComponent(condition.urlPattern->pathname, EncodingCallbackType::Path))
            return exception;
        if (auto exception = validateURLPatternComponent(condition.urlPattern->port, EncodingCallbackType::Port))
            return exception;
        if (auto exception = validateURLPatternComponent(condition.urlPattern->search, EncodingCallbackType::Search))
            return exception;
        if (auto exception = validateURLPatternComponent(condition.urlPattern->hash, EncodingCallbackType::Hash))
            return exception;
        if (auto exception = validateURLPatternComponent(condition.urlPattern->protocol, EncodingCallbackType::Protocol))
            return exception;
        if (auto exception = validateURLPatternComponent(condition.urlPattern->protocol, EncodingCallbackType::Protocol))
            return exception;
        if (auto exception = validateURLPatternComponent(condition.urlPattern->protocol, EncodingCallbackType::Protocol))
            return exception;
        if (auto exception = validateURLPatternComponent(condition.urlPattern->protocol, EncodingCallbackType::Protocol))
            return exception;
    }

    Vector<ServiceWorkerRouteCondition> orConditions;
    for (auto& orCondition : condition.orConditions) {
        if (auto exception = validateServiceWorkerRouteCondition(orCondition, depth))
            return *exception;
    }

    if (condition.notCondition) {
        if (auto exception = validateServiceWorkerRouteCondition(*condition.notCondition, depth))
            return *exception;
    }
    return { };
}

std::optional<ExceptionData> validateServiceWorkerRoute(ServiceWorkerRoute& route, size_t maxRouteConditionDepth)
{
    return validateServiceWorkerRouteCondition(route.condition, maxRouteConditionDepth);
}

ServiceWorkerRouteCondition ServiceWorkerRouteCondition::isolatedCopy() &&
{
    std::unique_ptr<ServiceWorkerRouteCondition> notConditionCopy;
    if (notCondition)
        notConditionCopy = makeUnique<ServiceWorkerRouteCondition>(WTFMove(*notCondition));
    return {
        crossThreadCopy(WTFMove(urlPattern)),
        crossThreadCopy(WTFMove(requestMethod)),
        requestMode,
        requestDestination,
        runningStatus,
        crossThreadCopy(WTFMove(orConditions)),
        WTFMove(notConditionCopy)
    };
}

ServiceWorkerRoutePattern ServiceWorkerRoutePattern::isolatedCopy() &&
{
    return {
        crossThreadCopy(WTFMove(protocol)),
        crossThreadCopy(WTFMove(username)),
        crossThreadCopy(WTFMove(password)),
        crossThreadCopy(WTFMove(hostname)),
        crossThreadCopy(WTFMove(pathname)),
        crossThreadCopy(WTFMove(port)),
        crossThreadCopy(WTFMove(search)),
        crossThreadCopy(WTFMove(hash))
    };
}

} // namespace WebCore
