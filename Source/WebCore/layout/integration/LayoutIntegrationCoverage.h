/*
 * Copyright (C) 2020 Apple Inc. All rights reserved.
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

#include "RenderStyleConstants.h"
#include <wtf/Forward.h>

namespace WebCore {

class RenderBlockFlow;
class RenderFlexibleBox;
class RenderInline;
class RenderObject;

namespace LayoutIntegration {
class LineLayout;

enum class AvoidanceReason : uint64_t {
    ContentIsRuby                                = 1LLU  << 3,
    FlowIsInitialContainingBlock                 = 1LLU  << 9,
    FlowHasLineAlignEdges                        = 1LLU  << 20,
    FlowHasLineSnap                              = 1LLU  << 21,
    FeatureIsDisabled                            = 1LLU  << 41,
    MultiColumnFlowHasOutOfFlowChild             = 1LLU  << 50,
    ContentIsSVG                                 = 1LLU  << 56,
    EndOfReasons                                 = 1LLU  << 62
};

bool canUseForLineLayout(const RenderBlockFlow&);
bool canUseForLineLayoutAfterBlockStyleChange(const RenderBlockFlow&, StyleDifference);
bool canUseForPreferredWidthComputation(const RenderBlockFlow&);
enum class TypeOfChangeForInvalidation : uint8_t {
    NodeInsertion,
    NodeRemoval,
    NodeMutation
};
bool shouldInvalidateLineLayoutPathAfterChangeFor(const RenderBlockFlow& rootBlockContainer, const RenderObject& renderer, const LineLayout&, TypeOfChangeForInvalidation);

bool canUseForFlexLayout(const RenderFlexibleBox&);

enum class IncludeReasons { First , All };
OptionSet<AvoidanceReason> canUseForLineLayoutWithReason(const RenderBlockFlow&, IncludeReasons);

}
}

