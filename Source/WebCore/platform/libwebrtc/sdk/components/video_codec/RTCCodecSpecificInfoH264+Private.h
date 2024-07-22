/*
 *  Copyright 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "LibWebRTCMacros.h"
#import "RTCCodecSpecificInfoH264.h"

ALLOW_UNUSED_PARAMETERS_BEGIN
#include <webrtc/modules/video_coding/include/video_codec_interface.h>
ALLOW_UNUSED_PARAMETERS_END

NS_ASSUME_NONNULL_BEGIN

/* Interfaces for converting to/from internal C++ formats. */
@interface RTCCodecSpecificInfoH264 ()

- (webrtc::CodecSpecificInfo)nativeCodecSpecificInfo;

@end

NS_ASSUME_NONNULL_END
