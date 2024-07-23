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
#import "RTCVideoDecoderH264.h"

#import "LibWebRTCMacros.h"
#import "RTCVideoFrameReorderQueue.h"
#import "RTCVideoFrame.h"
#import "RTCVideoFrameBuffer.h"
#import "RTCCVPixelBuffer.h"
#import "helpers.h"
#import "nalu_rewriter.h"
#import <queue>
#import <wtf/RetainPtr.h>

ALLOW_UNUSED_PARAMETERS_BEGIN
#include <webrtc/modules/video_coding/include/video_error_codes.h>
#include <webrtc/rtc_base/checks.h>
#include <webrtc/rtc_base/logging.h>
#include <webrtc/rtc_base/synchronization/mutex.h>
#include <webrtc/rtc_base/time_utils.h>
ALLOW_UNUSED_PARAMETERS_END


#import <pal/cocoa/AVFoundationSoftLink.h>
#import <pal/cf/CoreMediaSoftLink.h>
#import "CoreVideoSoftLink.h"
#import "VideoToolboxSoftLink.h"

using namespace PAL;
using namespace WebCore;

// Struct that we pass to the decoder per frame to decode. We receive it again
// in the decoder callback.
struct RTCFrameDecodeParams {
    RTCFrameDecodeParams(int64_t timestamp, uint64_t reorderSize)
        : timestamp(timestamp)
        , reorderSize(reorderSize)
    {
    }

    int64_t timestamp { 0 };
    uint64_t reorderSize { 0 };
};

@interface RTCVideoDecoderH264 ()
- (void)setError:(OSStatus)error;
- (void)processFrame:(RTCVideoFrame*)decodedFrame reorderSize:(uint64_t)reorderSize;
@end

static void overrideColorSpaceAttachments(CVImageBufferRef imageBuffer) {
    CVBufferRemoveAttachment(imageBuffer, kCVImageBufferCGColorSpaceKey);
    CVBufferSetAttachment(imageBuffer, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(imageBuffer, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_sRGB, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(imageBuffer, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(imageBuffer, (CFStringRef)@"ColorInfoGuessedBy", (CFStringRef)@"RTCVideoDecoderH264", kCVAttachmentMode_ShouldPropagate);
}

// This is the callback function that VideoToolbox calls when decode is complete.
static void decompressionOutputCallback(void *decoderRef,
                                 void *params,
                                 OSStatus status,
                                 VTDecodeInfoFlags /* infoFlags */,
                                 CVImageBufferRef imageBuffer,
                                 CMTime timestamp,
                                 CMTime /* duration */) {
    std::unique_ptr<RTCFrameDecodeParams> decodeParams(reinterpret_cast<RTCFrameDecodeParams *>(params));
    RTCVideoDecoderH264 *decoder = (__bridge RTCVideoDecoderH264 *)decoderRef;
    if (status != noErr || !imageBuffer) {
        [decoder setError:status != noErr ? status : 1];
        RTC_LOG(LS_ERROR) << "Failed to decode frame. Status: " << status;
        [decoder processFrame:nil reorderSize:decodeParams->reorderSize];
        return;
    }

    overrideColorSpaceAttachments(imageBuffer);

    auto frameBuffer = adoptNS([[RTCCVPixelBuffer alloc] initWithPixelBuffer:imageBuffer]);
    auto decodedFrame = adoptNS([[RTCVideoFrame alloc] initWithBuffer:frameBuffer.get() rotation:RTCVideoRotation_0 timeStampNs:CMTimeGetSeconds(timestamp) * rtc::kNumNanosecsPerSec]);
    decodedFrame.get().timeStamp = decodeParams->timestamp;
    [decoder processFrame:decodedFrame.get() reorderSize:decodeParams->reorderSize];
}

// Decoder.
@implementation RTCVideoDecoderH264 {
    RetainPtr<CMVideoFormatDescriptionRef> _videoFormat;
    RetainPtr<CMMemoryPoolRef> _memoryPool;
    RetainPtr<VTDecompressionSessionRef> _decompressionSession;
    RTCVideoDecoderCallback _callback;
    OSStatus _error;
    bool _useAVC;
    WebCore::RTCVideoFrameReorderQueue _reorderQueue;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _memoryPool = CMMemoryPoolCreate(nil);
        _useAVC = false;
    }
    return self;
}

- (void)dealloc {
    if (_memoryPool)
        CMMemoryPoolInvalidate(_memoryPool.get());
    [self destroyDecompressionSession];
    [self setVideoFormat:nullptr];
    [super dealloc];
}

- (NSInteger)startDecodeWithNumberOfCores:(int)numberOfCores {
    return WEBRTC_VIDEO_CODEC_OK;
}

static RetainPtr<CMSampleBufferRef> H264BufferToCMSampleBuffer(const uint8_t* buffer, size_t buffer_size, CMVideoFormatDescriptionRef video_format)
{
    CMBlockBufferRef new_block_buffer;
    if (auto error = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, NULL, buffer_size, kCFAllocatorDefault, NULL, 0, buffer_size, kCMBlockBufferAssureMemoryNowFlag, &new_block_buffer)) {
        RTC_LOG(LS_ERROR) << "H264BufferToCMSampleBuffer CMBlockBufferCreateWithMemoryBlock failed with: " << error;
        return nullptr;
    }
    auto block_buffer = adoptCF(new_block_buffer);

    if (auto error = CMBlockBufferReplaceDataBytes(buffer, block_buffer.get(), 0, buffer_size)) {
        RTC_LOG(LS_ERROR) << "H264BufferToCMSampleBuffer CMBlockBufferReplaceDataBytes failed with: " << error;
        return nullptr;
    }

    CMSampleBufferRef sample_buffer = nullptr;
    if (auto error = CMSampleBufferCreate(kCFAllocatorDefault, block_buffer.get(), true, nullptr, nullptr, video_format, 1, 0, nullptr, 0, nullptr, &sample_buffer)) {
        RTC_LOG(LS_ERROR) << "H264BufferToCMSampleBuffer CMSampleBufferCreate failed with: " << error;
        return nullptr;
    }
    return adoptCF(sample_buffer);
}

- (NSInteger)decode:(RTCEncodedImage *)inputImage
        missingFrames:(BOOL)missingFrames
    codecSpecificInfo:(nullable id<RTCCodecSpecificInfo>)info
         renderTimeMs:(int64_t)renderTimeMs {
    RTC_DCHECK(inputImage.buffer);

    auto* data = reinterpret_cast<const uint8_t*>(inputImage.buffer.bytes);
    return [self decodeData:data size:inputImage.buffer.length timeStamp:inputImage.timeStamp];
}

- (NSInteger)decodeData:(const uint8_t *)data
        size:(size_t)size
        timeStamp:(int64_t)timeStamp {
    if (_error != noErr) {
        RTC_LOG(LS_WARNING) << "Last frame decode failed.";
        _error = noErr;
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (!data || !size) {
        RTC_LOG(LS_WARNING) << "Empty frame.";
        return WEBRTC_VIDEO_CODEC_ERROR;
    }

    if (!_useAVC) {
        auto inputFormat = adoptCF(CreateVideoFormatDescription(data, size));
        if (inputFormat) {
            _reorderQueue.setReorderSize(ComputeH264ReorderSizeFromAnnexB(data, size));
            // Check if the video format has changed, and reinitialize decoder if needed.
            if (!CMFormatDescriptionEqual(inputFormat.get(), _videoFormat.get())) {
                [self setVideoFormat:inputFormat.get()];
                int resetDecompressionSessionError = [self resetDecompressionSession];
                if (resetDecompressionSessionError != WEBRTC_VIDEO_CODEC_OK)
                    return resetDecompressionSessionError;
            }
        }
    }
    if (!_videoFormat) {
        // We received a frame but we don't have format information so we can't
        // decode it.
        // This can happen after backgrounding. We need to wait for the next
        // sps/pps before we can resume so we request a keyframe by returning an
        // error.
        RTC_LOG(LS_WARNING) << "Missing video format. Frame with sps/pps required.";
        return WEBRTC_VIDEO_CODEC_ERROR;
    }

    RetainPtr<CMSampleBufferRef> sampleBuffer;
    if (_useAVC) {
        sampleBuffer = H264BufferToCMSampleBuffer(data, size, _videoFormat.get());
        if (!sampleBuffer) {
            RTC_LOG(LS_ERROR) << "Cannot create sample from data.";
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
    } else {
        sampleBuffer = H264AnnexBBufferToCMSampleBuffer(data, size, _videoFormat.get(), _memoryPool.get());
        if (!sampleBuffer)
            return WEBRTC_VIDEO_CODEC_ERROR;
    }

    RTC_DCHECK(sampleBuffer);
    VTDecodeFrameFlags decodeFlags = kVTDecodeFrame_EnableAsynchronousDecompression;
    std::unique_ptr<RTCFrameDecodeParams> frameDecodeParams;
    frameDecodeParams.reset(new RTCFrameDecodeParams(timeStamp, _reorderQueue.reorderSize()));
    OSStatus status = VTDecompressionSessionDecodeFrame(_decompressionSession.get(), sampleBuffer.get(), decodeFlags, frameDecodeParams.release(), nullptr);
#if defined(WEBRTC_IOS)
    // Re-initialize the decoder if we have an invalid session while the app is
    // active or decoder malfunctions and retry the decode request.
    if ((status == kVTInvalidSessionErr || status == kVTVideoDecoderMalfunctionErr) && [self resetDecompressionSession] == WEBRTC_VIDEO_CODEC_OK) {
        RTC_LOG(LS_INFO) << "Failed to decode frame with code: " << status
                         << " retrying decode after decompression session reset";
        frameDecodeParams.reset(new RTCFrameDecodeParams(timeStamp, _reorderQueue.reorderSize()));
        status = VTDecompressionSessionDecodeFrame(_decompressionSession, sampleBuffer, decodeFlags, frameDecodeParams.release(), nullptr);
    }
#endif
    if (status != noErr) {
        RTC_LOG(LS_ERROR) << "Failed to decode frame with code: " << status;
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    return WEBRTC_VIDEO_CODEC_OK;
}

- (NSInteger)setAVCFormat:(const uint8_t *)data size:(size_t)size width:(uint16_t)width height:(uint16_t)height {
    auto codecConfig = adoptCF(CFDataCreate(kCFAllocatorDefault, data, size));
    auto configurationDict = @{ @"avcC": (__bridge NSData *)codecConfig.get() };
    auto extensions = @{ (__bridge NSString *)PAL::kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms: configurationDict };

    CMVideoFormatDescriptionRef videoFormatDescription = nullptr;
    auto err = PAL::CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_H264, width, height, (__bridge CFDictionaryRef)extensions, &videoFormatDescription);

    if (err) {
        RTC_LOG(LS_ERROR) << "Cannot create fromat description.";
        return err;
    }

    auto inputFormat = adoptCF(videoFormatDescription);
    if (inputFormat) {
        _reorderQueue.setReorderSize(ComputeH264ReorderSizeFromAVC(data, size));

        // Check if the video format has changed, and reinitialize decoder if needed.
        if (!CMFormatDescriptionEqual(inputFormat.get(), _videoFormat.get())) {
            [self setVideoFormat:inputFormat.get()];
            int resetDecompressionSessionError = [self resetDecompressionSession];
            if (resetDecompressionSessionError != WEBRTC_VIDEO_CODEC_OK)
                return resetDecompressionSessionError;
        }
    }
    _useAVC = true;
    return 0;
}

- (void)setCallback:(RTCVideoDecoderCallback)callback {
    _callback = callback;
}

- (void)setError:(OSStatus)error {
    _error = error;
}

- (NSInteger)releaseDecoder {
    // Need to invalidate the session so that callbacks no longer occur and it
    // is safe to null out the callback.
    [self destroyDecompressionSession];
    [self setVideoFormat:nullptr];
    _callback = nullptr;
    return WEBRTC_VIDEO_CODEC_OK;
}

#pragma mark - Private

- (int)resetDecompressionSession {
    [self destroyDecompressionSession];

    // Need to wait for the first SPS to initialize decoder.
    if (!_videoFormat)
        return WEBRTC_VIDEO_CODEC_OK;

    // Set keys for OpenGL and IOSurface compatibilty, which makes the encoder
    // create pixel buffers with GPU backed memory. The intent here is to pass
    // the pixel buffers directly so we avoid a texture upload later during
    // rendering. This currently is moot because we are converting back to an
    // I420 frame after decode, but eventually we will be able to plumb
    // CVPixelBuffers directly to the renderer.
    // TODO(tkchin): Maybe only set OpenGL/IOSurface keys if we know that that
    // we can pass CVPixelBuffers as native handles in decoder output.
    static size_t const attributesSize = 3;
    CFTypeRef keys[attributesSize] = {
        #if defined(WEBRTC_MAC) || defined(WEBRTC_MAC_CATALYST)
        kCVPixelBufferOpenGLCompatibilityKey,
        #elif defined(WEBRTC_IOS)
        kCVPixelBufferOpenGLESCompatibilityKey,
        #endif
        kCVPixelBufferIOSurfacePropertiesKey,
        kCVPixelBufferPixelFormatTypeKey
    };
    CFDictionaryRef ioSurfaceValue = CreateCFTypeDictionary(nullptr, nullptr, 0);
    int64_t nv12type = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    CFNumberRef pixelFormat = CFNumberCreate(nullptr, kCFNumberLongType, &nv12type);
    CFTypeRef values[attributesSize] = {kCFBooleanTrue, ioSurfaceValue, pixelFormat};
    CFDictionaryRef attributes = CreateCFTypeDictionary(keys, values, attributesSize);
    if (ioSurfaceValue) {
        CFRelease(ioSurfaceValue);
        ioSurfaceValue = nullptr;
    }
    if (pixelFormat) {
        CFRelease(pixelFormat);
        pixelFormat = nullptr;
    }
    VTDecompressionOutputCallbackRecord record = {
      decompressionOutputCallback, (__bridge void *)self,
    };

    VTDecompressionSessionRef decompressionSessionOut = nullptr;
    OSStatus status = VTDecompressionSessionCreate(nullptr, _videoFormat.get(), nullptr, attributes, &record, &decompressionSessionOut);
    CFRelease(attributes);
    if (status != noErr) {
        RTC_LOG(LS_ERROR) << "Failed to create decompression session: " << status;
        [self destroyDecompressionSession];
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    _decompressionSession = adoptCF(decompressionSessionOut);
    [self configureDecompressionSession];

    return WEBRTC_VIDEO_CODEC_OK;
}

- (void)configureDecompressionSession {
    RTC_DCHECK(_decompressionSession);
#if defined(WEBRTC_IOS)
    VTSessionSetProperty(_decompressionSession.get(), kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
#endif
}

- (void)destroyDecompressionSession {
    if (_decompressionSession) {
#if defined(WEBRTC_WEBKIT_BUILD)
        VTDecompressionSessionWaitForAsynchronousFrames(_decompressionSession.get());
#else
#if defined(WEBRTC_IOS)
        VTDecompressionSessionWaitForAsynchronousFrames(_decompressionSession);
#endif
#endif
        VTDecompressionSessionInvalidate(_decompressionSession.get());
        _decompressionSession = nullptr;
    }
}

- (void)flush {
    if (_decompressionSession)
        VTDecompressionSessionWaitForAsynchronousFrames(_decompressionSession.get());

    while (auto frame = _reorderQueue.takeIfAny()) {
        _callback(frame.get());
    }
}

- (void)setVideoFormat:(CMVideoFormatDescriptionRef)videoFormat {
    _videoFormat = videoFormat;
}

- (NSString *)implementationName {
    return @"VideoToolbox";
}

- (void)processFrame:(RTCVideoFrame*)decodedFrame reorderSize:(uint64_t)reorderSize {
    // FIXME: In case of IDR, we could push out all queued frames.
    if (!_reorderQueue.isEmpty() || reorderSize) {
        _reorderQueue.append(decodedFrame, reorderSize);
        while (auto frame = _reorderQueue.takeIfAvailable())
            _callback(frame.get());
        return;
    }
    _callback(decodedFrame);
}
@end
