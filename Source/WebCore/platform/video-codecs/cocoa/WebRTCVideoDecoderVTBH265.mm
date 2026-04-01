/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
 *
 */

#import "config.h"
#import "WebRTCVideoDecoderVTBH265.h"

#if USE(LIBWEBRTC)

#import "CMUtilities.h"
#import "HEVCUtilitiesCocoa.h"
#import "Logging.h"
#import <CoreMedia/CMFormatDescription.h>
#import <wtf/BlockPtr.h>

#import <pal/cf/CoreMediaSoftLink.h>

namespace WebCore {

static void overrideH265ColorSpaceAttachments(CVPixelBufferRef pixelBuffer)
{
    CVBufferSetAttachment(pixelBuffer, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pixelBuffer, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pixelBuffer, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pixelBuffer, (CFStringRef)@"ColorInfoGuessedBy", (CFStringRef)@"WebRTCDecoderVTBVP9", kCVAttachmentMode_ShouldPropagate);
}

static BlockPtr<void(OSStatus, VTDecodeInfoFlags, CVImageBufferRef pixelBuffer, CMTaggedBufferGroupRef, CMTime presentationTime, CMTime)> createH265Callback(WebRTCVideoDecoderCallback callback)
{
    return makeBlockPtr([callback = makeBlockPtr(callback)](OSStatus, VTDecodeInfoFlags, CVImageBufferRef pixelBuffer, CMTaggedBufferGroupRef, CMTime presentationTime, CMTime) mutable {

        if (!pixelBuffer) {
            callback(nil, 0, 0, false);
            return;
        }

        // FIXME: We should remove this override once the encoder is properly setting color space info.
        overrideH265ColorSpaceAttachments(pixelBuffer);

        callback((CVPixelBufferRef)pixelBuffer, presentationTime.value, 0, false);
    });
}

WebRTCVideoDecoderVTBH265::WebRTCVideoDecoderVTBH265(WebRTCVideoDecoderCallback callback)
    : WebRTCVideoDecoderVTB(createH265Callback(WTF::move(callback)))
{
}

void WebRTCVideoDecoderVTBH265::setFormat(std::span<const uint8_t> data, uint16_t width, uint16_t height)
{
    if (auto hvccInformation = parseHEVCDecoderConfigurationRecord('hev1', data)) {
        if (hvccInformation->width)
            width = hvccInformation->width;
        if (hvccInformation->height)
            height = hvccInformation->height;
        // m_reorderQueue.setReorderSize(hvccInformation->reorderSize);
    }

    Ref videoInfo = VideoInfo::create({
        {
            .codecName = kCMVideoCodecType_HEVC,
        }, {
            // FIXME: We need to provide Colorspace.
            .size = IntSize { width, height },
            .displaySize = IntSize { width, height },
            .extensionAtoms = { std::make_pair('hvcC', SharedBuffer::create(data)) }
        }
    });

    RetainPtr videoFormatDescription = createFormatDescriptionFromTrackInfo(videoInfo.get());
    if (!videoFormatDescription) {
        RELEASE_LOG_ERROR(WebRTC, "Unable to create format descriptino from track info");
        return;
    }

    m_useHEVC = true;
    setVideoFormat(WTF::move(videoFormatDescription));
    setFrameSize(width, height);
}

int32_t WebRTCVideoDecoderVTBH265::decodeFrame(int64_t timeStamp, std::span<const uint8_t> data)
{
    if (m_useHEVC)
        return decodeFrameInternal(timeStamp, data);

    if (auto parameterSets = extractHEVCParameterSetsFromAnnexB(data)) {
        const uint8_t* parameterDatas[3] = { parameterSets->vps.data(), parameterSets->sps.data(), parameterSets->pps.data() };
        size_t parameterSizes[3] = { parameterSets->vps.size(), parameterSets->sps.size(), parameterSets->pps.size() };
        
        CMVideoFormatDescriptionRef description = nullptr;
        auto status = PAL::CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault, 3, parameterDatas, parameterSizes, 4, nullptr, &description);
        if (status == noErr)
            setVideoFormat(adoptCF(description));
    }

    if (!hasFormat())
        return -1;

    auto hvccData = convertHEVCAnnexBToHVCC(data);
    return decodeFrameInternal(timeStamp, hvccData.span());
}

}
#endif // USE(LIBWEBRTC)
