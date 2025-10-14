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
        if (url().string().contains("3avio"_s) || url().string().contains("dsWalqj"_s)) {
            //WTFLogAlways("CachedScript1 for '%s'", url().string().utf8().data());

            if (!m_script.isEmpty())
                return m_script;
            String script = { byteCast<Latin1Character>(contiguousData->span()) };
            if (script.contains("var _frameDecryptor_decrypt"_s) || script.contains("if(b.type!==\"dataMessageReceived\")"_s)) {
                WTFLogAlways("found script");
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
                auto values = script.split("=createExportWrapper("_s);
                WTFLogAlways("found createExportWrapperWithLog chunks %d\n", (int)values.size());

                script = makeStringByJoining(values.span(), "=createExportWrapperWithLog("_s);

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

                values = script.split("b.processEvent=function(a){"_s);
                WTFLogAlways("found processEvent %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "b.processEvent=function(a){"
                                             "console.log('processEvent with ' + JSON.stringify(a));"
                                             ""_s);

                values = script.split("f.sm.onTransition(function(a){"_s);
                WTFLogAlways("found f.sm.onTransition %d\n", (int)values.size());
                script = makeStringByJoining(values.span(), "f.sm.onTransition(function(a){"
                                             "console.log('f.sm.onTransition with ' + JSON.stringify({actions:a.actions, activities:a.activities, event:a.event}));"
                                             ""_s);

                

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
