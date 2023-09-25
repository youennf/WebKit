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
#include "RubyFormattingContext.h"

#include "InlineLine.h"

namespace WebCore {
namespace Layout {

static inline size_t nextWrapOpportunityWithinRubyContainer(size_t startIndex, const InlineItemRange& rubyRange, const InlineItems& inlineItems)
{
    // We only allow wrap opportunity between ruby bases.
    for (size_t index = startIndex; index < rubyRange.endIndex(); ++index) {
        auto& rubyItem = inlineItems[index];
        if (rubyItem.isInlineBoxEnd() && rubyItem.layoutBox().isRubyBase()) {
            // We are at the end of a ruby base, need to check if we are between bases.
            if (index + 1 == rubyRange.endIndex()) {
                ASSERT_NOT_REACHED();
                continue;
            }
            auto& nextRubyItem = inlineItems[index + 1];
            if (nextRubyItem.isInlineBoxStart()) {
                ASSERT(nextRubyItem.layoutBox().isRubyBase());
                return index + 1;
            }
        }
    }
    return rubyRange.endIndex();
}

RubyFormattingContext::RubyFormattingContext(const InlineFormattingContext& parentFormattingContext)
    : m_parentFormattingContext(parentFormattingContext)
{
}

RubyFormattingContext::InlineLayoutResult RubyFormattingContext::layoutInlineAxis(const InlineItemRange& rubyRange, const InlineItems& inlineItems, Line& line, InlineLayoutUnit availableWidth)
{
    ASSERT(!rubyRange.isEmpty());
    // Ruby container inline item list is as follows:
    // [ruby container start][ruby base start][ruby base content][ruby base end][...][ruby container end]
    auto currentIndex = rubyRange.startIndex();
    while (currentIndex < rubyRange.endIndex()) {
        auto candidateContentEnd = nextWrapOpportunityWithinRubyContainer(currentIndex, rubyRange, inlineItems);
        auto contentLogicalWidth = logicaWidthForRubyRange({ currentIndex, candidateContentEnd }, inlineItems, line.contentLogicalRight());
        auto shouldPlaceRubyRange = contentLogicalWidth <= availableWidth || !line.hasContent();
        if (!shouldPlaceRubyRange)
            return { InlineContentBreaker::IsEndOfLine::Yes, currentIndex - rubyRange.startIndex() };
        placeRubyContent({ currentIndex, candidateContentEnd }, inlineItems, line);
        availableWidth -= contentLogicalWidth;
        currentIndex = candidateContentEnd;
    }
    return { availableWidth >= 0 ? InlineContentBreaker::IsEndOfLine::No : InlineContentBreaker::IsEndOfLine::Yes, currentIndex - rubyRange.startIndex() };
}

void RubyFormattingContext::placeRubyContent(WTF::Range<size_t> candidateRange, const InlineItems& inlineItems, Line& line)
{
    ASSERT(candidateRange.end() <= inlineItems.size());
    ASSERT(inlineItems[candidateRange.begin()].layoutBox().isRuby() || inlineItems[candidateRange.begin()].layoutBox().isRubyBase());
    auto& formattingGeometry = parentFormattingContext().formattingGeometry();

    auto index = candidateRange.begin();
    while (index < candidateRange.end()) {
        auto appendInlineBoxItem = [&](auto& inlineBoxItem) {
            ASSERT(inlineBoxItem.isInlineBoxStart() || inlineBoxItem.isInlineBoxEnd());
            ASSERT(inlineBoxItem.layoutBox().isRuby() || inlineBoxItem.layoutBox().isRubyBase());

            auto logicalWidth = formattingGeometry.inlineItemWidth(inlineBoxItem, line.contentLogicalRight(), { });
            line.append(inlineBoxItem, inlineBoxItem.style(), logicalWidth);
            ++index;
        };

        auto& rubyItem = inlineItems[index];
        auto& rubyLayoutBox = rubyItem.layoutBox();
        ASSERT(rubyItem.isInlineBoxStart() || rubyItem.isInlineBoxEnd());

        if (rubyLayoutBox.isRuby()) {
            appendInlineBoxItem(rubyItem);
            ASSERT(rubyItem.isInlineBoxStart() || (rubyItem.isInlineBoxEnd() && index == candidateRange.end()));
            continue;
        }
        if (rubyLayoutBox.isRubyBase()) {
            // ruby column: represented by a single ruby base and one ruby annotation
            // from each interlinear annotation level in its ruby segment.
            appendInlineBoxItem(rubyItem);
            if (rubyItem.isInlineBoxStart())
                index += layoutRubyBaseInlineAxis(line, rubyLayoutBox, index, inlineItems);
            continue;
        }
        ASSERT_NOT_REACHED();
    }
}

size_t RubyFormattingContext::layoutRubyBaseInlineAxis(Line& line, const Box& rubyBaseLayoutBox, size_t rubyBaseContentStartIndex, const InlineItems& inlineItems)
{
    // Append ruby base content (including start/end inline box) to the line and apply "ruby-align: space-around" on the ruby subrange.
    auto& formattingGeometry = parentFormattingContext().formattingGeometry();
    auto lineLogicalRight = line.contentLogicalRight();
    auto baseContentLogicalWidth = InlineLayoutUnit { };
    auto baseRunStart = line.runs().size();

    for (size_t index = rubyBaseContentStartIndex; index < inlineItems.size(); ++index) {
        auto& rubyBaseInlineItem = inlineItems[index];
        if (&rubyBaseInlineItem.layoutBox() == &rubyBaseLayoutBox) {
            auto baseRunCount = line.runs().size() - baseRunStart;
            if (baseRunCount)
                applyRubyAlign(line, { baseRunStart, baseRunStart + baseRunCount }, rubyBaseLayoutBox, baseContentLogicalWidth);
            return index - rubyBaseContentStartIndex;
        }
        auto logicalWidth = formattingGeometry.inlineItemWidth(rubyBaseInlineItem, lineLogicalRight + baseContentLogicalWidth, { });
        line.append(rubyBaseInlineItem, rubyBaseInlineItem.style(), logicalWidth);
        baseContentLogicalWidth += logicalWidth;
    }
    ASSERT_NOT_REACHED();
    return inlineItems.size() - rubyBaseContentStartIndex;
}

InlineLayoutPoint RubyFormattingContext::annotationPosition(const Box& rubyBaseLayoutBox)
{
    ASSERT(rubyBaseLayoutBox.isRubyBase());
    auto* annotationBox = rubyBaseLayoutBox.associatedRubyAnnotationBox();
    if (!annotationBox) {
        ASSERT_NOT_REACHED();
        return { };
    }
    auto annotationPosition = annotationBox->style().rubyPosition();
    if (annotationPosition == RubyPosition::InterCharacter) {
        auto rubyBaseMarginBox = BoxGeometry::marginBoxRect(parentFormattingContext().geometryForBox(rubyBaseLayoutBox));
        return { rubyBaseMarginBox.right(), { } };
    }
    auto annotationMarginBoxHeight = InlineLayoutUnit { parentFormattingContext().geometryForBox(*annotationBox).marginBoxHeight() };
    if (annotationPosition == RubyPosition::Before)
        return { { }, -annotationMarginBoxHeight };
    return { { }, parentFormattingContext().geometryForBox(rubyBaseLayoutBox).marginBoxHeight() };
}

RubyFormattingContext::OverUnder RubyFormattingContext::annotationVerticalExtent(const Box& rubyBaseLayoutBox)
{
    auto* annotationBox = rubyBaseLayoutBox.associatedRubyAnnotationBox();
    if (!annotationBox)
        return { };
    auto rubyPosition = annotationBox->style().rubyPosition();
    if (rubyPosition == RubyPosition::InterCharacter)
        return { };
    auto annotationBoxLogicalHeight = InlineLayoutUnit { parentFormattingContext().geometryForBox(*annotationBox).marginBoxHeight() };
    if (rubyPosition == RubyPosition::Before)
        return { annotationBoxLogicalHeight, { } };
    return { { }, annotationBoxLogicalHeight };
}

static inline InlineLayoutUnit halfOfAFullWidthCharacter(const Box& annotationBox)
{
    return annotationBox.style().computedFontSize() / 2.f;
}

InlineLayoutUnit RubyFormattingContext::overhangForAnnotationBefore(const Box& rubyBaseLayoutBox, size_t rubyBaseStartIndex, const InlineDisplay::Boxes& boxes)
{
    // [root inline box][ruby container][ruby base][ruby annotation]
    ASSERT(rubyBaseStartIndex >= 2);
    auto* annotationBox = rubyBaseLayoutBox.associatedRubyAnnotationBox();
    if (!annotationBox || annotationBox->style().rubyPosition() == RubyPosition::InterCharacter || rubyBaseStartIndex <= 2)
        return { };
    auto overhangValue = halfOfAFullWidthCharacter(*annotationBox);
    auto wouldAnnotationOverlap = [&] {
        // Check of adjacent (previous) content for overlapping.
        auto annotationMarginBoxRect = BoxGeometry::marginBoxRect(parentFormattingContext().geometryForBox(*annotationBox));
        auto overhangingRect = InlineLayoutRect { annotationMarginBoxRect.left() - overhangValue, annotationMarginBoxRect.top(), annotationMarginBoxRect.width(), annotationMarginBoxRect.height() };

        for (size_t index = rubyBaseStartIndex - 1; index > 0; --index) {
            if (auto wouldOverlap = annotationOverlapCheck(boxes[index], overhangingRect))
                return *wouldOverlap;
        }
        return true;
    };
    return wouldAnnotationOverlap() ? 0.f : overhangValue;
}

InlineLayoutUnit RubyFormattingContext::overhangForAnnotationAfter(const Box& rubyBaseLayoutBox, size_t rubyBaseContentEndIndex, const InlineDisplay::Boxes& boxes)
{
    auto* annotationBox = rubyBaseLayoutBox.associatedRubyAnnotationBox();
    if (!annotationBox || annotationBox->style().rubyPosition() == RubyPosition::InterCharacter || rubyBaseContentEndIndex == boxes.size() -1)
        return { };
    auto overhangValue = halfOfAFullWidthCharacter(*annotationBox);
    auto wouldAnnotationOverlap = [&] {
        // Check of adjacent (previous) content for overlapping.
        auto annotationMarginBoxRect = BoxGeometry::marginBoxRect(parentFormattingContext().geometryForBox(*annotationBox));
        auto overhangingRect = InlineLayoutRect { annotationMarginBoxRect.left(), annotationMarginBoxRect.top(), annotationMarginBoxRect.width() + overhangValue, annotationMarginBoxRect.height() };

        for (size_t index = rubyBaseContentEndIndex + 1; index < boxes.size(); ++index) {
            if (auto wouldOverlap = annotationOverlapCheck(boxes[index], overhangingRect))
                return *wouldOverlap;
        }
        return true;
    };
    return wouldAnnotationOverlap() ? 0.f : overhangValue;
}

std::optional<size_t> RubyFormattingContext::nextWrapOpportunity(size_t inlineItemIndex, std::optional<size_t> previousInlineItemIndex, const InlineItemRange& layoutRange, const InlineItems& inlineItems)
{
    auto& inlineItem = inlineItems[inlineItemIndex];
    ASSERT(inlineItem.isInlineBoxStart() || inlineItem.isInlineBoxEnd());

    auto& layoutBox = inlineItem.layoutBox();
    auto startsWithRubyInlineBox = layoutBox.isRuby() || layoutBox.isRubyBase();
    if (!startsWithRubyInlineBox) {
        // This is not ruby content.
        return { };
    }
    if (previousInlineItemIndex && startsWithRubyInlineBox) {
        // There's always a soft wrap opportunitiy before <ruby>/ruby base.
        return inlineItemIndex;
    }
    // Skip to the end of the ruby container.
    for (; inlineItemIndex < layoutRange.endIndex() && (!inlineItems[inlineItemIndex].isInlineBoxEnd() || !inlineItems[inlineItemIndex].layoutBox().isRuby()); ++inlineItemIndex) { }
    ASSERT(inlineItemIndex < layoutRange.endIndex());
    return inlineItemIndex + 1;
}

void RubyFormattingContext::applyRubyAlign(Line& line, WTF::Range<size_t> baseRunRange, const Box& rubyBaseLayoutBox, InlineLayoutUnit baseContentLogicalWidth)
{
    // https://drafts.csswg.org/css-ruby/#interlinear-inline
    // Within each base and annotation box, how the extra space is distributed when its content is narrower than
    // the measure of the box is specified by its ruby-align property.
    auto* annotationBox = rubyBaseLayoutBox.associatedRubyAnnotationBox();
    if (!annotationBox)
        return;
    auto annotationBoxLogicalWidth = InlineLayoutUnit { parentFormattingContext().geometryForBox(*annotationBox).marginBoxWidth() };
    if (annotationBoxLogicalWidth <= baseContentLogicalWidth)
        return;
    auto expansion = ExpansionInfo { };
    // ruby-align: space-around
    // As for space-between except that there exists an extra justification opportunities whose space is distributed half before and half after the ruby content.
    TextUtil::computedExpansions(line.runs(), baseRunRange, { }, expansion);
    if (expansion.opportunityCount) {
        auto extraSpace = annotationBoxLogicalWidth - baseContentLogicalWidth;
        auto baseContentOffset = extraSpace / (expansion.opportunityCount + 1) / 2;
        line.expandBy(baseRunRange.begin() - 1, baseContentOffset);
        extraSpace -= (2 * baseContentOffset);
        line.applyExpansionOnRange(baseRunRange, expansion, extraSpace);
        line.expandBy(baseRunRange.end() - 1, baseContentOffset);
    } else {
        auto centerOffset = (annotationBoxLogicalWidth - baseContentLogicalWidth) / 2;
        line.moveRunsBy(baseRunRange.begin(), centerOffset);
        line.expandBy(baseRunRange.begin(), centerOffset);
    }
}

std::optional<bool> RubyFormattingContext::annotationOverlapCheck(const InlineDisplay::Box& adjacentDisplayBox, const InlineLayoutRect& overhangingRect) const
{
    // We are in the middle of a line, should not see any line breaks or ellipsis boxes here.
    ASSERT(adjacentDisplayBox.isText() || adjacentDisplayBox.isAtomicInlineLevelBox() || adjacentDisplayBox.isInlineBox() || adjacentDisplayBox.isGenericInlineLevelBox() || adjacentDisplayBox.isWordSeparator());
    // Skip empty content like <span></span>
    if (adjacentDisplayBox.visualRectIgnoringBlockDirection().isEmpty())
        return { };
    if (adjacentDisplayBox.inkOverflow().intersects(overhangingRect))
        return true;
    auto& adjacentLayoutBox = adjacentDisplayBox.layoutBox();
    // Check if there might be some inline box (end decoration) overlapping as previous content.
    if (&adjacentLayoutBox.parent() == &parentFormattingContext().root())
        return false;
    if (adjacentLayoutBox.isRubyBase() && adjacentLayoutBox.associatedRubyAnnotationBox()) {
        auto annotationMarginBoxRect = InlineLayoutRect { BoxGeometry::marginBoxRect(parentFormattingContext().geometryForBox(*adjacentLayoutBox.associatedRubyAnnotationBox())) };
        if (annotationMarginBoxRect.intersects(overhangingRect))
            return true;
    }
    // FIXME: We should not let the neighboring content overlap with the base content either (currently base is sized to the annotation if shorter as we don't have the
    // inline box equivalent of ruby column).
    return { };
}

InlineLayoutUnit RubyFormattingContext::logicaWidthForRubyRange(WTF::Range<size_t> candidateRange, const InlineItems& inlineItems, InlineLayoutUnit lineContentLogicalRight) const
{
    ASSERT(candidateRange.end() <= inlineItems.size());
    ASSERT(inlineItems[candidateRange.begin()].layoutBox().isRuby() || inlineItems[candidateRange.begin()].layoutBox().isRubyBase());

    auto& formattingGeometry = parentFormattingContext().formattingGeometry();
    auto candidateContentLogicalWidth = InlineLayoutUnit { };
    auto index = candidateRange.begin();

    while (index < candidateRange.end()) {
        auto& rubyItem = inlineItems[index];
        auto& rubyLayoutBox = rubyItem.layoutBox();

        if (rubyLayoutBox.isRuby()) {
            ASSERT(rubyItem.isInlineBoxStart() || rubyItem.isInlineBoxEnd());
            candidateContentLogicalWidth += formattingGeometry.inlineItemWidth(rubyItem, lineContentLogicalRight + candidateContentLogicalWidth, { });
            ++index;
            continue;
        }

        if (rubyLayoutBox.isRubyBase()) {
            ASSERT(rubyItem.isInlineBoxStart());

            auto annotationMarginBoxWidth = [&]() -> InlineLayoutUnit {
                if (auto* annotationBox = rubyLayoutBox.associatedRubyAnnotationBox())
                    return InlineLayoutUnit { parentFormattingContext().geometryForBox(*annotationBox).marginBoxWidth() };
                return { };
            };

            auto baseLogicalWidth = [&] {
                // Base content needs special handling with taking annotation box into account.
                auto logicalWidth = InlineLayoutUnit { };
                for (; index < candidateRange.end(); ++index) {
                    auto& baseInlineItem = inlineItems[index];
                    logicalWidth += formattingGeometry.inlineItemWidth(baseInlineItem, lineContentLogicalRight + logicalWidth, { });
                    if (&baseInlineItem.layoutBox() == &rubyLayoutBox && baseInlineItem.isInlineBoxEnd()) {
                        // End of base.
                        ++index;
                        return logicalWidth;
                    }
                }
                ASSERT_NOT_REACHED();
                return InlineLayoutUnit { };
            };
            candidateContentLogicalWidth += std::max(baseLogicalWidth(), annotationMarginBoxWidth());
            continue;
        }
        ASSERT_NOT_REACHED();
    }
    return candidateContentLogicalWidth;
}

}
}

