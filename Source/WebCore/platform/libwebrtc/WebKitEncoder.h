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

#include "LibWebRTCVideoFrameUtilities.h"
#include "WebKitUtilities.h"

ALLOW_UNUSED_PARAMETERS_BEGIN
#include <webrtc/api/video_codecs/video_encoder.h>
#include <webrtc/api/video_codecs/video_encoder_factory.h>
#include <webrtc/api/video/encoded_image.h>
#include <webrtc/modules/include/module_common_types.h>
#include <webrtc/modules/video_coding/include/video_error_codes.h>
ALLOW_UNUSED_PARAMETERS_END

using CVPixelBufferRef = struct __CVBuffer*;

namespace webrtc {
class EncodedImageCallback;
class VideoCodec;
class VideoFrame;
struct SdpVideoFormat;
class Settings;
}

namespace WebCore {

std::unique_ptr<webrtc::VideoEncoderFactory> createWebKitEncoderFactory(WebKitH265, WebKitVP9, WebKitAv1);

using WebKitVideoEncoder = void*;
using VideoEncoderCreateCallback = WebKitVideoEncoder(*)(const webrtc::SdpVideoFormat& format);
using VideoEncoderReleaseCallback = int32_t(*)(WebKitVideoEncoder);
using VideoEncoderInitializeCallback = int32_t(*)(WebKitVideoEncoder, const webrtc::VideoCodec&);
using VideoEncoderEncodeCallback = int32_t(*)(WebKitVideoEncoder, const webrtc::VideoFrame&, bool shouldEncodeAsKeyFrame);
using VideoEncoderRegisterEncodeCompleteCallback = int32_t(*)(WebKitVideoEncoder, void* encodedImageCallback);
using VideoEncoderSetRatesCallback = void(*)(WebKitVideoEncoder, const webrtc::VideoEncoder::RateControlParameters&);

WEBCORE_EXPORT void setVideoEncoderCallbacks(VideoEncoderCreateCallback, VideoEncoderReleaseCallback, VideoEncoderInitializeCallback, VideoEncoderEncodeCallback, VideoEncoderRegisterEncodeCompleteCallback, VideoEncoderSetRatesCallback);

using WebKitEncodedFrameTiming = webrtc::EncodedImage::Timing;

enum class WebKitEncodedVideoRotation : uint8_t {
    kVideoRotation_0,
    kVideoRotation_90,
    kVideoRotation_180,
    kVideoRotation_270
};

struct WebKitEncodedFrameInfo {
    uint32_t width { 0 };
    uint32_t height { 0 };
    int64_t timeStamp { 0 };
    std::optional<uint64_t> duration;
    int64_t ntpTimeMS { 0 };
    int64_t captureTimeMS { 0 };
    webrtc::VideoFrameType frameType { webrtc::VideoFrameType::kVideoFrameDelta };
    WebKitEncodedVideoRotation rotation { WebKitEncodedVideoRotation::kVideoRotation_0 };
    webrtc::VideoContentType contentType { webrtc::VideoContentType::UNSPECIFIED };
    bool completeFrame = false;
    int qp { -1 };
    int temporalIndex { -1 };
    WebKitEncodedFrameTiming timing;
};

enum class LocalEncoderScalabilityMode : uint8_t { L1T1, L1T2 };

using LocalEncoder = void*;
using LocalEncoderCallback = void (^)(const uint8_t* buffer, size_t size, const WebKitEncodedFrameInfo&);
using LocalEncoderDescriptionCallback = void (^)(const uint8_t* buffer, size_t size);
using LocalEncoderErrorCallback = void (^)(bool isFrameDropped);
WEBCORE_EXPORT void* createLocalEncoder(const webrtc::SdpVideoFormat&, bool useAnnexB, LocalEncoderScalabilityMode, LocalEncoderCallback, LocalEncoderDescriptionCallback, LocalEncoderErrorCallback);
WEBCORE_EXPORT void releaseLocalEncoder(LocalEncoder);
WEBCORE_EXPORT void initializeLocalEncoder(LocalEncoder, uint16_t width, uint16_t height, unsigned int startBitrate, unsigned int maxBitrate, unsigned int minBitrate, uint32_t maxFramerate);
WEBCORE_EXPORT void encodeLocalEncoderFrame(LocalEncoder, CVPixelBufferRef, int64_t timeStampNs, int64_t timeStamp, std::optional<uint64_t> duration, webrtc::VideoRotation, bool isKeyframeRequired);
WEBCORE_EXPORT void setLocalEncoderRates(LocalEncoder, uint32_t bitRate, uint32_t frameRate);
WEBCORE_EXPORT void setLocalEncoderLowLatency(LocalEncoder, bool isLowLatencyEnabled);
WEBCORE_EXPORT void encoderVideoTaskComplete(void*, webrtc::VideoCodecType, const uint8_t* buffer, size_t length, const WebKitEncodedFrameInfo&);
WEBCORE_EXPORT void flushLocalEncoder(LocalEncoder);

}
