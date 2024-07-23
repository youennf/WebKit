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
#import "RTCVideoDecoderVP8.h"

#import "RTCWrappedNativeVideoDecoder.h"
#import <Foundation/Foundation.h>
ALLOW_UNUSED_PARAMETERS_BEGIN
#include <webrtc/api/environment/environment_factory.h>
#include <webrtc/modules/video_coding/codecs/vp8/include/vp8.h>
ALLOW_UNUSED_PARAMETERS_END

@implementation RTCVideoDecoderVP8

+ (id<RTCVideoDecoder>)vp8Decoder {
  return [[RTCWrappedNativeVideoDecoder alloc]
          initWithNativeDecoder:std::unique_ptr<webrtc::VideoDecoder>(webrtc::CreateVp8Decoder(webrtc::EnvironmentFactory().Create()))];
}

@end
