/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "VideoTrackGenerator.h"

#if ENABLE(MEDIA_STREAM)

#include "Exception.h"
#include "JSWebCodecsVideoFrame.h"
#include "MediaStreamTrack.h"
#include "WritableStream.h"
#include "WritableStreamSink.h"

namespace WebCore {

WTF_MAKE_ISO_ALLOCATED_IMPL(VideoTrackGenerator);

ExceptionOr<Ref<VideoTrackGenerator>> VideoTrackGenerator::create(ScriptExecutionContext& context)
{
    auto source = Source::create();
    auto sink = Sink::create(Ref { source });
    auto writableOrException = WritableStream::create(*JSC::jsCast<JSDOMGlobalObject*>(context.globalObject()), Ref { sink });

    if (writableOrException.hasException())
        return writableOrException.releaseException();

    callOnMainThread([source] {
        source->start();
    });

    auto track = MediaStreamTrack::create(context, MediaStreamTrackPrivate::create(Logger::create(&context), WTFMove(source), [identifier = context.identifier()](Function<void()>&& task) {
        ScriptExecutionContext::postTaskTo(identifier, [task = WTFMove(task)] (auto&) mutable {
            task();
        });
    }));
    return adoptRef(*new VideoTrackGenerator(WTFMove(sink), writableOrException.releaseReturnValue(), WTFMove(track)));
}

VideoTrackGenerator::VideoTrackGenerator(Ref<Sink>&& sink, Ref<WritableStream>&& writable, Ref<MediaStreamTrack>&& track)
    : m_sink(WTFMove(sink))
    , m_writable(WTFMove(writable))
    , m_track(WTFMove(track))
{
}

VideoTrackGenerator::~VideoTrackGenerator()
{
}

void VideoTrackGenerator::setMuted(ScriptExecutionContext& context, bool muted)
{
    if (muted == m_muted)
        return;

    m_muted = muted;
    if (m_hasMutedChanged)
        return;

    m_hasMutedChanged = true;
    context.postTask([this, protectedThis = Ref { *this }] (auto&) {
        m_hasMutedChanged = false;
        m_track->privateTrack().setMuted(m_muted);
        m_sink->setMuted(m_muted);
    });
}

Ref<WritableStream> VideoTrackGenerator::writable()
{
    return Ref { m_writable };
}

Ref<MediaStreamTrack> VideoTrackGenerator::track()
{
    return Ref { m_track };
}

VideoTrackGenerator::Source::Source()
    : RealtimeMediaSource(CaptureDevice { { }, CaptureDevice::DeviceType::Camera, emptyString() })
{
}

VideoTrackGenerator::Sink::Sink(Ref<Source>&& source)
    : m_source(WTFMove(source))
{
}

void VideoTrackGenerator::Sink::write(ScriptExecutionContext&, JSC::JSValue value, DOMPromiseDeferred<void>&& promise)
{
    if (m_muted)
        return;

    auto* jsFrameObject = jsCast<JSWebCodecsVideoFrame*>(value);
    RefPtr frameObject = jsFrameObject ? &jsFrameObject->wrapped() : nullptr;
    if (!frameObject) {
        promise.reject(Exception { ExceptionCode::TypeError, "Expected a VideoFrame object"_s });
        return;
    }

    auto videoFrame = frameObject->internalFrame();
    if (!videoFrame) {
        promise.reject(Exception { ExceptionCode::TypeError, "VideoFrame object is not valid"_s });
        return;
    }

    if (!m_muted)
        m_source->writeVideoFrame(*videoFrame, { });

    frameObject->close();
    promise.resolve();
}

void VideoTrackGenerator::Sink::close()
{
    callOnMainThread([source = m_source] {
        source->endImmediatly();
    });
}

void VideoTrackGenerator::Sink::error(String&&)
{
    close();
}

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM)
