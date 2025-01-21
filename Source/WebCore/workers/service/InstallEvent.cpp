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
#include "InstallEvent.h"

#include "ServiceWorkerGlobalScope.h"
#include "ServiceWorkerRoute.h"

namespace WebCore {

static constexpr size_t maxRouteConditionDepth = 10;
static constexpr size_t maxRouteCount = 256;

WTF_MAKE_TZONE_OR_ISO_ALLOCATED_IMPL(InstallEvent);

InstallEvent::InstallEvent(const AtomString& type, ExtendableEventInit&& initializer, IsTrusted isTrusted)
    : ExtendableEvent(EventInterfaceType::InstallEvent, type, initializer, isTrusted)
{
}

InstallEvent::~InstallEvent() = default;

static ExceptionOr<ServiceWorkerRouteCondition::Pattern> toServiceWorkerRoutePattern(const URLPattern& pattern)
{
    if (pattern.hasRegExpGroups())
        return Exception { ExceptionCode::TypeError, "Service Worker route url pattern has regexp groups"_s };

    return ServiceWorkerRouteCondition::Pattern {
        pattern.protocol(),
        pattern.username(),
        pattern.password(),
        pattern.hostname(),
        pattern.port(),
        pattern.pathname(),
        pattern.search(),
        pattern.hash()
    };
}

static ExceptionOr<ServiceWorkerRouteCondition> toServiceWorkerRouteCondition(RouterCondition&& condition, size_t depth = 0)
{
    if (++depth > maxRouteConditionDepth)
        return Exception { ExceptionCode::TypeError, "Service Worker route condition depth is too high"_s };

    std::optional<ServiceWorkerRouteCondition::Pattern> pattern;
    if (condition.urlPattern) {
        auto patternOrException = toServiceWorkerRoutePattern(*std::get<RefPtr<URLPattern>>(*condition.urlPattern));
        if (patternOrException.hasException())
            return patternOrException.releaseException();
        pattern = patternOrException.releaseReturnValue();
    }

    Vector<ServiceWorkerRouteCondition> orConditions;
    for (auto& orCondition : condition.orConditions) {
        auto orConditionOrException = toServiceWorkerRouteCondition(WTFMove(orCondition), depth);
        if (orConditionOrException.hasException())
            return orConditionOrException.releaseException();
        orConditions.append(orConditionOrException.releaseReturnValue());
    }

    std::unique_ptr<ServiceWorkerRouteCondition> notCondition;
    if (condition.notCondition) {
        auto notConditionOrException = toServiceWorkerRouteCondition(WTFMove(*condition.notCondition).value(), depth);
        if (notConditionOrException.hasException())
            return notConditionOrException.releaseException();
        notCondition = makeUnique<ServiceWorkerRouteCondition>(notConditionOrException.releaseReturnValue());
    }

    return ServiceWorkerRouteCondition {
        WTFMove(*pattern),
        WTFMove(condition.requestMethod),
        WTFMove(condition.requestMode),
        WTFMove(condition.requestDestination),
        WTFMove(condition.runningStatus),
        WTFMove(orConditions),
        WTFMove(notCondition)
    };
}

static ExceptionOr<ServiceWorkerRoute> toServiceWorkerRoute(RouterRule&& rule)
{
    auto conditionOrException = toServiceWorkerRouteCondition(WTFMove(rule.condition));
    if (conditionOrException.hasException())
        return conditionOrException.releaseException();

    return ServiceWorkerRoute {
        conditionOrException.releaseReturnValue(),
        WTFMove(rule.source)
    };
}

// https://w3c.github.io/ServiceWorker/#verify-router-condition
static std::optional<Exception> verifyRouterCondition(RouterCondition& condition, ServiceWorkerGlobalScope& scope)
{
    bool hasCondition = false;
    if (condition.urlPattern) {
        auto urlPatternOrException = URLPattern::create(scope, std::exchange(*condition.urlPattern, { }), scope.contextData().scriptURL.string());
        if (urlPatternOrException.hasException())
            return urlPatternOrException.releaseException();
        condition.urlPattern = urlPatternOrException.releaseReturnValue();
    }
    if (!condition.requestMethod.isNull())
        hasCondition = true;
    if (condition.requestMode)
        hasCondition = true;
    if (condition.requestDestination)
        hasCondition = true;
    if (condition.runningStatus)
        hasCondition = true;
    if (!condition.orConditions.isEmpty()) {
        if (hasCondition)
            return Exception { ExceptionCode::TypeError, "Or condition should not be present"_s };
        for (auto& orCondition : condition.orConditions) {
            if (auto exception = verifyRouterCondition(orCondition, scope))
                return *exception;
        }
        hasCondition = true;
    }
    if (condition.notCondition) {
        if (hasCondition)
            return Exception { ExceptionCode::TypeError, "Not condition should not be present"_s };
        if (auto exception = verifyRouterCondition(condition.notCondition->value(), scope))
            return *exception;
    }
    return { };
}

static std::optional<Exception> addServiceWorkerRoute(Vector<ServiceWorkerRoute>& routes, RouterRule&& rule, ServiceWorkerGlobalScope& scope)
{
    if (auto validationException = verifyRouterCondition(rule.condition, scope))
        return *validationException;

    auto routeOrException = toServiceWorkerRoute(WTFMove(rule));
    if (routeOrException.hasException())
        return routeOrException.releaseException();
    routes.append(routeOrException.releaseReturnValue());
    return { };
}

void InstallEvent::addRoutes(ScriptExecutionContext& context, std::variant<RouterRule, Vector<RouterRule>>&& rules, Ref<DeferredPromise>&& promise)
{
    if (!isWaiting()) {
        // This is not in spec, verify whether we need it.
        promise->reject(Exception { ExceptionCode::TypeError, "Too late to add a rule"_s });
        return;
    }

    RefPtr serviceWorkerGlobalScope = dynamicDowncast<ServiceWorkerGlobalScope>(context);

    Vector<ServiceWorkerRoute> routes;
    auto exception = switchOn(rules, [&](RouterRule& rule) -> std::optional<Exception> {
        return addServiceWorkerRoute(routes, WTFMove(rule), *serviceWorkerGlobalScope);
    }, [&](auto&& rules) -> std::optional<Exception> {
        for (auto& rule : rules) {
            if (auto exception = addServiceWorkerRoute(routes, WTFMove(rule), *serviceWorkerGlobalScope))
                return *exception;
        }
        return { };
    });
    if (exception) {
        promise->reject(WTFMove(*exception));
        return;
    }

    m_rulesCount += routes.size();
    if (m_rulesCount > maxRouteCount) {
        promise->reject(Exception { ExceptionCode::TypeError, "Too many rules"_s });
        return;
    }

    promise->reject(Exception { ExceptionCode::NotSupportedError, "Not yet implemented"_s });
}

} // namespace WebCore
