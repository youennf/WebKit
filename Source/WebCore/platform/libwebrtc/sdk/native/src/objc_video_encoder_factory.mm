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
#include "objc_video_encoder_factory.h"

#include <string>

#import "LibWebRTCMacros.h"
#import "NSString+StdString.h"
#import "RTCVideoEncoder.h"
#import "RTCVideoEncoderFactory.h"
#import "RTCCodecSpecificInfoH264+Private.h"
#import "RTCCodecSpecificInfoH265+Private.h"
#import "RTCEncodedImage+Private.h"
#import "RTCVideoCodecInfo+Private.h"
#import "RTCVideoEncoderSettings+Private.h"
#import "RTCVideoCodecConstants.h"
#import "RTCWrappedNativeVideoEncoder.h"
#import "objc_video_frame.h"

ALLOW_UNUSED_PARAMETERS_BEGIN
#import <webrtc/api/video/video_frame.h>
#import <webrtc/api/video_codecs/sdp_video_format.h>
#import <webrtc/api/video_codecs/video_encoder.h>
#import <webrtc/modules/include/module_common_types.h>
#import <webrtc/modules/video_coding/include/video_codec_interface.h>
#import <webrtc/modules/video_coding/include/video_error_codes.h>
#import <webrtc/modules/video_coding/utility/simulcast_utility.h>
#import <webrtc/rtc_base/logging.h>
ALLOW_UNUSED_PARAMETERS_END

namespace webrtc {

namespace {

static std::string fromNSString(NSString *nsString)
{
    NSData *charData = [nsString dataUsingEncoding:NSUTF8StringEncoding];
    return { reinterpret_cast<const char *>(charData.bytes), charData.length };
}

class ObjCVideoEncoder : public VideoEncoder {
 public:
  ObjCVideoEncoder(id<RTCVideoEncoder> encoder)
      : encoder_(encoder), implementation_name_(fromNSString([encoder implementationName])) {}

  int32_t InitEncode(const VideoCodec *codec_settings, const Settings &encoder_settings) override {
    int number_of_streams = SimulcastUtility::NumberOfSimulcastStreams(*codec_settings);
    if (number_of_streams > 1) {
      return WEBRTC_VIDEO_CODEC_ERR_SIMULCAST_PARAMETERS_NOT_SUPPORTED;
    }
    RTCVideoEncoderSettings *settings =
        [[RTCVideoEncoderSettings alloc] initWithNativeVideoCodec:codec_settings];
    return [encoder_ startEncodeWithSettings:settings
                               numberOfCores:encoder_settings.number_of_cores];
  }

  int32_t RegisterEncodeCompleteCallback(EncodedImageCallback *callback) override {
    [encoder_ setCallback:^BOOL(RTCEncodedImage *_Nonnull frame,
                                id<RTCCodecSpecificInfo> _Nonnull info,
                                RTCRtpFragmentationHeader *_Nonnull /* header */) {
      if (!callback)
        return WEBRTC_VIDEO_CODEC_OK;
      EncodedImage encodedImage = [frame nativeEncodedImage];

      // Handle types that can be converted into one of CodecSpecificInfo's hard coded cases.
      CodecSpecificInfo codecSpecificInfo;
      // Because of symbol conflict, isKindOfClass doesn't work as expected.
      // See https://bugs.webkit.org/show_bug.cgi?id=198782.
      if ([NSStringFromClass([info class]) isEqual:@"WebCodecSpecificInfoH264"]) {
        // if ([info isKindOfClass:[RTCCodecSpecificInfoH264 class]]) {
        codecSpecificInfo = [(RTCCodecSpecificInfoH264 *)info nativeCodecSpecificInfo];
#ifndef DISABLE_H265
      } else if ([NSStringFromClass([info class]) isEqual:@"WebCodecSpecificInfoH265"]) {
        // if ([info isKindOfClass:[RTCCodecSpecificInfoH265 class]]) {
        codecSpecificInfo = [(RTCCodecSpecificInfoH265 *)info nativeCodecSpecificInfo];
#endif
      }

      EncodedImageCallback::Result res =
          callback->OnEncodedImage(encodedImage, &codecSpecificInfo);
      return res.error == EncodedImageCallback::Result::OK;
    }];

    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Release() override { return [encoder_ releaseEncoder]; }

  int32_t Encode(const VideoFrame &frame,
                 const std::vector<VideoFrameType> *frame_types) override {
    NSMutableArray<NSNumber *> *rtcFrameTypes = [NSMutableArray array];
    for (size_t i = 0; i < frame_types->size(); ++i) {
      [rtcFrameTypes addObject:@(RTCFrameType(frame_types->at(i)))];
    }

    return [encoder_ encode:webrtc::ToObjCVideoFrame(frame)
          codecSpecificInfo:nil
                 frameTypes:rtcFrameTypes];
  }

  void SetRates(const RateControlParameters &parameters) override {
    const uint32_t bitrate = parameters.bitrate.get_sum_kbps();
    const uint32_t framerate = static_cast<uint32_t>(parameters.framerate_fps + 0.5);
    [encoder_ setBitrate:bitrate framerate:framerate];
  }

  VideoEncoder::EncoderInfo GetEncoderInfo() const override {
    EncoderInfo info;
    info.supports_native_handle = true;
    info.implementation_name = implementation_name_;

    RTCVideoEncoderQpThresholds *qp_thresholds = [encoder_ scalingSettings];
    info.scaling_settings = qp_thresholds ? ScalingSettings(qp_thresholds.low, qp_thresholds.high) :
                                            ScalingSettings::kOff;

    info.is_hardware_accelerated = true;
    return info;
  }

 private:
  id<RTCVideoEncoder> encoder_;
  const std::string implementation_name_;
};
}  // namespace

ObjCVideoEncoderFactory::ObjCVideoEncoderFactory(id<RTCVideoEncoderFactory> encoder_factory)
    : encoder_factory_(encoder_factory) {}

ObjCVideoEncoderFactory::~ObjCVideoEncoderFactory() {}

id<RTCVideoEncoderFactory> ObjCVideoEncoderFactory::wrapped_encoder_factory() const {
  return encoder_factory_;
}

std::vector<SdpVideoFormat> ObjCVideoEncoderFactory::GetSupportedFormats() const {
  std::vector<SdpVideoFormat> supported_formats;
  for (RTCVideoCodecInfo *supportedCodec in [encoder_factory_ supportedCodecs]) {
    SdpVideoFormat format = [supportedCodec nativeSdpVideoFormat];
    supported_formats.push_back(format);
  }

  return supported_formats;
}

std::vector<SdpVideoFormat> ObjCVideoEncoderFactory::GetImplementations() const {
  if ([encoder_factory_ respondsToSelector:SEL("implementations")]) {
    std::vector<SdpVideoFormat> supported_formats;
    for (RTCVideoCodecInfo *supportedCodec in [encoder_factory_ implementations]) {
      SdpVideoFormat format = [supportedCodec nativeSdpVideoFormat];
      supported_formats.push_back(format);
    }
    return supported_formats;
  }
  return GetSupportedFormats();
}

std::unique_ptr<VideoEncoder> ObjCVideoEncoderFactory::Create(
    const Environment& /* environment */,
    const SdpVideoFormat &format) {
  RTCVideoCodecInfo *info = [[RTCVideoCodecInfo alloc] initWithNativeSdpVideoFormat:format];
  id<RTCVideoEncoder> encoder = [encoder_factory_ createEncoder:info];
  // Because of symbol conflict, isKindOfClass doesn't work as expected.
  // See https://bugs.webkit.org/show_bug.cgi?id=198782.
  // if ([encoder isKindOfClass:[RTCWrappedNativeVideoEncoder class]]) {
  if ([info.name isEqual:@"VP8"] || [info.name isEqual:@"VP9"] || [info.name isEqual:@"AV1"]) {
    return [(RTCWrappedNativeVideoEncoder *)encoder releaseWrappedEncoder];
  } else {
    return std::unique_ptr<ObjCVideoEncoder>(new ObjCVideoEncoder(encoder));
  }
}

}  // namespace webrtc
