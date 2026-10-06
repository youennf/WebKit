/*
 * Copyright (C) 2021 Apple Inc. All rights reserved.
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
#import "DisplayCaptureSessionManager.h"

#if PLATFORM(COCOA) && ENABLE(MEDIA_STREAM)

#import "APIPageConfiguration.h"
#import "Logging.h"
#import "MediaPermissionUtilities.h"
#import "PageLoadState.h"
#import "RemoteLayerTreeDrawingAreaProxy.h"
#import "RemoteLayerTreeHost.h"
#import "WKWebViewInternal.h"
#import "WebPageProxy.h"
#import "WebProcess.h"
#import "WebProcessPool.h"
#import "WebProcessProxy.h"
#import <WebCore/CaptureDeviceManager.h>
#import <WebCore/LocalizedStrings.h>
#import <WebCore/MockRealtimeMediaSourceCenter.h>
#import <WebCore/ScreenCaptureKitCaptureSource.h>
#import <WebCore/ScreenCaptureKitSharingSessionManager.h>
#import <WebCore/SecurityOriginData.h>
#import <wtf/BlockPtr.h>
#import <wtf/Borrow.h>
#import <wtf/MainThread.h>
#import <wtf/NeverDestroyed.h>
#import <wtf/URLHelpers.h>
#import <wtf/WeakObjCPtr.h>
#import <wtf/cocoa/TypeCastsCocoa.h>
#import <wtf/text/StringToIntegerConversion.h>

#import <pal/spi/cg/CoreGraphicsSPI.h>
#import <pal/spi/cocoa/QuartzCoreSPI.h>

// Forward declarations of the private CGVirtualDisplay API. The header lives at
// CoreGraphics.framework/PrivateHeaders/CGVirtualDisplay.h in the Internal SDK, but the module map
// doesn't export it so <CoreGraphics/CGVirtualDisplay.h> doesn't resolve under modular import. The
// symbols themselves are exported by CoreGraphics.framework at runtime, so declaring the interfaces
// we touch is enough — no header, no __has_include dance.
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

@interface WKTabCaptureMirrorFlippedView : NSView
@end
@implementation WKTabCaptureMirrorFlippedView
- (BOOL)isFlipped { return YES; }
@end

@interface WKTabCapturePickerOverlay : NSView
- (instancetype)initWithFrame:(NSRect)frame shareBlock:(void(^)(void))shareBlock cancelBlock:(void(^)(void))cancelBlock;
- (void)setShowsCaptureTint:(BOOL)shows;
- (NSView *)cardView;
- (void)invalidate;
@end

@implementation WKTabCapturePickerOverlay {
    void (^_shareBlock)(void);
    void (^_cancelBlock)(void);
    BOOL _showsCaptureTint;
    RetainPtr<NSView> _cardView;
}

- (instancetype)initWithFrame:(NSRect)frame shareBlock:(void(^)(void))shareBlock cancelBlock:(void(^)(void))cancelBlock
{
    if (!(self = [super initWithFrame:frame]))
        return nil;

    _shareBlock = [shareBlock copy];
    _cancelBlock = [cancelBlock copy];

    [self setWantsLayer:YES];
    [self setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [[self layer] setBackgroundColor:[NSColor clearColor].CGColor];
    [[self layer] setBorderWidth:0];

    NSRect bounds = [self bounds];
    CGFloat cardWidth = 260;
    CGFloat cardHeight = 88;
    NSRect cardFrame = NSMakeRect((NSWidth(bounds) - cardWidth) / 2, (NSHeight(bounds) - cardHeight) / 2, cardWidth, cardHeight);
    RetainPtr card = adoptNS([[NSVisualEffectView alloc] initWithFrame:cardFrame]);
    [card setMaterial:NSVisualEffectMaterialHUDWindow];
    [card setBlendingMode:NSVisualEffectBlendingModeWithinWindow];
    [card setState:NSVisualEffectStateActive];
    [card setWantsLayer:YES];
    [[card layer] setCornerRadius:12];
    [[card layer] setMasksToBounds:YES];
    [[card layer] setBorderColor:[[NSColor systemBlueColor] colorWithAlphaComponent:0.9].CGColor];
    [[card layer] setBorderWidth:1];
    [card setAutoresizingMask:NSViewMinXMargin | NSViewMaxXMargin | NSViewMinYMargin | NSViewMaxYMargin];
    [self addSubview:card.get()];
    _cardView = card;

    CGFloat buttonWidth = cardWidth - 24;
    NSRect shareFrame = NSMakeRect((cardWidth - buttonWidth) / 2, 46, buttonWidth, 28);
    RetainPtr shareButton = adoptNS([[NSButton alloc] initWithFrame:shareFrame]);
    [shareButton setBezelStyle:NSBezelStyleRounded];
    [shareButton setTitle:@"Share This Tab"];
    [shareButton setKeyEquivalent:@"\r"];
    [shareButton setTarget:self];
    [shareButton setAction:@selector(_shareClicked:)];
    [card addSubview:shareButton.get()];

    NSRect cancelFrame = NSMakeRect((cardWidth - buttonWidth) / 2, 10, buttonWidth, 26);
    RetainPtr cancelButton = adoptNS([[NSButton alloc] initWithFrame:cancelFrame]);
    [cancelButton setBezelStyle:NSBezelStyleRounded];
    [cancelButton setTitle:@"Cancel"];
    [cancelButton setKeyEquivalent:@"\E"];
    [cancelButton setTarget:self];
    [cancelButton setAction:@selector(_cancelClicked:)];
    [card addSubview:cancelButton.get()];

    return self;
}

- (void)setShowsCaptureTint:(BOOL)shows
{
    if (_showsCaptureTint == shows)
        return;
    _showsCaptureTint = shows;
    if (shows) {
        [[self layer] setBackgroundColor:[[NSColor systemBlueColor] colorWithAlphaComponent:0.18].CGColor];
        [[self layer] setBorderColor:[[NSColor systemBlueColor] colorWithAlphaComponent:0.9].CGColor];
        [[self layer] setBorderWidth:3];
    } else {
        [[self layer] setBackgroundColor:[NSColor clearColor].CGColor];
        [[self layer] setBorderWidth:0];
    }
}

- (NSView *)cardView { return _cardView.get(); }

- (void)invalidate
{
    _shareBlock = nil;
    _cancelBlock = nil;
    [self removeFromSuperview];
}

- (void)_shareClicked:(id)sender
{
    auto block = _shareBlock;
    _shareBlock = nil;
    _cancelBlock = nil;
    if (block)
        block();
}

- (void)_cancelClicked:(id)sender
{
    auto block = _cancelBlock;
    _shareBlock = nil;
    _cancelBlock = nil;
    if (block)
        block();
}

- (NSView *)hitTest:(NSPoint)point
{
    NSPoint local = [self convertPoint:point fromView:[self superview]];
    if (_cardView && NSPointInRect(local, [_cardView frame]))
        return [_cardView hitTest:point];
    return nil;
}

@end

namespace WebKit {

#if HAVE(SCREEN_CAPTURE_KIT)
void DisplayCaptureSessionManager::alertForGetDisplayMedia(WebPageProxy& page, const WebCore::SecurityOriginData& origin, CompletionHandler<void(DisplayCaptureSessionManager::CaptureSessionType)>&& completionHandler)
{
#if HAVE(WINDOW_CAPTURE)
    auto webView = page.cocoaView();
    if (!webView) {
        completionHandler(DisplayCaptureSessionManager::CaptureSessionType::None);
        return;
    }

    RetainPtr visibleOrigin = applicationVisibleNameFromOrigin(origin);
    if (!visibleOrigin)
        visibleOrigin = applicationVisibleName();

    SUPPRESS_UNRETAINED_ARG RetainPtr alertTitle = adoptNS([[NSString alloc] initWithFormat:@"Allow “%@” to observe a window, screen, or tab?", visibleOrigin.get()]);
    RetainPtr<NSString> allowWindowOrScreenButtonString = @"Allow to Share Window or Screen";
    RetainPtr<NSString> allowTabButtonString = @"Allow to Share Tab";
    RetainPtr<NSString> doNotAllowButtonString = @"Don’t Allow";

    RetainPtr alert = adoptNS([[NSAlert alloc] init]);
    [alert setMessageText:alertTitle.get()];

    RetainPtr button = [alert addButtonWithTitle:allowWindowOrScreenButtonString.get()];
    button.get().keyEquivalent = @"";

    button = [alert addButtonWithTitle:allowTabButtonString.get()];
    button.get().keyEquivalent = @"";

    button = [alert addButtonWithTitle:doNotAllowButtonString.get()];
    button.get().keyEquivalent = @"\E";

    [alert beginSheetModalForWindow:retainPtr([webView window]).get() completionHandler:[completionBlock = makeBlockPtr(WTF::move(completionHandler))](NSModalResponse returnCode) {
        DisplayCaptureSessionManager::CaptureSessionType result = DisplayCaptureSessionManager::CaptureSessionType::None;
        switch (returnCode) {
        case NSAlertFirstButtonReturn:
            result = DisplayCaptureSessionManager::CaptureSessionType::Window;
            break;
        case NSAlertSecondButtonReturn:
            result = DisplayCaptureSessionManager::CaptureSessionType::Tab;
            break;
        case NSAlertThirdButtonReturn:
            result = DisplayCaptureSessionManager::CaptureSessionType::None;
            break;
        }

        completionBlock(result);
    }];
#else
    UNUSED_PARAM(page);
    UNUSED_PARAM(origin);
    UNUSED_PARAM(completionHandler);
#endif // HAVE(WINDOW_CAPTURE)
}

void DisplayCaptureSessionManager::showTabPicker(WebPageProxy& requestingPage, CompletionHandler<void(std::optional<WebCore::CaptureDevice>)>&& completionHandler)
{
    if (!requestingPage.cocoaView()) {
        completionHandler(std::nullopt);
        return;
    }
    if (m_pickerCompletion) {
        completionHandler(std::nullopt);
        return;
    }

    m_pickerRequestingPage = requestingPage;
    m_pickerCompletion = std::make_shared<CompletionHandler<void(std::optional<WebCore::CaptureDevice>)>>(WTF::move(completionHandler));
    m_pickerOverlays = adoptNS([[NSMutableArray alloc] init]);

    // Event monitor. WKWebView bypasses NSView subview hit-testing for its own content, so overlay
    // subviews cannot swallow clicks via -hitTest:, nor can NSButtons receive them via normal dispatch.
    // Intercept mouse events at the app level:
    //  - Click on a card button: fire the button's action directly, swallow the event.
    //  - Mouse-moved over an overlay: force arrow cursor and swallow (don't change to text/link cursor).
    //  - Scroll-wheel over an overlay: swallow so the page beneath doesn't scroll.
    //  - Otherwise pass through.
    m_pickerEventMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:(NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown | NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp | NSEventMaskMouseMoved | NSEventMaskScrollWheel) handler:^NSEvent *(NSEvent *event) {
        auto& mgr = DisplayCaptureSessionManager::singleton();
        if (!mgr.m_pickerOverlays)
            return event;
        NSWindow *eventWindow = [event window];
        NSPoint locationInWindow = [event locationInWindow];
        for (WKTabCapturePickerOverlay *overlay in mgr.m_pickerOverlays.get()) {
            if ([overlay window] != eventWindow)
                continue;
            NSView *card = [overlay cardView];
            NSPoint locationInOverlay = [overlay convertPoint:locationInWindow fromView:nil];
            bool insideOverlay = NSPointInRect(locationInOverlay, [overlay bounds]);
            if (!insideOverlay)
                return event;
            bool insideCard = card && NSPointInRect(locationInOverlay, [card frame]);
            NSEventType type = [event type];
            if (type == NSEventTypeMouseMoved) {
                [[NSCursor arrowCursor] set];
                return nil;
            }
            if (type == NSEventTypeScrollWheel)
                return nil;
            if (insideCard) {
                if (type == NSEventTypeLeftMouseDown || type == NSEventTypeRightMouseDown) {
                    NSPoint locationInCard = [card convertPoint:locationInWindow fromView:nil];
                    for (NSView *subview in [card subviews]) {
                        if (NSPointInRect(locationInCard, [subview frame]) && [subview isKindOfClass:[NSButton class]]) {
                            [(NSButton *)subview performClick:nil];
                            break;
                        }
                    }
                }
                return nil;
            }
            // Outside the card but inside the overlay — swallow only on hovered window (tinted).
            if (eventWindow == mgr.m_pickerHoveredWindow.get())
                return nil;
            return event;
        }
        return event;
    }];

    // Enable mouse-moved events on Safari's windows for the picker's lifetime so our monitor fires
    // and we can override cursor + swallow scrolling.
    m_pickerWindowsWithMouseMovedEnabled = adoptNS([[NSMutableArray alloc] init]);
    for (Ref process : WebProcessProxy::allProcesses()) {
        for (Ref page : process->pages()) {
            RetainPtr<NSView> view = page->cocoaView();
            NSWindow *window = view ? [view window] : nil;
            if (!window || [window acceptsMouseMovedEvents])
                continue;
            [window setAcceptsMouseMovedEvents:YES];
            [m_pickerWindowsWithMouseMovedEnabled addObject:window];
        }
    }

    // Poll to install/refresh overlays and track hovered window.
    m_pickerHoverTimer = [NSTimer timerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *) {
        DisplayCaptureSessionManager::singleton().pollPickerOverlays();
    }];
    [[NSRunLoop mainRunLoop] addTimer:m_pickerHoverTimer.get() forMode:NSRunLoopCommonModes];
    pollPickerOverlays();
}

static RetainPtr<NSView> pickerVisibleWKWebViewInWindow(NSWindow *window)
{
    if (!window)
        return { };
    RetainPtr<NSView> fallback;
    for (Ref process : WebProcessProxy::allProcesses()) {
        for (Ref page : process->pages()) {
            RetainPtr<NSView> view = page->cocoaView();
            if (!view || [view window] != window)
                continue;
            if (!fallback)
                fallback = view;
            if (![view isHidden] && [view superview])
                return view;
        }
    }
    return fallback;
}

static RefPtr<WebPageProxy> pickerPageForCocoaView(NSView *view)
{
    if (!view)
        return nullptr;
    for (Ref process : WebProcessProxy::allProcesses()) {
        for (Ref page : process->pages()) {
            if (page->cocoaView().get() == view)
                return page.ptr();
        }
    }
    return nullptr;
}

static NSWindow *pickerWindowUnderMouse()
{
    NSPoint mouseLocation = [NSEvent mouseLocation];
    NSInteger windowNumber = [NSWindow windowNumberAtPoint:mouseLocation belowWindowWithWindowNumber:0];
    if (!windowNumber)
        return nil;
    return [NSApp windowWithWindowNumber:windowNumber];
}

static bool pickerWindowContainsWKWebView(NSWindow *window)
{
    if (!window)
        return false;
    for (Ref process : WebProcessProxy::allProcesses()) {
        for (Ref page : process->pages()) {
            RetainPtr<NSView> view = page->cocoaView();
            if (view && [view window] == window)
                return true;
        }
    }
    return false;
}

void DisplayCaptureSessionManager::pollPickerOverlays()
{
    if (!m_pickerCompletion)
        return;

    // Update hovered window first so tint reflects latest state.
    NSWindow *hovered = pickerWindowUnderMouse();
    if (hovered && pickerWindowContainsWKWebView(hovered))
        m_pickerHoveredWindow = hovered;

    // Remove stale overlays (their WKWebView host is no longer the visible one, or they got detached).
    RetainPtr<NSMutableSet> windowsWithOverlays = adoptNS([[NSMutableSet alloc] init]);
    RetainPtr<NSMutableArray> stale = adoptNS([[NSMutableArray alloc] init]);
    for (WKTabCapturePickerOverlay *overlay in m_pickerOverlays.get()) {
        NSWindow *win = [overlay window];
        NSView *superview = [overlay superview];
        RetainPtr<NSView> desired = win ? pickerVisibleWKWebViewInWindow(win) : nullptr;
        // Overlay is attached to the WKWebView's superview (so empty-tab WKWebViews, which are
        // hidden but still in the hierarchy, remain pickable). Fall back to the window's content
        // view if the WKWebView ever has no superview.
        RetainPtr<NSView> desiredHost = desired ? [desired superview] : nullptr;
        if (!desiredHost)
            desiredHost = win ? [win contentView] : nullptr;
        if (!win || !desired || !desiredHost || superview != desiredHost.get()) {
            [stale addObject:overlay];
            continue;
        }
        // Keep overlay on top of any sibling that Safari may have inserted.
        [desiredHost addSubview:overlay positioned:NSWindowAbove relativeTo:nil];
        [windowsWithOverlays addObject:win];
    }
    for (WKTabCapturePickerOverlay *overlay in stale.get())
        [overlay invalidate];
    [m_pickerOverlays removeObjectsInArray:stale.get()];

    // Install on any Safari window that lacks an overlay.
    for (Ref process : WebProcessProxy::allProcesses()) {
        for (Ref page : process->pages()) {
            if (!page->hasRunningProcess())
                continue;
            RetainPtr<NSView> view = page->cocoaView();
            if (!view)
                continue;
            NSWindow *window = [view window];
            if (!window || [windowsWithOverlays containsObject:window])
                continue;
            [windowsWithOverlays addObject:window];
            installPickerOverlayInWindow(window);
        }
    }

    // Toggle tint: only the hovered window shows the blue capture-region tint.
    for (WKTabCapturePickerOverlay *overlay in m_pickerOverlays.get())
        [overlay setShowsCaptureTint:([overlay window] == m_pickerHoveredWindow.get())];
}

void DisplayCaptureSessionManager::installPickerOverlayInWindow(NSWindow *window)
{
    if (!m_pickerCompletion || !window)
        return;
    RetainPtr<NSView> webView = pickerVisibleWKWebViewInWindow(window);
    if (!webView)
        return;

    // Attach the overlay to the WKWebView's superview rather than the WKWebView itself. On empty
    // Safari tabs (Start Page / Favorites / Top Sites) the WKWebView is still present in the view
    // hierarchy but with hidden=YES — a subview overlay would be hidden along with it, so empty
    // tabs could not be picked. Attaching to the superview makes the overlay a sibling of the
    // WKWebView and keeps it visible regardless of the WKWebView's hidden state. We still bind the
    // picker to the (possibly hidden) WKWebView's WebPageProxy, so selecting an empty tab starts
    // capture on that page — frames come out blank until the user navigates somewhere, at which
    // point the same WKWebView becomes visible and the mirror picks up the content automatically.
    RetainPtr<NSView> overlayHost = [webView superview];
    if (!overlayHost)
        overlayHost = [window contentView];
    if (!overlayHost)
        return;

    // Clip overlay to the intersection of the WKWebView with the window's contentLayoutRect so it
    // doesn't cover Safari's title/tab bar area (Safari uses FullSizeContentView).
    NSRect webViewInWindow = [webView convertRect:[webView bounds] toView:nil];
    NSRect visibleContentInWindow = NSIntersectionRect(webViewInWindow, [window contentLayoutRect]);
    NSRect overlayFrame = [overlayHost convertRect:visibleContentInWindow fromView:nil];

    RetainPtr window_ = window;
    auto shareBlock = ^{
        auto& mgr = DisplayCaptureSessionManager::singleton();
        if (!mgr.m_pickerCompletion)
            return;
        auto completion = WTF::move(*mgr.m_pickerCompletion);
        RetainPtr<NSView> host = pickerVisibleWKWebViewInWindow(window_.get());
        RefPtr picked = pickerPageForCocoaView(host.get());
        mgr.endPickerSession();
        if (!picked) {
            completion(std::nullopt);
            return;
        }
        mgr.startTabCapture(*picked, WTF::move(completion));
    };
    auto cancelBlock = ^{
        auto& mgr = DisplayCaptureSessionManager::singleton();
        if (!mgr.m_pickerCompletion)
            return;
        auto completion = WTF::move(*mgr.m_pickerCompletion);
        mgr.endPickerSession();
        completion(std::nullopt);
    };

    RetainPtr overlay = adoptNS([[WKTabCapturePickerOverlay alloc] initWithFrame:overlayFrame shareBlock:shareBlock cancelBlock:cancelBlock]);
    [overlay setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [overlayHost addSubview:overlay.get() positioned:NSWindowAbove relativeTo:nil];
    [m_pickerOverlays addObject:overlay.get()];
}

void DisplayCaptureSessionManager::dismissPickerOverlays()
{
    if (!m_pickerOverlays)
        return;
    for (WKTabCapturePickerOverlay *overlay in m_pickerOverlays.get())
        [overlay invalidate];
    [m_pickerOverlays removeAllObjects];
}

void DisplayCaptureSessionManager::endPickerSession()
{
    dismissPickerOverlays();
    m_pickerOverlays = nullptr;
    if (m_pickerHoverTimer) {
        [m_pickerHoverTimer invalidate];
        m_pickerHoverTimer = nullptr;
    }
    if (m_pickerEventMonitor) {
        [NSEvent removeMonitor:m_pickerEventMonitor.get()];
        m_pickerEventMonitor = nullptr;
    }
    if (m_pickerWindowsWithMouseMovedEnabled) {
        for (NSWindow *window in m_pickerWindowsWithMouseMovedEnabled.get())
            [window setAcceptsMouseMovedEvents:NO];
        m_pickerWindowsWithMouseMovedEnabled = nullptr;
    }
    m_pickerHoveredWindow = nullptr;
    m_pickerRequestingPage = nullptr;
    m_pickerCompletion = nullptr;
}

UNUSED_FUNCTION static bool webViewIsVisibleOnScreen(NSView *webView)
{
    if (!webView)
        return false;
    NSWindow *window = [webView window];
    if (!window || ![window isVisible])
        return false;
    if ([webView isHiddenOrHasHiddenAncestor])
        return false;
    return true;
}

// Normalized (0-1) crop rect of the Safari window — intersection of WKWebView rect with the window's
// contentLayoutRect (excludes title bar). Expressed as fractions of the window frame. In SCK's
// coordinate the rect's Y is "from top" because SCK's contentRect is top-left-origin.
UNUSED_FUNCTION static std::optional<WebCore::FloatRect> safariWindowCropForWebView(NSView *webView)
{
    if (!webView)
        return std::nullopt;
    NSWindow *window = [webView window];
    if (!window)
        return std::nullopt;
    NSRect windowFrame = [window frame];
    if (NSWidth(windowFrame) <= 0 || NSHeight(windowFrame) <= 0)
        return std::nullopt;
    NSRect webViewInWindow = [webView convertRect:[webView bounds] toView:nil];
    NSRect contentLayout = [window contentLayoutRect];
    NSRect visible = NSIntersectionRect(webViewInWindow, contentLayout);
    if (NSIsEmptyRect(visible))
        return std::nullopt;
    return WebCore::FloatRect {
        static_cast<float>(NSMinX(visible) / NSWidth(windowFrame)),
        static_cast<float>((NSHeight(windowFrame) - NSMaxY(visible)) / NSHeight(windowFrame)),
        static_cast<float>(NSWidth(visible) / NSWidth(windowFrame)),
        static_cast<float>(NSHeight(visible) / NSHeight(windowFrame))
    };
}

// Create an off-screen virtual display sized to the WKWebView we are capturing. The display is at
// 2x density so SCK's point→pixel scale is correct and content stays sharp. Returns the display
// (which the caller must keep alive for the lifetime of the capture) along with the backing
// `CGDirectDisplayID`.
static RetainPtr<CGVirtualDisplay> createTabCaptureVirtualDisplay(uint32_t pointsWide, uint32_t pointsHigh, uint32_t& displayIDOut)
{
    displayIDOut = 0;

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
    [descriptor setMaxPixelsWide:4000];
    [descriptor setMaxPixelsHigh:3000];
    // Fake an unambiguously HiDPI physical panel so the OS picks the HiDPI (2x) variant of the mode
    // rather than the 1x variant. hiDPI = Enable installs both; the chooser is density-based and
    // only picks HiDPI when pixels/inch is high enough. ~290 ppi (what you'd get from treating
    // 1 point ≈ 25.4/144 mm) turns out to NOT be enough on this hardware — the OS still picks the
    // 1x variant. Shrink the fake physical size dramatically so density is beyond any plausible
    // threshold: pointsWide/20 mm → ~1000 ppi at 2x, which has reliably picked HiDPI.
    [descriptor setSizeInMillimeters:CGSizeMake(pointsWide / 20.0, pointsHigh / 20.0)];

    RetainPtr settings = adoptNS([[CGVirtualDisplaySettings alloc] init]);
    [settings setHiDPI:CGVirtualDisplayHiDPIModeEnable];
    RetainPtr mode = adoptNS([[CGVirtualDisplayMode alloc] initWithWidth:pointsWide height:pointsHigh refreshRate:60]);
    [settings setModes:@[mode.get()]];

    RetainPtr display = adoptNS([[CGVirtualDisplay alloc] initWithDescriptor:descriptor.get()]);
    if (!display) {
        RELEASE_LOG_ERROR(WebRTC, "createTabCaptureVirtualDisplay - initWithDescriptor returned nil.");
        return nullptr;
    }
    if (![display applySettings:settings.get()]) {
        RELEASE_LOG_ERROR(WebRTC, "createTabCaptureVirtualDisplay - applySettings failed.");
        return nullptr;
    }

    displayIDOut = [display displayID];
    RELEASE_LOG(WebRTC, "createTabCaptureVirtualDisplay - created %ux%u (points) display, displayID=%u.", pointsWide, pointsHigh, displayIDOut);
    return display;
}

// Returns the NSScreen whose CGDirectDisplayID matches the given value, or nil. Virtual displays
// may not appear in [NSScreen screens] on the exact run-loop turn they're created; a short poll
// is reasonable before giving up.
static NSScreen *screenForDisplayID(uint32_t displayID)
{
    for (NSScreen *screen in [NSScreen screens]) {
        NSNumber *idNumber = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
        if ([idNumber unsignedIntValue] == displayID)
            return screen;
    }
    return nil;
}

// Wait for the newly-created virtual display to appear in [NSScreen screens]. Uses CoreGraphics'
// display-reconfiguration notification instead of a blind poll: the callback fires on the main
// thread the moment the display is registered, so we return within a run-loop turn of the add
// rather than at the next poll tick. A short deadline guards against the (unexpected) case where
// the notification never arrives.
struct VirtualDisplayWaitContext {
    uint32_t displayID { 0 };
    std::atomic<bool> appeared { false };
};

static void virtualDisplayReconfigurationCallback(CGDirectDisplayID display, CGDisplayChangeSummaryFlags flags, void *userInfo)
{
    auto *context = static_cast<VirtualDisplayWaitContext*>(userInfo);
    if (static_cast<uint32_t>(display) == context->displayID && (flags & kCGDisplayAddFlag))
        context->appeared.store(true, std::memory_order_release);
}

static NSScreen *waitForVirtualDisplayScreen(uint32_t displayID, NSTimeInterval maxWait)
{
    if (NSScreen *screen = screenForDisplayID(displayID))
        return screen;

    VirtualDisplayWaitContext context;
    context.displayID = displayID;
    CGError err = CGDisplayRegisterReconfigurationCallback(virtualDisplayReconfigurationCallback, &context);
    if (err != kCGErrorSuccess)
        RELEASE_LOG_ERROR(WebRTC, "waitForVirtualDisplayScreen - CGDisplayRegisterReconfigurationCallback failed (%d); falling back to polling only.", err);
    // Spin the run loop so the Add-flag callback can fire, but also keep polling [NSScreen screens]
    // directly. The two signals can race: the callback may fire slightly before NSScreen's cache
    // updates (so appeared=true but screenForDisplayID still returns nil), or the callback can be
    // missed entirely and polling is the only way to notice.
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:maxWait];
    NSScreen *screen = nil;
    while (!screen && [[NSDate date] compare:deadline] == NSOrderedAscending) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
        screen = screenForDisplayID(displayID);
    }
    if (err == kCGErrorSuccess)
        CGDisplayRemoveReconfigurationCallback(virtualDisplayReconfigurationCallback, &context);
    return screen;
}


void DisplayCaptureSessionManager::startTabCapture(WebPageProxy& page, CompletionHandler<void(std::optional<WebCore::CaptureDevice>)>&& completionHandler)
{
#if HAVE(WINDOW_CAPTURE)
    RetainPtr webView = page.cocoaView();
    if (!webView) {
        completionHandler(std::nullopt);
        return;
    }

    // Bring the picked tab's Safari window to the foreground so the user can see what they're
    // about to share. The picker overlay only installs itself on the visible WKWebView of each
    // Safari window, so the picked WKWebView is already its window's current tab — making the
    // window key is enough.
    if (NSWindow *webViewWindow = [webView window])
        [webViewWindow makeKeyAndOrderFront:nil];

    m_capturedPage = page;

    // Force the captured page's activity state to stay visible / in-window even when Safari moves it
    // to the background, so WebContent keeps committing fresh backing-store (otherwise the mirror
    // ends up with white tiles).
    page.setIsBeingCapturedForTabCapture(true);

    // Offscreen window used to host the mirror layer tree and the cursor overlay. SCK captures it by
    // windowNumber even though it's positioned off-screen.
    //
    // Use a flipped contentView (isFlipped=YES) so its layer's coordinate system matches WKFlippedView.
    // Shrink the window height by obscuredContentInsets.top() and shift the mirror root up by the same
    // amount via sublayerTransform on the content layer — WebKit lays out content below the inset, so
    // this crops the empty strip. The cursor mirror compensates for this shift in its own position.
    NSRect webViewFrame = [webView frame];
    auto topInset = page.obscuredContentInsets().top();
    NSSize contentSize = NSMakeSize(NSWidth(webViewFrame) ?: 800, std::max<CGFloat>(1, (NSHeight(webViewFrame) ?: 600) - topInset));

    // Create a dedicated virtual display sized to the WKWebView and move the off-screen mirror
    // window onto it. The point of the virtual display is to give the window a proper screen
    // context — specifically a known 2x backing scale — so SCK's own capture-source machinery
    // (SCContentFilter.pointPixelScale and friends) returns the right value. SCK capture itself
    // still targets just our window via its windowID; the window contains the full captured
    // content so there's no menu bar / desktop / etc. in the output. Falls back to the far-off
    // placement if virtual-display creation isn't available or fails.
    //
    // Explicitly release any previous virtual display before creating a new one. CoreGraphics
    // appears to cap the number of concurrently live app-owned virtual displays very low (if the
    // previous one is still retained when a second `initWithDescriptor:` is called, the second
    // call returns nil). We want each capture session to get its own freshly-sized display.
    m_tabCaptureVirtualDisplay = nullptr;
    m_tabCaptureVirtualDisplayID = 0;
    NSRect offscreenFrame = NSMakeRect(-100000, -100000, contentSize.width, contentSize.height);
    uint32_t virtualDisplayID = 0;
    RetainPtr virtualDisplay = createTabCaptureVirtualDisplay(static_cast<uint32_t>(contentSize.width), static_cast<uint32_t>(contentSize.height), virtualDisplayID);
    if (virtualDisplay && virtualDisplayID) {
        if (NSScreen *virtualScreen = waitForVirtualDisplayScreen(virtualDisplayID, 1.0)) {
            // We do NOT try to switch the virtual display to a HiDPI mode. CoreGraphics rejects
            // CGDisplaySetDisplayMode on our virtual displays (err=1001), and even when the HiDPI
            // mode is installed CG's auto-pick keeps the 1x variant. Instead, we accept the 1x
            // display and compensate by sizing the window to 2× the content and scaling the mirror
            // layer tree 2× via sublayerTransform. On a 1x display, a point equals a pixel, so a
            // 2×-sized window gives us exactly the pixel count a retina display would provide at
            // native size — the mirror's retina (contentsScale=2) bitmaps then land 1:1 on the
            // window's pixel grid. See `m_tabCaptureMirrorScale` below.
            virtualScreen = screenForDisplayID(virtualDisplayID) ?: virtualScreen;
            NSRect virtualFrame = [virtualScreen frame];
            m_tabCaptureMirrorScale = 2.0;
            NSSize windowSize = NSMakeSize(contentSize.width * m_tabCaptureMirrorScale, contentSize.height * m_tabCaptureMirrorScale);
            offscreenFrame = NSMakeRect(NSMinX(virtualFrame), NSMinY(virtualFrame), windowSize.width, windowSize.height);
            m_tabCaptureVirtualDisplay = virtualDisplay;
            m_tabCaptureVirtualDisplayID = virtualDisplayID;
            RELEASE_LOG(WebRTC, "startTabCapture - virtual display %u at (%g,%g,%gx%g); mirrorScale=%g, window at (%g,%g,%gx%g).",
                virtualDisplayID, NSMinX(virtualFrame), NSMinY(virtualFrame), NSWidth(virtualFrame), NSHeight(virtualFrame),
                m_tabCaptureMirrorScale,
                NSMinX(offscreenFrame), NSMinY(offscreenFrame), NSWidth(offscreenFrame), NSHeight(offscreenFrame));
        } else {
            RELEASE_LOG_ERROR(WebRTC, "startTabCapture - virtual display %u created but NSScreen never appeared; falling back to off-screen placement.", virtualDisplayID);
            m_tabCaptureMirrorScale = 1.0;
        }
    } else
        m_tabCaptureMirrorScale = 1.0;
    RetainPtr window = adoptNS([[NSWindow alloc] initWithContentRect:offscreenFrame styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO]);
    [window setReleasedWhenClosed:NO];
    [window setOpaque:YES];
    [window setHasShadow:NO];
    [window setBackgroundColor:[NSColor whiteColor]];
    [window setLevel:NSNormalWindowLevel];
    // contentView matches window dims (= contentSize × mirrorScale). Mirror layers are at native
    // content-point dims (742×565 etc.); we scale them 2× via sublayerTransform so they fill a
    // 2×-sized window and render at 1 bitmap-pixel per 1 window-pixel on a 1x display.
    RetainPtr contentView = adoptNS([[WKTabCaptureMirrorFlippedView alloc] initWithFrame:NSMakeRect(0, 0, NSWidth(offscreenFrame), NSHeight(offscreenFrame))]);
    [contentView setWantsLayer:YES];
    [[contentView layer] setBackgroundColor:[NSColor whiteColor].CGColor];
    // sublayerTransform combines: scale by mirrorScale, then translate by -topInset (in child
    // coord space, applied before scale). Order constructed via CATransform3DTranslate on a scale
    // matrix gives: scale × translate (translate applied first to child points, then scale).
    CATransform3D sublayerXform = CATransform3DMakeScale(m_tabCaptureMirrorScale, m_tabCaptureMirrorScale, 1.0);
    if (topInset > 0)
        sublayerXform = CATransform3DTranslate(sublayerXform, 0, -topInset, 0);
    [[contentView layer] setSublayerTransform:sublayerXform];
    RELEASE_LOG(WebRTC, "startTabCapture - obscuredContentInsets top=%g webViewFrame=%gx%g mirrorScale=%g.", topInset, NSWidth(webViewFrame), NSHeight(webViewFrame), m_tabCaptureMirrorScale);
    [window setContentView:contentView.get()];

    [window orderFrontRegardless];
    m_tabCaptureOffscreenWindow = window;

    // Capture always targets the offscreen window and reads from the mirror tree. The primary tree
    // stays with Safari untouched, so the user's WKWebView keeps rendering normally while capture runs.
    NSWindow *targetWindow = window.get();
    m_tabCaptureMode = CaptureMode::OffscreenReparent;
    RELEASE_LOG(WebRTC, "startTabCapture - page %" PRIu64 " mirror-capture target=%p windowID=%ld.",
        page.identifier().toUInt64(), targetWindow, (long)[targetWindow windowNumber]);

    // Attach a mirror RemoteLayerTreeHost. Attach is async: the completion fires once WebContent has
    // resent the full layer tree (via SeedFullLayerTreeInNextTransaction) and the mirror has a root.
    WeakObjCPtr<CALayer> weakContentViewLayer = [contentView layer];
    if (auto* da = dynamicDowncast<RemoteLayerTreeDrawingAreaProxy>(page.drawingArea())) {
        da->enableCaptureMirrorLayerTree([weakContentViewLayer](RetainPtr<CALayer> mirrorRoot) mutable {
            RetainPtr<CALayer> contentViewLayer = weakContentViewLayer.get();
            if (!contentViewLayer || !mirrorRoot) {
                RELEASE_LOG_ERROR(WebRTC, "startTabCapture mirror attach - contentViewLayer=%p mirrorRoot=%p; aborting.", contentViewLayer.get(), mirrorRoot.get());
                return;
            }
            // Insert below the cursor layer (highest zPosition). The sublayerTransform on contentViewLayer
            // crops the top chrome inset.
            [contentViewLayer insertSublayer:mirrorRoot.get() atIndex:0];
            RELEASE_LOG(WebRTC, "startTabCapture mirror attach - mirrorRoot=%p added to offscreen window.", mirrorRoot.get());
        });
    } else
        RELEASE_LOG_ERROR(WebRTC, "startTabCapture - drawing area is not a RemoteLayerTreeDrawingAreaProxy; mirror unavailable.");

    // Cursor mirror layer: sibling of the (soon-to-arrive) mirror root. Also under sublayerTransform's
    // -topInset shift, so updateTabCaptureCursorMirror pre-adds topInset to the layer's Y position so
    // the hotspot lands on the correct content-area coordinate once the transform is applied.
    //
    // contentsScale accounts for BOTH the window's native backing scale AND the 2× mirrorScale
    // transform. On a 1x VD, backingScale=1 and mirrorScale=2 → contentsScale=2 so the @2x cursor
    // bitmap lands 1:1 on window pixels after being scaled up by the sublayerTransform. On the
    // retina-main-screen fallback, backingScale=2 and mirrorScale=1 → contentsScale=2, same effect.
    RetainPtr cursorLayer = adoptNS([[CALayer alloc] init]);
    [cursorLayer setAnchorPoint:CGPointMake(0, 0)];
    [cursorLayer setContentsScale:[window backingScaleFactor] * m_tabCaptureMirrorScale];
    [cursorLayer setZPosition:1000];
    [cursorLayer setHidden:YES];
    [[contentView layer] addSublayer:cursorLayer.get()];
    m_tabCaptureCursorLayer = cursorLayer;

    uint32_t windowID = static_cast<uint32_t>([targetWindow windowNumber]);
    if (!windowID) {
        RELEASE_LOG_ERROR(WebRTC, "startTabCapture - no windowNumber for target %p.", targetWindow);
        completionHandler(std::nullopt);
        return;
    }

    // No mode switching in the prototype — the poller is retained only to simplify teardown bookkeeping.
    if (m_tabCaptureModePoll)
        [m_tabCaptureModePoll invalidate];
    m_tabCaptureModePoll = nullptr;

    // Track WKWebView size changes so the offscreen window stays sized to match. NSViewFrameDidChangeNotification
    // fires whenever the observed view's frame changes.
    [webView setPostsFrameChangedNotifications:YES];
    if (m_tabCaptureViewFrameObserver)
        [[NSNotificationCenter defaultCenter] removeObserver:m_tabCaptureViewFrameObserver.get()];
    WeakObjCPtr<NSView> weakWebView = webView.get();
    RetainPtr<NSWindow> weakWindow = window;
    WeakPtr<WebPageProxy> weakPage { page };
    m_tabCaptureViewFrameObserver = [[NSNotificationCenter defaultCenter] addObserverForName:NSViewFrameDidChangeNotification object:webView.get() queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification *) {
        RetainPtr strongWebView = weakWebView.get();
        RetainPtr strongWindow = weakWindow;
        RefPtr strongPage = weakPage.get();
        if (!strongWebView || !strongWindow || !strongPage)
            return;
        auto& mgr = DisplayCaptureSessionManager::singleton();
        CGFloat mirrorScale = mgr.m_tabCaptureMirrorScale;
        NSRect newFrame = [strongWebView frame];
        CGFloat inset = strongPage->obscuredContentInsets().top();
        NSSize newContentSize = NSMakeSize(NSWidth(newFrame) ?: 1, std::max<CGFloat>(1, (NSHeight(newFrame) ?: 1) - inset));
        NSSize newWindowSize = NSMakeSize(newContentSize.width * mirrorScale, newContentSize.height * mirrorScale);
        NSRect currentWindowFrame = [strongWindow frame];
        if (NSWidth(currentWindowFrame) == newWindowSize.width && NSHeight(currentWindowFrame) == newWindowSize.height)
            return;

        // Keep the virtual display's mode in sync with the window size (in points on the 1x VD).
        // applySettings: reuses the existing displayID — the matching NSScreen's frame just updates
        // in place.
        NSRect targetFrame = NSMakeRect(NSMinX(currentWindowFrame), NSMinY(currentWindowFrame), newWindowSize.width, newWindowSize.height);
        if (mgr.m_tabCaptureVirtualDisplay && mgr.m_tabCaptureVirtualDisplayID) {
            RetainPtr settings = adoptNS([[CGVirtualDisplaySettings alloc] init]);
            [settings setHiDPI:CGVirtualDisplayHiDPIModeEnable];
            RetainPtr mode = adoptNS([[CGVirtualDisplayMode alloc] initWithWidth:static_cast<uint32_t>(newWindowSize.width) height:static_cast<uint32_t>(newWindowSize.height) refreshRate:60]);
            [settings setModes:@[mode.get()]];
            if (![mgr.m_tabCaptureVirtualDisplay applySettings:settings.get()])
                RELEASE_LOG_ERROR(WebRTC, "startTabCapture frameObserver - applySettings failed on virtual display %u.", mgr.m_tabCaptureVirtualDisplayID);
        }
        [strongWindow setFrame:targetFrame display:NO];
        [[strongWindow contentView] setFrame:NSMakeRect(0, 0, newWindowSize.width, newWindowSize.height)];
        CATransform3D xform = CATransform3DMakeScale(mirrorScale, mirrorScale, 1.0);
        if (inset > 0)
            xform = CATransform3DTranslate(xform, 0, -inset, 0);
        [[[strongWindow contentView] layer] setSublayerTransform:xform];
        RELEASE_LOG(WebRTC, "startTabCapture - resized offscreen window to %gx%g (contentSize=%gx%g inset=%g mirrorScale=%g).", newWindowSize.width, newWindowSize.height, newContentSize.width, newContentSize.height, inset, mirrorScale);
    }];

    // Install NSEvent monitors to drive the cursor mirror. Local for events within our app's windows,
    // global for events in other apps (so capture of a hovered link cursor still works when Safari isn't
    // the key app). Both only reposition the mirror layer — they never consume the event.
    auto cursorEventBlock = ^(NSEvent *event) {
        DisplayCaptureSessionManager::singleton().updateTabCaptureCursorMirror();
    };
    m_tabCaptureCursorLocalMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:(NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged | NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp | NSEventMaskRightMouseDown | NSEventMaskRightMouseUp) handler:^NSEvent *(NSEvent *event) {
        cursorEventBlock(event);
        return event;
    }];
    m_tabCaptureCursorGlobalMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:(NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged) handler:cursorEventBlock];

    // Safari's own windows only deliver mouse-moved events to the local monitor if they accept them.
    if (webView.get().window && ![webView.get().window acceptsMouseMovedEvents])
        [webView.get().window setAcceptsMouseMovedEvents:YES];

    // Refresh timer catches cursor changes that occur without mouse motion (e.g. JS-driven cursor changes,
    // or a late IPC arrival after the last mouse event).
    if (m_tabCaptureCursorRefreshTimer)
        [m_tabCaptureCursorRefreshTimer invalidate];
    m_tabCaptureCursorRefreshTimer = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *) {
        DisplayCaptureSessionManager::singleton().updateTabCaptureCursorMirror();
    }];
    [[NSRunLoop mainRunLoop] addTimer:m_tabCaptureCursorRefreshTimer.get() forMode:NSRunLoopCommonModes];
    updateTabCaptureCursorMirror();

    if (protect(page.preferences())->useGPUProcessForDisplayCapture()) {
        Ref gpuProcess = protect(page.configuration().processPool())->ensureGPUProcess();
        gpuProcess->updateSandboxAccess(false, false, true);
        gpuProcess->promptForGetDisplayMediaForWindowID(windowID, std::nullopt, WTF::move(completionHandler));
        return;
    }
    WebCore::ScreenCaptureKitSharingSessionManager::singleton().promptForGetDisplayMediaForWindowID(windowID, std::nullopt, WTF::move(completionHandler));
#else
    UNUSED_PARAM(page);
    completionHandler(std::nullopt);
#endif
}

void DisplayCaptureSessionManager::updateCaptureMode()
{
#if HAVE(WINDOW_CAPTURE)
    // Prototype: always-offscreen. The visibility-driven mode switch is intentionally disabled so the
    // compositor tree stays reparented into our capture window even when the WKWebView is foregrounded.
    // Safari's own view will render blank while capture is active; that trade is accepted for the experiment.
#endif
}

void DisplayCaptureSessionManager::updateTabCaptureCursorMirror()
{
#if HAVE(WINDOW_CAPTURE)
    if (!m_tabCaptureCursorLayer || !m_tabCaptureOffscreenWindow)
        return;
    RefPtr page = m_capturedPage.get();
    if (!page)
        return;
    RetainPtr webView = page->cocoaView();
    if (!webView) {
        [m_tabCaptureCursorLayer setHidden:YES];
        return;
    }
    NSWindow *webViewWindow = [webView window];
    if (!webViewWindow) {
        [m_tabCaptureCursorLayer setHidden:YES];
        return;
    }

    // Map screen mouse location → WKWebView-local coordinates with top-left origin.
    NSPoint mouseScreen = [NSEvent mouseLocation];
    NSPoint mouseInWindow = [webViewWindow convertPointFromScreen:mouseScreen];
    NSRect webViewInWindow = [webView convertRect:[webView bounds] toView:nil];
    CGFloat localX = mouseInWindow.x - NSMinX(webViewInWindow);
    CGFloat localYFromBottom = mouseInWindow.y - NSMinY(webViewInWindow);
    CGFloat localYFromTop = NSHeight(webViewInWindow) - localYFromBottom;

    // Hide if the mouse is outside the WKWebView's content area, or above the chrome inset.
    CGFloat topInset = page->obscuredContentInsets().top();
    CGFloat mirrorY = localYFromTop - topInset;
    bool insideContent = localX >= 0 && localX <= NSWidth(webViewInWindow)
        && localYFromTop >= topInset && localYFromTop <= NSHeight(webViewInWindow);
    if (!insideContent) {
        [m_tabCaptureCursorLayer setHidden:YES];
        return;
    }

    // Pull image + hotspot from the currently-set NSCursor. WebKit's PageClientImplMac::setCursor already
    // called [NSCursor set] with the Cursor::platformCursor() for the current hit-tested element, so this
    // reflects link / IBeam / resize / custom cursors as they are selected.
    NSCursor *current = [NSCursor currentCursor];
    if (!current) {
        [m_tabCaptureCursorLayer setHidden:YES];
        return;
    }
    NSImage *image = [current image];
    NSSize imageSize = image ? [image size] : NSZeroSize;
    if (!image || imageSize.width <= 0 || imageSize.height <= 0) {
        [m_tabCaptureCursorLayer setHidden:YES];
        return;
    }
    NSPoint hotSpot = [current hotSpot];

    CGImageRef cgImage = [image CGImageForProposedRect:nullptr context:nil hints:nil];
    if (!cgImage) {
        [m_tabCaptureCursorLayer setHidden:YES];
        return;
    }

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [m_tabCaptureCursorLayer setContents:(__bridge id)cgImage];
    [m_tabCaptureCursorLayer setBounds:CGRectMake(0, 0, imageSize.width, imageSize.height)];
    // The cursor layer is a sublayer of the content view's layer which carries a sublayerTransform of
    // -topInset on Y. Pre-add topInset so the net effect places the hotspot at content-coord mirrorY.
    [m_tabCaptureCursorLayer setPosition:CGPointMake(localX - hotSpot.x, mirrorY - hotSpot.y + topInset)];
    [m_tabCaptureCursorLayer setHidden:NO];
    [CATransaction commit];
#endif
}
#endif // HAVE(SCREEN_CAPTURE_KIT)

std::optional<WebCore::CaptureDevice> DisplayCaptureSessionManager::deviceSelectedForTesting(WebCore::CaptureDevice::DeviceType deviceType, unsigned indexOfDeviceSelectedForTesting)
{
    unsigned index = 0;
    for (auto& device : borrow(WebCore::RealtimeMediaSourceCenter::singleton().displayCaptureFactory().displayCaptureDeviceManager().captureDevices())) {
        if (device.enabled() && device.type() == deviceType) {
            if (index == indexOfDeviceSelectedForTesting)
                return { device };
            ++index;
        }
    }

    return std::nullopt;
}

bool DisplayCaptureSessionManager::useMockCaptureDevices() const
{
    return m_indexOfDeviceSelectedForTesting || WebCore::MockRealtimeMediaSourceCenter::mockRealtimeMediaSourceCenterEnabled();
}

void DisplayCaptureSessionManager::showWindowPicker(const WebCore::SecurityOriginData& origin, CompletionHandler<void(std::optional<WebCore::CaptureDevice>)>&& completionHandler)
{
    if (useMockCaptureDevices()) {
        completionHandler(deviceSelectedForTesting(WebCore::CaptureDevice::DeviceType::Window, m_indexOfDeviceSelectedForTesting.value_or(0)));
        return;
    }

    completionHandler(std::nullopt);
}

void DisplayCaptureSessionManager::showScreenPicker(const WebCore::SecurityOriginData&, CompletionHandler<void(std::optional<WebCore::CaptureDevice>)>&& completionHandler)
{
    if (useMockCaptureDevices()) {
        completionHandler(deviceSelectedForTesting(WebCore::CaptureDevice::DeviceType::Screen, m_indexOfDeviceSelectedForTesting.value_or(0)));
        return;
    }

    completionHandler(std::nullopt);
}

bool DisplayCaptureSessionManager::isAvailable()
{
#if HAVE(SCREEN_CAPTURE_KIT)
    return WebCore::ScreenCaptureKitCaptureSource::isAvailable();
#else
    return false;
#endif
}

DisplayCaptureSessionManager& DisplayCaptureSessionManager::singleton()
{
    ASSERT(isMainRunLoop());
    static NeverDestroyed<DisplayCaptureSessionManager> manager;
    return manager;
}

DisplayCaptureSessionManager::DisplayCaptureSessionManager()
{
}

DisplayCaptureSessionManager::~DisplayCaptureSessionManager() = default;

bool DisplayCaptureSessionManager::canRequestDisplayCapturePermission()
{
    if (useMockCaptureDevices())
        return m_systemCanPromptForTesting == PromptOverride::CanPrompt;

#if HAVE(SCREEN_CAPTURE_KIT)
    return true;
#else
    return false;
#endif
}

#if HAVE(SCREEN_CAPTURE_KIT)
NODELETE static WebCore::DisplayCapturePromptType toScreenCaptureKitPromptType(UserMediaPermissionRequestProxy::UserMediaDisplayCapturePromptType promptType)
{
    if (promptType == UserMediaPermissionRequestProxy::UserMediaDisplayCapturePromptType::Screen)
        return WebCore::DisplayCapturePromptType::Screen;
    if (promptType == UserMediaPermissionRequestProxy::UserMediaDisplayCapturePromptType::Window)
        return WebCore::DisplayCapturePromptType::Window;
    if (promptType == UserMediaPermissionRequestProxy::UserMediaDisplayCapturePromptType::UserChoose)
        return WebCore::DisplayCapturePromptType::UserChoose;

    ASSERT_NOT_REACHED();
    return WebCore::DisplayCapturePromptType::Screen;
}
#endif

void DisplayCaptureSessionManager::promptForGetDisplayMedia(UserMediaPermissionRequestProxy::UserMediaDisplayCapturePromptType promptType, WebPageProxy& page, const WebCore::SecurityOriginData& origin, CompletionHandler<void(std::optional<WebCore::CaptureDevice>)>&& completionHandler)
{
    if (useMockCaptureDevices()) {
        if (promptType == UserMediaPermissionRequestProxy::UserMediaDisplayCapturePromptType::Window)
            showWindowPicker(origin, WTF::move(completionHandler));
        else
            showScreenPicker(origin, WTF::move(completionHandler));
        return;
    }

#if HAVE(SCREEN_CAPTURE_KIT)
    ASSERT(isAvailable());

    if (!isAvailable() || !completionHandler) {
        completionHandler(std::nullopt);
        return;
    }

    if (WebCore::ScreenCaptureKitSharingSessionManager::isAvailable() && promptType != UserMediaPermissionRequestProxy::UserMediaDisplayCapturePromptType::UserChoose) {
        if (!protect(page.preferences())->useGPUProcessForDisplayCapture()) {
            WebCore::ScreenCaptureKitSharingSessionManager::singleton().promptForGetDisplayMedia(toScreenCaptureKitPromptType(promptType), WTF::move(completionHandler));
            return;
        }
        Ref gpuProcess = protect(page.configuration().processPool())->ensureGPUProcess();
        gpuProcess->updateSandboxAccess(false, false, true);
        gpuProcess->promptForGetDisplayMedia(toScreenCaptureKitPromptType(promptType), WTF::move(completionHandler));
        return;
    }

    if (promptType == UserMediaPermissionRequestProxy::UserMediaDisplayCapturePromptType::Screen) {
        showScreenPicker(origin, WTF::move(completionHandler));
        return;
    }

    if (promptType == UserMediaPermissionRequestProxy::UserMediaDisplayCapturePromptType::Window) {
        showWindowPicker(origin, WTF::move(completionHandler));
        return;
    }

#if HAVE(WINDOW_CAPTURE)
    alertForGetDisplayMedia(page, origin, [this, weakPage = WeakPtr { page }, completionHandler = WTF::move(completionHandler)] (DisplayCaptureSessionManager::CaptureSessionType sessionType) mutable {
        if (sessionType == CaptureSessionType::None) {
            completionHandler(std::nullopt);
            return;
        }

        RefPtr protectedPage = weakPage.get();
        if (!protectedPage) {
            completionHandler(std::nullopt);
            return;
        }

        if (sessionType == CaptureSessionType::Tab) {
            showTabPicker(*protectedPage, WTF::move(completionHandler));
            return;
        }

        // Window/Screen → route to SCK's SCContentSharingPicker with UserChoose so the system picker
        // offers both windows and screens.
        if (WebCore::ScreenCaptureKitSharingSessionManager::isAvailable()) {
            if (!protect(protectedPage->preferences())->useGPUProcessForDisplayCapture()) {
                WebCore::ScreenCaptureKitSharingSessionManager::singleton().promptForGetDisplayMedia(WebCore::DisplayCapturePromptType::UserChoose, WTF::move(completionHandler));
                return;
            }
            Ref gpuProcess = protect(protectedPage->configuration().processPool())->ensureGPUProcess();
            gpuProcess->updateSandboxAccess(false, false, true);
            gpuProcess->promptForGetDisplayMedia(WebCore::DisplayCapturePromptType::UserChoose, WTF::move(completionHandler));
            return;
        }
        completionHandler(std::nullopt);
    });
#else
    completionHandler(std::nullopt);
#endif // HAVE(WINDOW_CAPTURE)

#endif // HAVE(SCREEN_CAPTURE_KIT)
}

void DisplayCaptureSessionManager::cancelGetDisplayMediaPrompt(WebPageProxy& page)
{
#if HAVE(SCREEN_CAPTURE_KIT)
    ASSERT(isAvailable());

    if (!isAvailable() || !WebCore::ScreenCaptureKitSharingSessionManager::isAvailable())
        return;

    if (!protect(page.preferences())->useGPUProcessForDisplayCapture()) {
        WebCore::ScreenCaptureKitSharingSessionManager::singleton().cancelGetDisplayMediaPrompt();
        return;
    }

    RefPtr gpuProcess = page.configuration().processPool().gpuProcess();
    if (!gpuProcess)
        return;

    gpuProcess->cancelGetDisplayMediaPrompt();
#endif
}

void DisplayCaptureSessionManager::migrateTabCaptureIfNeeded(WebPageProxy& oldPage, WebPageProxy& newPage)
{
#if HAVE(SCREEN_CAPTURE_KIT) && HAVE(WINDOW_CAPTURE)
    RefPtr captured = m_capturedPage.get();
    if (!captured || captured.get() != &oldPage) {
        RELEASE_LOG(WebRTC, "migrateTabCaptureIfNeeded - oldPage %" PRIu64 " is not the captured page; no-op.", oldPage.identifier().toUInt64());
        return;
    }
    if (&oldPage == &newPage)
        return;
    if (!m_tabCaptureOffscreenWindow) {
        RELEASE_LOG_ERROR(WebRTC, "migrateTabCaptureIfNeeded - no offscreen capture window; aborting migration.");
        return;
    }

    RELEASE_LOG(WebRTC, "migrateTabCaptureIfNeeded - migrating capture from page %" PRIu64 " to page %" PRIu64 ".",
        oldPage.identifier().toUInt64(), newPage.identifier().toUInt64());

    // Tear down on the old page: drop activity-state override and remove the mirror from its drawing area.
    oldPage.setIsBeingCapturedForTabCapture(false);
    if (auto* oldDA = dynamicDowncast<RemoteLayerTreeDrawingAreaProxy>(oldPage.drawingArea()))
        oldDA->disableCaptureMirrorLayerTree();

    // Remove the old mirror root from our offscreen content view. We don't know which sublayer is the
    // old mirror root (we didn't retain a direct reference), so remove any CALayer that is NOT the
    // cursor layer we installed on top.
    RetainPtr<CALayer> contentViewLayer = [[m_tabCaptureOffscreenWindow contentView] layer];
    if (contentViewLayer) {
        for (CALayer *sublayer in [[contentViewLayer sublayers] copy]) {
            if (sublayer == m_tabCaptureCursorLayer.get())
                continue;
            [sublayer removeFromSuperlayer];
        }
    }

    // Rebind to the new page and install the mirror there. Insert the arriving mirror root into the
    // same offscreen content view when it attaches, below the cursor overlay.
    m_capturedPage = newPage;
    newPage.setIsBeingCapturedForTabCapture(true);

    WeakObjCPtr<CALayer> weakContentViewLayer = contentViewLayer.get();
    if (auto* newDA = dynamicDowncast<RemoteLayerTreeDrawingAreaProxy>(newPage.drawingArea())) {
        newDA->enableCaptureMirrorLayerTree([weakContentViewLayer](RetainPtr<CALayer> mirrorRoot) mutable {
            RetainPtr<CALayer> cv = weakContentViewLayer.get();
            if (!cv || !mirrorRoot) {
                RELEASE_LOG_ERROR(WebRTC, "migrateTabCaptureIfNeeded mirror attach - cv=%p mirrorRoot=%p; aborting.", cv.get(), mirrorRoot.get());
                return;
            }
            [cv insertSublayer:mirrorRoot.get() atIndex:0];
            RELEASE_LOG(WebRTC, "migrateTabCaptureIfNeeded mirror attach - new mirrorRoot=%p inserted into offscreen window.", mirrorRoot.get());
        });
    } else
        RELEASE_LOG_ERROR(WebRTC, "migrateTabCaptureIfNeeded - new page has no RemoteLayerTreeDrawingAreaProxy; mirror unavailable.");
#else
    UNUSED_PARAM(oldPage);
    UNUSED_PARAM(newPage);
#endif
}

void DisplayCaptureSessionManager::reattachTabCaptureMirrorAfterProcessSwap(WebPageProxy& page)
{
#if HAVE(SCREEN_CAPTURE_KIT) && HAVE(WINDOW_CAPTURE)
    RefPtr captured = m_capturedPage.get();
    if (!captured || captured.get() != &page)
        return;
    if (!m_tabCaptureOffscreenWindow)
        return;

    RELEASE_LOG(WebRTC, "reattachTabCaptureMirrorAfterProcessSwap - page %" PRIu64 " swapped WebContent; re-seeding mirror.",
        page.identifier().toUInt64());

    // Pop any orphaned mirror root sublayers off the offscreen content view (cursor overlay stays).
    RetainPtr<CALayer> contentViewLayer = [[m_tabCaptureOffscreenWindow contentView] layer];
    if (contentViewLayer) {
        for (CALayer *sublayer in [[contentViewLayer sublayers] copy]) {
            if (sublayer == m_tabCaptureCursorLayer.get())
                continue;
            [sublayer removeFromSuperlayer];
        }
    }

    auto* da = dynamicDowncast<RemoteLayerTreeDrawingAreaProxy>(page.drawingArea());
    if (!da) {
        RELEASE_LOG_ERROR(WebRTC, "reattachTabCaptureMirrorAfterProcessSwap - no RemoteLayerTreeDrawingAreaProxy.");
        return;
    }

    // Disable the mirror (drops the host carrying stale pre-swap layer IDs), then re-enable so a
    // fresh seed IPC is sent to the newly-attached WebContent process. The attach completion
    // reinstalls the new mirror root as the base sublayer of the offscreen content view.
    da->disableCaptureMirrorLayerTree();
    WeakObjCPtr<CALayer> weakContentViewLayer = contentViewLayer.get();
    da->enableCaptureMirrorLayerTree([weakContentViewLayer](RetainPtr<CALayer> mirrorRoot) mutable {
        RetainPtr<CALayer> cv = weakContentViewLayer.get();
        if (!cv || !mirrorRoot) {
            RELEASE_LOG_ERROR(WebRTC, "reattachTabCaptureMirrorAfterProcessSwap mirror attach - cv=%p mirrorRoot=%p; aborting.", cv.get(), mirrorRoot.get());
            return;
        }
        [cv insertSublayer:mirrorRoot.get() atIndex:0];
        RELEASE_LOG(WebRTC, "reattachTabCaptureMirrorAfterProcessSwap mirror attach - mirrorRoot=%p inserted.", mirrorRoot.get());
    });
#else
    UNUSED_PARAM(page);
#endif
}

} // namespace WebKit

#endif // PLATFORM(COCOA) && ENABLE(MEDIA_STREAM)
