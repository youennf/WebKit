/*
 *  Copyright (c) 2021 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 *
 */

#import "config.h"
#import "RTCVideoEncoderAV1.h"

#import "RTCMacros.h"
#import "RTCWrappedNativeVideoEncoder.h"
#import <Foundation/Foundation.h>
ALLOW_UNUSED_PARAMETERS_BEGIN
#import <webrtc/api/environment/environment_factory.h>
#import <webrtc/modules/video_coding/codecs/av1/libaom_av1_encoder.h>
ALLOW_UNUSED_PARAMETERS_END

@implementation RTCVideoEncoderAV1

+ (id<RTCVideoEncoder>)av1Encoder {
  std::unique_ptr<webrtc::VideoEncoder> nativeEncoder(webrtc::CreateLibaomAv1Encoder(webrtc::EnvironmentFactory().Create()));
  if (nativeEncoder == nullptr) {
    return nil;
  }
  return [[RTCWrappedNativeVideoEncoder alloc]
      initWithNativeEncoder:std::move(nativeEncoder)];
}

@end
