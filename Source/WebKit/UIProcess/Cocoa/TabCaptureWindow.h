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
 */

#pragma once

#if HAVE(WINDOW_CAPTURE)

#include <WebCore/IntSize.h>
#include <wtf/NativePromise.h>
#include <wtf/RefCountedAndCanMakeWeakPtr.h>
#include <wtf/RetainPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/WeakPtr.h>

OBJC_CLASS CALayer;
OBJC_CLASS CGVirtualDisplay;
OBJC_CLASS NSObject;
OBJC_CLASS NSTimer;
OBJC_CLASS NSWindow;
OBJC_CLASS WKWebView;

namespace WebKit {

class WebPageProxy;

class TabCaptureWindow : public RefCountedAndCanMakeWeakPtr<TabCaptureWindow> {
    WTF_MAKE_TZONE_ALLOCATED(TabCaptureWindow);
public:
    static RefPtr<TabCaptureWindow> create(WebPageProxy&);
    ~TabCaptureWindow();

    uint32_t windowID() const;
    // ScreenCaptureKit does not know the size of a window that was just created.
    WebCore::IntSize pixelSize() const;
    bool isCapturing(const WebPageProxy&) const;
    // Moves the capture to another page, for instance when a browser replaces the WKWebView of the captured tab.
    void setPage(WebPageProxy&);

    // Matches the window size to the WKWebView frame and obscured content insets.
    void updateSize();

private:
    TabCaptureWindow(WebPageProxy&, RetainPtr<NSWindow>&&, RetainPtr<CGVirtualDisplay>&&);

    RetainPtr<WKWebView> webView() const;
    void attachLayer();
    void detachFromPage();
    void observeWebViewFrame();
    void installCursorOverlay();
    void updateCursor();

    WeakPtr<WebPageProxy> m_page;
    RetainPtr<NSWindow> m_window;
    // Keeps the display hosting the window, if any, alive for the lifetime of the window.
    RetainPtr<CGVirtualDisplay> m_virtualDisplay;
    // The virtual display is 1x, so the window is sized twice the WKWebView and scales the layer tree mirror by 2 to keep
    // the retina pixel density. 1 without a virtual display.
    CGFloat m_mirrorScale { 1 };
    RetainPtr<CALayer> m_layer;
    // Tracks the attachment of the layer tree mirror.
    const Ref<NativePromiseRequest> m_mirrorRequest;
    RetainPtr<NSObject> m_webViewFrameObserver;
    RetainPtr<CALayer> m_cursorLayer;
    RetainPtr<NSTimer> m_cursorRefreshTimer;
    RetainPtr<NSObject> m_cursorLocalMonitor;
    RetainPtr<NSObject> m_cursorGlobalMonitor;
};

} // namespace WebKit

#endif // HAVE(WINDOW_CAPTURE)
