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
#import "RemoteLayerTreeMirror.h"

#import "LayerProperties.h"
#import "RemoteLayerTreeCommitBundle.h"
#import "RemoteLayerTreeDrawingAreaProxy.h"
#import "RemoteLayerTreeTransaction.h"
#import <QuartzCore/QuartzCore.h>
#import <wtf/MainThread.h>
#import <wtf/TZoneMallocInlines.h>

@interface WKMirrorContainerLayer : CALayer
- (void)setDidEnterLayerTreeHandler:(Function<void()>&&)handler;
@end

@implementation WKMirrorContainerLayer {
    Function<void()> _didEnterLayerTreeHandler;
}

- (void)setDidEnterLayerTreeHandler:(Function<void()>&&)handler
{
    _didEnterLayerTreeHandler = WTF::move(handler);
}

- (id<CAAction>)actionForKey:(NSString *)key
{
    // Sent when the layer joins a layer tree that is in a context, directly or through an ancestor.
    if (_didEnterLayerTreeHandler && isMainThread() && [key isEqualToString:kCAOnOrderIn])
        _didEnterLayerTreeHandler();

    // No implicit animations, like WebActionDisablingCALayerDelegate.
    return nil;
}

@end

namespace WebKit {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RemoteLayerTreeMirror);

RemoteLayerTreeMirror::RemoteLayerTreeMirror(RemoteLayerTreeDrawingAreaProxy& drawingArea)
    : m_host(drawingArea, RemoteLayerTreeHost::Role::Mirror)
    , m_layer(adoptNS([[WKMirrorContainerLayer alloc] init]))
{
    [m_layer setName:@"Layer tree mirror container"];
    [m_layer setAnchorPoint:CGPointZero];
    [m_layer setMasksToBounds:YES];
    [m_layer setDidEnterLayerTreeHandler:[weakDrawingArea = WeakPtr { drawingArea }] {
        if (RefPtr drawingArea = weakDrawingArea.get())
            drawingArea->mirrorDidEnterLayerTree();
    }];
}

RemoteLayerTreeMirror::~RemoteLayerTreeMirror()
{
    // Clients can keep the layer alive.
    [m_layer setDidEnterLayerTreeHandler:nullptr];
}

CALayer *RemoteLayerTreeMirror::layer() const
{
    return m_layer.get();
}

void RemoteLayerTreeMirror::willRequestFullState(WebCore::ProcessIdentifier processIdentifier)
{
    m_processesAwaitingFullState.add(processIdentifier);
}

void RemoteLayerTreeMirror::didReceiveFullState(const IPC::Connection& connection, WebCore::ProcessIdentifier processIdentifier, const RemoteLayerTreeFullState& fullState, bool hasMainFrame, const RemoteLayerTreeHost& primaryHost)
{
    ASSERT(m_processesAwaitingFullState.contains(processIdentifier));
    if (!m_processesAwaitingFullState.remove(processIdentifier))
        return;

    for (auto& transaction : fullState) {
        std::optional<MainFrameData> mainFrameData;
        if (hasMainFrame && !transaction->remoteContextHostedIdentifier())
            mainFrameData = MainFrameData { };
        m_host.updateLayerTree(connection, transaction, mainFrameData);
    }

    for (auto& transaction : fullState) {
        for (auto layerID : transaction->changedLayerProperties().keys()) {
            copyContentsFromPrimaryLayer(layerID, primaryHost);
            // The full state does not contain the CA animations added before it, but the primary layers have them.
            copyAnimationsFromPrimaryLayer(layerID, primaryHost);
        }
    }

    if (RetainPtr rootLayer = m_host.rootLayer(); rootLayer && hasMainFrame)
        [m_layer setSublayers:@[ rootLayer.get() ]];

    updateLayerFrom(primaryHost);
}

void RemoteLayerTreeMirror::didCommitTransactionToPrimaryTree(const IPC::Connection& connection, const RemoteLayerTreeTransaction& transaction, const std::optional<MainFrameData>& mainFrameData, const TransactionID& transactionID, const RemoteLayerTreeHost& primaryHost)
{
    bool isWaitingForFullState = m_processesAwaitingFullState.contains(transactionID.processIdentifier());
    if (isWaitingForFullState)
        return;

    m_host.updateLayerTree(connection, transaction, mainFrameData);
    ASSERT(!m_host.rootLayer() || [m_host.rootLayer() superlayer] == m_layer.get());

    for (auto layerID : transaction.changedLayerProperties().keys())
        copyContentsFromPrimaryLayer(layerID, primaryHost);

    updateLayerFrom(primaryHost);
}

void RemoteLayerTreeMirror::didSetLayerContentsInPrimaryTree(WebCore::PlatformLayerIdentifier layerID, const RemoteLayerTreeHost& primaryHost)
{
    copyContentsFromPrimaryLayer(layerID, primaryHost);
}

void RemoteLayerTreeMirror::copyContentsFromPrimaryLayer(WebCore::PlatformLayerIdentifier layerID, const RemoteLayerTreeHost& primaryHost)
{
    RefPtr primaryNode = primaryHost.nodeForID(layerID);
    RefPtr mirrorNode = m_host.nodeForID(layerID);
    if (primaryNode && mirrorNode)
        mirrorNode->copyContentsFrom(*primaryNode);
}

void RemoteLayerTreeMirror::copyAnimationsFromPrimaryLayer(WebCore::PlatformLayerIdentifier layerID, const RemoteLayerTreeHost& primaryHost)
{
    RetainPtr primaryLayer = primaryHost.layerForID(layerID);
    RetainPtr mirrorLayer = m_host.layerForID(layerID);
    if (!primaryLayer || !mirrorLayer)
        return;

    [mirrorLayer removeAllAnimations];
    // FIXME: Remove the suppression once https://github.com/llvm/llvm-project/pull/230818 is deployed (rdar://189710680).
    for (SUPPRESS_UNRETAINED_LOCAL NSString *key in [primaryLayer animationKeys]) {
        // The copy keeps the begin time that CA set on the primary animation, so both layers stay in sync.
        RetainPtr animation = adoptNS([[primaryLayer animationForKey:key] copy]);
        [animation setDelegate:nil];
        [mirrorLayer addAnimation:animation.get() forKey:key];
    }
}

void RemoteLayerTreeMirror::copyAnimationsFromPrimaryTree(const RemoteLayerTreeHost& primaryHost)
{
    for (auto layerID : m_host.nodes().keys())
        copyAnimationsFromPrimaryLayer(layerID, primaryHost);
}

void RemoteLayerTreeMirror::updateGeometryFromPrimaryTree(const RemoteLayerTreeHost& primaryHost)
{
    // This runs after every commit and display refresh, so only set what differs.
    for (auto& [layerID, node] : m_host.nodes()) {
        RetainPtr primaryLayer = primaryHost.layerForID(layerID);
        if (!primaryLayer)
            continue;

        RetainPtr mirrorLayer = node->layer();
        if (auto position = [primaryLayer position]; !CGPointEqualToPoint(position, [mirrorLayer position]))
            [mirrorLayer setPosition:position];
        if (auto bounds = [primaryLayer bounds]; !CGRectEqualToRect(bounds, [mirrorLayer bounds]))
            [mirrorLayer setBounds:bounds];
        if (auto transform = [primaryLayer transform]; !CATransform3DEqualToTransform(transform, [mirrorLayer transform]))
            [mirrorLayer setTransform:transform];
        if (auto sublayerTransform = [primaryLayer sublayerTransform]; !CATransform3DEqualToTransform(sublayerTransform, [mirrorLayer sublayerTransform]))
            [mirrorLayer setSublayerTransform:sublayerTransform];
    }

    updateLayerFrom(primaryHost);
}

void RemoteLayerTreeMirror::updateLayerFrom(const RemoteLayerTreeHost& primaryHost)
{
    // Use the coordinate space of the primary root layer, so that the mirror root keeps the same geometry.
    RetainPtr primaryRootSuperlayer = [primaryHost.rootLayer() superlayer];
    if (!primaryRootSuperlayer)
        return;

    auto bounds = [primaryRootSuperlayer bounds];
    if (!CGRectEqualToRect(bounds, [m_layer bounds]))
        [m_layer setBounds:bounds];

    // The primary root superlayer may inherit its flipping from an ancestor, like the web view layer on macOS.
    bool isGeometryFlipped = [primaryRootSuperlayer contentsAreFlipped];
    if (isGeometryFlipped != [m_layer isGeometryFlipped])
        [m_layer setGeometryFlipped:isGeometryFlipped];

    auto sublayerTransform = [primaryRootSuperlayer sublayerTransform];
    if (!CATransform3DEqualToTransform(sublayerTransform, [m_layer sublayerTransform]))
        [m_layer setSublayerTransform:sublayerTransform];
}

void RemoteLayerTreeMirror::processDidTerminate(WebCore::ProcessIdentifier processIdentifier)
{
    m_processesAwaitingFullState.remove(processIdentifier);
    m_host.remotePageProcessDidTerminate(processIdentifier);
}

void RemoteLayerTreeMirror::processDidLeavePage(WebCore::ProcessIdentifier processIdentifier)
{
    m_processesAwaitingFullState.remove(processIdentifier);
}

} // namespace WebKit
