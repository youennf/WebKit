/*
 * Copyright (C) 2015-2022 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of Google Inc. nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#import "config.h"
#import "MockRealtimeVideoSourceMac.h"

#if ENABLE(MEDIA_STREAM)

#import "GraphicsContextCG.h"
#import "ImageBuffer.h"
#import "ImageTransferSessionVT.h"
#import "MediaConstraints.h"
#import "MockRealtimeMediaSourceCenter.h"
#import "NotImplemented.h"
#import "PlatformLayer.h"
#import "RealtimeMediaSourceSettings.h"
#import "RealtimeVideoUtilities.h"
#import "VideoFrame.h"
#import <QuartzCore/CALayer.h>
#import <QuartzCore/CATransaction.h>
#import <objc/runtime.h>
#import <wtf/MediaTime.h>

#import "CoreVideoSoftLink.h"
#import <pal/cf/CoreMediaSoftLink.h>

namespace WebCore {

CaptureSourceOrError MockRealtimeVideoSource::create(CaptureDevice&& device, MediaDeviceHashSalts&& hashSalts, const MediaConstraints* constraints, std::optional<PageIdentifier> pageIdentifier)
{
#ifndef NDEBUG
    auto mockDevice = MockRealtimeMediaSourceCenter::mockDeviceWithPersistentID(device.persistentId());
    ASSERT(mockDevice);
    if (!mockDevice)
        return CaptureSourceOrError({ "No mock camera device"_s , MediaAccessDenialReason::PermissionDenied });
#endif

    Ref<RealtimeMediaSource> source = MockRealtimeVideoSourceMac::create(WTFMove(device), WTFMove(hashSalts), pageIdentifier);
    if (constraints) {
        if (auto error = source->applyConstraints(*constraints))
            return CaptureSourceOrError(CaptureSourceError { error->invalidConstraint });
    }

    return source;
}

Ref<MockRealtimeVideoSource> MockRealtimeVideoSourceMac::createForMockDisplayCapturer(String&& deviceID, AtomString&& name, MediaDeviceHashSalts&& hashSalts, std::optional<PageIdentifier> pageIdentifier)
{
    return adoptRef(*new MockRealtimeVideoSourceMac({ WTFMove(deviceID), CaptureDevice::DeviceType::Screen, WTFMove(name) }, WTFMove(hashSalts), pageIdentifier));
}

MockRealtimeVideoSourceMac::MockRealtimeVideoSourceMac(CaptureDevice&& device, MediaDeviceHashSalts&& hashSalts, std::optional<PageIdentifier> pageIdentifier)
    : MockRealtimeVideoSource(WTFMove(device), WTFMove(hashSalts), pageIdentifier)
{
}

MockRealtimeVideoSourceMac::~MockRealtimeVideoSourceMac() = default;

void MockRealtimeVideoSourceMac::updateSampleBuffer()
{
    ASSERT(!isMainThread());

    RefPtr imageBuffer = this->imageBufferInternal();
    if (!imageBuffer)
        return;

    if (!m_imageTransferSession) {
        m_imageTransferSession = ImageTransferSessionVT::create(preferedPixelBufferFormat());
        m_imageTransferSession->setMaximumBufferPoolSize(10);
    }

    PlatformImagePtr platformImage;
    if (auto nativeImage = imageBuffer->copyNativeImage())
        platformImage = nativeImage->platformImage();

    auto presentationTime = MediaTime::createWithDouble((elapsedTime() + 100_ms).seconds());
    auto videoFrame = m_imageTransferSession->createVideoFrame(platformImage.get(), presentationTime, size(), videoFrameRotation());
    if (!videoFrame) {
        static const size_t MaxPixelGenerationFailureCount = 150;
        if (++m_pixelGenerationFailureCount > MaxPixelGenerationFailureCount) {
            callOnMainThread([protectedThis = Ref { *this }] {
                protectedThis->captureFailed();
            });
        }
        return;
    }

    m_pixelGenerationFailureCount = 0;
    VideoFrameTimeMetadata metadata;
    metadata.captureTime = MonotonicTime::now().secondsSinceEpoch();
    dispatchVideoFrameToObservers(*videoFrame, metadata);
}

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM)
