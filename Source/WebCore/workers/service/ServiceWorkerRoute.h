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

#include "FetchRequestDestination.h"
#include "FetchRequestMode.h"
#include "RunningStatus.h"
#include "RouterSourceDict.h"
#include "RouterSourceEnum.h"
#include <optional>
#include <variant>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

struct ServiceWorkerRouteCondition {
    WTF_MAKE_STRUCT_FAST_ALLOCATED;

    using Component = String;
    struct Pattern {
        Component protocol;
        Component username;
        Component password;
        Component hostname;
        Component pathname;
        Component port;
        Component search;
        Component hash;
    };

    Pattern urlPattern;
    String requestMethod;
    std::optional<FetchRequestMode> requestMode;
    std::optional<FetchRequestDestination> requestDestination;
    std::optional<RunningStatus> runningStatus;

    Vector<ServiceWorkerRouteCondition> orConditions;
    std::unique_ptr<ServiceWorkerRouteCondition> notCondition;
};

struct ServiceWorkerRoute {
    ServiceWorkerRouteCondition condition;
    std::variant<RouterSourceDict, RouterSourceEnum> source;
};

} // namespace WebCore
