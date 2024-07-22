/*
 * Copyright (C) 2022 Apple Inc. All rights reserved.
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
#include "WebRTCVideoDecoder.h"

#if USE(LIBWEBRTC)

#import "RTCVideoDecoderVTBAV1.h"
#import "WebKitDecoder.h"

namespace WebCore {

class WebRTCLocalVideoDecoder final : public WebRTCVideoDecoder {
    WTF_MAKE_FAST_ALLOCATED;
public:
    explicit WebRTCLocalVideoDecoder(WebCore::LocalDecoder decoder)
        : m_decoder(decoder)
    {
    }
    ~WebRTCLocalVideoDecoder()
    {
        WebCore::releaseLocalDecoder(m_decoder);
    }

private:
    void flush() final { WebCore::flushLocalDecoder(m_decoder); }
    void setFormat(std::span<const uint8_t> data, uint16_t width, uint16_t height) final { WebCore::setDecodingFormat(m_decoder, data.data(), data.size(), width, height); }
    int32_t decodeFrame(int64_t timeStamp, std::span<const uint8_t> data) final { return WebCore::decodeFrame(m_decoder, timeStamp, data.data(), data.size()); }
    void setFrameSize(uint16_t width, uint16_t height) final { WebCore::setDecoderFrameSize(m_decoder, width, height); }

    WebCore::LocalDecoder m_decoder;
};

UniqueRef<WebRTCVideoDecoder> WebRTCVideoDecoder::createFromLocalDecoder(WebCore::LocalDecoder decoder)
{
    return makeUniqueRef<WebRTCLocalVideoDecoder>(decoder);
}

class WebRTCDecoderVTBAV1 final : public WebRTCVideoDecoder {
    WTF_MAKE_FAST_ALLOCATED;
public:
    explicit WebRTCDecoderVTBAV1(RTCVideoDecoderVTBAV1Callback callback)
        : m_decoder(adoptNS([[RTCVideoDecoderVTBAV1 alloc] init]))
    {
        [m_decoder setCallback:callback];
    }

    ~WebRTCDecoderVTBAV1()
    {
        [m_decoder releaseDecoder];
    }

private:
    void flush() final { [m_decoder flush]; }
    void setFormat(std::span<const uint8_t>, uint16_t width, uint16_t height) final { setFrameSize(width, height); }
    int32_t decodeFrame(int64_t timeStamp, std::span<const uint8_t> data) final { return [m_decoder decodeData:data.data() size:data.size() timeStamp:timeStamp]; }
    void setFrameSize(uint16_t width, uint16_t height) final { [m_decoder setWidth:width height:height]; }

    RetainPtr<RTCVideoDecoderVTBAV1> m_decoder;
};

UniqueRef<WebRTCVideoDecoder> createAV1VTBDecoder(RTCVideoDecoderVTBAV1Callback callback)
{
    return makeUniqueRef<WebRTCDecoderVTBAV1>(callback);
}

}

#endif //  USE(LIBWEBRTC)
