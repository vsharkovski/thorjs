#ifndef THORZEEL_RANGE_STORE_H_
#define THORZEEL_RANGE_STORE_H_

#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include "mojo/public/cpp/bindings/remote.h"
#include "thorzeel/mojom/range_file_service.mojom.h"

class GURL;

namespace network {
class SimpleURLLoader;
}  // namespace network

namespace thorzeel {

// Singleton
class RangeStore final {
 public:
  using DataTuple = std::tuple<std::string, uint32_t>;

  RangeStore(const RangeStore&) = delete;
  RangeStore& operator=(const RangeStore&) = delete;
  ~RangeStore();

  // Creates the RangeStore. Can only be called if no RangeStore instance
  // exists in the current process. The current process' instance can be
  // retrieved with Get().
  static void Create(mojo::Remote<mojom::RangeFileService> range_file_service);

  // Get the RangeStore (if not present, returns nullptr).
  static RangeStore* Get();

  bool IsReady() const;
  std::optional<DataTuple> GetData(const std::string& requesting_host,
                                   const GURL& file_url) const;

 private:
  using HostURLPair = std::pair<std::string, std::string>;

  struct HostURLPairHash {
    std::size_t operator()(const HostURLPair& p) const noexcept;
  };

  // Creates RangeStore. Only one is allowed.
  explicit RangeStore(mojo::Remote<mojom::RangeFileService> range_file_service);

  void ParseRangeFile(const std::string& file);

  mojo::Remote<mojom::RangeFileService> range_file_service_;
  std::unordered_map<HostURLPair, DataTuple, HostURLPairHash> data_;
  bool is_data_ready_ = false;
};

}  // namespace thorzeel

#endif  // THORZEEL_RANGE_STORE_H_
