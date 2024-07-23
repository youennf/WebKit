/*
 *  Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 *
 */

#import "config.h"
#import "RTCVideoEncoderVP8.h"

#import "RTCWrappedNativeVideoEncoder.h"
#import <Foundation/Foundation.h>
ALLOW_UNUSED_PARAMETERS_BEGIN
#import <webrtc/api/environment/environment_factory.h>
#import <webrtc/modules/video_coding/codecs/vp8/include/vp8.h>
ALLOW_UNUSED_PARAMETERS_END

@implementation RTCVideoEncoderVP8

+ (id<RTCVideoEncoder>)vp8Encoder {
  return [[RTCWrappedNativeVideoEncoder alloc]
          initWithNativeEncoder:std::unique_ptr<webrtc::VideoEncoder>(webrtc::CreateVp8Encoder(webrtc::EnvironmentFactory().Create()))];
}

@end
