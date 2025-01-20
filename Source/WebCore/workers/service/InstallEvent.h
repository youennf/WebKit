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

#pragma once

#include "ExtendableEvent.h"
#include "FetchRequestDestination.h"
#include "FetchRequestMode.h"
#include "URLPatternInit.h"
#include "URLPattern.h"
#include <wtf/Vector.h>

namespace WebCore {

class DeferredPromise;
class ScriptExecutionContext;
class URLPattern;

class InstallEvent final : public ExtendableEvent {
    WTF_MAKE_TZONE_OR_ISO_ALLOCATED(InstallEvent);
public:
    static Ref<InstallEvent> create(const AtomString& type, ExtendableEventInit&& initializer, IsTrusted isTrusted = IsTrusted::No)
    {
        return adoptRef(*new InstallEvent(type, WTFMove(initializer), isTrusted));
    }
    ~InstallEvent();

    struct RouterCondition;
    class RouterNotCondition {
        WTF_MAKE_FAST_ALLOCATED;
    public:
        RouterNotCondition(RouterCondition&& value)
            : m_value(makeUniqueRef<RouterCondition>(WTFMove(value)))
        {
        }

        const RouterCondition& value() const { return m_value.get(); }

    private:
        UniqueRef<RouterCondition> m_value;
    };

    enum class RunningStatus : bool { Running, NotRunning };
    struct RouterCondition {
        WTF_MAKE_STRUCT_FAST_ALLOCATED;

        std::variant<String, URLPatternInit, RefPtr<URLPattern>> urlPattern;
        String requestMethod;
        std::optional<FetchRequestMode> requestMode;
        std::optional<FetchRequestDestination> requestDestination;
        std::optional<RunningStatus> runningStatus;

        Vector<RouterCondition> orConditions;
        std::optional<RouterNotCondition> notCondition;
    };

    struct RouterSourceDict {
        String cacheName;
    };
 
    enum class RouterSourceEnum : uint8_t { Cache, FetchEvent, Network };

    struct RouterRule {
        RouterCondition condition;
        std::variant<RouterSourceDict, RouterSourceEnum> source;
    };

    void addRoutes(ScriptExecutionContext&, std::variant<RouterRule, Vector<RouterRule>>&&, Ref<DeferredPromise>&&);

private:
    WEBCORE_EXPORT InstallEvent(const AtomString&, ExtendableEventInit&&, IsTrusted);

};

} // namespace WebCore
