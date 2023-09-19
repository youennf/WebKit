/*
 * Copyright (C) 2017-2020 Apple Inc. All rights reserved.
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
#include "LayoutIntegrationPagination.h"

#include "FloatingState.h"
#include "FontCascade.h"
#include "InlineIteratorLineBox.h"
#include "RenderBlockFlow.h"
#include "RenderStyleInlines.h"
#include "RenderTableCell.h"

namespace WebCore {
namespace LayoutIntegration {

Vector<LineAdjustment> computeAdjustmentsForPagination(const InlineContent& inlineContent, const Layout::FloatingState& floatingState, RenderBlockFlow& flow)
{
    auto lineCount = inlineContent.displayContent().lines.size();
    Vector<LineAdjustment> adjustments { lineCount };

    HashMap<size_t, LayoutUnit, DefaultHash<size_t>, WTF::UnsignedWithZeroKeyHashTraits<size_t>>  lineFloatBottomMap;
    for (auto& item : floatingState.floats()) {
        if (!item.layoutBox())
            continue;

        auto& renderer = downcast<RenderBox>(inlineContent.rendererForLayoutBox(*item.layoutBox()));
        bool isUsplittable = renderer.isUnsplittableForPagination();

        auto placedByLine = item.placedByLine();
        if (!placedByLine) {
            if (isUsplittable) {
                auto rect = item.absoluteRectWithMargin();
                flow.updateMinimumPageHeight(rect.top(), rect.height());
            }
            continue;
        }

        auto floatMinimumBottom = [&] {
            auto* block = dynamicDowncast<RenderBlockFlow>(renderer);
            if (block && !isUsplittable) {
                if (auto firstLine = InlineIterator::firstLineBoxFor(*block))
                    return LayoutUnit { firstLine->logicalBottom() };
            }
            return item.absoluteRectWithMargin().bottom();
        }();

        auto result = lineFloatBottomMap.add(*placedByLine, floatMinimumBottom);
        if (!result.isNewEntry)
            result.iterator->value = std::max(floatMinimumBottom, result.iterator->value);
    }

    std::optional<size_t> previousPageBreakIndex;

    size_t widows = flow.style().hasAutoWidows() ? 0 : flow.style().widows();
    size_t orphans = flow.style().orphans();

    auto accumulatedOffset = 0_lu;
    for (size_t lineIndex = 0; lineIndex < lineCount;) {
        auto line = InlineIterator::lineBoxFor(inlineContent, lineIndex);

        auto it = lineFloatBottomMap.find(lineIndex);
        auto floatMinimumBottom = it != lineFloatBottomMap.end() ? it->value : 0_lu;

        auto adjustment = flow.computeLineAdjustmentForPagination(line, accumulatedOffset, floatMinimumBottom);

        if (adjustment.isFirstAfterPageBreak) {
            auto remainingLines = lineCount - lineIndex;
            // Ignore the last line if it is completely empty.
            if (inlineContent.displayContent().lines.last().lineBoxRect().isEmpty())
                remainingLines--;

            // See if there are enough lines left to meet the widow requirement.
            if (remainingLines < widows && !flow.didBreakAtLineToAvoidWidow()) {
                auto previousPageLineCount = lineIndex - previousPageBreakIndex.value_or(0);
                auto neededLines = widows - remainingLines;
                auto availableLines = previousPageLineCount > orphans ? previousPageLineCount - orphans : 0;
                auto breakIndex = lineIndex - std::min(neededLines, availableLines);
                // Set the widow break and recompute the adjustments starting from that line.
                flow.setBreakAtLineToAvoidWidow(breakIndex + 1);
                lineIndex = breakIndex;
                continue;
            }

            previousPageBreakIndex = lineIndex;
        }

        accumulatedOffset += adjustment.strut;

        if (adjustment.isFirstAfterPageBreak && !lineIndex)
            accumulatedOffset += inlineContent.clearGapBeforeFirstLine;

        adjustments[lineIndex] = LineAdjustment { accumulatedOffset, adjustment.isFirstAfterPageBreak };

        ++lineIndex;
    }

    flow.clearDidBreakAtLineToAvoidWidow();

    if (!previousPageBreakIndex)
        return { };

    return adjustments;
}

void adjustLinePositionsForPagination(InlineContent& inlineContent, const Vector<LineAdjustment>& adjustments)
{
    if (adjustments.isEmpty())
        return;

    inlineContent.isPaginated = true;
    auto& displayContent = inlineContent.displayContent();
    for (size_t lineIndex = 0; lineIndex < displayContent.lines.size(); ++lineIndex) {
        auto& line = displayContent.lines[lineIndex];
        auto& adjustment = adjustments[lineIndex];
        line.moveVertically(adjustment.offset);
        if (adjustment.isFirstAfterPageBreak)
            line.setIsFirstAfterPageBreak();
    }
    for (auto& box : displayContent.boxes)
        box.moveVertically(adjustments[box.lineIndex()].offset);

    inlineContent.firstLinePaginationOffset = adjustments[0].offset;
}

}
}

