/*
 * Copyright (C) 2013 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2015 Ericsson AB. All rights reserved.
 * Copyright (C) 2013-2019 Apple Inc. All rights reserved.
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

#include "config.h"
#include "MediaStreamTrackPrivate.h"

#if ENABLE(MEDIA_STREAM)

#include "GraphicsContext.h"
#include "IntRect.h"
#include "Logging.h"
#include "MediaStreamTrackDataHolder.h"
#include "PlatformMediaSessionManager.h"
#include <wtf/CrossThreadCopier.h>
#include <wtf/NativePromise.h>
#include <wtf/UUID.h>

#if PLATFORM(COCOA)
#include "MediaStreamTrackAudioSourceProviderCocoa.h"
#elif ENABLE(WEB_AUDIO) && ENABLE(MEDIA_STREAM) && USE(GSTREAMER)
#include "AudioSourceProviderGStreamer.h"
#else
#include "WebAudioSourceProvider.h"
#endif

namespace WebCore {

Ref<MediaStreamTrackPrivate> MediaStreamTrackPrivate::create(Ref<const Logger>&& logger, Ref<RealtimeMediaSource>&& source, Function<void(Function<void()>&&)>&& postTask)
{
    return create(WTFMove(logger), WTFMove(source), createVersion4UUIDString(), WTFMove(postTask));
}

Ref<MediaStreamTrackPrivate> MediaStreamTrackPrivate::create(Ref<const Logger>&& logger, Ref<RealtimeMediaSource>&& source, String&& id, Function<void(Function<void()>&&)>&& postTask)
{
    return adoptRef(*new MediaStreamTrackPrivate(WTFMove(logger), WTFMove(source), WTFMove(id), WTFMove(postTask)));
}

Ref<MediaStreamTrackPrivate> MediaStreamTrackPrivate::create(Ref<const Logger>&& logger, UniqueRef<MediaStreamTrackDataHolder>&& dataHolder, Function<void(Function<void()>&&)>&& postTask)
{
    return adoptRef(*new MediaStreamTrackPrivate(WTFMove(logger), WTFMove(dataHolder), WTFMove(postTask)));
}

// We need when hitting main thread to send back all source states, since they may have changed.
class MediaStreamTrackPrivateSourceObserverWrapper : public ThreadSafeRefCounted<MediaStreamTrackPrivateSourceObserverWrapper, WTF::DestructionThread::Main> {
public:
    static Ref<MediaStreamTrackPrivateSourceObserverWrapper> create(MediaStreamTrackPrivate& privateTrack, Function<void(Function<void()>&&)>&& postTask) { return adoptRef(*new MediaStreamTrackPrivateSourceObserverWrapper(privateTrack, WTFMove(postTask))); }

    void requestToEnd()
    {
        ensureOnMainThread([protectedThis = Ref { *this }] {
            protectedThis->m_observer->requestToEnd();
        });
    }

    void setMuted(bool muted)
    {
        ensureOnMainThread([protectedThis = Ref { *this }, muted] {
            protectedThis->m_observer->setMuted(muted);
        });
    }

    void applyConstraints(const MediaConstraints& constraints, RealtimeMediaSource::ApplyConstraintsHandler&& completionHandler)
    {
        m_applyConstraintsCallbacks.add(++m_applyConstraintsCallbacksIdentifier, WTFMove(completionHandler));
        ensureOnMainThread([this, protectedThis = Ref { *this }, constraints = crossThreadCopy(constraints), identifier = m_applyConstraintsCallbacksIdentifier] () mutable {
            m_observer->applyConstraints(constraints, [weakObserver = WeakPtr { *m_observer }, protectedThis = WTFMove(protectedThis), identifier] (auto&& result) mutable {
                if (!weakObserver)
                    return;
                weakObserver->postTask([protectedThis = WTFMove(protectedThis), result = crossThreadCopy(WTFMove(result)), identifier] () mutable {
                    if (auto callback = protectedThis->m_applyConstraintsCallbacks.take(identifier))
                        callback(WTFMove(result));
                });
            });
        });
    }

private:
    class Observer final : public RealtimeMediaSource::Observer {
        WTF_MAKE_FAST_ALLOCATED;
    public:
        Observer(WeakPtr<MediaStreamTrackPrivate>&& privateTrack, Ref<RealtimeMediaSource>&& source, Function<void(Function<void()>&&)>&& postTask)
            : m_privateTrack(WTFMove(privateTrack))
            , m_source(WTFMove(source))
            , m_postTask(WTFMove(postTask))
        {
            ASSERT(isMainThread());
        }

        void initialize(bool interrupted, bool muted)
        {
            ASSERT(isMainThread());
            if (m_source->isEnded()) {
                sourceStopped();
                return;
            }

            if (muted != m_source->muted() || interrupted != m_source->interrupted())
                sourceMutedChanged();

            // FIXME: We should check for settings capabilities changes.

            m_isStarted = true;
            m_source->addObserver(*this);
        }

        ~Observer()
        {
            ASSERT(isMainThread());
            if (m_isStarted)
                m_source->removeObserver(*this);
        }

        void requestToEnd()
        {
            m_shouldPreventSourceFromEnding = false;
            m_source->requestToEnd(*this);
        }

        void setMuted(bool muted)
        {
            m_source->setMuted(muted);
        }

        void applyConstraints(const MediaConstraints& constraints, RealtimeMediaSource::ApplyConstraintsHandler&& completionHandler)
        {
            m_source->applyConstraints(constraints, WTFMove(completionHandler));
        }

        void postTask(Function<void()>&& task)
        {
            m_postTask(WTFMove(task));
        }

    private:
        void sourceStarted() final
        {
            sendToMediaStreamTrackPrivate([] (auto& privateTrack) {
                privateTrack.sourceStarted();
            });
        }

        void sourceStopped() final
        {
            sendToMediaStreamTrackPrivate([] (auto& privateTrack) {
                privateTrack.sourceStopped();
            });
        }

        void sourceMutedChanged() final
        {
            sendToMediaStreamTrackPrivate([muted = m_source->muted(), interrupted = m_source->interrupted()] (auto& privateTrack) {
                privateTrack.sourceMutedChanged(interrupted, muted);
            });
        }

        void sourceSettingsChanged() final
        {
            // We need to isolate copy.
            sendToMediaStreamTrackPrivate([settings = crossThreadCopy(m_source->settings()), capabilities = crossThreadCopy(m_source->capabilities())] (auto& privateTrack) mutable {
                privateTrack.sourceSettingsChanged(WTFMove(settings), WTFMove(capabilities));
            });
        }

        void sourceConfigurationChanged() final
        {
            // We need to isolate copy.
            sendToMediaStreamTrackPrivate([settings = crossThreadCopy(m_source->settings()), capabilities = crossThreadCopy(m_source->capabilities())] (auto& privateTrack) mutable {
                privateTrack.sourceConfigurationChanged(WTFMove(settings), WTFMove(capabilities));
            });
        }

        void hasStartedProducingData() final
        {
            sendToMediaStreamTrackPrivate([] (auto& privateTrack) {
                privateTrack.hasStartedProducingData();
            });
        }

        void audioUnitWillStart() final { }

        void sendToMediaStreamTrackPrivate(Function<void(MediaStreamTrackPrivate&)>&& task)
        {
            m_postTask([task = WTFMove(task), privateTrack = m_privateTrack] () mutable {
                if (RefPtr protectedPrivateTrack = privateTrack.get())
                    task(*protectedPrivateTrack);
            });
        }

        bool preventSourceFromEnding() { return m_shouldPreventSourceFromEnding; }

        WeakPtr<MediaStreamTrackPrivate> m_privateTrack;
        Ref<RealtimeMediaSource> m_source;
        Function<void(Function<void()>&&)> m_postTask;
        bool m_shouldPreventSourceFromEnding { true };
        bool m_isStarted { true };
    };

private:
    MediaStreamTrackPrivateSourceObserverWrapper(MediaStreamTrackPrivate& privateTrack, Function<void(Function<void()>&&)>&& postTask)
    {
        callOnMainThread([this, protectedThis = Ref { *this }, privateTrack = WeakPtr { privateTrack }, postTask = WTFMove(postTask), source = Ref { privateTrack.source() }, interrupted = privateTrack.interrupted(), muted = privateTrack.muted()] () mutable {
            m_observer = makeUnique<Observer>(WTFMove(privateTrack), WTFMove(source), WTFMove(postTask));
            m_observer->initialize(interrupted, muted);
        });
    }

    std::unique_ptr<Observer> m_observer;
    HashMap<uint64_t, RealtimeMediaSource::ApplyConstraintsHandler> m_applyConstraintsCallbacks;
    uint64_t m_applyConstraintsCallbacksIdentifier { 0 };
};

MediaStreamTrackPrivate::MediaStreamTrackPrivate(Ref<const Logger>&& trackLogger, Ref<RealtimeMediaSource>&& source, String&& id, Function<void(Function<void()>&&)>&& postTask)
    : m_source(WTFMove(source))
    , m_id(WTFMove(id))
    , m_label(m_source->name())
    , m_type(m_source->type())
    , m_deviceType(m_source->deviceType())
    , m_isCaptureTrack(m_source->isCaptureSource())
    , m_captureDidFail(m_source->captureDidFail())
    , m_logger(WTFMove(trackLogger))
#if !RELEASE_LOG_DISABLED
    , m_logIdentifier(uniqueLogIdentifier())
#endif
    , m_isProducingData(m_source->isProducingData())
    , m_isMuted(m_source->muted())
    , m_isInterrupted(m_source->interrupted())
    , m_settings(m_source->settings())
    , m_capabilities(m_source->capabilities())
#if ASSERT_ENABLED
    , m_creationThreadId(isMainThread() ? 0 : Thread::current().uid())
#endif
{
    UNUSED_PARAM(trackLogger);
    ALWAYS_LOG(LOGIDENTIFIER);
    if (!isMainThread()) {
        ASSERT(postTask);
        m_sourceObserver = MediaStreamTrackPrivateSourceObserverWrapper::create(*this, WTFMove(postTask));
        return;
    }

#if !RELEASE_LOG_DISABLED
    m_source->setLogger(m_logger.copyRef(), m_logIdentifier);
#endif

    m_source->addObserver(*this);
}

MediaStreamTrackPrivate::MediaStreamTrackPrivate(Ref<const Logger>&& logger, UniqueRef<MediaStreamTrackDataHolder>&& dataHolder, Function<void(Function<void()>&&)>&& postTask)
    : m_source(WTFMove(dataHolder->source))
    , m_id(WTFMove(dataHolder->trackId))
    , m_label(WTFMove(dataHolder->label))
    , m_type(dataHolder->type)
    , m_deviceType(dataHolder->deviceType)
    , m_isCaptureTrack(false)
    , m_captureDidFail(false)
    , m_logger(WTFMove(logger))
#if !RELEASE_LOG_DISABLED
    , m_logIdentifier(uniqueLogIdentifier())
#endif
    , m_isProducingData(dataHolder->isProducingData)
    , m_isMuted(dataHolder->muted)
    , m_isInterrupted(dataHolder->interrupted)
    , m_settings(WTFMove(dataHolder->settings))
    , m_capabilities(WTFMove(dataHolder->capabilities))
#if ASSERT_ENABLED
    , m_creationThreadId(isMainThread() ? 0 : Thread::current().uid())
#endif
{
    ASSERT(postTask);
    m_sourceObserver = MediaStreamTrackPrivateSourceObserverWrapper::create(*this, WTFMove(postTask));
}

MediaStreamTrackPrivate::~MediaStreamTrackPrivate()
{
    ASSERT(isOnCreationThread());

    ALWAYS_LOG(LOGIDENTIFIER);

    if (m_sourceObserver)
        return;

    m_source->removeObserver(*this);
}

#if ASSERT_ENABLED
bool MediaStreamTrackPrivate::isOnCreationThread()
{
    return m_creationThreadId ? m_creationThreadId == Thread::current().uid() : isMainThread();
}
#endif

void MediaStreamTrackPrivate::forEachObserver(const Function<void(Observer&)>& apply)
{
    ASSERT(isOnCreationThread());
    ASSERT(!m_observers.hasNullReferences());
    Ref protectedThis { *this };
    m_observers.forEach(apply);
}

void MediaStreamTrackPrivate::addObserver(MediaStreamTrackPrivate::Observer& observer)
{
    ASSERT(isOnCreationThread());
    m_observers.add(observer);
}

void MediaStreamTrackPrivate::removeObserver(MediaStreamTrackPrivate::Observer& observer)
{
    ASSERT(isOnCreationThread());
    m_observers.remove(observer);
}

void MediaStreamTrackPrivate::setContentHint(HintValue hintValue)
{
    m_contentHint = hintValue;
}

void MediaStreamTrackPrivate::setMuted(bool muted)
{
    ASSERT(isOnCreationThread());
    m_isMuted = muted;

    if (m_sourceObserver) {
        m_sourceObserver->setMuted(muted);
        return;
    }

    m_source->setMuted(muted);
}

void MediaStreamTrackPrivate::setEnabled(bool enabled)
{
    ASSERT(isOnCreationThread());
    if (m_isEnabled == enabled)
        return;

    ALWAYS_LOG(LOGIDENTIFIER, enabled);

    // Always update the enabled state regardless of the track being ended.
    m_isEnabled = enabled;

    forEachObserver([this](auto& observer) {
        observer.trackEnabledChanged(*this);
    });
}

void MediaStreamTrackPrivate::endTrack()
{
    ASSERT(isOnCreationThread());
    if (m_isEnded)
        return;

    ALWAYS_LOG(LOGIDENTIFIER);

    // Set m_isEnded to true before telling the source it can stop, so if this is the
    // only track using the source and it does stop, we will only call each observer's
    // trackEnded method once.
    m_isEnded = true;
    updateReadyState();

    if (m_sourceObserver) {
        m_sourceObserver->requestToEnd();
        return;
    }

    m_source->requestToEnd(*this);

    forEachObserver([this](auto& observer) {
        observer.trackEnded(*this);
    });
}

Ref<MediaStreamTrackPrivate> MediaStreamTrackPrivate::clone()
{
    ASSERT(isOnCreationThread());

    auto clonedMediaStreamTrackPrivate = create(m_logger.copyRef(), m_source->clone());

    ALWAYS_LOG(LOGIDENTIFIER, clonedMediaStreamTrackPrivate->logIdentifier());

    clonedMediaStreamTrackPrivate->m_isEnabled = this->m_isEnabled;
    clonedMediaStreamTrackPrivate->m_isEnded = this->m_isEnded;
    clonedMediaStreamTrackPrivate->m_contentHint = this->m_contentHint;
    clonedMediaStreamTrackPrivate->updateReadyState();

    if (m_isProducingData)
        clonedMediaStreamTrackPrivate->startProducingData();

    return clonedMediaStreamTrackPrivate;
}
Ref<RealtimeMediaSource::PhotoCapabilitiesNativePromise> MediaStreamTrackPrivate::getPhotoCapabilities()
{
    ASSERT(isMainThread());
    return m_source->getPhotoCapabilities();
}

Ref<RealtimeMediaSource::PhotoSettingsNativePromise> MediaStreamTrackPrivate::getPhotoSettings()
{
    ASSERT(isMainThread());
    return m_source->getPhotoSettings();
}

Ref<RealtimeMediaSource::TakePhotoNativePromise> MediaStreamTrackPrivate::takePhoto(PhotoSettings&& settings)
{
    ASSERT(isMainThread());
    return m_source->takePhoto(WTFMove(settings));
}

void MediaStreamTrackPrivate::applyConstraints(const MediaConstraints& constraints, RealtimeMediaSource::ApplyConstraintsHandler&& completionHandler)
{
    if (m_sourceObserver) {
        m_sourceObserver->applyConstraints(constraints, WTFMove(completionHandler));
        return;
    }
    m_source->applyConstraints(constraints, WTFMove(completionHandler));
}

RefPtr<WebAudioSourceProvider> MediaStreamTrackPrivate::createAudioSourceProvider()
{
    ASSERT(isMainThread());
    ALWAYS_LOG(LOGIDENTIFIER);

#if PLATFORM(COCOA)
    return MediaStreamTrackAudioSourceProviderCocoa::create(*this);
#elif USE(GSTREAMER)
    return AudioSourceProviderGStreamer::create(*this);
#else
    return nullptr;
#endif
}

void MediaStreamTrackPrivate::sourceStarted()
{
    ASSERT(isOnCreationThread());
    ALWAYS_LOG(LOGIDENTIFIER);

    m_isProducingData = true;
    forEachObserver([this](auto& observer) {
        observer.trackStarted(*this);
    });
}

void MediaStreamTrackPrivate::sourceStopped()
{
    ASSERT(isOnCreationThread());
    m_isProducingData = false;

    if (m_isEnded)
        return;

    ALWAYS_LOG(LOGIDENTIFIER);

    m_isEnded = true;
    updateReadyState();

    forEachObserver([this](auto& observer) {
        observer.trackEnded(*this);
    });
}

void MediaStreamTrackPrivate::sourceMutedChanged()
{
    ASSERT(isMainThread());
    sourceMutedChanged(m_source->interrupted(), m_source->muted());
}

void MediaStreamTrackPrivate::sourceMutedChanged(bool interrupted, bool muted)
{
    ASSERT(isOnCreationThread());
    ALWAYS_LOG(LOGIDENTIFIER);

    m_isInterrupted = interrupted;
    m_isMuted = muted;
    forEachObserver([this](auto& observer) {
        observer.trackMutedChanged(*this);
    });
}

void MediaStreamTrackPrivate::sourceSettingsChanged()
{
    ASSERT(isMainThread());
    sourceSettingsChanged(RealtimeMediaSourceSettings { m_source->settings() }, RealtimeMediaSourceCapabilities { m_source->capabilities() });
}

void MediaStreamTrackPrivate::sourceSettingsChanged(RealtimeMediaSourceSettings&& settings, RealtimeMediaSourceCapabilities&& capabilities)
{
    ASSERT(isOnCreationThread());
    ALWAYS_LOG(LOGIDENTIFIER);

    m_settings = WTFMove(settings);
    m_capabilities = WTFMove(capabilities);
    forEachObserver([this](auto& observer) {
        observer.trackSettingsChanged(*this);
    });
}

void MediaStreamTrackPrivate::sourceConfigurationChanged()
{
    ASSERT(isMainThread());
    sourceConfigurationChanged(RealtimeMediaSourceSettings { m_source->settings() }, RealtimeMediaSourceCapabilities { m_source->capabilities() });
}

void MediaStreamTrackPrivate::sourceConfigurationChanged(RealtimeMediaSourceSettings&& settings, RealtimeMediaSourceCapabilities&& capabilities)
{
    ASSERT(isOnCreationThread());
    ALWAYS_LOG(LOGIDENTIFIER);

    m_settings = WTFMove(settings);
    m_capabilities = WTFMove(capabilities);
    forEachObserver([this](auto& observer) {
        observer.trackConfigurationChanged(*this);
    });
}

bool MediaStreamTrackPrivate::preventSourceFromEnding()
{
    ALWAYS_LOG(LOGIDENTIFIER, m_isEnded);

    // Do not allow the source to end if we are still using it.
    return !m_isEnded;
}

void MediaStreamTrackPrivate::hasStartedProducingData()
{
    ASSERT(isOnCreationThread());
    if (m_hasStartedProducingData)
        return;
    ALWAYS_LOG(LOGIDENTIFIER);
    m_hasStartedProducingData = true;
    updateReadyState();
}

void MediaStreamTrackPrivate::updateReadyState()
{
    ASSERT(isOnCreationThread());
    ReadyState state = ReadyState::None;

    if (m_isEnded)
        state = ReadyState::Ended;
    else if (m_hasStartedProducingData)
        state = ReadyState::Live;

    if (state == m_readyState)
        return;

    ALWAYS_LOG(LOGIDENTIFIER, state == ReadyState::Ended ? "Ended" : "Live");

    m_readyState = state;
    forEachObserver([this](auto& observer) {
        observer.readyStateChanged(*this);
    });
}

void MediaStreamTrackPrivate::audioUnitWillStart()
{
    ASSERT(isMainThread());
    if (!m_isEnded)
        PlatformMediaSessionManager::sharedManager().sessionCanProduceAudioChanged();
}

UniqueRef<MediaStreamTrackDataHolder> MediaStreamTrackPrivate::toDataHolder()
{
    return makeUniqueRef<MediaStreamTrackDataHolder>(
        m_isProducingData,
        m_isEnabled,
        m_isEnded,
        m_isMuted,
        m_isInterrupted,
        m_id.isolatedCopy(),
        m_label.isolatedCopy(),
        m_type,
        m_deviceType,
        m_settings.isolatedCopy(),
        m_capabilities.isolatedCopy(),
        Ref { m_source });
}

#if !RELEASE_LOG_DISABLED
WTFLogChannel& MediaStreamTrackPrivate::logChannel() const
{
    return LogWebRTC;
}
#endif

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM)
