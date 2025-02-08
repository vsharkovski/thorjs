#ifndef THORZEEL_THORZEEL_THROTTLE_H_
#define THORZEEL_THORZEEL_THROTTLE_H_

#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "net/http/http_response_headers.h"
#include "services/network/public/mojom/url_response_head.mojom-forward.h"
#include "third_party/blink/public/common/loader/url_loader_throttle.h"

namespace thorzeel {

class ThorzeelThrottle final : public blink::URLLoaderThrottle {
 public:
  static std::unique_ptr<ThorzeelThrottle> MaybeCreate(
      const network::ResourceRequest& request,
      scoped_refptr<base::SequencedTaskRunner> task_runner);

  ~ThorzeelThrottle() override;

  // Implements blink::URLLoaderThrottle.
  void DetachFromCurrentSequence() override;
  const char* NameForLoggingWillStartRequest() override;
  void WillStartRequest(network::ResourceRequest* request,
                        bool* defer) override;
  const char* NameForLoggingWillProcessResponse() override;
  void WillProcessResponse(const GURL& response_url,
                           network::mojom::URLResponseHead* response_head,
                           bool* defer) override;

 private:
  explicit ThorzeelThrottle(
      scoped_refptr<base::SequencedTaskRunner> task_runner);
  void UpdateContentLength(network::mojom::URLResponseHead* response_head);
  bool InterceptResponseWithPartialContentLoader(
      network::mojom::URLResponseHead* response_head,
      bool* defer,
      const std::string& url);
  bool InterceptResponseWithSuccessLoader(
      network::mojom::URLResponseHead* response_head,
      bool* defer,
      const std::string& url);
  void DeferredCancelWithError(int error_code);

  std::string GetRequestingHost(network::ResourceRequest* request);

  // Attemps to extract the multipart boundary from the Content-Type header.
  // Fails and returns an empty string if the header does not exist or if the
  // header does not have the expected value or format (see
  // |kContentTypeMultipartPrefix| in implementation).
  std::string GetMultipartBoundary(
      const scoped_refptr<net::HttpResponseHeaders>& headers);

  bool did_add_ranges_ = false;
  uint32_t final_content_length_ = 0;
  std::string range_header_;
  scoped_refptr<base::SequencedTaskRunner> task_runner_;
  base::WeakPtrFactory<ThorzeelThrottle> weak_factory_{this};
};

}  // namespace thorzeel

#endif  // THORZEEL_THORZEEL_THROTTLE_H_
