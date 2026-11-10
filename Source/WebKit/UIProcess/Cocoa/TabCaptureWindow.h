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
#include <wtf/RefCountedAndCanMakeWeakPtr.h>
#include <wtf/RetainPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/WeakPtr.h>

OBJC_CLASS CALayer;
OBJC_CLASS NSObject;
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
    bool isCapturing(const WebPageProxy&) const;

    // Matches the window size to the WKWebView frame and obscured content insets.
    void updateSize();

private:
    TabCaptureWindow(WebPageProxy&, RetainPtr<NSWindow>&&);

    RetainPtr<WKWebView> webView() const;
    void attachLayer();
    void observeWebViewFrame();

    WeakPtr<WebPageProxy> m_page;
    RetainPtr<NSWindow> m_window;
    RetainPtr<CALayer> m_layer;
    RetainPtr<NSObject> m_webViewFrameObserver;
};

} // namespace WebKit

#endif // HAVE(WINDOW_CAPTURE)
