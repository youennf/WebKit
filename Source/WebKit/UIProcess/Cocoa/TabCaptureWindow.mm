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

#import "config.h"
#import "TabCaptureWindow.h"

#if HAVE(WINDOW_CAPTURE)

#import "Logging.h"
#import "RemoteLayerTreeDrawingAreaProxy.h"
#import "WKWebViewInternal.h"
#import "WebPageProxy.h"
#import <wtf/TZoneMallocInlines.h>
#import <wtf/WeakObjCPtr.h>

@interface WKTabCaptureFlippedView : NSView
@end

@implementation WKTabCaptureFlippedView
- (BOOL)isFlipped
{
    return YES;
}
@end

namespace WebKit {

WTF_MAKE_TZONE_ALLOCATED_IMPL(TabCaptureWindow);

constexpr CGFloat fallbackWidth = 640;
constexpr CGFloat fallbackHeight = 480;

static RetainPtr<NSWindow> createOffscreenWindow()
{
    NSRect offscreenFrame = NSMakeRect(-100000, -100000, fallbackWidth, fallbackHeight);
    RetainPtr window = adoptNS([[NSWindow alloc] initWithContentRect:offscreenFrame styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO]);
    [window setReleasedWhenClosed:NO];
    [window setOpaque:YES];
    [window setHasShadow:NO];
    [window setBackgroundColor:[NSColor whiteColor]];
    [window setLevel:NSNormalWindowLevel];

    RetainPtr contentView = adoptNS([[WKTabCaptureFlippedView alloc] initWithFrame:NSMakeRect(0, 0, fallbackWidth, fallbackHeight)]);
    [contentView setWantsLayer:YES];
    [[contentView layer] setBackgroundColor:[NSColor whiteColor].CGColor];
    [window setContentView:contentView.get()];
    [window orderFrontRegardless];

    if (![window windowNumber]) {
        RELEASE_LOG_ERROR(WebRTC, "TabCaptureWindow createOffscreenWindow - no windowNumber for window %p.", window.get());
        [window close];
        return nullptr;
    }

    return window;
}

RefPtr<TabCaptureWindow> TabCaptureWindow::create(WebPageProxy& page)
{
    RetainPtr webView = page.cocoaView();
    if (!page.cocoaView())
        return nullptr;

    RetainPtr window = createOffscreenWindow();
    if (!window)
        return nullptr;

    return adoptRef(*new TabCaptureWindow(page, WTF::move(window)));
}

TabCaptureWindow::TabCaptureWindow(WebPageProxy& page, RetainPtr<NSWindow>&& window)
    : m_page(page)
    , m_window(WTF::move(window))
    , m_mirrorRequest(NativePromiseRequest::create())
{
    page.setIsBeingCapturedForTabCapture(true);

    updateSize();
    attachLayer();
    installCursorOverlay();
    observeWebViewFrame();
}

TabCaptureWindow::~TabCaptureWindow()
{
    if (m_cursorLocalMonitor)
        [NSEvent removeMonitor:m_cursorLocalMonitor.get()];
    if (m_cursorGlobalMonitor)
        [NSEvent removeMonitor:m_cursorGlobalMonitor.get()];
    [m_cursorRefreshTimer invalidate];
    detachFromPage();
    [m_window close];
}

void TabCaptureWindow::setPage(WebPageProxy& page)
{
    if (m_page == &page)
        return;

    detachFromPage();

    m_page = page;
    page.setIsBeingCapturedForTabCapture(true);

    updateSize();
    attachLayer();
    observeWebViewFrame();
    updateCursor();
}

void TabCaptureWindow::detachFromPage()
{
    if (RetainPtr observer = std::exchange(m_webViewFrameObserver, nil))
        [[NSNotificationCenter defaultCenter] removeObserver:observer.get()];
    [std::exchange(m_layer, nil) removeFromSuperlayer];

    if (m_mirrorRequest->hasCallback())
        m_mirrorRequest->disconnect();

    RefPtr page = m_page.get();
    if (!page)
        return;

    page->setIsBeingCapturedForTabCapture(false);
    if (RefPtr drawingArea = dynamicDowncast<RemoteLayerTreeDrawingAreaProxy>(page->drawingArea()))
        drawingArea->disableMirrorLayerTree();
}

uint32_t TabCaptureWindow::windowID() const
{
    return static_cast<uint32_t>([m_window windowNumber]);
}

WebCore::IntSize TabCaptureWindow::pixelSize() const
{
    NSSize size = [m_window frame].size;
    CGFloat scale = [m_window backingScaleFactor];
    return { static_cast<int>(std::round(size.width * scale)), static_cast<int>(std::round(size.height * scale)) };
}

bool TabCaptureWindow::isCapturing(const WebPageProxy& page) const
{
    return m_page == &page;
}

RetainPtr<WKWebView> TabCaptureWindow::webView() const
{
    RefPtr page = m_page;
    return page ? page->cocoaView() : RetainPtr<WKWebView> { };
}

void TabCaptureWindow::attachLayer()
{
    RefPtr page = m_page.get();
    if (!page)
        return;

    RefPtr drawingArea = dynamicDowncast<RemoteLayerTreeDrawingAreaProxy>(page->drawingArea());
    if (!drawingArea) {
        RELEASE_LOG_ERROR(WebRTC, "TabCaptureWindow::attachLayer - drawing area is not a RemoteLayerTreeDrawingAreaProxy; mirror unavailable.");
        return;
    }

    // The mirror layer arrives once every web process of the page sent the full state of its layer tree.
    // It reproduces the geometry of the layer hosting the primary root layer, so it goes right under the
    // flipped content view, like the hosting layer under the WKWebView.
    // detachFromPage() disconnects m_mirrorRequest before disabling the mirror, so this is not called once the window left the page.
    drawingArea->enableMirrorLayerTree()->whenSettled(RunLoop::mainSingleton(), [weakThis = WeakPtr { *this }](auto&& result) {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis)
            return;

        protectedThis->m_mirrorRequest->complete();
        if (!result) {
            RELEASE_LOG_ERROR(WebRTC, "TabCaptureWindow::attachLayer - could not mirror the layer tree; aborting.");
            return;
        }

        RetainPtr contentViewLayer = [[protectedThis->m_window contentView] layer];
        if (!contentViewLayer) {
            RELEASE_LOG_ERROR(WebRTC, "TabCaptureWindow::attachLayer - no content view layer; aborting.");
            return;
        }

        [contentViewLayer insertSublayer:result->layer.get() atIndex:0];
        protectedThis->m_layer = result->layer;
    })->track(m_mirrorRequest);
}

void TabCaptureWindow::observeWebViewFrame()
{
    RetainPtr webView = this->webView();
    [webView setPostsFrameChangedNotifications:YES];
    WeakPtr weakThis { *this };
    m_webViewFrameObserver = [[NSNotificationCenter defaultCenter] addObserverForName:NSViewFrameDidChangeNotification object:webView.get() queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification *) {
        if (RefPtr protectedThis = weakThis.get())
            protectedThis->updateSize();
    }];
}

void TabCaptureWindow::installCursorOverlay()
{
    // The captured window does not contain the mouse cursor, so draw it in a layer on top of the layer tree mirror.
    // This layer is also under the -topInset sublayer transform of the content view, so updateCursor() adds the
    // top inset to its position.
    RetainPtr cursorLayer = adoptNS([[CALayer alloc] init]);
    [cursorLayer setAnchorPoint:CGPointMake(0, 0)];
    [cursorLayer setContentsScale:[m_window backingScaleFactor]];
    [cursorLayer setZPosition:1000];
    [cursorLayer setHidden:YES];
    [[[m_window contentView] layer] addSublayer:cursorLayer.get()];
    m_cursorLayer = WTF::move(cursorLayer);

    // Local monitor for events in the browser windows, global monitor for events in other applications, so that
    // the cursor is still mirrored when the browser is not the active application. Neither consumes events.
    WeakPtr weakThis { *this };
    auto cursorEventBlock = ^(NSEvent *) {
        if (RefPtr protectedThis = weakThis.get())
            protectedThis->updateCursor();
    };
    m_cursorLocalMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:(NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged | NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp | NSEventMaskRightMouseDown | NSEventMaskRightMouseUp) handler:^NSEvent *(NSEvent *event) {
        cursorEventBlock(event);
        return event;
    }];
    m_cursorGlobalMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:(NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged) handler:cursorEventBlock];

    // Browser windows only deliver mouse moved events to the local monitor if they accept them.
    if (RetainPtr webViewWindow = [webView() window]; webViewWindow && ![webViewWindow acceptsMouseMovedEvents])
        [webViewWindow setAcceptsMouseMovedEvents:YES];

    // The cursor can change without mouse moves, for instance from JavaScript.
    m_cursorRefreshTimer = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *) {
        if (RefPtr protectedThis = weakThis.get())
            protectedThis->updateCursor();
    }];
    [[NSRunLoop mainRunLoop] addTimer:m_cursorRefreshTimer.get() forMode:NSRunLoopCommonModes];

    updateCursor();
}

void TabCaptureWindow::updateCursor()
{
    RefPtr page = m_page.get();
    if (!m_cursorLayer || !page)
        return;

    RetainPtr webView = this->webView();
    RetainPtr webViewWindow = [webView window];
    if (!webViewWindow) {
        [m_cursorLayer setHidden:YES];
        return;
    }

    // Map the mouse location to WKWebView coordinates with a top-left origin.
    NSPoint mouseInWindow = [webViewWindow convertPointFromScreen:[NSEvent mouseLocation]];
    NSRect webViewInWindow = [webView convertRect:[webView bounds] toView:nil];
    CGFloat localX = mouseInWindow.x - NSMinX(webViewInWindow);
    CGFloat localYFromTop = NSHeight(webViewInWindow) - (mouseInWindow.y - NSMinY(webViewInWindow));

    // Hide the cursor when the mouse is outside of the WKWebView or above the top obscured content inset.
    CGFloat topInset = page->obscuredContentInsets().top();
    bool isInsideContent = localX >= 0 && localX <= NSWidth(webViewInWindow) && localYFromTop >= topInset && localYFromTop <= NSHeight(webViewInWindow);
    if (!isInsideContent) {
        [m_cursorLayer setHidden:YES];
        return;
    }

    // PageClientImplMac::setCursor sets the cursor of the hit-tested element, so the current cursor reflects
    // link, I-beam, resize and custom cursors.
    RetainPtr cursor = [NSCursor currentCursor];
    RetainPtr image = [cursor image];
    NSSize imageSize = image ? [image size] : NSZeroSize;
    if (imageSize.width <= 0 || imageSize.height <= 0) {
        [m_cursorLayer setHidden:YES];
        return;
    }

    CGImageRef cgImage = [image CGImageForProposedRect:nullptr context:nil hints:nil];
    if (!cgImage) {
        [m_cursorLayer setHidden:YES];
        return;
    }

    NSPoint hotSpot = [cursor hotSpot];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [m_cursorLayer setContents:(__bridge id)cgImage];
    [m_cursorLayer setBounds:CGRectMake(0, 0, imageSize.width, imageSize.height)];
    // The content view sublayer transform shifts the cursor layer up by the top inset, like the mirror.
    [m_cursorLayer setPosition:CGPointMake(localX - hotSpot.x, localYFromTop - hotSpot.y)];
    [m_cursorLayer setHidden:NO];
    [CATransaction commit];
}

void TabCaptureWindow::updateSize()
{
    RefPtr page = m_page;
    if (!m_page)
        return;

    RetainPtr webView = this->webView();
    if (!webView)
        return;

    // Make sure to render the WKWebView below the top obscured content inset.
    CGFloat inset = page->obscuredContentInsets().top();
    RetainPtr contentView = [m_window contentView];
    [[contentView layer] setSublayerTransform:inset > 0 ? CATransform3DMakeTranslation(0, -inset, 0) : CATransform3DIdentity];

    NSRect webViewFrame = [webView frame];
    CGFloat width = NSWidth(webViewFrame);
    CGFloat height = NSHeight(webViewFrame) - inset;
    NSSize newSize = NSMakeSize(width > 0 ? width : fallbackWidth, height > 0 ? height : fallbackHeight);
    NSRect currentWindowFrame = [m_window frame];
    if (NSEqualSizes(currentWindowFrame.size, newSize))
        return;
    [m_window setFrame:NSMakeRect(NSMinX(currentWindowFrame), NSMinY(currentWindowFrame), newSize.width, newSize.height) display:NO];
    [contentView setFrame:NSMakeRect(0, 0, newSize.width, newSize.height)];
}

} // namespace WebKit

#endif // HAVE(WINDOW_CAPTURE)
