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
#include "MediaStreamTrackDataHolder.h"

#if ENABLE(MEDIA_STREAM)

namespace WebCore {

class PreventSourceFromEndingObserverWrapper : public ThreadSafeRefCounted<PreventSourceFromEndingObserverWrapper, WTF::DestructionThread::Main> {
public:
    static Ref<PreventSourceFromEndingObserverWrapper> create(Ref<RealtimeMediaSource>&& source)
    {
        auto wrapper = adoptRef(*new PreventSourceFromEndingObserverWrapper(WTFMove(source)));
        wrapper->initialize();
        return wrapper;
    }

private:
    explicit PreventSourceFromEndingObserverWrapper(Ref<RealtimeMediaSource>&& source)
        : m_observer(makeUniqueRef<PreventSourceFromEndingObserver>(WTFMove(source)))
    {
    }

    void initialize()
    {
        ensureOnMainThread([protectedThis = Ref { *this }] {
            protectedThis->m_observer->initialize();
        });
    }

    class PreventSourceFromEndingObserver final : public RealtimeMediaSource::Observer {
        WTF_MAKE_FAST_ALLOCATED;
    public:
        explicit PreventSourceFromEndingObserver(Ref<RealtimeMediaSource>&& source)
            : m_source(WTFMove(source))
        {
        }

        void initialize()
        {
            ASSERT(isMainThread());
            m_source->addObserver(*this);
        }
        
        ~PreventSourceFromEndingObserver() {
            ASSERT(isMainThread());
            m_source->removeObserver(*this);
        }
    private:
        bool preventSourceFromEnding() final { return true; }
        
        Ref<RealtimeMediaSource> m_source;
    };

    UniqueRef<PreventSourceFromEndingObserver> m_observer;
};

MediaStreamTrackDataHolder::MediaStreamTrackDataHolder(bool isProducingData, bool enabled, bool ended, bool muted, bool interrupted, String&& trackId, String&& label, RealtimeMediaSource::Type type, CaptureDevice::DeviceType deviceType, RealtimeMediaSourceSettings&& settings, RealtimeMediaSourceCapabilities&& capabilities, Ref<RealtimeMediaSource>&& source)
    : isProducingData(isProducingData)
    , enabled(enabled)
    , ended(ended)
    , muted(muted)
    , interrupted(interrupted)
    , trackId(WTFMove(trackId))
    , label(WTFMove(label))
    , type(type)
    , deviceType(deviceType)
    , settings(WTFMove(settings))
    , capabilities(WTFMove(capabilities))
    , source(source.get())
    , preventSourceFromEndingObserverWrapper(PreventSourceFromEndingObserverWrapper::create(WTFMove(source)))
{
}

MediaStreamTrackDataHolder::~MediaStreamTrackDataHolder()
{
}

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM)
