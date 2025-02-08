#ifndef THORZEEL_RANGE_FILE_SERVICE_H_
#define THORZEEL_RANGE_FILE_SERVICE_H_

#include <mutex>

#include "base/memory/scoped_refptr.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "thorzeel/mojom/range_file_service.mojom.h"

namespace network {
class SimpleURLLoader;
}  // namespace network

namespace thorzeel {

class RangeFileService : public mojom::RangeFileService {
 public:
  RangeFileService(const RangeFileService&) = delete;
  RangeFileService& operator=(const RangeFileService&) = delete;
  ~RangeFileService() override;

  // Creates and the RangeFileService. Can only be called if no RangeFileService
  // instance exists in the current process. The current process' instance can
  // be retrieved with Get().
  static void Create(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory);

  // Get the RangeStore (if not present, returns nullptr).
  static RangeFileService* Get();

  void AddReceiver(mojo::PendingReceiver<mojom::RangeFileService> receiver);

 private:
  explicit RangeFileService(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory);

  void DownloadRangeFile();
  void OnSimpleLoaderComplete(std::optional<std::string> response_body);

  // mojom::RangeFileService:
  void GetFile(GetFileCallback callback) override;

  mojo::ReceiverSet<mojom::RangeFileService> receivers_;

  // Used for downloading the range file.
  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;
  std::unique_ptr<network::SimpleURLLoader> simple_url_loader_;

  // Initially has no value. When the range file is downloaded, its value is set
  // to either the response body, or an empty string if the download was not
  // successful.
  std::optional<std::string> file_;

  // Callbacks received from calls to GetFile() before the result of the range
  // file download is known. They are run when the result becomes known.
  std::vector<GetFileCallback> pending_get_file_callbacks_;

  std::mutex file_callbacks_mutex_;
};

}  // namespace thorzeel

#endif  // THORZEEL_RANGE_FILE_SERVICE_H_
