/*
 * Copyright (C) 2020-2021 Apple Inc. All rights reserved.
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

#include <Availability.h>
#include <TargetConditionals.h>

#include "LibWebRTCVideoFrameUtilities.h"
#include "WebKitUtilities.h"

ALLOW_UNUSED_PARAMETERS_BEGIN
ALLOW_COMMA_BEGIN

#include <webrtc/api/video/encoded_image.h>
#include <webrtc/rtc_base/ref_counted_object.h>

ALLOW_COMMA_END
ALLOW_UNUSED_PARAMETERS_END

typedef struct __CVBuffer* CVPixelBufferRef;

namespace webrtc {
struct SdpVideoFormat;
class VideoDecoderFactory;
}

namespace WebCore {

#if (TARGET_OS_OSX || TARGET_OS_MACCATALYST) && TARGET_CPU_X86_64
    #define CMBASE_OBJECT_NEEDS_ALIGNMENT 1
#else
    #define CMBASE_OBJECT_NEEDS_ALIGNMENT 0
#endif

struct WebKitVideoDecoder {
    using Value = void*;
    Value value { nullptr };
    bool isWebRTCVideoDecoder { false };
};
using VideoDecoderCreateCallback = WebKitVideoDecoder(*)(const webrtc::SdpVideoFormat& format);
using VideoDecoderReleaseCallback = int32_t(*)(WebKitVideoDecoder::Value);
using VideoDecoderDecodeCallback = int32_t(*)(WebKitVideoDecoder::Value, uint32_t timeStamp, const uint8_t*, size_t length, uint16_t width, uint16_t height);
using VideoDecoderRegisterDecodeCompleteCallback = int32_t(*)(WebKitVideoDecoder::Value, void* decodedImageCallback);

WEBCORE_EXPORT void setVideoDecoderCallbacks(VideoDecoderCreateCallback, VideoDecoderReleaseCallback, VideoDecoderDecodeCallback, VideoDecoderRegisterDecodeCompleteCallback);

std::unique_ptr<webrtc::VideoDecoderFactory> createWebKitDecoderFactory(WebKitH265, WebKitVP9, WebKitVP9VTB, WebKitAv1);
WEBCORE_EXPORT void videoDecoderTaskComplete(void* callback, uint32_t timeStamp, uint32_t timeStampRTP, CVPixelBufferRef);
WEBCORE_EXPORT void videoDecoderTaskComplete(void* callback, uint32_t timeStamp, uint32_t timeStampRTP, void*, GetBufferCallback, ReleaseBufferCallback, int width, int height);

using LocalDecoder = void*;
using LocalDecoderCallback = void (^)(CVPixelBufferRef, int64_t timeStamp, int64_t timeStampNs);
WEBCORE_EXPORT void* createLocalH264Decoder(LocalDecoderCallback);
WEBCORE_EXPORT void* createLocalH265Decoder(LocalDecoderCallback);
WEBCORE_EXPORT void* createLocalVP9Decoder(LocalDecoderCallback);
WEBCORE_EXPORT void releaseLocalDecoder(LocalDecoder);
WEBCORE_EXPORT void flushLocalDecoder(LocalDecoder);
WEBCORE_EXPORT int32_t setDecodingFormat(LocalDecoder, const uint8_t*, size_t, uint16_t width, uint16_t height);
WEBCORE_EXPORT int32_t decodeFrame(LocalDecoder, int64_t timeStamp, const uint8_t*, size_t);
WEBCORE_EXPORT void setDecoderFrameSize(LocalDecoder, uint16_t width, uint16_t height);

class WebKitEncodedImageBufferWrapper : public webrtc::EncodedImageBufferInterface {
public:
    static rtc::scoped_refptr<WebKitEncodedImageBufferWrapper> create(uint8_t* data, size_t size) { return rtc::make_ref_counted<WebKitEncodedImageBufferWrapper>(data, size); }

    WebKitEncodedImageBufferWrapper(uint8_t* data, size_t size)
        : m_data(data)
        , m_size(size)
    {
    }

    const uint8_t* data() const final { return m_data; }
    uint8_t* data() final { return m_data; }
    size_t size() const final { return m_size; }

 private:
    uint8_t* m_data { nullptr };
    size_t m_size { 0 };
};

}
