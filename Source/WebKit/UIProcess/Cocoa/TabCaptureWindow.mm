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

// Forward declarations of the private CGVirtualDisplay API. The header lives at
// CoreGraphics.framework/PrivateHeaders/CGVirtualDisplay.h in the Internal SDK, but the module map
// doesn't export it so <CoreGraphics/CGVirtualDisplay.h> doesn't resolve under modular import. The
// symbols themselves are exported by CoreGraphics.framework at runtime, so declaring the interfaces
// we touch is enough.
typedef NS_ENUM(uint32_t, CGVirtualDisplayHiDPIMode) {
    CGVirtualDisplayHiDPIModeDisable = 0,
    CGVirtualDisplayHiDPIModeEnable  = 1,
    CGVirtualDisplayHiDPIModeAuto    = 2,
};

@interface CGVirtualDisplayDescriptor : NSObject
@property (readwrite, assign, nonatomic) uint32_t vendorID;
@property (readwrite, assign, nonatomic) uint32_t productID;
@property (readwrite, assign, nonatomic) uint32_t serialNumber;
@property (readwrite, strong, nullable, nonatomic) NSString *name;
@property (readwrite, assign, nonatomic) CGSize sizeInMillimeters;
@property (readwrite, assign, nonatomic) uint32_t maxPixelsWide;
@property (readwrite, assign, nonatomic) uint32_t maxPixelsHigh;
@end

@interface CGVirtualDisplayMode : NSObject
- (nonnull instancetype)initWithWidth:(uint32_t)width height:(uint32_t)height refreshRate:(double)refreshRate;
@end

@interface CGVirtualDisplaySettings : NSObject
@property (readwrite, strong, nullable, nonatomic) NSArray<CGVirtualDisplayMode *> *modes;
@property (readwrite, assign, nonatomic) CGVirtualDisplayHiDPIMode hiDPI;
@end

@interface CGVirtualDisplay : NSObject
- (nullable instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor * _Nonnull)descriptor;
- (BOOL)applySettings:(CGVirtualDisplaySettings * _Nonnull)settings;
@property (readonly, assign, nonatomic) CGDirectDisplayID displayID;
@end

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

// Creates a virtual display of the given size in points. The caller keeps the display alive for as long as it hosts a window.
static RetainPtr<CGVirtualDisplay> createVirtualDisplay(uint32_t pointsWide, uint32_t pointsHigh, uint32_t& displayID)
{
    displayID = 0;

    RetainPtr descriptor = adoptNS([[CGVirtualDisplayDescriptor alloc] init]);
    [descriptor setName:@"Safari Tab Capture Mirror"];
    [descriptor setVendorID:0x610 /* ASCII 'a' */];
    [descriptor setProductID:0x746361 /* ASCII 'tca' */];
    [descriptor setSerialNumber:0];
    // Give the display plenty of pixel headroom. CG installs its own standard mode sizes (800×600,
    // 1024×768, 1280×960, …) up to `maxPixelsWide/High`, and only installs a HiDPI (2x) variant for a
    // standard mode when both dimensions fit in half of max. If max is close to our content size,
    // the HiDPI variants top out well below our content and the only mode that fits is a 1x one.
    // 4000×3000 ensures HiDPI variants exist for all standard sizes up to 2000×1500 points, which
    // covers any plausible WKWebView.
    // The display is 1x at twice the requested size, which can be more than that on large screens.
    [descriptor setMaxPixelsWide:std::max<uint32_t>(4000, 2 * pointsWide)];
    [descriptor setMaxPixelsHigh:std::max<uint32_t>(3000, 2 * pointsHigh)];
    // Fake an unambiguously HiDPI physical panel so the OS picks the HiDPI (2x) variant of the mode
    // rather than the 1x variant. hiDPI = Enable installs both; the chooser is density-based and
    // only picks HiDPI when pixels/inch is high enough. ~290 ppi turns out to NOT be enough, so
    // shrink the fake physical size so density is beyond any plausible threshold: pointsWide/20 mm
    // gives ~1000 ppi at 2x.
    [descriptor setSizeInMillimeters:CGSizeMake(pointsWide / 20.0, pointsHigh / 20.0)];

    RetainPtr settings = adoptNS([[CGVirtualDisplaySettings alloc] init]);
    [settings setHiDPI:CGVirtualDisplayHiDPIModeEnable];
    RetainPtr mode = adoptNS([[CGVirtualDisplayMode alloc] initWithWidth:pointsWide height:pointsHigh refreshRate:60]);
    [settings setModes:@[mode.get()]];

    RetainPtr display = adoptNS([[CGVirtualDisplay alloc] initWithDescriptor:descriptor.get()]);
    if (!display) {
        RELEASE_LOG_ERROR(WebRTC, "TabCaptureWindow createVirtualDisplay - initWithDescriptor returned nil.");
        return nullptr;
    }
    if (![display applySettings:settings.get()]) {
        RELEASE_LOG_ERROR(WebRTC, "TabCaptureWindow createVirtualDisplay - applySettings failed.");
        return nullptr;
    }

    displayID = [display displayID];
    RELEASE_LOG(WebRTC, "TabCaptureWindow createVirtualDisplay - created %ux%u (points) display, displayID=%u.", pointsWide, pointsHigh, displayID);
    return display;
}

static NSScreen *screenForDisplayID(uint32_t displayID)
{
    for (NSScreen *screen in [NSScreen screens]) {
        NSNumber *idNumber = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
        if ([idNumber unsignedIntValue] == displayID)
            return screen;
    }
    return nil;
}

struct VirtualDisplayWaitContext {
    uint32_t displayID { 0 };
    std::atomic<bool> appeared { false };
};

static void virtualDisplayReconfigurationCallback(CGDirectDisplayID display, CGDisplayChangeSummaryFlags flags, void* userInfo)
{
    auto* context = static_cast<VirtualDisplayWaitContext*>(userInfo);
    if (static_cast<uint32_t>(display) == context->displayID && (flags & kCGDisplayAddFlag))
        context->appeared.store(true, std::memory_order_release);
}

// A virtual display may not appear in [NSScreen screens] on the run loop turn it is created. Spin the run loop so that
// the display reconfiguration callback can fire, and also keep polling [NSScreen screens]: the callback may fire before
// NSScreen updates, or be missed entirely.
static NSScreen *waitForVirtualDisplayScreen(uint32_t displayID, NSTimeInterval maxWait)
{
    if (NSScreen *screen = screenForDisplayID(displayID))
        return screen;

    VirtualDisplayWaitContext context;
    context.displayID = displayID;
    CGError error = CGDisplayRegisterReconfigurationCallback(virtualDisplayReconfigurationCallback, &context);
    if (error != kCGErrorSuccess)
        RELEASE_LOG_ERROR(WebRTC, "TabCaptureWindow waitForVirtualDisplayScreen - CGDisplayRegisterReconfigurationCallback failed (%d); falling back to polling only.", error);

    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:maxWait];
    NSScreen *screen = nil;
    while (!screen && [[NSDate date] compare:deadline] == NSOrderedAscending) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
        screen = screenForDisplayID(displayID);
    }
    if (error == kCGErrorSuccess)
        CGDisplayRemoveReconfigurationCallback(virtualDisplayReconfigurationCallback, &context);
    return screen;
}

// The window is far off screen, where it does not belong to any display, which makes its backing scale and
// ScreenCaptureKit's view of it unreliable. Put it on a virtual display instead when possible.
static RetainPtr<CGVirtualDisplay> createVirtualDisplayForWebView(WKWebView *webView, CGFloat topInset, NSPoint& windowOrigin)
{
    NSRect webViewFrame = [webView frame];
    NSSize contentSize = NSMakeSize(NSWidth(webViewFrame) ?: fallbackWidth, std::max<CGFloat>(1, (NSHeight(webViewFrame) ?: fallbackHeight) - topInset));

    // Size the display for the largest the WKWebView can get on its screen, so that resizing the
    // capture window never needs to reconfigure the display, which can abort a live resize.
    NSSize displaySize = contentSize;
    if (NSScreen *webViewScreen = [[webView window] screen])
        displaySize = NSMakeSize(std::max(contentSize.width, NSWidth([webViewScreen frame])), std::max(contentSize.height, NSHeight([webViewScreen frame])));

    uint32_t displayID = 0;
    RetainPtr virtualDisplay = createVirtualDisplay(static_cast<uint32_t>(displaySize.width), static_cast<uint32_t>(displaySize.height), displayID);
    if (!virtualDisplay || !displayID)
        return nullptr;

    NSScreen *screen = waitForVirtualDisplayScreen(displayID, 1.0);
    if (!screen) {
        RELEASE_LOG_ERROR(WebRTC, "TabCaptureWindow createVirtualDisplayForWebView - virtual display %u created but its screen never appeared; falling back to off-screen placement.", displayID);
        return nullptr;
    }

    // CoreGraphics rejects CGDisplaySetDisplayMode on virtual displays, and keeps the 1x variant of the mode. The window
    // compensates by scaling the layer tree mirror, see m_mirrorScale.
    windowOrigin = [screen frame].origin;
    RELEASE_LOG(WebRTC, "TabCaptureWindow createVirtualDisplayForWebView - virtual display %u at (%g,%g).", displayID, windowOrigin.x, windowOrigin.y);
    return virtualDisplay;
}

static RetainPtr<NSWindow> createOffscreenWindow(NSPoint origin)
{
    NSRect offscreenFrame = NSMakeRect(origin.x, origin.y, fallbackWidth, fallbackHeight);
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

    NSPoint windowOrigin = NSMakePoint(-100000, -100000);
    RetainPtr<CGVirtualDisplay> virtualDisplay;
    // To compare both approaches, the WebKitTabCaptureUsesOffscreenWindow default keeps the window far off screen.
    if ([[NSUserDefaults standardUserDefaults] boolForKey:@"WebKitTabCaptureUsesOffscreenWindow"])
        RELEASE_LOG(WebRTC, "TabCaptureWindow::create - WebKitTabCaptureUsesOffscreenWindow is set; not using a virtual display.");
    else
        virtualDisplay = createVirtualDisplayForWebView(webView.get(), page.obscuredContentInsets().top(), windowOrigin);

    RetainPtr window = createOffscreenWindow(windowOrigin);
    if (!window)
        return nullptr;

    return adoptRef(*new TabCaptureWindow(page, WTF::move(window), WTF::move(virtualDisplay)));
}

TabCaptureWindow::TabCaptureWindow(WebPageProxy& page, RetainPtr<NSWindow>&& window, RetainPtr<CGVirtualDisplay>&& virtualDisplay)
    : m_page(page)
    , m_window(WTF::move(window))
    , m_virtualDisplay(WTF::move(virtualDisplay))
    , m_mirrorScale(m_virtualDisplay ? 2 : 1)
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
    // The mirror of the previous drawing area, if any, has no content anymore.
    [std::exchange(m_layer, nil) removeFromSuperlayer];

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

        // The mirror goes away with its drawing area, for instance when a navigation swaps processes.
        // The page then has a new drawing area, which needs its own mirror.
        result->invalidationPromise->whenSettled(RunLoop::mainSingleton(), [weakThis](auto&&) {
            RefPtr protectedThis = weakThis.get();
            if (!protectedThis)
                return;

            protectedThis->m_mirrorRequest->complete();
            RELEASE_LOG(WebRTC, "TabCaptureWindow::attachLayer - mirror went away; attaching a new one.");
            protectedThis->attachLayer();
        })->track(protectedThis->m_mirrorRequest);

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
    // The cursor layer is also scaled by the mirror scale.
    [cursorLayer setContentsScale:[m_window backingScaleFactor] * m_mirrorScale];
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

    // Make sure to render the WKWebView below the top obscured content inset, scaled by the mirror scale.
    CGFloat inset = page->obscuredContentInsets().top();
    RetainPtr contentView = [m_window contentView];
    CATransform3D sublayerTransform = CATransform3DMakeScale(m_mirrorScale, m_mirrorScale, 1);
    if (inset > 0)
        sublayerTransform = CATransform3DTranslate(sublayerTransform, 0, -inset, 0);
    [[contentView layer] setSublayerTransform:sublayerTransform];

    // The virtual display is sized for the largest the WKWebView can get, so only the window changes.
    NSRect webViewFrame = [webView frame];
    CGFloat width = NSWidth(webViewFrame);
    CGFloat height = NSHeight(webViewFrame) - inset;
    NSSize newSize = NSMakeSize((width > 0 ? width : fallbackWidth) * m_mirrorScale, (height > 0 ? height : fallbackHeight) * m_mirrorScale);
    NSRect currentWindowFrame = [m_window frame];
    if (NSEqualSizes(currentWindowFrame.size, newSize))
        return;
    [m_window setFrame:NSMakeRect(NSMinX(currentWindowFrame), NSMinY(currentWindowFrame), newSize.width, newSize.height) display:NO];
    [contentView setFrame:NSMakeRect(0, 0, newSize.width, newSize.height)];
}

} // namespace WebKit

#endif // HAVE(WINDOW_CAPTURE)
