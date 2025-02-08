#include "thorzeel/thorzeel_throttle.h"

#include <cstring>
#include <sstream>

#include "base/check_op.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_runner.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_status_code.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "thorzeel/partial_content_url_loader.h"
#include "thorzeel/range_store.h"
#include "thorzeel/success_url_loader.h"

namespace thorzeel {

namespace {

constexpr char kThrottleName[] = "ThorzeelThrottle";
constexpr char kRangeAddedHeaderName[] = "X-Thorzeel-Range-Added";
constexpr char kRangeAddedHeaderValue[] = "true";
constexpr char kMimeTypeMultipartByteRanges[] = "multipart/byteranges";
constexpr char kContentTypeJavaScript[] = "text/javascript";
constexpr char kContentTypeMultipartPrefix[] =
    "multipart/byteranges; boundary=";
constexpr char kResponseStatusLineSuccess[] = "HTTP/1.1 200 OK";
constexpr char kContentRangeHeader[] = "Content-Range";

}  // namespace

// static
std::unique_ptr<ThorzeelThrottle> ThorzeelThrottle::MaybeCreate(
    const network::ResourceRequest& request,
    scoped_refptr<base::SequencedTaskRunner> task_runner) {
  RangeStore* range_store = RangeStore::Get();
  if (range_store == nullptr || !range_store->IsReady()) {
    // LOG(INFO) << "MaybeCreate: url [" << request.url.spec()
    //           << "], No RangeStore, is null [" << (range_store == nullptr)
    //           << "], is ready ["
    //           << (range_store != nullptr && range_store->IsReady()) << "]";
    return nullptr;
  }

  return base::WrapUnique(new ThorzeelThrottle(std::move(task_runner)));
}

ThorzeelThrottle::ThorzeelThrottle(
    scoped_refptr<base::SequencedTaskRunner> task_runner)
    : task_runner_(std::move(task_runner)) {}

ThorzeelThrottle::~ThorzeelThrottle() = default;

void ThorzeelThrottle::DetachFromCurrentSequence() {
  // This should only happen when the throttle loader runs on its own sequenced
  // task runner (so getting the current sequenced runner later should be
  // fine).
  task_runner_ = nullptr;
}

const char* ThorzeelThrottle::NameForLoggingWillStartRequest() {
  return kThrottleName;
}

void ThorzeelThrottle::WillStartRequest(network::ResourceRequest* request,
                                        bool* defer) {
  if (request->method != net::HttpRequestHeaders::kGetMethod ||
      !request->url.SchemeIsHTTPOrHTTPS()) {
    // LOG(INFO) << "WillStartRequest: url [" << request->url.spec()
    //           << "], not GET or https, request method [" << request->method
    //           << "]";
    return;
  }
  std::string requesting_host = GetRequestingHost(request);
  if (requesting_host.empty()) {
    // LOG(INFO) << "WillStartRequest: url [" << request->url.spec()
    //           << "], could not get requesting host";
    return;
  }
  // LOG(INFO) << "WillStartRequest: url [" << request->url.spec()
  //           << "], requesting host [" << requesting_host << "]";
  // {
  //   std::string headers = request->headers.ToString();
  //   auto IsWhitespace = [](char c) {
  //     return c == '\r' || c == '\n' || c == '\t';
  //   };
  //   std::replace_if(headers.begin(), headers.end(), IsWhitespace, ' ');
  //   LOG(INFO) << "WillStartRequest: requesting host [" << requesting_host
  //             << "], headers [" << headers << "]";
  // }
  for (const auto& header : request->headers.GetHeaderVector()) {
    if (header.key == kRangeAddedHeaderName) {
      // A previous instance has added the Range header already.
      // LOG(INFO) << "WillStartRequest: url [" << request->url.spec()
      //           << "], Range header added by us already";
      did_add_ranges_ = true;
      return;
    } else if (base::EqualsCaseInsensitiveASCII(
                   header.key, net::HttpRequestHeaders::kContentType)) {
      if (!base::StartsWith(header.value, kContentTypeJavaScript,
                            base::CompareCase::INSENSITIVE_ASCII)) {
        // Content-Type header is not for JavaScript.
        // LOG(INFO) << "WillStartRequest: url [" << request->url.spec()
        //           << "], ContentType [" << header.value
        //           << "] not for javascript";
        return;
      }
    } else if (base::EqualsCaseInsensitiveASCII(
                   header.key, net::HttpRequestHeaders::kRange)) {
      // Range header already exists and was not added by this throttle or a
      // previous instance of this throttle.
      // LOG(INFO) << "WillStartRequest: url [" << request->url.spec()
      //           << "], Range header added by something else";
      return;
    }
  }

  auto url_data = RangeStore::Get()->GetData(requesting_host, request->url);
  if (!url_data) {
    // LOG(INFO) << "WillStartRequest: url [" << request->url.spec()
    //           << "], range data not found";
    return;
  }
  std::tie(range_header_, final_content_length_) = url_data.value();

  // Local Mode: Don't add headers
  // request->headers.SetHeader(net::HttpRequestHeaders::kRange, range_header_);
  // request->headers.SetHeader(kRangeAddedHeaderName, kRangeAddedHeaderValue);

  did_add_ranges_ = true;
  // LOG(INFO) << "WillStartRequest: url [" << request->url.spec()
  //           << "], Added Range header [" << range_header_ << "] and ["
  //           << kRangeAddedHeaderName << "] header [" <<
  //           kRangeAddedHeaderValue
  //           << "]";
}

const char* ThorzeelThrottle::NameForLoggingWillProcessResponse() {
  return kThrottleName;
}

void ThorzeelThrottle::WillProcessResponse(
    const GURL& response_url,
    network::mojom::URLResponseHead* response_head,
    bool* defer) {
  if (!did_add_ranges_ || !response_head->has_range_requested) {
    // LOG(INFO) << "WillProcessResponse: url [" << response_url.spec()
    //           << "], response code [" <<
    //           response_head->headers->response_code()
    //           << "], did add ranges [" << did_add_ranges_
    //           << "], has range requested ["
    //           << response_head->has_range_requested << "]";
    return;
  }

  int response_code = response_head->headers->response_code();
  if (response_code == net::HTTP_PARTIAL_CONTENT) {
    if (!base::StartsWith(response_head->mime_type,
                          kMimeTypeMultipartByteRanges)) {
      // LOG(INFO) << "WillProcessResponse: url [" << response_url.spec()
      //           << "], partial content, mime type [" <<
      //           response_head->mime_type
      //           << "] is not multipart byte ranges";
      return;
    }
    // LOG(INFO) << "WillProcessResponse: url [" << response_url.spec()
    //           << "], intercepting partial content";
    response_head->headers->ReplaceStatusLine(kResponseStatusLineSuccess);
    response_head->headers->RemoveHeader(kContentRangeHeader);
    UpdateContentLength(response_head);
    if (!InterceptResponseWithPartialContentLoader(response_head, defer,
                                                   response_url.spec())) {
      return;
    }
  } else if (response_code == net::HTTP_OK) {
    // LOG(INFO) << "WillProcessResponse: url [" << response_url.spec()
    //           << "], intercepting success";
    UpdateContentLength(response_head);
    if (!InterceptResponseWithSuccessLoader(response_head, defer,
                                            response_url.spec())) {
      return;
    }
  } else {
    // Otherwise, do nothing.
    // LOG(INFO) << "WillProcessResponse: url [" << response_url.spec()
    //           << "], response code [" <<
    //           response_head->headers->response_code()
    //           << "], not intercepting";
  }
}

void ThorzeelThrottle::UpdateContentLength(
    network::mojom::URLResponseHead* response_head) {
  std::stringstream stringstream;
  stringstream << final_content_length_;
  response_head->headers->SetHeader(net::HttpRequestHeaders::kContentLength,
                                    stringstream.view());
}

bool ThorzeelThrottle::InterceptResponseWithPartialContentLoader(
    network::mojom::URLResponseHead* response_head,
    bool* defer,
    const std::string& url) {
  std::string boundary = GetMultipartBoundary(response_head->headers);
  if (boundary.empty()) {
    return false;
  }
  // LOG(INFO) << "InterceptResponseWithPartialContentLoader: Boundary ["
  //           << boundary << "]";

  mojo::ScopedDataPipeConsumerHandle body;
  mojo::ScopedDataPipeProducerHandle producer_handle;
  MojoResult create_pipe_result =
      mojo::CreateDataPipe(nullptr, producer_handle, body);
  if (create_pipe_result != MOJO_RESULT_OK) {
    // Synchronous call of `delegate_->CancelWithError` can cause a UAF error.
    // So defer the request here.
    *defer = true;
    task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(base::BindOnce(
            &ThorzeelThrottle::DeferredCancelWithError,
            weak_factory_.GetWeakPtr(), net::ERR_INSUFFICIENT_RESOURCES)));
    return false;
  }

  mojo::PendingRemote<network::mojom::URLLoader> new_remote;
  mojo::PendingRemote<network::mojom::URLLoaderClient> url_loader_client;
  mojo::PendingReceiver<network::mojom::URLLoaderClient> new_receiver =
      url_loader_client.InitWithNewPipeAndPassReceiver();
  mojo::PendingRemote<network::mojom::URLLoader> source_loader;
  mojo::PendingReceiver<network::mojom::URLLoaderClient> source_client_receiver;

  auto loader = std::make_unique<PartialContentURLLoader>(
      std::move(url_loader_client), boundary, url);

  PartialContentURLLoader* loader_rawptr = loader.get();
  // `loader` will be deleted when `new_remote` is disconnected.
  // `new_remote` is binded to ThrottlingURLLoader::url_loader_. So when
  // ThrottlingURLLoader is deleted, `loader` will be deleted.
  mojo::MakeSelfOwnedReceiver(std::move(loader),
                              new_remote.InitWithNewPipeAndPassReceiver());
  delegate_->InterceptResponse(std::move(new_remote), std::move(new_receiver),
                               &source_loader, &source_client_receiver, &body);

  // ThorzeelURLLoader always sends a valid DataPipeConsumerHandle. So
  // InterceptResponse() must return a valid `body`.
  DCHECK(body);
  loader_rawptr->Start(std::move(source_loader),
                       std::move(source_client_receiver), std::move(body),
                       std::move(producer_handle));
  return true;
}

bool ThorzeelThrottle::InterceptResponseWithSuccessLoader(
    network::mojom::URLResponseHead* response_head,
    bool* defer,
    const std::string& url) {
  mojo::ScopedDataPipeConsumerHandle body;
  mojo::ScopedDataPipeProducerHandle producer_handle;
  MojoResult create_pipe_result =
      mojo::CreateDataPipe(nullptr, producer_handle, body);
  if (create_pipe_result != MOJO_RESULT_OK) {
    // Synchronous call of `delegate_->CancelWithError` can cause a UAF error.
    // So defer the request here.
    *defer = true;
    task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(base::BindOnce(
            &ThorzeelThrottle::DeferredCancelWithError,
            weak_factory_.GetWeakPtr(), net::ERR_INSUFFICIENT_RESOURCES)));
    return false;
  }

  mojo::PendingRemote<network::mojom::URLLoader> new_remote;
  mojo::PendingRemote<network::mojom::URLLoaderClient> url_loader_client;
  mojo::PendingReceiver<network::mojom::URLLoaderClient> new_receiver =
      url_loader_client.InitWithNewPipeAndPassReceiver();
  mojo::PendingRemote<network::mojom::URLLoader> source_loader;
  mojo::PendingReceiver<network::mojom::URLLoaderClient> source_client_receiver;

  // LOG(INFO) << "InterceptResponseWithSuccessLoader: Creating loader";
  auto loader = SuccessURLLoader::Create(std::move(url_loader_client),
                                         range_header_, url);
  if (!loader) {
    return false;
  }
  SuccessURLLoader* loader_rawptr = loader.get();
  // `loader` will be deleted when `new_remote` is disconnected.
  // `new_remote` is binded to ThrottlingURLLoader::url_loader_. So when
  // ThrottlingURLLoader is deleted, `loader` will be deleted.
  mojo::MakeSelfOwnedReceiver(std::move(loader),
                              new_remote.InitWithNewPipeAndPassReceiver());
  delegate_->InterceptResponse(std::move(new_remote), std::move(new_receiver),
                               &source_loader, &source_client_receiver, &body);

  // ThorzeelURLLoader always sends a valid DataPipeConsumerHandle. So
  // InterceptResponse() must return a valid `body`.
  DCHECK(body);
  loader_rawptr->Start(std::move(source_loader),
                       std::move(source_client_receiver), std::move(body),
                       std::move(producer_handle));
  return true;
}

void ThorzeelThrottle::DeferredCancelWithError(int error_code) {
  if (delegate_) {
    delegate_->CancelWithError(error_code, kThrottleName);
  }
}

std::string ThorzeelThrottle::GetRequestingHost(
    network::ResourceRequest* request) {
  if (request->request_initiator) {
    // LOG(INFO) << "GetRequestingHost: using request initiator";
    return request->request_initiator->host();
  }

  std::string origin_header;
  if (request->headers.GetHeader(net::HttpRequestHeaders::kOrigin,
                                 &origin_header)) {
    // LOG(INFO) << "GetRequestingHost: using Origin header [" << origin_header
    //           << "]";
    GURL origin_url(origin_header);
    return origin_url.host();
  }

  // TEMPORARY: Use request URL as a fallback. This may lead to incorrect
  // results if a cross-origin request is being made but the Origin header was
  // not set.
  // LOG(INFO) << "GetRequestingHost: falling back to request url host";
  return request->url.host();
}

std::string ThorzeelThrottle::GetMultipartBoundary(
    const scoped_refptr<net::HttpResponseHeaders>& headers) {
  std::string content_type_header;
  if (!headers->GetNormalizedHeader(net::HttpRequestHeaders::kContentType,
                                    &content_type_header)) {
    return "";
  }
  if (!base::StartsWith(content_type_header, kContentTypeMultipartPrefix)) {
    return "";
  }

  // start_index should be optimized by the compiler into a constant.
  size_t start_index = strlen(kContentTypeMultipartPrefix);
  if (start_index == content_type_header.length()) {
    // No boundary after prefix
    return "";
  }

  // The boundary is the rest of the header after the prefix.
  return content_type_header.substr(start_index);
}

}  // namespace thorzeel
