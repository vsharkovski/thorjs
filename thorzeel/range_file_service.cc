#include "thorzeel/range_file_service.h"

#include "base/logging.h"
#include "net/base/load_flags.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "url/gurl.h"

namespace thorzeel {

namespace {

RangeFileService* g_range_file_service = nullptr;

constexpr char kRangeFileURL[] = "http://10.224.40.169/thorzeel/ranges.txt";
constexpr int kMaxRetries = 3;
// constexpr size_t kRangeFileMaxLogSize = 1024;

const net::NetworkTrafficAnnotationTag kRangeFileTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("range_file_request", R"(
      semantics {
        sender: "Thorzeel Range File Service"
        description: "A request for the range file."
        trigger: "Browser startup."
        user_data {
          type: NONE
        }
        data: "No data is sent."
        destination: WEBSITE
        last_reviewed: "2024-09-10"
      }
      policy {
        cookies_allowed: NO
        setting: "This feature cannot be turned off by the user."
        policy_exception_justification = "Experimental feature."
      }
    )");

}  // namespace

// static
void RangeFileService::Create(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory) {
  if (g_range_file_service) {
    LOG(ERROR) << "Create: Range file service already exists";
    return;
  }
  g_range_file_service = new RangeFileService(std::move(url_loader_factory));
}

// static
RangeFileService* RangeFileService::Get() {
  return g_range_file_service;
}

RangeFileService::RangeFileService(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory)
    : url_loader_factory_(std::move(url_loader_factory)) {
  DownloadRangeFile();
}

RangeFileService::~RangeFileService() = default;

void RangeFileService::AddReceiver(
    mojo::PendingReceiver<mojom::RangeFileService> receiver) {
  receivers_.Add(this, std::move(receiver));
}

void RangeFileService::DownloadRangeFile() {
  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = GURL(kRangeFileURL);
  resource_request->method = "GET";
  resource_request->load_flags = net::LOAD_NORMAL;

  simple_url_loader_ = network::SimpleURLLoader::Create(
      std::move(resource_request), kRangeFileTrafficAnnotation);
  simple_url_loader_->SetRetryOptions(
      kMaxRetries,
      network::SimpleURLLoader::RetryMode::RETRY_ON_5XX |
          network::SimpleURLLoader::RetryMode::RETRY_ON_NETWORK_CHANGE);

  LOG(INFO) << "DownloadRangeFile: Sending request";
  simple_url_loader_->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&RangeFileService::OnSimpleLoaderComplete,
                     base::Unretained(this)),
      network::SimpleURLLoader::kMaxBoundedStringDownloadSize);
}

void RangeFileService::OnSimpleLoaderComplete(
    std::optional<std::string> response_body) {
  if (!response_body.has_value()) {
    LOG(INFO) << "OnSimpleLoaderComplete: no response body";
    response_body = "";
  } else {
    LOG(INFO) << "OnSimpleLoaderComplete: file has size ["
              << response_body->size() << "]";
  }

  file_callbacks_mutex_.lock();
  file_ = std::move(response_body);
  file_callbacks_mutex_.unlock();

  // LOG(INFO) << "OnSimpleLoaderComplete: file first [" << kRangeFileMaxLogSize
  //           << "] bytes: ["
  //           << std::string_view(
  //                  file_.value().begin(),
  //                  file_.value().begin() +
  //                      std::min(file_.value().size(), kRangeFileMaxLogSize))
  //           << "]";

  // int index = 0;
  // LOG(INFO) << "OnSimpleLoaderComplete: Running ["
  //           << pending_get_file_callbacks_.size() << "] pending callbacks";
  for (auto&& callback : pending_get_file_callbacks_) {
    // LOG(INFO) << "OnSimpleLoaderComplete: Running callback # [" << index <<
    // "]";
    std::move(callback).Run(file_.value());
    // index++;
  }
  pending_get_file_callbacks_.clear();
}

void RangeFileService::GetFile(GetFileCallback callback) {
  file_callbacks_mutex_.lock();
  if (file_.has_value()) {
    file_callbacks_mutex_.unlock();
    // LOG(INFO) << "GetFile: File has value, running callback";
    std::move(callback).Run(file_.value());
  } else {
    // LOG(INFO) << "GetFile: File has no value yet, adding callback # ["
    //           << pending_get_file_callbacks_.size() << "]";
    pending_get_file_callbacks_.emplace_back(std::move(callback));
    file_callbacks_mutex_.unlock();
  }
}

}  // namespace thorzeel
