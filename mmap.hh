#pragma once
#include <cerrno>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <span>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <stdexcept>

#include "types.hh"

using namespace types;

class MmapView {
  std::span<const u8> map_;

public:
  MmapView() = default;
  ~MmapView() {
    if (!map_.empty()) {
      ::munmap(const_cast<u8 *>(map_.data()), map_.size());
    }
  }

  MmapView(const MmapView &) = delete;
  MmapView &operator=(const MmapView &) = delete;

  MmapView(MmapView &&o) noexcept : map_(std::exchange(o.map_, {})) {}

  MmapView &operator=(MmapView &&o) noexcept {
    if (this != &o) {
      if (!map_.empty())
        ::munmap(const_cast<u8 *>(map_.data()), map_.size());
      map_ = std::exchange(o.map_, {});
    }
    return *this;
  }

  static std::expected<MmapView, int>
  map_file(const std::filesystem::path &path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      return std::unexpected(errno);

    struct stat st;
    if (::fstat(fd, &st) < 0 || st.st_size == 0) {
      ::close(fd);
      return std::unexpected(errno);
    }

    size_t fsize = static_cast<size_t>(st.st_size);
    void *ptr = ::mmap(nullptr, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);

    if (ptr == MAP_FAILED)
      return std::unexpected(errno);

    ::madvise(ptr, fsize, MADV_RANDOM);

    MmapView view;
    view.map_ = std::span{static_cast<const u8 *>(ptr), fsize};
    return view;
  }

  explicit MmapView(const std::filesystem::path &path) {
    auto res = map_file(path);
    if (!res)
      throw std::runtime_error("map failed: " + path.string());
    *this = std::move(*res);
  }

  [[nodiscard]] std::span<const u8> span() const noexcept { return map_; }
  [[nodiscard]] size_t size() const noexcept { return map_.size(); }
};
