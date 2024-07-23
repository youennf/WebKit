/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 *
 */

#import "config.h"
#import "RTCVideoFrameReorderQueue.h"

namespace WebCore {

bool RTCVideoFrameReorderQueue::isEmpty()
{
    return m_reorderQueue.empty();
}

uint8_t RTCVideoFrameReorderQueue::reorderSize() const
{
    Locker locker(m_reorderQueueLock);
    return m_reorderSize;
}

void RTCVideoFrameReorderQueue::setReorderSize(uint8_t size)
{
    Locker locker(m_reorderQueueLock);
    m_reorderSize = size;
}

void RTCVideoFrameReorderQueue::append(RTCVideoFrame* frame, uint8_t reorderSize)
{
    Locker locker(m_reorderQueueLock);
    m_reorderQueue.push_back(std::make_unique<RTCVideoFrameWithOrder>(frame, reorderSize));
    std::sort(m_reorderQueue.begin(), m_reorderQueue.end(), [](auto& a, auto& b) {
        return a->timeStamp < b->timeStamp;
    });
}

RetainPtr<RTCVideoFrame> RTCVideoFrameReorderQueue::takeIfAvailable()
{
    Locker locker(m_reorderQueueLock);
    if (m_reorderQueue.size() && m_reorderQueue.size() > m_reorderQueue.front()->reorderSize) {
        auto frame = m_reorderQueue.front()->take();
        m_reorderQueue.pop_front();
        return frame;
    }
    return nil;
}

RetainPtr<RTCVideoFrame> RTCVideoFrameReorderQueue::takeIfAny()
{
    Locker locker(m_reorderQueueLock);
    if (m_reorderQueue.size()) {
        auto frame = m_reorderQueue.front()->take();
        m_reorderQueue.pop_front();
        return frame;
    }
    return nil;
}

}
