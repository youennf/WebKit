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
#include "MediaStreamTrackProcessor.h"

#if ENABLE(MEDIA_STREAM)

#include "JSWebCodecsVideoFrame.h"
#include "ReadableStream.h"

namespace WebCore {

WTF_MAKE_ISO_ALLOCATED_IMPL(MediaStreamTrackProcessor);

ExceptionOr<Ref<MediaStreamTrackProcessor>> MediaStreamTrackProcessor::create(ScriptExecutionContext& context, Init&& init)
{
    if (!init.track->isVideo())
        return Exception { ExceptionCode::TypeError, "Track is not video"_s };

    if (!init.track->ended())
        return Exception { ExceptionCode::TypeError, "Track is ended"_s };

    return adoptRef(*new MediaStreamTrackProcessor(context, init.track->source()));
}

MediaStreamTrackProcessor::MediaStreamTrackProcessor(ScriptExecutionContext& context, Ref<RealtimeMediaSource>&& realtimeVideoSource)
    : ContextDestructionObserver(&context)
    , m_videoFrameObserver(makeUnique<VideoFrameObserver>(context.identifier(), *this, WTFMove(realtimeVideoSource)))
{
}

MediaStreamTrackProcessor::~MediaStreamTrackProcessor()
{
    stopVideoFrameObserver();
}

ExceptionOr<Ref<ReadableStream>> MediaStreamTrackProcessor::readable(JSC::JSGlobalObject& globalObject)
{
    if (!m_readable) {
        auto readableStreamSource = Source::create();
        auto readableOrException = ReadableStream::create(*JSC::jsCast<JSDOMGlobalObject*>(&globalObject), readableStreamSource.get());
        if (readableOrException.hasException())
            return readableOrException.releaseException();
        m_readableStreamSource = WTFMove(readableStreamSource);
        m_readable = readableOrException.releaseReturnValue();
        m_videoFrameObserver->start();
    }
    
    return Ref { *m_readable };
}

void MediaStreamTrackProcessor::contextDestroyed()
{
    m_readableStreamSource = nullptr;
    stopVideoFrameObserver();
}

void MediaStreamTrackProcessor::stopVideoFrameObserver()
{
    if (m_videoFrameObserver)
        callOnMainThread([observer = WTFMove(m_videoFrameObserver)] { });
}

void MediaStreamTrackProcessor::trackEnded(MediaStreamTrackPrivate&)
{
    if (m_readableStreamSource)
        m_readableStreamSource->close();
}

void MediaStreamTrackProcessor::tryEnqueueingVideoFrame()
{
    RefPtr context = scriptExecutionContext();
    if (!context || !!m_videoFrameObserver || !m_readableStreamSource || !m_readableStreamSource->isWaiting())
        return;

    if (auto videoFrame = m_videoFrameObserver->takeVideoFrame(*context))
        m_readableStreamSource->enqueue(*videoFrame, *context);
}

MediaStreamTrackProcessor::VideoFrameObserver::VideoFrameObserver(ScriptExecutionContextIdentifier identifier, MediaStreamTrackProcessor& processor, Ref<RealtimeMediaSource>&& source)
    : m_contextIdentifier(identifier)
    , m_processor(processor)
    , m_realtimeVideoSource(WTFMove(source))
{
}

void MediaStreamTrackProcessor::VideoFrameObserver::start()
{
    m_isStarted = true;
    ASSERT(!isMainThread());
    callOnMainThread([this, source = m_realtimeVideoSource] {
        source->addVideoFrameObserver(*this);
    });
}

MediaStreamTrackProcessor::VideoFrameObserver::~VideoFrameObserver()
{
    ASSERT(isMainThread());
    if (m_isStarted)
        m_realtimeVideoSource->removeVideoFrameObserver(*this);
}

RefPtr<WebCodecsVideoFrame> MediaStreamTrackProcessor::VideoFrameObserver::takeVideoFrame(ScriptExecutionContext& context)
{
    Locker lock(m_videoFrameLock);
    if (!m_videoFrame)
        return nullptr;

    // FIXME: use metadata.
    return WebCodecsVideoFrame::create(context, m_videoFrame.releaseNonNull(), { });
}

void MediaStreamTrackProcessor::VideoFrameObserver::videoFrameAvailable(VideoFrame& frame, VideoFrameTimeMetadata metadata)
{
    {
        Locker lock(m_videoFrameLock);
        m_videoFrame = &frame;
        m_metadata = metadata;
    }
    ScriptExecutionContext::postTaskTo(m_contextIdentifier, [processor = m_processor] (auto&) mutable {
        if (auto protectedProcessor = processor.get())
            protectedProcessor->tryEnqueueingVideoFrame();
    });
}

bool MediaStreamTrackProcessor::Source::isWaiting() const
{
    return m_isWaiting;
}

void MediaStreamTrackProcessor::Source::close()
{
    if (!m_isCancelled)
        controller().close();
}

void MediaStreamTrackProcessor::Source::enqueue(WebCodecsVideoFrame& frame, ScriptExecutionContext& context)
{
    auto* globalObject = JSC::jsCast<JSDOMGlobalObject*>(context.globalObject());
    if (!globalObject)
        return;

    auto& vm = globalObject->vm();
    JSC::JSLockHolder lock(vm);

    m_isWaiting = false;

    auto value = toJS(globalObject, globalObject, frame);
    if (!m_isCancelled)
        controller().enqueue(value);
}

void MediaStreamTrackProcessor::Source::doPull()
{
    m_isWaiting = true;
    if (RefPtr protectedProcessor = m_processor.get())
        protectedProcessor->tryEnqueueingVideoFrame();
}

void MediaStreamTrackProcessor::Source::doCancel()
{
    m_isCancelled = true;
    if (RefPtr protectedProcessor = m_processor.get())
        protectedProcessor->stopVideoFrameObserver();
}

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM)
