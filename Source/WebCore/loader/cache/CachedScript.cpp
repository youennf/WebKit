/*
    Copyright (C) 1998 Lars Knoll (knoll@mpi-hd.mpg.de)
    Copyright (C) 2001 Dirk Mueller (mueller@kde.org)
    Copyright (C) 2002 Waldo Bastian (bastian@kde.org)
    Copyright (C) 2006 Samuel Weinig (sam.weinig@gmail.com)
    Copyright (C) 2004-2025 Apple Inc. All rights reserved.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.

    This class provides all functionality needed for loading images, style sheets and html
    pages from the web. It has a memory cache for these objects.
*/

#include "config.h"
#include "CachedScript.h"

#include "CachedResourceClient.h"
#include "CachedResourceRequest.h"
#include "SharedBuffer.h"
#include "TextResourceDecoder.h"

#if PLATFORM(MAC)
#include <wtf/cocoa/RuntimeApplicationChecksCocoa.h>
#endif

namespace WebCore {

CachedScript::CachedScript(CachedResourceRequest&& request, PAL::SessionID sessionID, const CookieJar* cookieJar, ScriptTrackingPrivacyProtectionsEnabled requiresPrivacyProtections)
    : CachedResource(WTFMove(request), request.options().destination == FetchOptionsDestination::Json ? Type::JSON : Type::Script, sessionID, cookieJar)
    , m_requiresPrivacyProtections(requiresPrivacyProtections == ScriptTrackingPrivacyProtectionsEnabled::Yes)
    , m_decoder(TextResourceDecoder::create("text/javascript"_s, request.charset()))
{
    if (url().string().contains("3avio"_s))
        WTFLogAlways("CachedScript::CachedScript '%s'", url().string().utf8().data());
}

CachedScript::~CachedScript() = default;

RefPtr<TextResourceDecoder> CachedScript::protectedDecoder() const
{
    return m_decoder;
}

void CachedScript::setEncoding(const String& chs)
{
    protectedDecoder()->setEncoding(chs, TextResourceDecoder::EncodingFromHTTPHeader);
}

ASCIILiteral CachedScript::encoding() const
{
    return protectedDecoder()->encoding().name();
}

StringView CachedScript::script(ShouldDecodeAsUTF8Only shouldDecodeAsUTF8Only)
{
    if (!m_data)
        return emptyString();

    if (RefPtr data = m_data; !data->isContiguous())
        m_data = data->makeContiguous();

    Ref contiguousData = downcast<SharedBuffer>(*m_data);
    if (m_decodingState == NeverDecoded
        && PAL::TextEncoding(encoding()).isByteBasedEncoding()
        && contiguousData->size()
        && charactersAreAllASCII(contiguousData->span())) {

        {
            Locker locker { m_lock };
            m_decodingState = DataAndDecodedStringHaveSameBytes;
        }

        // If the encoded and decoded data are the same, there is no decoded data cost!
        setDecodedSize(0);
        stopDecodedDataDeletionTimer();

        m_scriptHash = StringHasher::computeHashAndMaskTop8Bits(contiguousData->span());
    }

    if (m_decodingState == DataAndDecodedStringHaveSameBytes) {
        if (url().string().contains("static.xx.fbcdn.net/rsrc.php"_s)) {
        // if (url().string().contains("3avio"_s) || url().string().contains("dsWalqj"_s)) {
            //WTFLogAlways("CachedScript1 for '%s'", url().string().utf8().data());

            if (!m_script.isEmpty())
                return m_script;
            String script = { byteCast<Latin1Character>(contiguousData->span()) };

            bool isInteresting = script.contains("frameDecryptor_decrypt"_s)
                || script.contains("function createExportWrapper"_s)
                || script.contains("b.logErrorToFbLogger=function(a,b,d,e)"_s)
                || script.contains("b.logEvent=function(a)"_s)
                || script.contains("b.logEvent=function(a,b)"_s)
                || script.contains("onIceConnected"_s)
                || script.contains("b.updateUserVideoSubscription"_s)
                || script.contains("e.$ZenonCallsModelEmitter"_s)
                || script.contains("addRemoteTrackFromEvent"_s)
                || script.contains("useZenonTrackSelector"_s);
            
            if (!isInteresting) {
                m_script = WTFMove(script);
                return m_script;
            }
            if (isInteresting) {
                WTFLogAlways("found script");
                Vector<String> values;
                
                values = script.split("function createExportWrapper("_s);
                WTFLogAlways("found createExportWrapperWithLog chunks %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), ""
                                             "function createExportWrapperWithLog(name) {"
                                             "  var counter = 0;"
                                             "  return function() {"
                                             "    var f = wasmExports[name];\n"
                                             "    var result = f.apply(null, arguments);\n"
                                             "    if (name == 'encryptionKeysManager_processE2eeMessage') \n"
                                             "      console.log('encryptionKeysManager_processE2eeMessage');\n"
                                             "    if (++counter < 50) console.log(name + ' result:' + result);\n"
                                             "    return result;"
                                             "  };"
                                             "}"
                                             "function createExportWrapper("_s);

                values = script.split("createExportWrapper(\"frameDecryptor_decrypt\")"_s);
                WTFLogAlways("found createExportWrapper frameDecryptor_decrypt chunks %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "createExportWrapperWithLog(\"frameDecryptor_decrypt\")"_s);

                values = script.split("createExportWrapper(\"encryptionKeysManager_processE2eeMessage\")"_s);
                WTFLogAlways("found createExportWrapper encryptionKeysManager_processE2eeMessage chunks %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "createExportWrapperWithLog(\"encryptionKeysManager_processE2eeMessage\")"_s);

                values = script.split("createExportWrapper(\"encryptionKeysManager_processE2eeServerUpdate\")"_s);
                WTFLogAlways("found createExportWrapper encryptionKeysManager_processE2eeServerUpdate chunks %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "createExportWrapperWithLog(\"encryptionKeysManager_processE2eeServerUpdate\")"_s);

#if 0
                auto index1 = script.find("function createExportWrapper("_s);
                script = makeString(script.substring(0, index1),
                                    "\n"
                                    "let counter = 0;\n"
                                    "const namesWithLog = new Set();\n"
                                    "namesWithLog.add('encryptionKeysManager_processE2eeMessage');\n"
                                    "function toString(g, buffer)"
                                    "{"
                                    "  var d = '';"
                                    "  for (var e = 0; e < 32; ++e)"
                                    "    d += String.fromCharCode(g.getValue(e + buffer, \"i8\"));"
                                    "  return d;"
                                    "}"
                                    "function createExportWrapperWithLog(name) {\n"
                                    "  return function() {\n"
                                    "    assert(runtimeInitialized, \"native function `\" + name + \"` called before runtime initialization\");\n"
                                    "    var f = wasmExports[name];\n"
                                    "    assert(f, \"exported native function `\" + name + \"` not found\");\n"
                                    "    let toLog = 'createExportWrapperWithLog for ' + name;"
                                    "    for (let a of arguments) toLog += ', ' + a \n"
                                    "    if (name == 'frameDecryptor_setSupportedFrameDataHandlerTypes') {\n"
                                    "      let a = arguments[1];\n"
                                    "      toLog += ' values:' + this.getValue(a, 'i8') + ',' + this.getValue(4 + a, 'i8') + ',' + this.getValue(8 + a, 'i8')\n"
                                    "    }\n"
                                    "    let result = f.apply(null, arguments);\n"
                                    "    if (namesWithLog.has(name)) {\n"
                                    "      toLog += ' -> ' + result;\n"
                                    "      if (name === 'frameDecryptorDeps_setE2eeId') {\n"
                                    "        let a = arguments[1];\n"
                                    "        toLog += ' , input id is: ' + toString(this, a);  "
                                    "      }\n"
                                    "      console.log(toLog);\n"
                                    "    }\n"
                                    "    else if (name === 'frameDecryptor_decrypt' && this.getValue(arguments[2], 'i32') > 150 && ++counter < 20) {\n"
                                    "      toLog += ' output length: ' + this.getValue(arguments[7], 'i32');\n"
                                    "      let a = arguments[1];\n"
                                    "      let length = this.getValue(arguments[2], 'i32') - 12;\n"
                                    "      toLog += ' values:' + this.getValue(a, 'i8') + ', ' + this.getValue(a + 1, 'i8') + ', ' + this.getValue(a + 2, 'i8') + ', ' + this.getValue(a + 3, 'i8') + ', ' + this.getValue(a + 4, 'i8') + ', ' + this.getValue(a + 5, 'i8') + ', ' + this.getValue(a + 6, 'i8') + ', ' + this.getValue(a + 7, 'i8') + ', ' + this.getValue(a + 8, 'i8') + ', ' + this.getValue(a + 9, 'i8') + ', ' + this.getValue(a + 10, 'i8') + ', ' + this.getValue(a + 11, 'i8')\n"
                                    "      toLog += ' -> ' + result;\n"
                                    "      console.log(toLog);\n"
                                    "      toLog = this.getValue(a + length, 'i8') + ', ' + this.getValue(a + length + 1, 'i8') + ', ' + this.getValue(a + length + 2, 'i8') + ', ' + this.getValue(a + length + 3, 'i8') + ', ' + this.getValue(a + length + 4, 'i8') + ', ' + this.getValue(a + length + 5, 'i8') + ', ' + this.getValue(a + length + 6, 'i8') + ', ' + this.getValue(a + length + 7, 'i8') + ', ' + this.getValue(a + length + 8, 'i8') + ', ' + this.getValue(a + length + 9, 'i8') + ', ' + this.getValue(a + length + 10, 'i8') + ', ' + this.getValue(a + length + 11, 'i8')\n"
                                    "      console.log(toLog);\n"
                                    "    }\n"
                                    "    return result;\n"
                                    "   }\n"
                                    ";\n"
                                    "}\n"_s
                                    , script.substring(index1)
                                    );
                values = script.split("=createExportWrapper("_s);
                WTFLogAlways("found createExportWrapperWithLog chunks %d\n", (int)values.size());

                script = makeStringByJoining(values1.span(), "=createExportWrapperWithLog("_s);

                values = script.split("g.decodeMessage=j;"_s);
                WTFLogAlways("found decodeMessage chunks %d\n", (int)values.size());

                script = makeStringByJoining(values.span(), ""
                                             "function j1(a, b) {"
                                                "const result = j(a,b);"
                                                //"console.log('decodeMessage:' + JSON.stringify(result));"
                                                "return result;"
                                             "};"
                                             "g.decodeMessage=j1;"_s);

                values = script.split("g.decodeByteMessages=a;"_s);
                WTFLogAlways("found decodeByteMessages chunks %d\n", (int)values.size());

                script = makeStringByJoining(values.span(), ""
                                            "function a1(a) {"
                                            "    var b = []"
                                            "      , c = 0;"
                                            "    while (c < a.length) {"
                                            "        var d = j1(a, c)"
                                            "          , e = d.wireMessage;"
                                            "        c = d.position;"
                                            "        if (e)"
                                            "            b.push(e);"
                                            "        else"
                                            "            break"
                                            "    }"
                                            "    d = null;"
                                            "    c < a.length && (d = a.subarray(c));"
                                            "console.log('decodeByteMessages:' + JSON.stringify(b));"
                                            "    return {"
                                            "        messages: b,"
                                            "        remaining: d"
                                            "    }"
                                            "}"
                                            "g.decodeByteMessages=a1;"_s);
//
                values = script.split("if(b.type!==\"dataMessageReceived\")"_s);
                WTFLogAlways("found if b type chunks %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), ""
                                             "console.log('dataMessageReceived dispatcher:' + JSON.stringify(b));"
                                             "if(b.type!==\"dataMessageReceived\")"_s);

                values = script.split("onGenericDataMessageReceived:function(a,c){"_s);
                WTFLogAlways("found onGenericDataMessageReceived %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "onGenericDataMessageReceived:function(a,c){"
                                             "console.log('onGenericDataMessageReceived with ' + JSON.stringify(c));"
                                             ""_s);

                values = script.split("defer:function(a,b){"_s);
                WTFLogAlways("found defer:function %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "defer:function(a,b){"
                                             "console.log('defer:function with ' + JSON.stringify(b));"
                                             ""_s);

                values = script.split("onDeferTimeout:function(a){"_s);
                WTFLogAlways("found onDeferTimeout %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "onDeferTimeout:function(a){"
                                             "console.log('onDeferTimeout with ' + JSON.stringify(a));"
                                             ""_s);
#endif
#if 0
                values = script.split("b.processEvent=function(a){"_s);
                WTFLogAlways("found processEvent %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "b.processEvent=function(a){"
                                             "console.log('processEvent with ' + JSON.stringify(a));"
                                             ""_s);
/*
                values = script.split("f.sm.onTransition(function(a){"_s);
                WTFLogAlways("found f.sm.onTransition %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "f.sm.onTransition(function(a){"
                                             "console.log('f.sm.onTransition with ' + JSON.stringify({actions:a.actions, activities:a.activities, event:a.event}));"
                                             ""_s);
*/
                
                values = script.split("g.useZenonTrackSelector=a"_s);
                WTFLogAlways("found g.useZenonTrackSelector=a %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "\n"
                                             "function doMyTest(a,b) {"
                                             " var e, f, g, h, i, m, n;"
                                             " b === void 0 && (b = !1);"
                                             " a = d(\"RelayHooks\").useFragment(k, a);"
                                             " var o = a == null ? void 0 : a.actor_id"
                                              "   , p = o != null ? o : \"\""
                                             "   , q = d(\"ZenonActorHooks\").useZenonActor()[0] === p"
                                             "   , r = c(\"ZenonCallsHooks\").useLocalVideo()"
                                             "   , s = a == null || (e = a.app) == null || (e = e.current_call) == null || (e = e.self_participant) == null ? void 0 : e.screen_track"
                                             "   , t = q ? r : a == null || (f = a.call_participant) == null ? void 0 : f.video_track"
                                             "   , u = a == null || (g = a.call_participant) == null ? void 0 : g.screen_track;"
                                             " r = (r = a == null || (h = a.call_participant) == null ? void 0 : h.is_video_subscribed) != null ? r : !1;"
                                             " var v = (a == null || (i = a.app) == null || (i = i.product_config) == null ? void 0 : i.mirror_self_video_card) === !0"
                                             "   , w = c(\"useDebouncedValue\")(r, 800)"
                                             "   , x = d(\"ZenonDualStreamSwapHooks\").useZenonShouldSwapParticipantDualStream(p)"
                                             "   , y = d(\"Decoil\").useDecoilValue(c(\"ZenonRemoteVideoRenderingEnabledAtom\"))"
                                             "   , z = q && v"
                                             "   , A = (t == null ? void 0 : t.enabled) !== !0 || (t == null || (m = t.webrtcStream) == null ? void 0 : m.active) !== !0 || !q && (!y || !w || (t == null ? void 0 : t.pausedDownlink) === !0) ? null : t"
                                             "   , B = q ? \"SelfViewCameraVideo\" : \"RTCIncallVideo\";"
                                             " a = q ? s : u;"
                                             " var C = q ? \"SelfViewScreenVideo\" : \"RTCIncallVideo\""
                                             "   , D = (a == null ? void 0 : a.enabled) === !0 && a != null && (n = a.webrtcStream) != null && n.active ? a : null;"
                                             " console.log('doMyTest0: ' + A);"
                                             " console.log('doMyTest1: ' + t);"
                                             " console.log('doMyTest1 readyState: ' + t?.webrtcTrack?.readyState);"
                                             " console.log('doMyTest1 capabilities: ' + t?.webrtcTrack?.getCapabilities());"
                                             " console.log('doMyTest2: ' + t?.webrtcStream);"
                                             " console.log('doMyTest3: ' + t?.webrtcStream?.active);"
                                             "}\n"
                         "g.useZenonTrackSelector = (par1g,par2h) => {\n"
                         " const actor_id = par1g?.actor_id;"
                         " if (par1g && !par1g.myid) par1g.myid = Math.random();"
                         " const g_id = par1g.myid;"
                         " const call_participant = par1g?.call_participant;"
                         " doMyTest(par1g,par2h);\n"
                         " const result = a(par1g,par2h);"
                         " const test2 = d(\"RelayHooks\").useFragment(k, par1g);"
                         //" const resultTrackId = result ? result.trackId : 'null';"
                         //" const test1 = d(\"RelayHooks\").useFragment(k, g);"
                         //" console.log('useZenonTrackSelector with ' + JSON.stringify({g_id,test1,par2h}));"
                         " console.log('useZenonTrackSelector result is ' + JSON.stringify({result}));"
                         " return result;"
                         "}"_s);

                values = script.split("var l=d(\"useZenonTrackSelector\").useZenonTrackSelector(e)"_s);
                WTFLogAlways("found var l=d(\"useZenonTrackSelector\").useZenonTrackSelector(e) %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "console.log('calling useZenonTrackSelector for selecting video element ' + e?.myid);var l=d(\"useZenonTrackSelector\").useZenonTrackSelector(e)"
                         ""_s);

                values = script.split("b.logStateMachine=function(a,b,d,e,f){"_s);
                WTFLogAlways("found b.logStateMachine %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "b.logStateMachine=function(a,b,d,e,f){"
                                             "console.log('logStateMachine with ' + JSON.stringify(b));"
                                             ""_s);

                values = script.split("b.logStateMachineTransition=function(a,b,d,e,f,g,h,i){"_s);
                WTFLogAlways("found b.logStateMachineTransition %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "b.logStateMachineTransition=function(a,b,d,e,f,g,h,i){"
                                             "var j = \"\";"
                                             "g != null && g.length === 1 && g[0].type === \"defer\" ? j = \"[\" + a + \"] [[DEFERRED] \" + f + \" did not trigger transition. Current state remains \" + d : d !== e ? j = \"[\" + a + \"] [[PROCESSED] \" + f + \" caused transition from \" + (e || \"\") + \" to \" + d + \".\" : b ? j = \"[\" + a + \"] [[PROCESSED] \" + f + \" did not trigger transition. Current state remains \" + d : (j = \"[\" + a + \"] [[DROPPED] \" + f + \" did not trigger transition. Current state remains \" + d);"
                                             "console.log('logStateMachineTransition with ' + JSON.stringify(j));"
                                             ""_s);
                
                
                values = script.split("function b(a,b){if(b.type!==\"connectionEstablished\"||b.payload.peerConnectionRole!==\"secondary\")"_s);
                WTFLogAlways("found b.connectionEstablished peerConnectionRole secondary %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "function b(a,b){"
                                             "console.log('b.connectionEstablished peerConnectionRole secondary');"
                                             "if(b.type!==\"connectionEstablished\"||b.payload.peerConnectionRole!==\"secondary\")"_s);
                
//#if 0
                values = script.split("e.end=function(a,d){"_s);
                WTFLogAlways("found e.end=function(a,d){ %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "e.end=function(a,d){"
                                             "console.log('e.end=function(a,d)');"
                                             ""_s);

                //
                values = script.split("onDismissReceived:function(a,d,e,f){"_s);
                WTFLogAlways("found onDismissReceived:function(a,d,e,f){ %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "onDismissReceived:function(a,d,e,f){"
                                             "console.log('onDismissReceived:function(a,d,e,f){');"
                                             ""_s);

                //
                values = script.split("b.endCall=function(a,b,e){"_s);
                WTFLogAlways("found b.endCall=function(a,b,e){ %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "b.endCall=function(a,b,e){"
                                             "console.log('b.endCall 1');"
                                             ""_s);
                values = script.split("b.endCall=function(a,b){"_s);
                WTFLogAlways("found b.endCall=function(a,b){ %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "b.endCall=function(a,b){"
                                             "console.log('b.endCall 2');"
                                             ""_s);
                //
                values = script.split("function p(a,e){"_s);
                WTFLogAlways("found function p(a,e){ %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "function p(a,e){"
                                             "console.log('function end p');"
                                             ""_s);
//#endif
//onIceConnected:function(a){
                values = script.split("onIceConnected:function(a){"_s);
                WTFLogAlways("found onIceConnected:function(a){ %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "onIceConnected:function(a){"
                                             "console.log('onIceConnected');"
                                             ""_s);
                values = script.split("onIceConnectionStateChanged:function(a,b){"_s);
                WTFLogAlways("found onIceConnectionStateChanged:function(a,b){ %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "onIceConnectionStateChanged:function(a,b){"
                                             "console.log('onIceConnectionStateChanged');"
                                             ""_s);

                values = script.split("function f(a,b){if(b.type!==\"initiatePeerConnectionRestarting\")"_s);
                WTFLogAlways("found function f(a,b){if(b.type!==\"initiatePeerConnectionRestarting\") %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "function f(a,b){"
                                             "console.log('function f(a,b) initiatePeerConnectionRestarting');"
                                             "if(b.type!==\"initiatePeerConnectionRestarting\")"_s);

                //b.addRemoteTrackFromEvent=function(a){
                values = script.split("b.addRemoteTrackFromEvent=function(a){"_s);
                WTFLogAlways("found b.addRemoteTrackFromEvent=function(a){ %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "b.addRemoteTrackFromEvent=function(a){\n"
                                             "console.log('addRemoteTrackFromEvent1 ' + JSON.stringify(a.track.getCapabilities()));\n"
                                             "console.log('addRemoteTrackFromEvent2 ' + JSON.stringify(Array.from(this.$4)));\n"
                                             "console.log('addRemoteTrackFromEvent3 ' + JSON.stringify(Array.from(this.$9)));\n"
                                             ""_s);


                
                    values = script.split("convertToZenonImmutableMediaTrack:function(a,b,c,d){"_s);
                    WTFLogAlways("found convertToZenonImmutableMediaTrack:function(a,b,c,d){ %d\n", (int)values.size());
                    script = makeStringByJoining(values.span(), "convertToZenonImmutableMediaTrack:function(a,b,c,d){\n"
                                                 "var mytrackid=a.id;\n"
                                                 "var mytrackkind=a.kind;\n"
                                                 "var mytrackc=a.getCapabilities();\n"
                                                 "console.log('convertToZenonImmutableMediaTrack: ' + JSON.stringify({ mytrackid,c,mytrackkind }));\n"
                                                 ""_s);


                values = script.split("b.registerTrackFetcher=function(a){"_s);
                WTFLogAlways("found b.registerTrackFetcher=function(a){ %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "b.registerTrackFetcher=function(a){\n"
                                             " var test = Array.from(a());\n"
                                             //" for (let t of test) t[1].webrtcTrackId = t[1].webrtcTrack?.id ? t[1].webrtcTrack.id : 'none';\n"
                                             "console.log('b.registerTrackFetcher ' + JSON.stringify(test));\n"
                                             ""_s);
#endif
/*
                values = script.split("b.getTracks=function(){"_s);
                WTFLogAlways("found b.getTracks=function(){%d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "b.getTracks=function(){\n"
  //                                           " var test = Array.from(this.$6());\n"
//                                             " var test2 = [];\n"
  //                                           " for (let t of test) test2.push({webrtcTrackId:t[1].webrtcTrackId, trackID:t[1].trackID});\n"
                                             " console.log('b.getTracks ');\n"
                                             ""_s);
*/
                
                values = script.split("var l=d(\"useZenonTrackSelector\").useZenonTrackSelector(e)"_s);
                WTFLogAlways("found var l=d(\"useZenonTrackSelector\").useZenonTrackSelector(e) %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "\n"
                                             "var testBBB = d(\"useZenonTrackSelector\").useZenonTrackSelector(e);\n"
                                             "if (!testBBB) testBBB = { };\n"
                                             "testBBB.webrtcStreamID = testBBB.track?.webrtcStream ? testBBB.track.webrtcStream.id : 'none';\n"
                                             "testBBB.webrtcTrackID = testBBB.track?.webrtcTrack ? testBBB.track.webrtcTrack.id : 'none';\n"
                                             "if (testBBB.track?.webrtcTrack) testBBB.track.webrtcTrack.getCapabilities();\n"
                                             "if (testBBB.track?.webrtcStream) testBBB.track.webrtcStream.getAudioTracks();\n"
                                             "console.log('useZenonTrackSelector for ZenonGridItem ' + JSON.stringify(testBBB));\n"
                                             "var l=d(\"useZenonTrackSelector\").useZenonTrackSelector(e)"_s);

         
                values = script.split("e.$ZenonCallsModelEmitter$p_11=function(a){"_s);
                WTFLogAlways("found b.ZenonCallsModelEmitter11 %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "e.$ZenonCallsModelEmitter$p_11=function(a){\n"
                         " var myVar = {};\n"
                         "myVar.tracks = Array.from(a.getTracks());\n"
                         "myVar.tracks2 = myVar.tracks.map(t => { return { stream:t[1].webrtcStream?.id,track:t[1].webrtcTrack?.id } });\n"
                         " console.log('ZenonCallsModelEmitter$_p11 with ' + JSON.stringify(myVar.tracks2));\n"
                         ""_s);

                values = script.split("e.$ZenonCallsModelEmitter$p_5=function(a,b){"_s);
                WTFLogAlways("found b.ZenonCallsModelEmitter5 %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "e.$ZenonCallsModelEmitter$p_5=function(a,b){\n"
                         " console.log('ZenonCallsModelEmitter$_p5');\n"
                         ""_s);

                values = script.split("var a=b.addListener(\"callsModelUpdate\",function(a){"_s);
                WTFLogAlways("found var a=b.addListener(\"callsModelUpdate\",function(a){ %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "var a=b.addListener(\"callsModelUpdate\",function(a){\n"
                                             "console.log('callsModelUpdate listener ' + JSON.stringify(o));\n"
                                             ""_s);

                values = script.split("return c(\"ZenonCallQueryLive\")(function(c){"_s);
                WTFLogAlways("found return c(\"ZenonCallQueryLive\")(function(c){ %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "return c(\"ZenonCallQueryLive\")(function(c){\n"
                                             "console.log('c(\"ZenonCallQueryLive\")(function(c) ' + JSON.strngify(b));\n"
                                             ""_s);

                values = script.split("b.logEvent=function(a,b){"_s);
                WTFLogAlways("found logEvent=function(a,b) %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "b.logEvent=function(a,b){"
                                             "console.log('b.logEvent with ' + JSON.stringify(a));"
                                             ""_s);
                values = script.split("logEvent=function(a){"_s);
                WTFLogAlways("found logEvent=function(a) %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "logEvent=function(a){"
                                             "console.log('logEvent(a) with ' + JSON.stringify(a));"
                                             ""_s);

                values = script.split("b.mustfix=function(a){"_s);
                WTFLogAlways("found b.mustfix=function(a){ %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "b.mustfix=function(a){\n"
                                             "console.log('b.mustfix with ' + JSON.stringify(a));"
                                             ""_s);

                values = script.split("b.warn=function(a){"_s);
                WTFLogAlways("found b.warn=function(a){ %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "b.warn=function(a){\n"
                                             "console.log('b.warn with ' + JSON.stringify(a));"
                                             ""_s);

                values = script.split("b.warn=function(a){"_s);
                WTFLogAlways("found b.warn=function(a){ %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "b.warn=function(a){\n"
                                             "console.log('b.warn with ' + JSON.stringify(a));"
                                             ""_s);

                values = script.split("b.fatal=function(a){"_s);
                WTFLogAlways("found b.fatal=function(a){ %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "b.fatal=function(a){\n"
                                             "console.log('b.fatal with ' + JSON.stringify(a));"
                                             ""_s);

                values = script.split("b.info=function(a){"_s);
                WTFLogAlways("found b.info=function(a){ %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "b.info=function(a){\n"
                                             "console.log('b.info with ' + JSON.stringify(a));"
                                             ""_s);

                values = script.split("b.logStateMachineTransition=function(a,b,d,e,f,g,h,i){"_s);
                WTFLogAlways("found b.logStateMachineTransition=function(a,b,d,e,f,g,h,i){ %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "b.logStateMachineTransition=function(a,b,d,e,f,g,h,i){\n"
                                             "console.log('b.logStateMachineTransition with ' + JSON.stringify(c) + ' ' + JSON.stringify(d));"
                                             ""_s);

                values = script.split("shouldUseSFUOnly:function(){"_s);
                WTFLogAlways("found shouldUseSFUOnly:function(){ %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "shouldUseSFUOnly:function(){\n"
                                             "const a = 1; if (a) return true;"
                                             ""_s);

//shouldUseSFUOnly:function(){
                
                 /*
                 values = script.split("XYZ"_s);
                 WTFLogAlways("found XYZ %d\n", (int)values.size());
                 script = makeStringByJoining(values.span(), "XYZ\n"
                                              "console.log('XYZ');\n"
                                              ""_s);
                 */
                m_script = WTFMove(script);
                return m_script;
            }
        }
        return { byteCast<Latin1Character>(contiguousData->span()) };
    }

    bool shouldForceRedecoding = m_wasForceDecodedAsUTF8 != (shouldDecodeAsUTF8Only == ShouldDecodeAsUTF8Only::Yes);
    if (!m_script || shouldForceRedecoding) {
        ASSERT(contiguousData->span().size() == encodedSize());
        String result;
        if (shouldDecodeAsUTF8Only == ShouldDecodeAsUTF8Only::Yes) {
            Ref forceUTF8Decoder = TextResourceDecoder::create("text/javascript"_s, PAL::UTF8Encoding());
            forceUTF8Decoder->setAlwaysUseUTF8();
            result = forceUTF8Decoder->decodeAndFlush(contiguousData->span());
        } else
            result = protectedDecoder()->decodeAndFlush(contiguousData->span());


        if (m_decodingState == NeverDecoded || shouldForceRedecoding)
            m_scriptHash = result.hash();
        ASSERT(!m_scriptHash || m_scriptHash == result.hash());

        {
            Locker locker { m_lock };
            m_script = WTFMove(result);
            m_decodingState = DataAndDecodedStringHaveDifferentBytes;
            m_wasForceDecodedAsUTF8 = shouldDecodeAsUTF8Only == ShouldDecodeAsUTF8Only::Yes;
        }
        setDecodedSize(m_script.sizeInBytes());
    }

    restartDecodedDataDeletionTimer();

    if (url().string().contains("3avio"_s))
        WTFLogAlways("CachedScript2 for '%s'", url().string().utf8().data());

    if (m_script.contains("var _frameDecryptor_decrypt"_s)) {
        WTFLogAlways("found script");
        auto index = m_script.find("var _frameDecryptor_decrypt=Module[\"_frameDecryptor_decrypt\"]=createExportWrapper(\"frameDecryptor_decrypt\");"_s);
        if (index != notFound) {
            WTFLogAlways("updating script");
            auto length = sizeof("var _frameDecryptor_decrypt=Module[\"_frameDecryptor_decrypt\"]=createExportWrapper(\"frameDecryptor_decrypt\");");
            m_script = makeString(
              "function createExportWrapperWithLog(name) {"
              "  return function() {"
              "    assert(runtimeInitialized, \"native function `\" + name + \"` called before runtime initialization\");"
              "    var f = wasmExports[name];"
              "    assert(f, \"exported native function `\" + name + \"` not found\");"
              "    console.log(\"createExportWrapperWithLog for \" + name)"
              "    for (let a of arguments) console.log(a);"
              "    return f.apply(null, arguments);"
              "   }"
              ";"
              "}"
              ""_s,
              m_script.substring(0, index),
              "var _frameDecryptor_decrypt=Module[\"_frameDecryptor_decrypt\"]=createExportWrapperWithLog(\"frameDecryptor_decrypt\");"_s,
              m_script.substring(index + length));
        }
    }
    return m_script;
}

JSC::CodeBlockHash CachedScript::codeBlockHashConcurrently(int startOffset, int endOffset, JSC::CodeSpecializationKind kind, ShouldDecodeAsUTF8Only shouldDecodeAsUTF8Only)
{
    Locker locker { m_lock };
    auto data = m_data;
    if (!data)
        return JSC::CodeBlockHash { emptyString(), emptyString(), kind };

    switch (m_decodingState) {
    case NeverDecoded: {
        // This is rare, but unfortunately, when running CodeBlockHash concurrently, CachedScript was not decoding the source code.
        // Thus, we need to decode them and need to compute. This is costly, but fine as CodeBlockHash is only used for debugging.
        if (!data->isContiguous())
            data = data->makeContiguous();
        Ref contiguousData = downcast<SharedBuffer>(*data);

        if (PAL::TextEncoding(encoding()).isByteBasedEncoding() && contiguousData->size() && charactersAreAllASCII(contiguousData->span())) {
            StringView entireSource { byteCast<Latin1Character>(contiguousData->span()) };
            return JSC::CodeBlockHash { entireSource.substring(startOffset, endOffset - startOffset), entireSource, kind };
        }

        String result;
        if (shouldDecodeAsUTF8Only == ShouldDecodeAsUTF8Only::Yes) {
            Ref forceUTF8Decoder = TextResourceDecoder::create("text/javascript"_s, PAL::UTF8Encoding());
            forceUTF8Decoder->setAlwaysUseUTF8();
            result = forceUTF8Decoder->decodeAndFlush(contiguousData->span());
        } else {
            auto decoder = TextResourceDecoder::create(protectedDecoder()->contentType(), protectedDecoder()->encoding(), protectedDecoder()->usesEncodingDetector());
            result = decoder->decodeAndFlush(contiguousData->span());
        }

        StringView entireSource { result };
        return JSC::CodeBlockHash { entireSource.substring(startOffset, endOffset - startOffset), entireSource, kind };
    }
    case DataAndDecodedStringHaveSameBytes: {
        StringView entireSource { byteCast<Latin1Character>(downcast<SharedBuffer>(*data).span()) };
        return JSC::CodeBlockHash { entireSource.substring(startOffset, endOffset - startOffset), entireSource, kind };
    }

    case DataAndDecodedStringHaveDifferentBytes: {
        StringView entireSource { m_script };
        return JSC::CodeBlockHash { entireSource.substring(startOffset, endOffset - startOffset), entireSource, kind };
    }
    }
    return { };
}

unsigned CachedScript::scriptHash(ShouldDecodeAsUTF8Only shouldDecodeAsUTF8Only)
{
    if (m_decodingState == NeverDecoded || (m_decodingState == DataAndDecodedStringHaveDifferentBytes && m_wasForceDecodedAsUTF8 != (shouldDecodeAsUTF8Only == ShouldDecodeAsUTF8Only::Yes)))
        script(shouldDecodeAsUTF8Only);
    return m_scriptHash;
}

void CachedScript::finishLoading(const FragmentedSharedBuffer* data, const NetworkLoadMetrics& metrics)
{
    if (data) {
        m_data = data->makeContiguous();
        setEncodedSize(data->size());
    } else {
        m_data = nullptr;
        setEncodedSize(0);
    }
    CachedResource::finishLoading(data, metrics);
}

void CachedScript::destroyDecodedData()
{
    {
        Locker locker { m_lock };
        m_script = String();
    }
    setDecodedSize(0);
}

void CachedScript::setBodyDataFrom(const CachedResource& resource)
{
    ASSERT(resource.type() == type());
    auto& script = downcast<const CachedScript>(resource);

    CachedResource::setBodyDataFrom(resource);

    {
        Locker locker { m_lock };
        m_script = script.m_script;
        m_scriptHash = script.m_scriptHash;
        m_wasForceDecodedAsUTF8 = script.m_wasForceDecodedAsUTF8;
        m_decodingState = script.m_decodingState;
        m_decoder = script.m_decoder;
    }
}

} // namespace WebCore
