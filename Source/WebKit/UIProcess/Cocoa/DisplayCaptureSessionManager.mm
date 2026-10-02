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
#import <wtf/MainThread.h>
#import <wtf/NeverDestroyed.h>
#import <wtf/URLHelpers.h>
#import <wtf/WeakObjCPtr.h>
#import <wtf/cocoa/TypeCastsCocoa.h>
#import <wtf/text/StringToIntegerConversion.h>

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
        if (!win || !desired || superview != desired.get() || ![overlay superview]) {
            [stale addObject:overlay];
            continue;
        }
        // Keep overlay on top of any sibling that Safari may have inserted.
        [desired addSubview:overlay positioned:NSWindowAbove relativeTo:nil];
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

    // Clip overlay to the intersection of the WKWebView with the window's contentLayoutRect so it
    // doesn't cover Safari's title/tab bar area (Safari uses FullSizeContentView).
    NSRect webViewInWindow = [webView convertRect:[webView bounds] toView:nil];
    NSRect visibleContentInWindow = NSIntersectionRect(webViewInWindow, [window contentLayoutRect]);
    NSRect overlayFrame = [webView convertRect:visibleContentInWindow fromView:nil];

    RetainPtr window_ = window;
    auto shareBlock = ^{
        auto& mgr = DisplayCaptureSessionManager::singleton();
        if (!mgr.m_pickerCompletion)
            return;
        auto completion = WTF::move(*mgr.m_pickerCompletion);
        mgr.endPickerSession();
        // Tab capture is not implemented yet in this commit; picking a tab tears the picker down and
        // returns no device so getDisplayMedia rejects cleanly.
        completion(std::nullopt);
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
    [webView addSubview:overlay.get() positioned:NSWindowAbove relativeTo:nil];
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

#endif // HAVE(SCREEN_CAPTURE_KIT)

std::optional<WebCore::CaptureDevice> DisplayCaptureSessionManager::deviceSelectedForTesting(WebCore::CaptureDevice::DeviceType deviceType, unsigned indexOfDeviceSelectedForTesting)
{
    unsigned index = 0;
    for (auto& device : WebCore::RealtimeMediaSourceCenter::singleton().displayCaptureFactory().displayCaptureDeviceManager().captureDevices()) {
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
static WebCore::DisplayCapturePromptType NODELETE toScreenCaptureKitPromptType(UserMediaPermissionRequestProxy::UserMediaDisplayCapturePromptType promptType)
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

} // namespace WebKit

#endif // PLATFORM(COCOA) && ENABLE(MEDIA_STREAM)
