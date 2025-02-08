#include "thorzeel/range_store.h"

#include <limits>
#include <optional>
#include <string>

#include "base/functional/callback_forward.h"
#include "base/logging.h"
#include "url/gurl.h"

namespace thorzeel {

namespace {

// The global instance.
RangeStore* g_range_store = nullptr;

}  // namespace

std::size_t RangeStore::HostURLPairHash::operator()(
    const RangeStore::HostURLPair& p) const noexcept {
  std::size_t h1 = std::hash<std::string>{}(p.first);
  std::size_t h2 = std::hash<std::string>{}(p.second);
  return h1 ^ (h2 << 1);
}

// static
void RangeStore::Create(
    mojo::Remote<mojom::RangeFileService> range_file_service) {
  if (g_range_store) {
    LOG(ERROR) << "Create: RangeStore already exists";
    return;
  }
  g_range_store = new RangeStore(std::move(range_file_service));
}

// static
RangeStore* RangeStore::Get() {
  return g_range_store;
}

RangeStore::RangeStore(mojo::Remote<mojom::RangeFileService> range_file_service)
    : range_file_service_(std::move(range_file_service)) {
  range_file_service_->GetFile(
      base::BindOnce(&RangeStore::ParseRangeFile, base::Unretained(this)));
}

RangeStore::~RangeStore() = default;

void RangeStore::ParseRangeFile(const std::string& file) {
  LOG(INFO) << "ParseRangeFile: parsing file with size [" << file.size() << "]";
  if (file.empty()) {
    return;
  }

  std::istringstream file_stream(file);
  std::string line;

  while (std::getline(file_stream, line)) {
    // Tokenize the line.
    if (line.empty()) {
      continue;
    }

    std::istringstream line_stream(line);
    std::string requesting_host;
    line_stream >> requesting_host;
    std::string file_url;
    line_stream >> file_url;
    uint32_t content_length = 0;
    line_stream >> content_length;
    std::string range_header;
    // Ignore the ' ' after content_length.
    line_stream.ignore(std::numeric_limits<std::streamsize>::max(), ' ');
    std::getline(line_stream, range_header);

    if (requesting_host.empty() || file_url.empty() || content_length == 0 ||
        range_header.empty()) {
      LOG(ERROR) << "ParseRangeFile: Could not parse line [" << line << "]";
      return;
    }

    data_.emplace(std::make_pair(requesting_host, file_url),
                  std::make_tuple(range_header, content_length));
  }

  is_data_ready_ = true;
}

bool RangeStore::IsReady() const {
  return is_data_ready_;
}

std::optional<RangeStore::DataTuple> RangeStore::GetData(
    const std::string& requesting_host,
    const GURL& file_url) const {
  auto find_result =
      data_.find(std::make_pair(requesting_host, file_url.spec()));
  // LOG(INFO) << "GetData: requesting_host [" << requesting_host
  //           << "], file_url [" << file_url.spec() << "], did find ["
  //           << (find_result != data_.end()) << "]";
  if (find_result == data_.end()) {
    return std::nullopt;
  }
  return find_result->second;
}

}  // namespace thorzeel
