#include "thorzeel/success_url_loader.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_set>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string_util.h"
#include "mojo/public/c/system/types.h"
#include "mojo/public/cpp/system/string_data_source.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/url_loader_completion_status.h"

namespace thorzeel {

namespace {

constexpr char kRangeHeaderStart[] = "bytes=";

}  // namespace

// static
std::unique_ptr<SuccessURLLoader> SuccessURLLoader::Create(
    mojo::PendingRemote<network::mojom::URLLoaderClient>
        destination_url_loader_client,
    const std::string& range_header,
    const std::string& url) {
  auto loader = base::WrapUnique(
      new SuccessURLLoader(std::move(destination_url_loader_client), url));
  if (!loader->SetRanges(range_header)) {
    return nullptr;
  }
  return loader;
}

SuccessURLLoader::SuccessURLLoader(
    mojo::PendingRemote<network::mojom::URLLoaderClient>
        destination_url_loader_client,
    const std::string& url)
    : destination_url_loader_client_(std::move(destination_url_loader_client)),
      url_(url) {}

SuccessURLLoader::~SuccessURLLoader() = default;

bool SuccessURLLoader::SetRanges(const std::string& range_header) {
  // LOG(INFO) << "SetRanges: range header [" << range_header << "]";
  if (!base::StartsWith(range_header, kRangeHeaderStart,
                        base::CompareCase::INSENSITIVE_ASCII)) {
    LOG(ERROR) << "SetRanges: url [" << url_
               << "], range header does not start with [" << kRangeHeaderStart
               << "], stopping";
    return false;
  }

  const char* ptr = range_header.c_str() + strlen(kRangeHeaderStart);
  const char* end = range_header.c_str() + range_header.size();
  while (ptr < end) {
    // Skip leading whitespace.
    while (ptr < end && std::isspace(*ptr)) {
      // LOG(INFO) << "SetRanges: skipped space";
      ++ptr;
    }
    if (ptr == end) {
      // LOG(INFO) << "SetRanges: reached end";
      break;
    }

    // Parse the range start and end.
    char* next_ptr;
    uint64_t range_start = std::strtoull(ptr, &next_ptr, 10);
    if (next_ptr == ptr || next_ptr >= end || *next_ptr != '-') {
      // Parsing error or unexpected format.
      LOG(ERROR) << "SetRanges: url [" << url_ << "], parsing error, ptr ["
                 << ptr << "], *ptr [" << *ptr << "], next_ptr [" << next_ptr
                 << "] diff [" << next_ptr - ptr << "], *next_ptr ["
                 << *next_ptr << "], end [" << end << "]";
      return false;
    }
    // LOG(INFO) << "SetRanges: parsed range start [" << range_start << "]";
    ptr = next_ptr + 1;
    uint64_t range_end = std::strtoull(ptr, &next_ptr, 10);
    if (next_ptr == ptr || (next_ptr < end && *next_ptr != ',')) {
      // Parsing error or unexpected format.
      LOG(ERROR) << "SetRanges: url [" << url_ << "], parsing error, ptr ["
                 << ptr << "], *ptr [" << *ptr << "], next_ptr [" << next_ptr
                 << "] diff [" << next_ptr - ptr << "], *next_ptr ["
                 << *next_ptr << "], end [" << end << "]";
      return false;
    }
    // LOG(INFO) << "SetRanges: parsed range end [" << range_end << "]";
    ptr = next_ptr + 1;
    ranges_.emplace_back(range_start, range_end);
    // LOG(INFO) << "SetRanges: Range [" << range_start << "," << range_end <<
    // "]";
  }

  current_range_ = ranges_.begin();
  return true;
}

void SuccessURLLoader::Start(
    mojo::PendingRemote<network::mojom::URLLoader> source_url_loader_remote,
    mojo::PendingReceiver<network::mojom::URLLoaderClient>
        source_url_client_receiver,
    mojo::ScopedDataPipeConsumerHandle body,
    mojo::ScopedDataPipeProducerHandle producer_handle) {
  source_url_loader_.Bind(std::move(source_url_loader_remote));
  source_url_client_receiver_.Bind(std::move(source_url_client_receiver));
  data_drainer_ =
      std::make_unique<mojo::DataPipeDrainer>(this, std::move(body));
  data_producer_ =
      std::make_unique<mojo::DataPipeProducer>(std::move(producer_handle));
  state_ = State::kParsing;
}

// network::mojom::URLLoaderClient implementation (called from the source of
// the response):

void SuccessURLLoader::OnReceiveEarlyHints(
    network::mojom::EarlyHintsPtr early_hints) {
  // OnReceiveEarlyHints() shouldn't be called because SuccessURLLoader
  // is created by ThorzeelThrottle::WillProcessResponse(), which is equivalent
  // to OnReceiveResponse().
  NOTREACHED();
}

void SuccessURLLoader::OnReceiveResponse(
    network::mojom::URLResponseHeadPtr response_head,
    mojo::ScopedDataPipeConsumerHandle body,
    std::optional<mojo_base::BigBuffer> cached_metadata) {
  // OnReceiveResponse() shouldn't be called because SuccessURLLoader is
  // created by ThorzeelThrottle::WillProcessResponse(), which is equivalent to
  // OnReceiveResponse().
  NOTREACHED();
}

void SuccessURLLoader::OnReceiveRedirect(
    const net::RedirectInfo& redirect_info,
    network::mojom::URLResponseHeadPtr response_head) {
  // OnReceiveRedirect() shouldn't be called because SuccessURLLoader is
  // created by ThorzeelThrottle::WillProcessResponse(), which is equivalent to
  // OnReceiveResponse().
  NOTREACHED();
}

void SuccessURLLoader::OnUploadProgress(int64_t current_position,
                                        int64_t total_size,
                                        OnUploadProgressCallback ack_callback) {
  // OnUploadProgress() shouldn't be called because SuccessURLLoader is
  // created by ThorzeelThrottle::WillProcessResponse(), which is equivalent to
  // OnReceiveResponse().
  NOTREACHED();
  // destination_url_loader_client_->OnUploadProgress(current_position,
  // total_size, std::move(ack_callback));
}

void SuccessURLLoader::OnTransferSizeUpdated(int32_t transfer_size_diff) {
  destination_url_loader_client_->OnTransferSizeUpdated(transfer_size_diff);
}

void SuccessURLLoader::OnComplete(
    const network::URLLoaderCompletionStatus& status) {
  DCHECK(!original_complete_status_.has_value());
  // LOG(INFO) << "OnComplete: status error code [" << status.error_code
  //           << "], state [" << static_cast<int>(state_) << "]";

  switch (state_) {
    case State::kWaitForBody:
      // An error occurred before receiving any data.
      DCHECK_NE(net::OK, status.error_code);
      state_ = State::kCompleted;
      destination_url_loader_client_->OnComplete(status);
      return;
    case State::kParsing:
    case State::kWaitForWrite:
      // Defer calling OnComplete() until the whole body has been parsed and all
      // data has been written.
      original_complete_status_ = status;
      return;
    case State::kCompleted:
      destination_url_loader_client_->OnComplete(status);
      return;
    case State::kAborted:
      NOTREACHED();
      return;
  }
  NOTREACHED();
}

// network::mojom::URLLoader implementation (called from the destination of
// the response):

void SuccessURLLoader::FollowRedirect(
    const std::vector<std::string>& removed_headers,
    const net::HttpRequestHeaders& modified_headers,
    const net::HttpRequestHeaders& modified_cors_exempt_headers,
    const std::optional<GURL>& new_url) {
  // SuccessURLLoader starts handling the request after
  // OnReceivedResponse(). A redirect response is not expected.
  NOTREACHED();
}

void SuccessURLLoader::SetPriority(net::RequestPriority priority,
                                   int32_t intra_priority_value) {
  if (state_ == State::kAborted) {
    return;
  }
  source_url_loader_->SetPriority(priority, intra_priority_value);
}

void SuccessURLLoader::PauseReadingBodyFromNet() {
  if (state_ == State::kAborted) {
    return;
  }
  source_url_loader_->PauseReadingBodyFromNet();
}

void SuccessURLLoader::ResumeReadingBodyFromNet() {
  if (state_ == State::kAborted) {
    return;
  }
  source_url_loader_->ResumeReadingBodyFromNet();
}

// mojo::DataPipeDrainer::Client implementation:

void SuccessURLLoader::OnDataAvailable(const void* data, size_t num_bytes) {
  if (state_ != State::kParsing) {
    return;
  }

  const char* buffer = static_cast<const char*>(data);
  uint32_t buffer_size = static_cast<uint32_t>(num_bytes);
  uint32_t buffer_end = buffer_start_ + buffer_size - 1;
  // LOG(INFO) << "OnDataAvailable: buffer ["
  //           << std::string_view(buffer, buffer_size) << "]";
  // LOG(INFO) << "OnDataAvailable: url [" << url_ << "], buffer size ["
  //           << buffer_size << "], bytes [" << buffer_start_ << ", "
  //           << buffer_end << "]";

  while (current_range_ != ranges_.end()) {
    auto part_start = current_range_->first;
    auto part_end = current_range_->second;
    // LOG(INFO) << "OnDataAvailable: part [" << part_start << ", " << part_end
    //           << "]";

    if (buffer_end < part_start) {
      // Part is completely after buffer. Ignore this buffer.
      // LOG(INFO) << "OnDataAvailable: part completely after buffer, stopping";
      break;
    } else if (part_end < buffer_start_) {
      // Not possible.
      LOG(ERROR) << "OnDataAvailable: url [" << url_ << "], part ["
                 << part_start << ", " << part_end << "] is before buffer ["
                 << buffer_start_ << ", " << buffer_end << "]";
      Abort();
      return;
    }

    // There is an intersection. Write those bytes.
    auto intersection_start = std::max(part_start, buffer_start_);
    auto intersection_end = std::min(part_end, buffer_end);
    auto intersection_size = intersection_end - intersection_start + 1;
    const char* buffer_intersection_start =
        buffer + (intersection_start - buffer_start_);
    auto data_to_write = std::make_unique<std::string>(
        buffer_intersection_start, intersection_size);
    pending_writes_.push(std::move(data_to_write));
    // LOG(INFO) << "OnDataAvailable: intersection [" << intersection_start <<
    // ", " << intersection_end << "]";

    if (intersection_end == part_end) {
      // The whole part has been handled. Proceed to the next part.
      ++current_range_;
      // LOG(INFO) << "OnDataAvailable: Advancing range";
    } else {
      // intersection_end < part_end, thus
      // intersection_end = buffer_end < part_end
      // and no more data can be used from the buffer.
      // LOG(INFO) << "OnDataAvailable: Buffer used up, stopping";
      break;
    }
  }

  if (current_range_ == ranges_.end()) {
    // Handled all ranges.
    state_ = State::kWaitForWrite;
  }

  buffer_start_ = buffer_end + 1;

  if (!waiting_for_data_written_) {
    WriteNextPendingData();
  }
}

void SuccessURLLoader::OnDataComplete() {
  // LOG(INFO) << "OnDataComplete: state [" << static_cast<int>(state_) << "]";
  switch (state_) {
    case State::kParsing:
      LOG(ERROR) << "OnDataComplete: url [" << url_
                 << "], data complete when still parsing";
      Abort();
      return;
    case State::kWaitForWrite:
      // An |OnDataWritten| call is expected.
    case State::kCompleted:
      // Nothing to do anymore.
      return;
    default:
      LOG(ERROR) << "OnDataComplete: url [" << url_ << "], invalid state";
      Abort();
      return;
  }
}

void SuccessURLLoader::OnDataWritten(MojoResult result) {
  // LOG(INFO) << "OnDataWritten: now pending [" << pending_writes_.size() <<
  // "]";
  waiting_for_data_written_ = false;
  if (result == MOJO_RESULT_OK) {
    if (state_ == State::kWaitForWrite && pending_writes_.empty()) {
      CompleteWaitForWrite();
    } else if (!pending_writes_.empty()) {
      WriteNextPendingData();
    }
  } else {
    LOG(ERROR) << "OnDataWritten: url [" << url_ << "], result ["
               << static_cast<int>(result) << "] not OK";
    destination_url_loader_client_->OnComplete(
        network::URLLoaderCompletionStatus(net::ERR_INSUFFICIENT_RESOURCES));
    Abort();
  }
}

void SuccessURLLoader::CompleteParsing() {
  // LOG(INFO) << "CompleteParsing";
  DCHECK_EQ(State::kParsing, state_);
  state_ = State::kWaitForWrite;

  if (pending_writes_.empty()) {
    CompleteWaitForWrite();
  }
}

void SuccessURLLoader::CompleteWaitForWrite() {
  // LOG(INFO) << "CompleteWaitForWrite";
  DCHECK_EQ(State::kWaitForWrite, state_);
  state_ = State::kCompleted;
  // Call client's OnComplete() if |this|'s OnComplete() has already been
  // called.
  if (original_complete_status_.has_value()) {
    destination_url_loader_client_->OnComplete(
        original_complete_status_.value());
  }

  data_drainer_.reset();
  data_producer_.reset();
}

void SuccessURLLoader::Abort() {
  LOG(WARNING) << "Abort: url [" << url_ << "]";
  state_ = State::kAborted;

  data_drainer_.reset();
  data_producer_.reset();
  source_url_loader_.reset();
  source_url_client_receiver_.reset();
  destination_url_loader_client_.reset();
  // |this| should be removed since the owner will destroy |this| or the owner
  // has already been destroyed by some reason.
}

void SuccessURLLoader::WriteNextPendingData() {
  auto data = std::move(pending_writes_.front());
  pending_writes_.pop();
  waiting_for_data_written_ = true;

  // LOG(INFO) << "WriteNextPendingData: url [" << url_ << "], writing ["
  //           << data->size() << "] bytes [" << *data << "]";

  // To avoid unnecessary string copy, use STRING_STAYS_VALID_UNTIL_COMPLETION
  // here, and keep the original data hold in the closure below.
  auto source = std::make_unique<mojo::StringDataSource>(
      *data, mojo::StringDataSource::AsyncWritingMode::
                 STRING_STAYS_VALID_UNTIL_COMPLETION);
  data_producer_.get()->Write(
      std::move(source),
      base::BindOnce(
          [](std::unique_ptr<std::string> data,
             base::OnceCallback<void(MojoResult)> on_data_written_callback,
             MojoResult result) {
            std::move(on_data_written_callback).Run(result);
          },
          std::move(data),
          base::BindOnce(&SuccessURLLoader::OnDataWritten,
                         weak_factory_.GetWeakPtr())));
}

}  // namespace thorzeel
