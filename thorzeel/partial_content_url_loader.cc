#include "thorzeel/partial_content_url_loader.h"

#include <cstring>
#include <unordered_set>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "mojo/public/c/system/types.h"
#include "mojo/public/cpp/system/string_data_source.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/url_loader_completion_status.h"

namespace thorzeel {

namespace {

constexpr size_t kLineMaxSize = 128;
constexpr size_t kNewlineSize = 2;
constexpr char kBoundaryDashes[] = "--";
// See: https://stackoverflow.com/a/9664327/4580269
std::unordered_set<std::string> kContentTypeValidValues = {
    "text/javascript",    "application/javascript", "application/x-javascript",
    "text/javascript1.0", "text/javascript1.1",     "text/javascript1.2",
    "text/javascript1.3", "text/javascript1.4",     "text/javascript1.5",
    "text/jscript",       "text/livescript"};

}  // namespace

PartialContentURLLoader::PartialContentURLLoader(
    mojo::PendingRemote<network::mojom::URLLoaderClient>
        destination_url_loader_client,
    const std::string& boundary_separator,
    const std::string& url)
    : destination_url_loader_client_(std::move(destination_url_loader_client)),
      url_(url) {
  // Construct the boundary
  boundary_.append(kBoundaryDashes);
  boundary_.append(boundary_separator);
  final_boundary_.append(kBoundaryDashes);
  final_boundary_.append(boundary_separator);
  final_boundary_.append(kBoundaryDashes);
  // LOG(INFO) << "Created, boundary [" << boundary_ << "], final boundary ["
  //           << final_boundary_ << "]";
}

PartialContentURLLoader::~PartialContentURLLoader() = default;

void PartialContentURLLoader::Start(
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
  expected_token_type_ = TokenType::kBoundary;
}

// network::mojom::URLLoaderClient implementation (called from the source of
// the response):

void PartialContentURLLoader::OnReceiveEarlyHints(
    network::mojom::EarlyHintsPtr early_hints) {
  // OnReceiveEarlyHints() shouldn't be called because PartialContentURLLoader
  // is created by ThorzeelThrottle::WillProcessResponse(), which is equivalent
  // to OnReceiveResponse().
  NOTREACHED();
}

void PartialContentURLLoader::OnReceiveResponse(
    network::mojom::URLResponseHeadPtr response_head,
    mojo::ScopedDataPipeConsumerHandle body,
    std::optional<mojo_base::BigBuffer> cached_metadata) {
  // OnReceiveResponse() shouldn't be called because PartialContentURLLoader is
  // created by ThorzeelThrottle::WillProcessResponse(), which is equivalent to
  // OnReceiveResponse().
  NOTREACHED();
}

void PartialContentURLLoader::OnReceiveRedirect(
    const net::RedirectInfo& redirect_info,
    network::mojom::URLResponseHeadPtr response_head) {
  // OnReceiveRedirect() shouldn't be called because PartialContentURLLoader is
  // created by ThorzeelThrottle::WillProcessResponse(), which is equivalent to
  // OnReceiveResponse().
  NOTREACHED();
}

void PartialContentURLLoader::OnUploadProgress(
    int64_t current_position,
    int64_t total_size,
    OnUploadProgressCallback ack_callback) {
  // OnUploadProgress() shouldn't be called because PartialContentURLLoader is
  // created by ThorzeelThrottle::WillProcessResponse(), which is equivalent to
  // OnReceiveResponse().
  NOTREACHED();
  // destination_url_loader_client_->OnUploadProgress(current_position,
  // total_size, std::move(ack_callback));
}

void PartialContentURLLoader::OnTransferSizeUpdated(
    int32_t transfer_size_diff) {
  destination_url_loader_client_->OnTransferSizeUpdated(transfer_size_diff);
}

void PartialContentURLLoader::OnComplete(
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

void PartialContentURLLoader::FollowRedirect(
    const std::vector<std::string>& removed_headers,
    const net::HttpRequestHeaders& modified_headers,
    const net::HttpRequestHeaders& modified_cors_exempt_headers,
    const std::optional<GURL>& new_url) {
  // PartialContentURLLoader starts handling the request after
  // OnReceivedResponse(). A redirect response is not expected.
  NOTREACHED();
}

void PartialContentURLLoader::SetPriority(net::RequestPriority priority,
                                          int32_t intra_priority_value) {
  if (state_ == State::kAborted) {
    return;
  }
  source_url_loader_->SetPriority(priority, intra_priority_value);
}

void PartialContentURLLoader::PauseReadingBodyFromNet() {
  if (state_ == State::kAborted) {
    return;
  }
  source_url_loader_->PauseReadingBodyFromNet();
}

void PartialContentURLLoader::ResumeReadingBodyFromNet() {
  if (state_ == State::kAborted) {
    return;
  }
  source_url_loader_->ResumeReadingBodyFromNet();
}

// mojo::DataPipeDrainer::Client implementation:

void PartialContentURLLoader::OnDataAvailable(const void* data,
                                              size_t num_bytes) {
  DCHECK_EQ(State::kParsing, state_);
  buffer_ = static_cast<const char*>(data);
  buffer_initial_size_ = static_cast<uint32_t>(num_bytes);
  buffer_size_ = buffer_initial_size_;
  // LOG(INFO) << "OnDataAvailable: url [" << url_ << "], buffer size ["
  //           << buffer_size_ << ", buffer ["
  //           << std::string_view(buffer_, buffer_size_) << "]";
  // LOG(INFO) << "OnDataAvailable: buffer size [" << buffer_size_ << "]";

  while (buffer_size_ > 0) {
    if (expected_token_type_ == TokenType::kPart) {
      ForwardPart();
      if (part_remaining_bytes_ == 0) {
        expected_token_type_ = TokenType::kBoundary;
        line_.clear();
      }
      continue;
    }
    // All other token types need a full line to be read and stored in |line_|.
    auto read_line_result = ReadLine();
    if (read_line_result == ReadLineResult::kTooLong) {
      LOG(ERROR) << "OnDataAvailable: url [" << url_
                 << "], line would be too long";
      Abort();
      return;
    } else if (read_line_result == ReadLineResult::kNotFound) {
      // |buffer_| ended before a newline was reached, so expect a newline in a
      // future call to |OnDataAvailable|.
      DCHECK_EQ(0, buffer_size_);
      break;
    }
    DCHECK_EQ(ReadLineResult::kFound, read_line_result);
    // |line_| ends with a newline, so now the other token types can be handled.
    // But first remove the newline from the end of |line_|.
    line_.resize(line_.size() - kNewlineSize);
    switch (expected_token_type_) {
      case TokenType::kBoundary:
        if (!HandleExpectedBoundary()) {
          return;
        }
        break;
      case TokenType::kHeader:
        if (!HandleExpectedHeader()) {
          return;
        }
        break;
      default:
        LOG(ERROR) << "OnDataAvailable: url [" << url_
                   << "], unhandled token type ["
                   << static_cast<int>(expected_token_type_) << "]";
        NOTREACHED();
        return;
    }
    // Since the line has been handled, clear it.
    line_.clear();
  }
}

void PartialContentURLLoader::OnDataComplete() {
  // LOG(INFO) << "OnDataComplete: state [" << static_cast<int>(state_) << "]";
  switch (state_) {
    case State::kParsing:
      // Still parsing.
      return;
    case State::kWaitForWrite:
      // An |OnDataWritten| call is expected.
    case State::kCompleted:
      // Nothing to do anymore.
      DCHECK_EQ(0, buffer_size_);
      return;
    default:
      LOG(ERROR) << "OnDataComplete: url [" << url_ << "], invalid state";
      Abort();
      return;
  }
}

void PartialContentURLLoader::OnDataWritten(MojoResult result) {
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

void PartialContentURLLoader::CompleteParsing() {
  // LOG(INFO) << "CompleteParsing";
  DCHECK_EQ(State::kParsing, state_);
  state_ = State::kWaitForWrite;

  if (pending_writes_.empty()) {
    CompleteWaitForWrite();
  }
}

void PartialContentURLLoader::CompleteWaitForWrite() {
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

void PartialContentURLLoader::Abort() {
  LOG(WARNING) << "Abort: url [" << url_ << "], buffer [" << buffer_size_
               << "] bytes [" << std::string_view(buffer_, buffer_size_)
               << "], line [" << line_.size() << "] bytes [" << line_ << "]";
  state_ = State::kAborted;

  data_drainer_.reset();
  data_producer_.reset();
  source_url_loader_.reset();
  source_url_client_receiver_.reset();
  destination_url_loader_client_.reset();
  // |this| should be removed since the owner will destroy |this| or the owner
  // has already been destroyed by some reason.
}

void PartialContentURLLoader::ForwardPart() {
  DCHECK_GT(buffer_size_, 0);
  DCHECK_GT(part_remaining_bytes_, 0);

  uint32_t bytes_to_write = std::min(buffer_size_, part_remaining_bytes_);
  // LOG(INFO) << "ForwardPart: queuing [" << bytes_to_write << "] bytes";

  // Add the data to the write queue.
  auto data = std::make_unique<std::string>(buffer_, bytes_to_write);
  pending_writes_.push(std::move(data));

  if (!waiting_for_data_written_) {
    WriteNextPendingData();
  }

  // Advance the buffer and part bytes.
  buffer_ += bytes_to_write;
  buffer_size_ -= bytes_to_write;
  part_remaining_bytes_ -= bytes_to_write;
}

void PartialContentURLLoader::WriteNextPendingData() {
  auto data = std::move(pending_writes_.front());
  pending_writes_.pop();
  waiting_for_data_written_ = true;

  // LOG(INFO) << "WriteNextPendingData: url [" << url_ << "], writing ["
  //           << data->size() << "] bytes [" << *data << "] ";

  // To avoid unnecessary string copy, use
  // STRING_STAYS_VALID_UNTIL_COMPLETION here, and keep the original data
  // hold in the closure below.
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
          base::BindOnce(&PartialContentURLLoader::OnDataWritten,
                         weak_factory_.GetWeakPtr())));

  // base::span<const char> data(buffer_, bytes_to_write);
  // auto source = std::make_unique<mojo::StringDataSource>(
  //     data, mojo::StringDataSource::AsyncWritingMode::
  //               STRING_MAY_BE_INVALIDATED_BEFORE_COMPLETION);
  // writes_pending_++;
  // data_producer_.get()->Write(
  //     std::move(source),
  //     base::BindOnce(
  //         [](mojo::DataPipeProducer* data_producer,
  //            base::OnceCallback<void(MojoResult)> on_data_written_callback,
  //            MojoResult result) {
  //           std::move(on_data_written_callback).Run(result);
  //         },
  //         base::Unretained(data_producer_.get()),
  //         base::BindOnce(&PartialContentURLLoader::OnDataWritten,
  //                        weak_factory_.GetWeakPtr())));
}

PartialContentURLLoader::ReadLineResult PartialContentURLLoader::ReadLine() {
  DCHECK_LE(line_.size(), kLineMaxSize);
  // Find first occurence of a newline i.e. '\r\n'. If there is such a newline,
  // |ptr| will point to the first character after the '\n'. If there is no
  // newline, |ptr| will point to right after |buffer_| i.e. its end.
  uint32_t remaining_bytes = buffer_size_;
  const char* ptr = buffer_;
  bool found_newline = false;
  while (remaining_bytes > 0) {
    const char* prev_ptr = ptr;
    ptr = static_cast<const char*>(
        std::memchr(static_cast<const void*>(ptr), '\n', remaining_bytes));
    if (ptr == nullptr) {
      // No newline in |buffer_|, stop.
      ptr = buffer_ + buffer_size_;
      break;
    } else if ((ptr > buffer_ && *(ptr - 1) == '\r') ||
               (ptr == buffer_ && !line_.empty() && line_.back() == '\r')) {
      // Previous character, which is either in |buffer_| or in |line_|, was
      // '\r', so this is a proper newline, so stop here.
      ++ptr;
      found_newline = true;
      break;
    } else {
      // This is a '\n' character but the previous character was not '\r'.
      // Continue searching for a '\n' later.
      ++ptr;
      uint32_t distance = ptr - prev_ptr;
      remaining_bytes -= distance;
    }
  }

  // Add all bytes from start of |buffer_| to |ptr| excluding |ptr| to |line_|.
  // But first check if line would become too long.
  size_t bytes_to_add = ptr - buffer_;
  size_t new_line_size = line_.size() + bytes_to_add;
  if (new_line_size > kLineMaxSize) {
    LOG(ERROR) << "ReadLine: url [" << url_ << "], Want to add ["
               << bytes_to_add << "] bytes which would make line have ["
               << new_line_size << "] characters which is above maximum of ["
               << kLineMaxSize << "]";
    return ReadLineResult::kTooLong;
  }
  line_.append(buffer_, bytes_to_add);
  buffer_ += bytes_to_add;
  buffer_size_ -= bytes_to_add;
  return found_newline ? ReadLineResult::kFound : ReadLineResult::kNotFound;
}

bool PartialContentURLLoader::HandleExpectedBoundary() {
  if (line_.empty()) {
    // Empty line should be skipped.
    // LOG(INFO) << "HandleExpectedBoundary: Empty line, skipping";
    return true;
  } else if (line_ == boundary_) {
    // LOG(INFO) << "HandleExpectedBoundary: Parsed boundary";
    expected_token_type_ = TokenType::kHeader;
    had_content_type_ = false;
    had_content_range_ = false;
    return true;
  } else if (line_ == final_boundary_) {
    // LOG(INFO) << "HandleExpectedBoundary: Parsed final boundary";
    if (buffer_size_ == 0) {
      CompleteParsing();
      return true;
    } else {
      LOG(ERROR) << "HandleExpectedBoundary: url [" << url_
                 << "], bad data, data exists after final "
                    "boundary";
      Abort();
      return false;
    }
  } else {
    LOG(ERROR) << "HandleExpectedBoundary: url [" << url_
               << "], bad data, could not parse boundary "
                  "or final boundary";
    Abort();
    return false;
  }
}

bool PartialContentURLLoader::HandleExpectedHeader() {
  if (line_.empty()) {
    if (had_content_range_) {
      // After a Content-Range header has been provided, an empty line
      // always precedes the part content, so prepare for the part.
      // LOG(INFO) << "HandleExpectedHeader: Empty line, now expecting part";
      expected_token_type_ = TokenType::kPart;
    } else {
      // Unexpected empty line. Skip it.
      // LOG(INFO) << "HandleExpectedHeader: Unexpected empty line, skipping";
    }
    return true;
  }
  if (!had_content_type_) {
    std::string content_type;
    if (ParseContentType(content_type)) {
      if (kContentTypeValidValues.contains(content_type)) {
        had_content_type_ = true;
        // LOG(INFO) << "HandleExpectedHeader: Valid content type ["
        //           << content_type << "]";
        return true;
      } else {
        LOG(ERROR) << "HandleExpectedHeader: url [" << url_
                   << "], invalid content type [" << content_type << "]";
        return false;
      }
    }
  }
  if (!had_content_range_) {
    uint32_t part_start_byte, part_end_byte;
    if (ParseContentRange(part_start_byte, part_end_byte)) {
      if (part_start_byte <= part_end_byte) {
        part_remaining_bytes_ = part_end_byte - part_start_byte + 1;
        had_content_range_ = true;
        // LOG(INFO)
        //     << "HandleExpectedHeader: Valid content range, part start byte ["
        //     << part_start_byte << "], part end byte [" << part_end_byte
        //     << "], part bytes [" << part_remaining_bytes_ << "]";
        return true;

      } else {
        LOG(ERROR) << "HandleExpectedHeader: url [" << url_
                   << "], invalid content range, part start byte ["
                   << part_start_byte << "] > part end byte [" << part_end_byte
                   << "]";
        return false;
      }
    }
  }
  LOG(ERROR) << "HandleExpectedHeader: url [" << url_ << "], could not handle";
  Abort();
  return false;
}

bool PartialContentURLLoader::ParseContentType(std::string& result) {
  char scan_buffer[kLineMaxSize];
  size_t characters_consumed = 0;
  int parameters_assigned = std::sscanf(line_.c_str(), "Content-Type: %s%n",
                                        scan_buffer, &characters_consumed);
  if (parameters_assigned != 1 || characters_consumed != line_.size()) {
    return false;
  }

  result.append(scan_buffer);
  return true;
}

bool PartialContentURLLoader::ParseContentRange(
    uint32_t& part_start_byte_result,
    uint32_t& part_end_byte_result) {
  // Todo: handle if start > end
  uint32_t part_start_byte, part_end_byte, total_bytes;
  size_t characters_consumed = 0;
  int parameters_assigned = std::sscanf(
      line_.c_str(), "Content-Range: bytes %u-%u/%u%n", &part_start_byte,
      &part_end_byte, &total_bytes, &characters_consumed);
  if (parameters_assigned != 3 || characters_consumed != line_.size()) {
    return false;
  }

  part_start_byte_result = part_start_byte;
  part_end_byte_result = part_end_byte;
  return true;
}

}  // namespace thorzeel
