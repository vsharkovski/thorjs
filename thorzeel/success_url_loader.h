#ifndef THORZEEL_SUCCESS_URL_LOADER_
#define THORZEEL_SUCCESS_URL_LOADER_

#include <cstdint>
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

class SuccessURLLoader : public network::mojom::URLLoaderClient,
                         public network::mojom::URLLoader,
                         public mojo::DataPipeDrainer::Client {
 public:
  ~SuccessURLLoader() override;

  static std::unique_ptr<SuccessURLLoader> Create(
      mojo::PendingRemote<network::mojom::URLLoaderClient>
          destination_url_loader_client,
      const std::string& range_header,
      const std::string& url);

  // Start waiting for the body.
  void Start(
      mojo::PendingRemote<network::mojom::URLLoader> source_url_loader_remote,
      mojo::PendingReceiver<network::mojom::URLLoaderClient>
          source_url_client_receiver,
      mojo::ScopedDataPipeConsumerHandle body,
      mojo::ScopedDataPipeProducerHandle producer_handle);

 private:
  explicit SuccessURLLoader(mojo::PendingRemote<network::mojom::URLLoaderClient>
                                destination_url_loader_client,
                            const std::string& url);
  bool SetRanges(const std::string& range_header);

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
  std::vector<std::pair<uint32_t, uint32_t>> ranges_;
  std::vector<std::pair<uint32_t, uint32_t>>::iterator current_range_;
  uint32_t buffer_start_ = 0;

  std::string url_;
  base::WeakPtrFactory<SuccessURLLoader> weak_factory_{this};
};

}  // namespace thorzeel

#endif  // THORZEEL_SUCCESS_URL_LOADER_
