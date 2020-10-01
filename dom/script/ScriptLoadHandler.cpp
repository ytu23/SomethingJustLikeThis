/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ScriptLoadHandler.h"
#include "ScriptLoader.h"
#include "ScriptTrace.h"

#include "nsContentUtils.h"
#include "nsICacheInfoChannel.h"
#include "nsMimeTypes.h"

#include "mozilla/dom/ScriptDecoding.h"  // mozilla::dom::ScriptDecoding
#include "mozilla/Telemetry.h"
#include "mozilla/StaticPrefs_dom.h"

namespace mozilla {
namespace dom {

#undef LOG
#define LOG(args) \
  MOZ_LOG(ScriptLoader::gScriptLoaderLog, mozilla::LogLevel::Debug, args)

#define LOG_ENABLED() \
  MOZ_LOG_TEST(ScriptLoader::gScriptLoaderLog, mozilla::LogLevel::Debug)

ScriptLoadHandler::ScriptLoadHandler(ScriptLoader* aScriptLoader,
                                     ScriptLoadRequest* aRequest,
                                     SRICheckDataVerifier* aSRIDataVerifier)
    : mScriptLoader(aScriptLoader),
      mRequest(aRequest),
      mSRIDataVerifier(aSRIDataVerifier),
      mSRIStatus(NS_OK),
      mDecoder() {
  MOZ_ASSERT(mRequest->IsUnknownDataType());
  MOZ_ASSERT(mRequest->IsLoading());
}

ScriptLoadHandler::~ScriptLoadHandler() {}

NS_IMPL_ISUPPORTS(ScriptLoadHandler, nsIIncrementalStreamLoaderObserver)

template <typename Unit>
nsresult ScriptLoadHandler::DecodeRawDataHelper(const uint8_t* aData,
                                                uint32_t aDataLength,
                                                bool aEndOfStream) {
  CheckedInt<size_t> needed =
      ScriptDecoding<Unit>::MaxBufferLength(mDecoder, aDataLength);
  if (!needed.isValid()) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  // Reference to the script source buffer which we will update.
  ScriptLoadRequest::ScriptTextBuffer<Unit>& scriptText =
      mRequest->ScriptText<Unit>();

  uint32_t haveRead = scriptText.length();

  CheckedInt<uint32_t> capacity = haveRead;
  capacity += needed.value();

  if (!capacity.isValid() || !scriptText.resize(capacity.value())) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  size_t written = ScriptDecoding<Unit>::DecodeInto(
      mDecoder, MakeSpan(aData, aDataLength),
      MakeSpan(scriptText.begin() + haveRead, needed.value()), aEndOfStream);
  MOZ_ASSERT(written <= needed.value());

  // Telemetry: Measure the throughput at which bytes are streamed and the ratio
  // of streamed bytes, such that we can determine the effectiveness of a
  // streaming parser for JavaScript.
  using namespace mozilla::Telemetry;
  if (aEndOfStream && haveRead) {
    // Compute the percent of data transfered incrementally.
    Accumulate(DOM_SCRIPT_LOAD_INCREMENTAL_RATIO,
               100 * haveRead / (haveRead + written));
    // Compute the rate of transfer of the incremental data calls averaged
    // across the time needed to complete the request.
    auto streamingTime = TimeStamp::Now() - mFirstOnIncrementalData;
    double ms = streamingTime.ToMilliseconds();
    Accumulate(DOM_SCRIPT_LOAD_INCREMENTAL_AVG_TRANSFER_RATE, haveRead / ms);
    mRequest->mStreamingTime = streamingTime;
  }
  if (!aEndOfStream && !haveRead) {
    mFirstOnIncrementalData = TimeStamp::Now();
  }

  haveRead += written;
  MOZ_ASSERT(haveRead <= capacity.value(),
             "mDecoder produced more data than expected");
  MOZ_ALWAYS_TRUE(scriptText.resize(haveRead));
  mRequest->mScriptTextLength = scriptText.length();

  return NS_OK;
}

nsresult ScriptLoadHandler::DecodeRawData(const uint8_t* aData,
                                          uint32_t aDataLength,
                                          bool aEndOfStream) {
  if (mRequest->IsUTF16Text()) {
    return DecodeRawDataHelper<char16_t>(aData, aDataLength, aEndOfStream);
  }

  return DecodeRawDataHelper<Utf8Unit>(aData, aDataLength, aEndOfStream);
}

NS_IMETHODIMP
ScriptLoadHandler::OnIncrementalData(nsIIncrementalStreamLoader* aLoader,
                                     nsISupports* aContext,
                                     uint32_t aDataLength, const uint8_t* aData,
                                     uint32_t* aConsumedLength) {
  if (mRequest->IsCanceled()) {
    // If request cancelled, ignore any incoming data.
    *aConsumedLength = aDataLength;
    return NS_OK;
  }

  nsresult rv = NS_OK;
  if (mRequest->IsUnknownDataType()) {
    rv = EnsureKnownDataType(aLoader);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  if (mRequest->IsTextSource()) {
    if (!EnsureDecoder(aLoader, aData, aDataLength,
                       /* aEndOfStream = */ false)) {
      return NS_OK;
    }

    // Below we will/shall consume entire data chunk.
    *aConsumedLength = aDataLength;

    // Decoder has already been initialized. -- trying to decode all loaded
    // bytes.
    rv = DecodeRawData(aData, aDataLength, /* aEndOfStream = */ false);
    NS_ENSURE_SUCCESS(rv, rv);

    // If SRI is required for this load, appending new bytes to the hash.
    if (mSRIDataVerifier && NS_SUCCEEDED(mSRIStatus)) {
      mSRIStatus = mSRIDataVerifier->Update(aDataLength, aData);
    }
  } else if (mRequest->IsBinASTSource()) {
    if (!mRequest->ScriptBinASTData().append(aData, aDataLength)) {
      return NS_ERROR_OUT_OF_MEMORY;
    }

    // Below we will/shall consume entire data chunk.
    *aConsumedLength = aDataLength;

    // If SRI is required for this load, appending new bytes to the hash.
    if (mSRIDataVerifier && NS_SUCCEEDED(mSRIStatus)) {
      mSRIStatus = mSRIDataVerifier->Update(aDataLength, aData);
    }
  } else {
    MOZ_ASSERT(mRequest->IsBytecode());
    if (!mRequest->mScriptBytecode.append(aData, aDataLength)) {
      return NS_ERROR_OUT_OF_MEMORY;
    }

    *aConsumedLength = aDataLength;
    rv = MaybeDecodeSRI();
    if (NS_FAILED(rv)) {
      nsCOMPtr<nsIRequest> channelRequest;
      aLoader->GetRequest(getter_AddRefs(channelRequest));
      return channelRequest->Cancel(mScriptLoader->RestartLoad(mRequest));
    }
  }

  return rv;
}

bool ScriptLoadHandler::TrySetDecoder(nsIIncrementalStreamLoader* aLoader,
                                      const uint8_t* aData,
                                      uint32_t aDataLength, bool aEndOfStream) {
  MOZ_ASSERT(mDecoder == nullptr,
             "can't have a decoder already if we're trying to set one");

  // JavaScript modules are always UTF-8.
  if (mRequest->IsModuleRequest()) {
    mDecoder = UTF_8_ENCODING->NewDecoderWithBOMRemoval();
    return true;
  }

  // Determine if BOM check should be done.  This occurs either
  // if end-of-stream has been reached, or at least 3 bytes have
  // been read from input.
  if (!aEndOfStream && (aDataLength < 3)) {
    return false;
  }

  // Do BOM detection.
  const Encoding* encoding;
  size_t bomLength;
  Tie(encoding, bomLength) = Encoding::ForBOM(MakeSpan(aData, aDataLength));
  if (encoding) {
    mDecoder = encoding->NewDecoderWithBOMRemoval();
    return true;
  }

  // BOM detection failed, check content stream for charset.
  nsCOMPtr<nsIRequest> req;
  nsresult rv = aLoader->GetRequest(getter_AddRefs(req));
  NS_ASSERTION(req, "StreamLoader's request went away prematurely");
  NS_ENSURE_SUCCESS(rv, false);

  nsCOMPtr<nsIChannel> channel = do_QueryInterface(req);

  if (channel) {
    nsAutoCString label;
    if (NS_SUCCEEDED(channel->GetContentCharset(label)) &&
        (encoding = Encoding::ForLabel(label))) {
      mDecoder = encoding->NewDecoderWithoutBOMHandling();
      return true;
    }
  }

  // Check the hint charset from the script element or preload
  // request.
  nsAutoString hintCharset;
  if (!mRequest->IsPreload()) {
    mRequest->Element()->GetScriptCharset(hintCharset);
  } else {
    nsTArray<ScriptLoader::PreloadInfo>::index_type i =
        mScriptLoader->mPreloads.IndexOf(
            mRequest, 0, ScriptLoader::PreloadRequestComparator());

    NS_ASSERTION(i != mScriptLoader->mPreloads.NoIndex,
                 "Incorrect preload bookkeeping");
    hintCharset = mScriptLoader->mPreloads[i].mCharset;
  }

  if ((encoding = Encoding::ForLabel(hintCharset))) {
    mDecoder = encoding->NewDecoderWithoutBOMHandling();
    return true;
  }

  // Get the charset from the charset of the document.
  if (mScriptLoader->mDocument) {
    encoding = mScriptLoader->mDocument->GetDocumentCharacterSet();
    mDecoder = encoding->NewDecoderWithoutBOMHandling();
    return true;
  }

  // Curiously, there are various callers that don't pass aDocument. The
  // fallback in the old code was ISO-8859-1, which behaved like
  // windows-1252.
  mDecoder = WINDOWS_1252_ENCODING->NewDecoderWithoutBOMHandling();
  return true;
}

nsresult ScriptLoadHandler::MaybeDecodeSRI() {
  if (!mSRIDataVerifier || mSRIDataVerifier->IsComplete() ||
      NS_FAILED(mSRIStatus)) {
    return NS_OK;
  }

  // Skip until the content is large enough to be decoded.
  if (mRequest->mScriptBytecode.length() <=
      mSRIDataVerifier->DataSummaryLength()) {
    return NS_OK;
  }

  mSRIStatus = mSRIDataVerifier->ImportDataSummary(
      mRequest->mScriptBytecode.length(), mRequest->mScriptBytecode.begin());

  if (NS_FAILED(mSRIStatus)) {
    // We are unable to decode the hash contained in the alternate data which
    // contains the bytecode, or it does not use the same algorithm.
    LOG(
        ("ScriptLoadHandler::MaybeDecodeSRI, failed to decode SRI, restart "
         "request"));
    return mSRIStatus;
  }

  mRequest->mBytecodeOffset = mSRIDataVerifier->DataSummaryLength();
  return NS_OK;
}

nsresult ScriptLoadHandler::EnsureKnownDataType(
    nsIIncrementalStreamLoader* aLoader) {
  MOZ_ASSERT(mRequest->IsUnknownDataType());
  MOZ_ASSERT(mRequest->IsLoading());

  nsCOMPtr<nsIRequest> req;
  nsresult rv = aLoader->GetRequest(getter_AddRefs(req));
  MOZ_ASSERT(req, "StreamLoader's request went away prematurely");
  NS_ENSURE_SUCCESS(rv, rv);

  if (mRequest->IsLoadingSource()) {
    mRequest->SetTextSource();
    TRACE_FOR_TEST(mRequest->Element(), "scriptloader_load_source");
    return NS_OK;
  }

  nsCOMPtr<nsICacheInfoChannel> cic(do_QueryInterface(req));
  if (cic) {
    nsAutoCString altDataType;
    cic->GetAlternativeDataType(altDataType);
    if (altDataType.Equals(nsContentUtils::JSBytecodeMimeType())) {
      mRequest->SetBytecode();
      TRACE_FOR_TEST(mRequest->Element(), "scriptloader_load_bytecode");
      return NS_OK;
    }
    MOZ_ASSERT(altDataType.IsEmpty());
  }

  if (nsJSUtils::BinASTEncodingEnabled()) {
    nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(req);
    if (httpChannel) {
      nsAutoCString mimeType;
      httpChannel->GetContentType(mimeType);
      if (mimeType.LowerCaseEqualsASCII(APPLICATION_JAVASCRIPT_BINAST)) {
        if (mRequest->ShouldAcceptBinASTEncoding()) {
          mRequest->SetBinASTSource();
          TRACE_FOR_TEST(mRequest->Element(), "scriptloader_load_source");
          return NS_OK;
        }

        // If the request isn't allowed to accept BinAST, fallback to text
        // source.  The possibly binary source will be passed to normal
        // JS parser and will throw error there.
        mRequest->SetTextSource();
        return NS_OK;
      }
    }
  }

  mRequest->SetTextSource();
  TRACE_FOR_TEST(mRequest->Element(), "scriptloader_load_source");

  MOZ_ASSERT(!mRequest->IsUnknownDataType());
  MOZ_ASSERT(mRequest->IsLoading());
  return NS_OK;
}

NS_IMETHODIMP
ScriptLoadHandler::OnStreamComplete(nsIIncrementalStreamLoader* aLoader,
                                    nsISupports* aContext, nsresult aStatus,
                                    uint32_t aDataLength,
                                    const uint8_t* aData) {
  nsresult rv = NS_OK;
  if (LOG_ENABLED()) {
    nsAutoCString url;
    mRequest->mURI->GetAsciiSpec(url);
    LOG(("ScriptLoadRequest (%p): Stream complete (url = %s)", mRequest.get(),
         url.get()));
  }

  nsCOMPtr<nsIRequest> channelRequest;
  aLoader->GetRequest(getter_AddRefs(channelRequest));

  if (!mRequest->IsCanceled()) {
    if (mRequest->IsUnknownDataType()) {
      rv = EnsureKnownDataType(aLoader);
      NS_ENSURE_SUCCESS(rv, rv);
    }

    if (mRequest->IsTextSource()) {
      DebugOnly<bool> encoderSet =
          EnsureDecoder(aLoader, aData, aDataLength, /* aEndOfStream = */ true);
      MOZ_ASSERT(encoderSet);
      rv = DecodeRawData(aData, aDataLength, /* aEndOfStream = */ true);
      NS_ENSURE_SUCCESS(rv, rv);

      LOG(("ScriptLoadRequest (%p): Source length in code units = %u",
           mRequest.get(), unsigned(mRequest->ScriptTextLength())));

      // If SRI is required for this load, appending new bytes to the hash.
      if (mSRIDataVerifier && NS_SUCCEEDED(mSRIStatus)) {
        mSRIStatus = mSRIDataVerifier->Update(aDataLength, aData);
      }
    } else if (mRequest->IsBinASTSource()) {
      if (!mRequest->ScriptBinASTData().append(aData, aDataLength)) {
        return NS_ERROR_OUT_OF_MEMORY;
      }

      // If SRI is required for this load, appending new bytes to the hash.
      if (mSRIDataVerifier && NS_SUCCEEDED(mSRIStatus)) {
        mSRIStatus = mSRIDataVerifier->Update(aDataLength, aData);
      }
    } else {
      MOZ_ASSERT(mRequest->IsBytecode());
      if (!mRequest->mScriptBytecode.append(aData, aDataLength)) {
        return NS_ERROR_OUT_OF_MEMORY;
      }

      LOG(("ScriptLoadRequest (%p): Bytecode length = %u", mRequest.get(),
           unsigned(mRequest->mScriptBytecode.length())));

      // If we abort while decoding the SRI, we fallback on explictly requesting
      // the source. Thus, we should not continue in
      // ScriptLoader::OnStreamComplete, which removes the request from the
      // waiting lists.
      rv = MaybeDecodeSRI();
      if (NS_FAILED(rv)) {
        return channelRequest->Cancel(mScriptLoader->RestartLoad(mRequest));
      }

      // The bytecode cache always starts with the SRI hash, thus even if there
      // is no SRI data verifier instance, we still want to skip the hash.
      rv = SRICheckDataVerifier::DataSummaryLength(
          mRequest->mScriptBytecode.length(), mRequest->mScriptBytecode.begin(),
          &mRequest->mBytecodeOffset);
      if (NS_FAILED(rv)) {
        return channelRequest->Cancel(mScriptLoader->RestartLoad(mRequest));
      }
    }
  }

  // Everything went well, keep the CacheInfoChannel alive such that we can
  // later save the bytecode on the cache entry.
  if (NS_SUCCEEDED(rv) && mRequest->IsSource() &&
      StaticPrefs::dom_script_loader_bytecode_cache_enabled()) {
    mRequest->mCacheInfo = do_QueryInterface(channelRequest);
    LOG(("ScriptLoadRequest (%p): nsICacheInfoChannel = %p", mRequest.get(),
         mRequest->mCacheInfo.get()));
  }

  // we have to mediate and use mRequest.
  rv = mScriptLoader->OnStreamComplete(aLoader, mRequest, aStatus, mSRIStatus,
                                       mSRIDataVerifier);

  // In case of failure, clear the mCacheInfoChannel to avoid keeping it alive.
  if (NS_FAILED(rv)) {
    mRequest->mCacheInfo = nullptr;
  }

  return rv;
}

#undef LOG_ENABLED
#undef LOG

}  // namespace dom
}  // namespace mozilla
