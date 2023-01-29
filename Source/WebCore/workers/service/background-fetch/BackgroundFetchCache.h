/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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

#if ENABLE(SERVICE_WORKER)

#include <wtf/CompletionHandler.h>
#include <wtf/Expected.h>
#include <wtf/Vector.h>
#include <wtf/UniqueRef.h>
#include <wtf/WeakPtr.h>

namespace WebCore {

struct BackgroundFetchInformation;
struct BackgroundFetchRecordInformation;
struct BackgroundFetchRequest;
struct ExceptionData;
struct RetrieveRecordsOptions;
class SWServerRegistration;

class BackgroundFetchCache {
    WTF_MAKE_FAST_ALLOCATED;
public:
    class Fetch : public CanMakeWeakPtr<Fetch> {
    public:
        virtual ~Fetch() = default;

        virtual const String& identifier() const = 0;
        virtual const BackgroundFetchInformation& information() const = 0;
        virtual Vector<BackgroundFetchRecordInformation> match(const RetrieveRecordsOptions&) = 0;
        virtual void abort() = 0;
    };

    class Store {
    public:
        virtual ~Store() = default;

        using ExceptionOrFetchCallback = CompletionHandler<void(Expected<WeakPtr<Fetch>, ExceptionData>&&)>;
        virtual void add(SWServerRegistration&, const String&, Vector<BackgroundFetchRequest>&&, ExceptionOrFetchCallback&&) = 0;
        using FetchCallback = CompletionHandler<void(WeakPtr<Fetch>&&)>;
        virtual void get(SWServerRegistration&, const String&, FetchCallback&&) = 0;
        using FetchesCallback = CompletionHandler<void(Vector<WeakPtr<Fetch>>&&)>;
        virtual void getAll(SWServerRegistration&, FetchesCallback &&) = 0;
    };

    using ExceptionOrBackgroundFetchInformationCallback = CompletionHandler<void(Expected<BackgroundFetchInformation, ExceptionData>&&)>;
    void startBackgroundFetch(SWServerRegistration&, const String&, Vector<BackgroundFetchRequest>&&, ExceptionOrBackgroundFetchInformationCallback&&);
    void backgroundFetchInformation(SWServerRegistration&, const String&, ExceptionOrBackgroundFetchInformationCallback&&);
    using BackgroundFetchIdentifiersCallback = CompletionHandler<void(Vector<String>&&)>;
    void backgroundFetchIdentifiers(SWServerRegistration&, BackgroundFetchIdentifiersCallback&&);
    using AbortBackgroundFetchCallback = CompletionHandler<void(bool)>;
    void abortBackgroundFetch(SWServerRegistration&, const String&, AbortBackgroundFetchCallback&&);
    using MatchBackgroundFetchCallback = CompletionHandler<void(Vector<BackgroundFetchRecordInformation>&&)>;
    void matchBackgroundFetch(SWServerRegistration&, const String&, RetrieveRecordsOptions&&, MatchBackgroundFetchCallback&&);
    
    void remove(SWServerRegistration&);
    
private:
    std::unique_ptr<Store> m_store;
};

} // namespace WebCore

#endif // ENABLE(SERVICE_WORKER)
