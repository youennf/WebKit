/*
 * Copyright (C) 2022-2023 Apple Inc. All rights reserved.
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
 */
#pragma once

#if HAVE(SCREEN_CAPTURE_KIT)

#include <WebCore/DisplayCapturePromptType.h>
#include <WebCore/IntSize.h>
#include <wtf/AbstractCanMakeCheckedPtr.h>
#include <wtf/CompletionHandler.h>
#include <wtf/RefCountedAndCanMakeWeakPtr.h>
#include <wtf/RetainPtr.h>
#include <wtf/RunLoop.h>
#include <wtf/text/WTFString.h>

OBJC_CLASS NSError;
OBJC_CLASS SCContentFilter;
OBJC_CLASS SCContentSharingPicker;
OBJC_CLASS SCStream;
OBJC_CLASS SCStreamConfiguration;
OBJC_CLASS SCStreamDelegate;
OBJC_CLASS WebDisplayMediaPromptHelper;

namespace WebCore {

class CaptureDevice;
class ScreenCaptureKitSharingSessionManager;

class ScreenCaptureSessionSourceObserver : public CanMakeWeakPtr<ScreenCaptureSessionSourceObserver>, public AbstractCanMakeCheckedPtr {
public:
    virtual ~ScreenCaptureSessionSourceObserver() = default;

    // Session state changes.
    virtual void sessionFilterDidChange(SCContentFilter*) = 0;
    virtual void sessionStreamDidEnd(SCStream*) = 0;
};

class ScreenCaptureSessionSource
    : public RefCounted<ScreenCaptureSessionSource>
    , public CanMakeWeakPtr<ScreenCaptureSessionSource> {
public:
    using CleanupFunction = CompletionHandler<void(ScreenCaptureSessionSource&)>;
    static Ref<ScreenCaptureSessionSource> create(WeakPtr<ScreenCaptureSessionSourceObserver>, RetainPtr<SCStream>, RetainPtr<SCContentFilter>, CleanupFunction&&);
    virtual ~ScreenCaptureSessionSource();

    SCStream* stream() const { return m_stream.get(); }
    SCContentFilter* contentFilter() const { return m_contentFilter.get(); }
    ScreenCaptureSessionSourceObserver* observer() const { return m_observer.get(); }

    void updateContentFilter(SCContentFilter*);
    void streamDidEnd();

    bool operator==(const ScreenCaptureSessionSource&) const;

private:
    ScreenCaptureSessionSource(WeakPtr<ScreenCaptureSessionSourceObserver>&&, RetainPtr<SCStream>&&, RetainPtr<SCContentFilter>&&, CleanupFunction&&);

    const RetainPtr<SCStream> m_stream;
    RetainPtr<SCContentFilter> m_contentFilter;
    WeakPtr<ScreenCaptureSessionSourceObserver> m_observer;
    CleanupFunction m_cleanupFunction;
};

// Content filter selected for a capture device.
// The destruction handler is called with the device ID when the content filter is destroyed, which happens when the capture
// of the device ends. The device ID is empty if no device was selected with this content filter.
class ScreenCaptureKitContentFilter {
    WTF_MAKE_NONCOPYABLE(ScreenCaptureKitContentFilter);
public:
    using DestructionHandler = CompletionHandler<void(String&& deviceID)>;

    ScreenCaptureKitContentFilter() = default;
    ScreenCaptureKitContentFilter(RetainPtr<SCContentFilter>&&, DestructionHandler&& = { });
    ScreenCaptureKitContentFilter(ScreenCaptureKitContentFilter&&) = default;
    ScreenCaptureKitContentFilter& operator=(ScreenCaptureKitContentFilter&&);
    ~ScreenCaptureKitContentFilter();

    SCContentFilter* get() const { return m_filter.get(); }
    explicit operator bool() const { return !!m_filter; }
    const String& deviceID() const { return m_deviceID; }
    // Size in pixels given when requesting the capture, for content filters that do not know it, like the one of a window that was just created.
    std::optional<IntSize> requestedContentSize() const { return m_requestedContentSize; }

    // Keeps the device ID and the destruction handler, as the capture of the device continues with the new filter.
    void setFilter(RetainPtr<SCContentFilter>&& filter) { m_filter = WTF::move(filter); }
    void setDeviceID(const String& deviceID) { m_deviceID = deviceID; }
    void setRequestedContentSize(std::optional<IntSize> size) { m_requestedContentSize = size; }

private:
    void callDestructionHandler();

    RetainPtr<SCContentFilter> m_filter;
    String m_deviceID;
    std::optional<IntSize> m_requestedContentSize;
    DestructionHandler m_destructionHandler;
};

class ScreenCaptureKitSharingSessionManager : public RefCountedAndCanMakeWeakPtr<ScreenCaptureKitSharingSessionManager> {
public:
    WEBCORE_EXPORT static ScreenCaptureKitSharingSessionManager& singleton();
    WEBCORE_EXPORT static bool isAvailable();

    ~ScreenCaptureKitSharingSessionManager();

    void contentSharingPickerSelectedFilterForStream(SCContentFilter*, SCStream*);
    void contentSharingPickerFailedWithError(NSError*);
    void contentSharingPickerUpdatedFilterForStream(SCContentFilter*, SCStream*);

    void cancelPicking();

    ScreenCaptureKitContentFilter takeContentFilter(const CaptureDevice&);
    RefPtr<ScreenCaptureSessionSource> createSessionSourceForDevice(WeakPtr<ScreenCaptureSessionSourceObserver>, SCContentFilter*, SCStreamConfiguration*, SCStreamDelegate*);
    void captureSourceWasDestroyedWithoutSession();

    WEBCORE_EXPORT void promptForGetDisplayMedia(DisplayCapturePromptType, CompletionHandler<void(std::optional<CaptureDevice>)>&&);
    // The content size, in pixels, is for windows whose content filter does not know it yet, like a window that was just created.
    // contentFilterDestructionHandler is called with the ID of the selected device when the capture of the window ends,
    // or with an empty string if the window cannot be selected.
    WEBCORE_EXPORT void selectWindowForCapture(uint32_t windowID, std::optional<IntSize> contentSize, CompletionHandler<void(std::optional<CaptureDevice>)>&&, ScreenCaptureKitContentFilter::DestructionHandler&& contentFilterDestructionHandler);
    WEBCORE_EXPORT void cancelGetDisplayMediaPrompt();

private:
    ScreenCaptureKitSharingSessionManager();

    void cleanupAllSessions();
    void completeDeviceSelection(ScreenCaptureKitContentFilter&&);
    String makeDeviceID(uint32_t contentID);

    bool promptWithSCContentSharingPicker(DisplayCapturePromptType);

    NODELETE bool promptingInProgress() const;

    void cleanupSessionSource(ScreenCaptureSessionSource&);

    Vector<WeakPtr<ScreenCaptureSessionSource>> m_activeSources;

    ScreenCaptureKitContentFilter m_pendingContentFilter;

    RetainPtr<WebDisplayMediaPromptHelper> m_promptHelper;
    CompletionHandler<void(std::optional<CaptureDevice>)> m_completionHandler;
    std::unique_ptr<RunLoop::Timer> m_watchdogTimer;
    size_t m_requestCount { 0 };
    uint64_t m_deviceIDCounter { 0 };
};

} // namespace WebCore

#endif // HAVE(SCREEN_CAPTURE_KIT)
