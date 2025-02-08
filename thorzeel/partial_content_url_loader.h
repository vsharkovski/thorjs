#ifndef THORZEEL_PARTIAL_CONTENT_URL_LOADER_
#define THORZEEL_PARTIAL_CONTENT_URL_LOADER_

#include <queue>

#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "mojo/public/cpp/system/data_pipe_producer.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "third_party/blink/public/common/loader/url_loader_throttle.h"
#include "url/gurl.h"

namespace thorzeel {

class ThorzeelThrottle;

class PartialContentURLLoader : public network::mojom::URLLoaderClient,
                                public network::mojom::URLLoader,
                                public mojo::DataPipeDrainer::Client {
 public:
  explicit PartialContentURLLoader(
      mojo::PendingRemote<network::mojom::URLLoaderClient>
          destination_url_loader_client,
      const std::string& boundary_separator,
      const std::string& url);
  ~PartialContentURLLoader() override;

  // Start waiting for the body.
  void Start(
      mojo::PendingRemote<network::mojom::URLLoader> source_url_loader_remote,
      mojo::PendingReceiver<network::mojom::URLLoaderClient>
          source_url_client_receiver,
      mojo::ScopedDataPipeConsumerHandle body,
      mojo::ScopedDataPipeProducerHandle producer_handle);

 private:
  // network::mojom::URLLoaderClient implementation (called from the source of
  // the response):
  void OnReceiveEarlyHints(network::mojom::EarlyHintsPtr early_hints) override;
  void OnReceiveResponse(
      network::mojom::URLResponseHeadPtr response_head,
      mojo::ScopedDataPipeConsumerHandle body,
      std::optional<mojo_base::BigBuffer> cached_metadata) override;
  void OnReceiveRedirect(
      const net::RedirectInfo& redirect_info,
      network::mojom::URLResponseHeadPtr response_head) override;
  void OnUploadProgress(int64_t current_position,
                        int64_t total_size,
                        OnUploadProgressCallback ack_callback) override;
  void OnTransferSizeUpdated(int32_t transfer_size_diff) override;
  void OnComplete(const network::URLLoaderCompletionStatus& status) override;

  // network::mojom::URLLoader implementation (called from the destination of
  // the response):
  void FollowRedirect(
      const std::vector<std::string>& removed_headers,
      const net::HttpRequestHeaders& modified_headers,
      const net::HttpRequestHeaders& modified_cors_exempt_headers,
      const std::optional<GURL>& new_url) override;
  void SetPriority(net::RequestPriority priority,
                   int32_t intra_priority_value) override;
  void PauseReadingBodyFromNet() override;
  void ResumeReadingBodyFromNet() override;

  // mojo::DataPipeDrainer::Client implementation:
  void OnDataAvailable(const void* data, size_t num_bytes) override;
  void OnDataComplete() override;

  void OnDataWritten(MojoResult result);
  void CompleteParsing();
  void CompleteWaitForWrite();
  void Abort();

  enum class ReadLineResult { kNotFound = 0, kFound, kTooLong };
  // Append characters from |buffer_| to |line_| until a newline (CRLF) is
  // appended or end of |buffer_| is reached. Returns kNotFound if a newline was
  // not found before |buffer_| ended, kFound if a newline was found, and
  // kTooLong if |line_| would become too long.
  ReadLineResult ReadLine();
  // Handle the boundary token type from |line_|. Returns false if an error
  // occurred and parsing should stop, and true otherwise.
  bool HandleExpectedBoundary();
  // Handle the header token type from |line_|. Returns false if an error
  // occurred and parsing should stop, and true otherwise.
  bool HandleExpectedHeader();
  // If the content type was successfully parsed from |line_|, stores the
  // content type into |result| and returns true. Returns false otherwise.
  bool ParseContentType(std::string& result);
  // If the content range was successfully parsed from |line_|, stores the
  // results and returns true. Returns false otherwise.
  bool ParseContentRange(uint32_t& part_start_byte, uint32_t& part_end_byte);
  // Forward as much of the part as possible from |buffer_| into
  // |data_producer_|.
  void ForwardPart();
  void WriteNextPendingData();

  // Mojo and control structures:
  enum class State {
    kWaitForBody = 0,
    kParsing,
    kWaitForWrite,
    kCompleted,
    kAborted
  };
  State state_ = State::kWaitForBody;

  bool waiting_for_data_written_ = false;
  std::queue<std::unique_ptr<std::string>> pending_writes_;

  // Set if OnComplete() is called during parsing.
  std::optional<network::URLLoaderCompletionStatus> original_complete_status_;
  std::unique_ptr<mojo::DataPipeDrainer> data_drainer_;
  std::unique_ptr<mojo::DataPipeProducer> data_producer_;
  mojo::Receiver<network::mojom::URLLoaderClient> source_url_client_receiver_{
      this};
  mojo::Remote<network::mojom::URLLoader> source_url_loader_;
  mojo::Remote<network::mojom::URLLoaderClient> destination_url_loader_client_;

  // Parsing:
  enum class TokenType {
    kNone = 0,
    kBoundary,
    kHeader,
    kPart,
  };

  std::string boundary_;
  std::string final_boundary_;
  const char* buffer_ = nullptr;
  uint32_t buffer_initial_size_ = 0;
  uint32_t buffer_size_ = 0;
  TokenType expected_token_type_ = TokenType::kNone;
  std::string line_;
  bool had_content_type_ = false;
  bool had_content_range_ = false;
  uint32_t part_remaining_bytes_ = 0;

  std::string url_;
  base::WeakPtrFactory<PartialContentURLLoader> weak_factory_{this};
};

}  // namespace thorzeel

#endif  // THORZEEL_PARTIAL_CONTENT_URL_LOADER_
