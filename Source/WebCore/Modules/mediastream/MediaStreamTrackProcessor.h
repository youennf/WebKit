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

#pragma once

#if ENABLE(MEDIA_STREAM)

#include "ExceptionOr.h"
#include "MediaStreamTrack.h"
#include "ReadableStreamSource.h"
#include "RealtimeMediaSource.h"

namespace JSC {
class JSGlobaObject;
}

namespace WebCore {

class ReadableStream;
class ScriptExecutionContext;
class WebCodecsVideoFrame;

class MediaStreamTrackProcessor
    : public RefCounted<MediaStreamTrackProcessor>
    , public MediaStreamTrackPrivate::Observer
    , private ContextDestructionObserver
{
    WTF_MAKE_ISO_ALLOCATED(MediaStreamTrackProcessor);
public:
    struct Init {
        RefPtr<MediaStreamTrack> track;
    };

    static ExceptionOr<Ref<MediaStreamTrackProcessor>> create(ScriptExecutionContext&, Init&&);
    ~MediaStreamTrackProcessor();

    ExceptionOr<Ref<ReadableStream>> readable(JSC::JSGlobalObject&);

private:
    MediaStreamTrackProcessor(ScriptExecutionContext&, Ref<RealtimeMediaSource>&&);

    // ContextDestructionObserver
    void contextDestroyed() final;

    // MediaStreamTrackPrivate::Observer
    void trackEnded(MediaStreamTrackPrivate&) final;
    void trackMutedChanged(MediaStreamTrackPrivate&) final { }
    void trackSettingsChanged(MediaStreamTrackPrivate&) final { }
    void trackEnabledChanged(MediaStreamTrackPrivate&) final { }

    void stopVideoFrameObserver();
    void tryEnqueueingVideoFrame();

    class VideoFrameObserver final : private RealtimeMediaSource::VideoFrameObserver {
        WTF_MAKE_FAST_ALLOCATED;
    public:
        explicit VideoFrameObserver(ScriptExecutionContextIdentifier, MediaStreamTrackProcessor&, Ref<RealtimeMediaSource>&&);
        ~VideoFrameObserver();

        void start();

        RefPtr<WebCodecsVideoFrame> takeVideoFrame(ScriptExecutionContext&);

    private:
        // RealtimeMediaSource::VideoFrameObserver
        void videoFrameAvailable(VideoFrame&, VideoFrameTimeMetadata) final;

        bool m_isStarted { false };
        ScriptExecutionContextIdentifier m_contextIdentifier;
        WeakPtr<MediaStreamTrackProcessor> m_processor;
        RefPtr<RealtimeMediaSource> m_realtimeVideoSource;

        Lock m_videoFrameLock;
        RefPtr<VideoFrame> m_videoFrame WTF_GUARDED_BY_LOCK(m_videoFrameLock);
        VideoFrameTimeMetadata m_metadata WTF_GUARDED_BY_LOCK(m_videoFrameLock);
    };

    class Source final : public ReadableStreamSource {
    public:
        static Ref<Source> create() { return adoptRef(*new Source()); }
        bool isWaiting() const;
        void close();
        void enqueue(WebCodecsVideoFrame&, ScriptExecutionContext&);

    private:
        Source() = default;

        void setActive() { };
        void setInactive() { };
        void doStart() final { }
        void doPull() final;
        void doCancel() final;

        bool m_isWaiting { false };
        bool m_isCancelled { false };
        WeakPtr<MediaStreamTrackProcessor> m_processor;
    };

    RefPtr<ReadableStream> m_readable;
    RefPtr<Source> m_readableStreamSource;
    std::unique_ptr<VideoFrameObserver> m_videoFrameObserver;
};

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM)
