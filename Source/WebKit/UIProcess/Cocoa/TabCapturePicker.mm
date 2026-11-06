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
#import "TabCapturePicker.h"

#if HAVE(WINDOW_CAPTURE)

#import "CapturePromptUtilities.h"
#import "Logging.h"
#import "MediaPermissionUtilities.h"
#import "WKWebViewInternal.h"
#import "WKWebViewPrivate.h"
#import "WebInspectorUtilities.h"
#import "WebPageProxy.h"
#import "WebProcessProxy.h"
#import <WebCore/FloatRect.h>
#import <WebCore/LocalizedStrings.h>
#import <wtf/BlockPtr.h>
#import <wtf/RunLoop.h>
#import <wtf/TZoneMallocInlines.h>

#import <pal/spi/mac/HIServicesSPI.h>

constexpr CGFloat promptCornerRadius = 12;

@interface WKTabCapturePickerButton : NSButton
@end

@implementation WKTabCapturePickerButton

// The first click in a background window should press the button, not only activate the window.
- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
    return YES;
}

@end

// Floating banner shown at the top of the screen for the tab picker's lifetime, similar to the
// system window picker's banner. It becomes key so that the requesting window loses focus, and it
// owns the picker's Escape key handling.
@interface WKTabCapturePickerBanner : NSPanel
- (instancetype)initWithScreen:(NSScreen *)screen cancelBlock:(void(^)(void))cancelBlock;
- (void)invalidate;
@end

@implementation WKTabCapturePickerBanner {
    BlockPtr<void()> _cancelBlock;
}

- (instancetype)initWithScreen:(NSScreen *)screen cancelBlock:(void(^)(void))cancelBlock
{
    // Full-width translucent bar covering the menu bar, mirroring ScreenCaptureKit's picker banner.
    NSRect screenFrame = [screen frame];
    CGFloat bannerWidth = NSWidth(screenFrame);
    CGFloat bannerHeight = 52;
    CGFloat padding = 20;
    NSRect bannerFrame = NSMakeRect(NSMinX(screenFrame), NSMaxY(screenFrame) - bannerHeight, bannerWidth, bannerHeight);
    if (!(self = [super initWithContentRect:bannerFrame styleMask:(NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel) backing:NSBackingStoreBuffered defer:NO]))
        return nil;

    _cancelBlock = makeBlockPtr(cancelBlock);

    [self setReleasedWhenClosed:NO];
    [self setLevel:NSStatusWindowLevel + 1];
    [self setOpaque:NO];
    [self setBackgroundColor:[NSColor clearColor]];
    [self setHasShadow:NO];
    [self setAppearance:[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]];
    [self setCollectionBehavior:(NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorTransient)];

    RetainPtr content = adoptNS([[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, 0, bannerWidth, bannerHeight)]);
    [content setMaterial:NSVisualEffectMaterialHUDWindow];
    [content setBlendingMode:NSVisualEffectBlendingModeBehindWindow];
    [content setState:NSVisualEffectStateActive];
    [content setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [self setContentView:content.get()];

    CGFloat iconSize = 30;
    RetainPtr icon = adoptNS([[NSImageView alloc] initWithFrame:NSMakeRect(padding, (bannerHeight - iconSize) / 2, iconSize, iconSize)]);
    [icon setImage:[NSApp applicationIconImage]];
    [icon setImageScaling:NSImageScaleProportionallyUpOrDown];
    [content addSubview:icon.get()];

    RetainPtr title = [NSTextField labelWithString:WEB_UI_NSSTRING(@"Choose a Tab to Share", "Title of the banner shown while the tab capture picker is shown")];
    [title setFont:[NSFont systemFontOfSize:17 weight:NSFontWeightSemibold]];
    [title setTextColor:[NSColor labelColor]];
    [title sizeToFit];
    CGFloat titleX = padding + iconSize + 10;
    [title setFrame:NSMakeRect(titleX, (bannerHeight - NSHeight([title frame])) / 2, NSWidth([title frame]), NSHeight([title frame]))];
    [content addSubview:title.get()];

    // Light appearance gives the white button of the system banner.
    RetainPtr cancelButton = adoptNS([[NSButton alloc] initWithFrame:NSZeroRect]);
    [cancelButton setAppearance:[NSAppearance appearanceNamed:NSAppearanceNameAqua]];
    [cancelButton setBezelStyle:NSBezelStyleRounded];
    [cancelButton setControlSize:NSControlSizeLarge];
    [cancelButton setTitle:WEB_UI_NSSTRING(@"Cancel", "Cancel")];
    [cancelButton setKeyEquivalent:@"\E"];
    [cancelButton setTarget:self];
    [cancelButton setAction:@selector(_cancelClicked:)];
    [cancelButton sizeToFit];
    NSSize cancelSize = NSMakeSize(std::max<CGFloat>(NSWidth([cancelButton frame]), 120), NSHeight([cancelButton frame]));
    [cancelButton setFrame:NSMakeRect(bannerWidth - cancelSize.width - padding, (bannerHeight - cancelSize.height) / 2, cancelSize.width, cancelSize.height)];
    [cancelButton setAutoresizingMask:NSViewMinXMargin];
    [content addSubview:cancelButton.get()];

    RetainPtr<NSString> showAllWindowsString = WEB_UI_NSSTRING(@"Show All Windows", "Tooltip of the button of the tab capture picker banner that shows all the application windows");
    RetainPtr<NSImage> showAllWindowsImage = [NSImage imageWithSystemSymbolName:@"rectangle.3.group" accessibilityDescription:showAllWindowsString.get()];
    if (showAllWindowsImage) {
        RetainPtr symbolConfiguration = [NSImageSymbolConfiguration configurationWithPointSize:20 weight:NSFontWeightRegular];
        showAllWindowsImage = [showAllWindowsImage imageWithSymbolConfiguration:symbolConfiguration.get()];
        CGFloat showAllWindowsSize = 36;
        RetainPtr showAllWindowsButton = [NSButton buttonWithImage:showAllWindowsImage.get() target:self action:@selector(_showAllWindowsClicked:)];
        [showAllWindowsButton setBordered:NO];
        [showAllWindowsButton setContentTintColor:[NSColor labelColor]];
        [showAllWindowsButton setToolTip:showAllWindowsString.get()];
        [showAllWindowsButton setFrame:NSMakeRect(NSMinX([cancelButton frame]) - showAllWindowsSize - 16, (bannerHeight - showAllWindowsSize) / 2, showAllWindowsSize, showAllWindowsSize)];
        [showAllWindowsButton setAutoresizingMask:NSViewMinXMargin];
        [content addSubview:showAllWindowsButton.get()];
    }

    return self;
}

- (BOOL)canBecomeKeyWindow
{
    return YES;
}

- (void)invalidate
{
    _cancelBlock = nullptr;
    [self orderOut:nil];
}

- (void)_cancelClicked:(id)sender
{
    if (auto block = std::exchange(_cancelBlock, nullptr))
        block();
}

- (void)_showAllWindowsClicked:(id)sender
{
    // Ask the Dock for App Exposé, which only shows the frontmost application's windows.
    if (OSStatus status = CoreDockSendNotification(CFSTR("com.apple.expose.front.awake"), 0))
        RELEASE_LOG_ERROR(WebRTC, "Failed to show all windows for tab picker: %d", static_cast<int>(status));
}

@end

@interface WKTabCapturePickerOverlay : NSView
- (instancetype)initWithWebView:(WKWebView *)webView frame:(NSRect)frame requestingOriginName:(NSString *)requestingOriginName shareBlock:(void(^)(void))shareBlock cancelBlock:(void(^)(void))cancelBlock hoverBlock:(void(^)(void))hoverBlock;
- (void)setShowsCaptureTint:(BOOL)shows;
- (void)invalidate;
@end

@implementation WKTabCapturePickerOverlay {
    BlockPtr<void()> _shareBlock;
    BlockPtr<void()> _cancelBlock;
    BlockPtr<void()> _hoverBlock;
    WeakObjCPtr<WKWebView> _webView;
    BOOL _webViewIgnoredMouseMoveEvents;
    BOOL _showsCaptureTint;
}

- (instancetype)initWithWebView:(WKWebView *)webView frame:(NSRect)frame requestingOriginName:(NSString *)requestingOriginName shareBlock:(void(^)(void))shareBlock cancelBlock:(void(^)(void))cancelBlock hoverBlock:(void(^)(void))hoverBlock
{
    if (!(self = [super initWithFrame:frame]))
        return nil;

    _shareBlock = makeBlockPtr(shareBlock);
    _cancelBlock = makeBlockPtr(cancelBlock);
    _hoverBlock = makeBlockPtr(hoverBlock);

    _webView = webView;
    _webViewIgnoredMouseMoveEvents = [webView _ignoresMouseMoveEvents];
    [webView _setIgnoresMouseMoveEvents:YES];

    [self setWantsLayer:YES];
    [self setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    RetainPtr layer = [self layer];
    [layer setBackgroundColor:retainPtr([NSColor clearColor].CGColor)];
    [layer setBorderWidth:0];

    [self addTrackingArea:adoptNS([[NSTrackingArea alloc] initWithRect:NSZeroRect options:NSTrackingMouseEnteredAndExited | NSTrackingCursorUpdate | NSTrackingActiveInActiveApp | NSTrackingInVisibleRect owner:self userInfo:nil])];

    [self _addPromptWithRequestingOriginName:requestingOriginName];

    return self;
}

// FIXME: Keep the prompt inside the overlay, so that its buttons stay visible in very small web views, and test it.
- (void)_addPromptWithRequestingOriginName:(NSString *)requestingOriginName
{
    RetainPtr buttonStack = adoptNS([[NSStackView alloc] init]);
    [buttonStack setOrientation:NSUserInterfaceLayoutOrientationVertical];
    [buttonStack setDistribution:NSStackViewDistributionFill];
    [buttonStack setSpacing:WebKit::capturePromptButtonSpacing];
    [buttonStack setTranslatesAutoresizingMaskIntoConstraints:NO];

    CGFloat widestButtonWidth = 0;
    auto addButton = [&](NSString *title, SEL action, NSString *keyEquivalent) {
        RetainPtr button = [WKTabCapturePickerButton buttonWithTitle:title target:self action:action];
        [button setKeyEquivalent:keyEquivalent];
        [button setControlSize:NSControlSizeLarge];
        [button setToolTip:title];
        [button setTranslatesAutoresizingMaskIntoConstraints:NO];
        widestButtonWidth = std::max(widestButtonWidth, [button fittingSize].width);
        [buttonStack addArrangedSubview:button];
        WebKit::pinToWidthOfStack(button, buttonStack);
    };
    addButton(WEB_UI_NSSTRING(@"Share This Tab", "Button title in the tab capture picker to share the selected tab"), @selector(_shareClicked:), @"\r");
    addButton(WEB_UI_NSSTRING(@"Cancel", "Cancel"), @selector(_cancelClicked:), @"\E");

    CGFloat contentWidth = std::clamp(widestButtonWidth, WebKit::capturePromptMinimumContentWidth, WebKit::capturePromptMaximumContentWidth);

    RetainPtr title = adoptNS([[NSString alloc] initWithFormat:WEB_UI_NSSTRING(@"Share this tab with “%@”?", "Title of the tab capture picker prompt"), requestingOriginName]);
    RetainPtr titleLabel = [NSTextField wrappingLabelWithString:title];
    [titleLabel setFont:[NSFont boldSystemFontOfSize:[NSFont systemFontSize]]];
    [titleLabel setSelectable:NO];
    [titleLabel setPreferredMaxLayoutWidth:contentWidth];
    [titleLabel setTranslatesAutoresizingMaskIntoConstraints:NO];

    RetainPtr contentStack = adoptNS([[NSStackView alloc] init]);
    [contentStack setOrientation:NSUserInterfaceLayoutOrientationVertical];
    [contentStack setDistribution:NSStackViewDistributionFill];
    [contentStack setSpacing:WebKit::capturePromptSpacing];
    [contentStack setTranslatesAutoresizingMaskIntoConstraints:NO];
    for (NSView *view in @[ titleLabel, buttonStack ]) {
        [contentStack addArrangedSubview:view];
        WebKit::pinToWidthOfStack(view, contentStack);
    }

    RetainPtr prompt = adoptNS([[NSVisualEffectView alloc] initWithFrame:NSZeroRect]);
    [prompt setMaterial:NSVisualEffectMaterialWindowBackground];
    [prompt setBlendingMode:NSVisualEffectBlendingModeWithinWindow];
    [prompt setState:NSVisualEffectStateActive];
    [prompt setWantsLayer:YES];
    [[prompt layer] setCornerRadius:promptCornerRadius];
    [[prompt layer] setMasksToBounds:YES];
    [prompt setTranslatesAutoresizingMaskIntoConstraints:NO];
    [prompt addSubview:contentStack];
    [self addSubview:prompt];

    [NSLayoutConstraint activateConstraints:@[
        [[contentStack topAnchor] constraintEqualToAnchor:[prompt topAnchor] constant:WebKit::capturePromptTopInset],
        [[contentStack bottomAnchor] constraintEqualToAnchor:[prompt bottomAnchor] constant:-WebKit::capturePromptInset],
        [[contentStack leadingAnchor] constraintEqualToAnchor:[prompt leadingAnchor] constant:WebKit::capturePromptInset],
        [[contentStack trailingAnchor] constraintEqualToAnchor:[prompt trailingAnchor] constant:-WebKit::capturePromptInset],
        [[contentStack widthAnchor] constraintEqualToConstant:contentWidth],
        [[prompt centerXAnchor] constraintEqualToAnchor:[self centerXAnchor]],
        [[prompt centerYAnchor] constraintEqualToAnchor:[self centerYAnchor]],
    ]];
}

// FIXME: Make the overlay accessible, especially with VoiceOver.
- (void)setShowsCaptureTint:(BOOL)shows
{
    if (_showsCaptureTint == shows)
        return;
    _showsCaptureTint = shows;

    RetainPtr layer = [self layer];
    if (!shows) {
        [layer setBackgroundColor:retainPtr([NSColor clearColor].CGColor)];
        [layer setBorderWidth:0];
        return;
    }

    [layer setBackgroundColor:retainPtr([[NSColor systemBlueColor] colorWithAlphaComponent:0.18].CGColor)];
    [layer setBorderColor:retainPtr([[NSColor systemBlueColor] colorWithAlphaComponent:0.9].CGColor)];
    [layer setBorderWidth:3];
}

- (void)invalidate
{
    _shareBlock = nullptr;
    _cancelBlock = nullptr;
    _hoverBlock = nullptr;
    if (RetainPtr webView = _webView.get())
        [webView _setIgnoresMouseMoveEvents:_webViewIgnoredMouseMoveEvents];
    _webView = nil;
    [self removeFromSuperview];
}

- (void)_shareClicked:(id)sender
{
    _cancelBlock = nullptr;
    if (auto block = std::exchange(_shareBlock, nullptr))
        block();
}

- (void)_cancelClicked:(id)sender
{
    _shareBlock = nullptr;
    if (auto block = std::exchange(_cancelBlock, nullptr))
        block();
}

- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
    return YES;
}

- (void)mouseEntered:(NSEvent *)event
{
    if (_hoverBlock)
        _hoverBlock();
}

- (void)mouseExited:(NSEvent *)event
{
}

- (void)cursorUpdate:(NSEvent *)event
{
    [[NSCursor arrowCursor] set];
}

// The overlay is a sibling of the WKWebView, so scroll events have to be forwarded for the page to still scroll.
- (void)scrollWheel:(NSEvent *)event
{
    [_webView.get() scrollWheel:event];
}

// The overlay takes the mouse and zoom events so that they do not reach the page underneath.
- (void)mouseDown:(NSEvent *)event
{
}

- (void)mouseDragged:(NSEvent *)event
{
}

- (void)mouseUp:(NSEvent *)event
{
}

- (void)rightMouseDown:(NSEvent *)event
{
}

- (void)rightMouseDragged:(NSEvent *)event
{
}

- (void)rightMouseUp:(NSEvent *)event
{
}

- (void)otherMouseDown:(NSEvent *)event
{
}

- (void)otherMouseDragged:(NSEvent *)event
{
}

- (void)otherMouseUp:(NSEvent *)event
{
}

- (void)magnifyWithEvent:(NSEvent *)event
{
}

- (void)smartMagnifyWithEvent:(NSEvent *)event
{
}

@end

namespace WebKit {

WTF_MAKE_TZONE_ALLOCATED_IMPL(TabCapturePicker);

static NSRect overlayFrame(WebPageProxy& page, WKWebView *webView, NSView *overlayHost)
{
    WebCore::FloatRect frame = [webView bounds];
    frame.contract(page.pendingOrActualObscuredContentInsets());
    return [overlayHost convertRect:frame fromView:webView];
}

// The overlay is a sibling of the WKWebView rather than a subview: on empty tabs, like the Start Page or Favorites,
// the browser hides the WKWebView but keeps it in the view hierarchy, and the overlay should stay visible.
static NSView *overlayHostForWebView(WKWebView *webView)
{
    if (RetainPtr superview = [webView superview])
        return superview.get();
    return [[webView window] contentView];
}

static bool canShowOverlayInWindow(NSWindow *window)
{
    return window && ([window isVisible] || [window isMiniaturized]);
}

// The page whose WKWebView is visible in the window. On empty tabs, the browser hides the WKWebView of the tab, so this
// falls back to a page of the window whose WKWebView is hidden.
// FIXME: for now, we only support a window that renders one web view. We should consider the case of a window rendering multiple web views.
static RefPtr<WebPageProxy> visiblePageInWindow(NSWindow *window)
{
    if (!window)
        return nullptr;
    RefPtr<WebPageProxy> hiddenPage;
    for (Ref process : WebProcessProxy::allProcesses()) {
        for (Ref page : process->pages()) {
            RetainPtr view = page->cocoaView();
            if (!view || [view window] != window || isInspectorPage(page))
                continue;
            if (![view isHiddenOrHasHiddenAncestor])
                return page;
            if (!hiddenPage)
                hiddenPage = page.ptr();
        }
    }
    return hiddenPage;
}

Ref<TabCapturePicker> TabCapturePicker::create(WebPageProxy& requestingPage, const WebCore::SecurityOriginData& requestingOrigin, Callback&& callback)
{
    return adoptRef(*new TabCapturePicker(requestingPage, requestingOrigin, WTF::move(callback)));
}

TabCapturePicker::TabCapturePicker(WebPageProxy& requestingPage, const WebCore::SecurityOriginData& requestingOrigin, Callback&& callback)
    : m_requestingPage(requestingPage)
    , m_requestingOriginName(applicationVisibleNameFromOrigin(requestingOrigin))
    , m_callback(WTF::move(callback))
{
    if (!m_requestingOriginName)
        m_requestingOriginName = applicationVisibleName();
}

TabCapturePicker::~TabCapturePicker()
{
    stop();
    if (m_callback)
        m_callback(nullptr);
}

void TabCapturePicker::start()
{
    RefPtr requestingPage = m_requestingPage;
    RetainPtr requestingWebView = requestingPage ? requestingPage->cocoaView() : nullptr;
    if (!requestingWebView) {
        finish(nullptr);
        return;
    }

    m_selectedWindow = [requestingWebView window];

    m_windowWillCloseObserver = [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:nil queue:nil usingBlock:makeBlockPtr([weakThis = WeakPtr { *this }](NSNotification *) {
        if (RefPtr protectedThis = weakThis)
            protectedThis->scheduleOverlaysUpdate();
    }).get()];

    updateOverlays();

    // Defer so that the getDisplayMedia alert sheet has fully torn down; otherwise AppKit hands key
    // status back to the sheet's parent, the requesting window, after we move focus.
    RunLoop::mainSingleton().dispatch([weakThis = WeakPtr { *this }] {
        if (RefPtr protectedThis = weakThis) {
            protectedThis->focusWindowOtherThanRequestingOne();
            protectedThis->showBanner();
        }
    });
}

void TabCapturePicker::cancel()
{
    finish(nullptr);
}

void TabCapturePicker::scheduleOverlaysUpdate()
{
    if (!m_callback || m_isOverlaysUpdateScheduled)
        return;

    m_isOverlaysUpdateScheduled = true;
    RunLoop::mainSingleton().dispatch([weakThis = WeakPtr { *this }] {
        if (RefPtr protectedThis = weakThis)
            protectedThis->updateOverlays();
    });
}

void TabCapturePicker::updateOverlays()
{
    m_isOverlaysUpdateScheduled = false;
    if (!m_callback)
        return;

    RetainPtr windowsWithOverlay = adoptNS([[NSMutableSet alloc] init]);
    m_overlays.removeAllMatching([&](auto& overlay) {
        // As the tabs of a window may share the overlay host, an overlay is stale when its page is no longer the
        // visible one of its window, for instance after a tab switch.
        RefPtr page = overlay.page;
        RetainPtr webView = page ? page->cocoaView() : nullptr;
        RetainPtr window = [webView window];
        RetainPtr overlayHost = webView ? overlayHostForWebView(webView) : nil;
        if (webView && canShowOverlayInWindow(window) && visiblePageInWindow(window) == page && [overlay.view superview] == overlayHost) {
            [overlay.view setFrame:overlayFrame(*page, webView, overlayHost)];
            [windowsWithOverlay addObject:window];
            return false;
        }
        [overlay.view invalidate];
        return true;
    });

    for (Ref process : WebProcessProxy::allProcesses()) {
        for (Ref page : process->pages()) {
            RetainPtr webView = page->cocoaView();
            RetainPtr window = [webView window];
            if (!canShowOverlayInWindow(window) || [windowsWithOverlay containsObject:window])
                continue;
            if (RefPtr visiblePage = visiblePageInWindow(window)) {
                [windowsWithOverlay addObject:window];
                installOverlay(*visiblePage);
            }
        }
    }

    updateTints();
}

void TabCapturePicker::installOverlay(WebPageProxy& page)
{
    RetainPtr webView = page.cocoaView();
    if (!webView)
        return;
    RetainPtr window = [webView window];
    RetainPtr overlayHost = overlayHostForWebView(webView);
    if (!overlayHost)
        return;

    auto shareBlock = makeBlockPtr([weakThis = WeakPtr { *this }, weakPage = WeakPtr { page }] {
        if (RefPtr protectedThis = weakThis)
            protectedThis->finish(weakPage);
    });
    auto cancelBlock = makeBlockPtr([weakThis = WeakPtr { *this }] {
        if (RefPtr protectedThis = weakThis)
            protectedThis->finish(nullptr);
    });
    auto hoverBlock = makeBlockPtr([weakThis = WeakPtr { *this }, weakWindow = WeakObjCPtr<NSWindow> { window }] {
        if (RefPtr protectedThis = weakThis)
            protectedThis->selectWindow(weakWindow.get());
    });

    RetainPtr overlay = adoptNS([[WKTabCapturePickerOverlay alloc] initWithWebView:webView frame:overlayFrame(page, webView, overlayHost) requestingOriginName:m_requestingOriginName shareBlock:shareBlock.get() cancelBlock:cancelBlock.get() hoverBlock:hoverBlock.get()]);
    [overlayHost addSubview:overlay positioned:NSWindowAbove relativeTo:nil];
    m_overlays.append({ WTF::move(overlay), page });
}

void TabCapturePicker::focusWindowOtherThanRequestingOne()
{
    if (!m_callback)
        return;

    // Like the system window picker, move focus away from the requesting window so the user is
    // steered towards picking another tab. -[NSApp orderedWindows] is front-to-back, so the first
    // match is the most recently used browser window. If the requesting window is the only one, keep
    // it: the user can still switch tabs in it. The window is only ordered front: the banner takes key status.
    RefPtr requestingPage = m_requestingPage;
    RetainPtr requestingWebView = requestingPage ? requestingPage->cocoaView() : nullptr;
    RetainPtr requestingWindow = [requestingWebView window];
    for (NSWindow *window in [NSApp orderedWindows]) {
        if (window == requestingWindow || ![window isVisible] || [window isMiniaturized] || !visiblePageInWindow(window))
            continue;
        [window orderFront:nil];
        selectWindow(window);
        return;
    }
}

void TabCapturePicker::showBanner()
{
    if (!m_callback || m_banner)
        return;

    RefPtr requestingPage = m_requestingPage;
    RetainPtr requestingWebView = requestingPage ? requestingPage->cocoaView() : nullptr;
    RetainPtr screen = [[requestingWebView window] screen];
    if (!screen)
        screen = [NSScreen mainScreen];

    m_banner = adoptNS([[WKTabCapturePickerBanner alloc] initWithScreen:screen.get() cancelBlock:makeBlockPtr([weakThis = WeakPtr { *this }] {
        if (RefPtr protectedThis = weakThis)
            protectedThis->finish(nullptr);
    }).get()]);
    [m_banner makeKeyAndOrderFront:nil];
}

void TabCapturePicker::selectWindow(NSWindow *window)
{
    if (!window)
        return;
    m_selectedWindow = window;
    updateTints();
}

void TabCapturePicker::updateTints()
{
    RetainPtr selectedWindow = m_selectedWindow.get();
    for (auto& overlay : borrow(m_overlays))
        [overlay.view setShowsCaptureTint:[overlay.view window] == selectedWindow];
}

void TabCapturePicker::finish(RefPtr<WebPageProxy>&& pickedPage)
{
    if (!m_callback)
        return;
    auto callback = WTF::move(m_callback);
    stop();
    callback(WTF::move(pickedPage));
}

void TabCapturePicker::stop()
{
    for (auto& overlay : std::exchange(m_overlays, { }))
        [overlay.view invalidate];
    if (RetainPtr banner = std::exchange(m_banner, nullptr))
        [banner invalidate];
    if (m_windowWillCloseObserver) {
        [[NSNotificationCenter defaultCenter] removeObserver:m_windowWillCloseObserver];
        m_windowWillCloseObserver = nullptr;
    }
    m_selectedWindow = nil;
}

} // namespace WebKit

#endif // HAVE(WINDOW_CAPTURE)
