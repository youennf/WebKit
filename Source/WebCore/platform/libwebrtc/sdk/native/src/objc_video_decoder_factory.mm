/*
 *  Copyright 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "config.h"
#include "objc_video_decoder_factory.h"

#import "RTCVideoDecoder.h"
#import "RTCVideoDecoderFactory.h"
#import "RTCVideoFrame.h"
#import "RTCVideoFrameBuffer.h"
#import "RTCCodecSpecificInfoH264.h"
#import "objc_frame_buffer.h"
#import "RTCEncodedImage+Private.h"
#import "RTCVideoCodecInfo+Private.h"
#import "RTCWrappedNativeVideoDecoder.h"
#import "NSString+StdString.h"

ALLOW_UNUSED_PARAMETERS_BEGIN
#include <webrtc/api/make_ref_counted.h>
#include <webrtc/api/video_codecs/sdp_video_format.h>
#include <webrtc/api/video_codecs/video_decoder.h>
#include <webrtc/modules/video_coding/include/video_codec_interface.h>
#include <webrtc/modules/video_coding/include/video_error_codes.h>
#include <webrtc/rtc_base/logging.h>
#include <webrtc/rtc_base/time_utils.h>
ALLOW_UNUSED_PARAMETERS_END

namespace webrtc {

static std::string fromNSString(NSString *nsString)
{
    NSData *charData = [nsString dataUsingEncoding:NSUTF8StringEncoding];
    return { reinterpret_cast<const char *>(charData.bytes), charData.length };
}

namespace {
class ObjCVideoDecoder : public VideoDecoder {
 public:
  ObjCVideoDecoder(id<RTCVideoDecoder> decoder)
      : decoder_(decoder), implementation_name_(fromNSString([decoder implementationName])) {}

  bool Configure(const Settings& settings) override {
    return [decoder_ startDecodeWithNumberOfCores:settings.number_of_cores()] == WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Decode(const EncodedImage &input_image,
                 bool missing_frames,
                 int64_t render_time_ms = -1) override {
    RTCEncodedImage *encodedImage =
        [[RTCEncodedImage alloc] initWithNativeEncodedImage:input_image];

    return [decoder_ decode:encodedImage
              missingFrames:missing_frames
          codecSpecificInfo:nil
               renderTimeMs:render_time_ms];
  }

  int32_t RegisterDecodeCompleteCallback(DecodedImageCallback *callback) override {
    [decoder_ setCallback:^(RTCVideoFrame *frame) {
      const rtc::scoped_refptr<VideoFrameBuffer> buffer =
          rtc::make_ref_counted<ObjCFrameBuffer>(frame.buffer);
      VideoFrame videoFrame =
          VideoFrame::Builder()
              .set_video_frame_buffer(buffer)
              .set_timestamp_rtp((uint32_t)(frame.timeStampNs / rtc::kNumNanosecsPerMicrosec))
              .set_timestamp_ms(0)
              .set_rotation((VideoRotation)frame.rotation)
              .build();
      videoFrame.set_timestamp(frame.timeStamp);

      callback->Decoded(videoFrame);
    }];

    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Release() override { return [decoder_ releaseDecoder]; }

  const char *ImplementationName() const override { return implementation_name_.c_str(); }

 private:
  id<RTCVideoDecoder> decoder_;
  const std::string implementation_name_;
};
}  // namespace

ObjCVideoDecoderFactory::ObjCVideoDecoderFactory(id<RTCVideoDecoderFactory> decoder_factory)
    : decoder_factory_(decoder_factory) {}

ObjCVideoDecoderFactory::~ObjCVideoDecoderFactory() {}

id<RTCVideoDecoderFactory> ObjCVideoDecoderFactory::wrapped_decoder_factory() const {
  return decoder_factory_;
}

ObjCVideoDecoderFactory::CodecSupport ObjCVideoDecoderFactory::QueryCodecSupport(const SdpVideoFormat& format, bool /* reference_scaling */) const {
    CodecSupport codec_support;
    codec_support.is_supported = format.IsCodecInList(GetSupportedFormats());
    return codec_support;
}

std::unique_ptr<VideoDecoder> ObjCVideoDecoderFactory::Create(
    const Environment& /* environment */,
    const SdpVideoFormat &format) {
  NSString *codecName = [NSString stringWithUTF8String:format.name.c_str()];
  for (RTCVideoCodecInfo *codecInfo in decoder_factory_.supportedCodecs) {
    if ([codecName isEqualToString:codecInfo.name]) {
      id<RTCVideoDecoder> decoder = [decoder_factory_ createDecoder:codecInfo];

      // Because of symbol conflict, isKindOfClass doesn't work as expected.
      // See https://bugs.webkit.org/show_bug.cgi?id=198782.
      // if ([decoder isKindOfClass:[RTCWrappedNativeVideoDecoder class]]) {
#if !defined(WEBRTC_WEBKIT_BUILD)
        if ([codecName isEqual:@"VP8"] || [codecName isEqual:@"VP9"]) {
#else
        if (![decoder.implementationName isEqual:@"VideoToolbox"]) {
#endif
        return [(RTCWrappedNativeVideoDecoder *)decoder releaseWrappedDecoder];
      } else {
        return std::unique_ptr<ObjCVideoDecoder>(new ObjCVideoDecoder(decoder));
      }
    }
  }

  return nullptr;
}

std::vector<SdpVideoFormat> ObjCVideoDecoderFactory::GetSupportedFormats() const {
  std::vector<SdpVideoFormat> supported_formats;
  for (RTCVideoCodecInfo *supportedCodec in decoder_factory_.supportedCodecs) {
    SdpVideoFormat format = [supportedCodec nativeSdpVideoFormat];
    supported_formats.push_back(format);
  }

  return supported_formats;
}

}  // namespace webrtc
