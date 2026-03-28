#pragma once
#include <algorithm>
#include <array>
#include <fcntl.h>
#include <span>
#include <stdexcept>
#include <sys/stat.h>
#include <cstring>
#include <unistd.h>
#include <vector>
#include <zlib-ng.h>

#include "types.hh"
#include "mmap.hh"

using namespace types;

namespace {
struct ZngStream {
    zng_stream strm{};

    ZngStream() {
        strm.zalloc = nullptr;
        strm.zfree = nullptr;
        strm.opaque = nullptr;
        if (zng_inflateInit2(&strm, -15) != Z_OK) {
            throw std::runtime_error("zng_inflateInit2 failed");
        }
    }

    ~ZngStream() { zng_inflateEnd(&strm); }

    ZngStream(const ZngStream&) = delete;
    ZngStream& operator=(const ZngStream&) = delete;

    void reset() { zng_inflateReset(&strm); }
};

struct Cache {
    u64 last_used{0};
    u32 chunk_idx{0};
    std::vector<u8> data;
    bool valid{false};
};

} // namespace

class Dictzip {
    static constexpr size_t Cachesets = 4;

    MmapView file_map;
    std::span<const u8> map;

    u32 chunk_length{0};
    u32 chunk_count{0};
    u32 uncompressed_size{0};

    std::vector<u64> chunk_offsets;
    std::vector<u16> chunk_sizes;

    std::array<Cache, Cachesets> cache;
    u64 access_counter{0};

    ZngStream z_stream;

    [[nodiscard]] std::span<const u8> get_chunk(u32 idx) {
        if (idx >= chunk_count) return {};

        for (auto& c : cache) {
            if (c.valid && c.chunk_idx == idx) {
                c.last_used = ++access_counter;
                u32 expected = (idx == chunk_count - 1)
                                        ? (uncompressed_size - idx * chunk_length)
                                        : chunk_length;
                return std::span{c.data}.first(expected);
            }
        }

        auto it = std::ranges::min_element(cache, std::less<>{},[](const Cache& c) {
            return c.valid ? c.last_used : 0;
        });

        z_stream.reset();
        z_stream.strm.next_in = map.data() + chunk_offsets[idx];
        z_stream.strm.avail_in = chunk_sizes[idx];
        z_stream.strm.next_out = it->data.data();
        z_stream.strm.avail_out = chunk_length;

        int ret = zng_inflate(&z_stream.strm, Z_SYNC_FLUSH);
        if (ret < 0 && ret != Z_BUF_ERROR) {
            throw std::runtime_error("zng_inflate failed");
        }

        it->valid = true;
        it->chunk_idx = idx;
        it->last_used = ++access_counter;

        return std::span{it->data}.first(z_stream.strm.total_out);
    }

    template <typename T>
    constexpr T read_le(std::span<const u8> src) {
        T val;
        std::memcpy(&val, src.data(), sizeof(T));
        if constexpr (std::endian::native == std::endian::big) {
            return std::byteswap(val);
        }
        return val;
    }

public:
    explicit Dictzip(const std::filesystem::path& path) : file_map(path) {
        map = file_map.span();

        if (map[0] != 0x1f || map[1] != 0x8b) {
            throw std::runtime_error("Not a valid gzip file");
        }

        u8 flg = map[3];
        if (!(flg & 0x04)) {
            throw std::runtime_error("Missing FEXTRA flag");
        }

        u32 pos = 10;
        u16 xlen = read_le<u16>(map.subspan(pos));
        pos += 2;

        u32 end = pos + xlen;
        bool ra_found = false;

        while (pos + 4 <= end) {
            u16 sublen = read_le<u16>(map.subspan(pos + 2));
            if (map[pos] == 'R' && map[pos + 1] == 'A') {
                chunk_length = read_le<u16>(map.subspan(pos + 6));
                chunk_count = read_le<u16>(map.subspan(pos + 8));
                
                chunk_sizes.resize(chunk_count);
                chunk_offsets.resize(chunk_count + 1);

                for (u32 i = 0; i < chunk_count; ++i) {
                    chunk_sizes[i] = read_le<u16>(map.subspan(pos + 10 + i * 2));
                }
                ra_found = true;
                break;
            }
            pos += 4 + sublen;
        }

        if (!ra_found) {
            throw std::runtime_error("Dictzip RA chunk not found");
        }

        pos = end;

        for (u8 m = 0x08; m <= 0x10; m <<= 1) {
            if (flg & m) {
                auto it = std::ranges::find(map.subspan(pos), 0);
                if (it == map.end()) throw std::runtime_error("Invalid null-terminated field");
                pos = static_cast<u32>(std::distance(map.begin(), it) + 1);
            }
        }
        if (flg & 0x02) pos += 2;

        chunk_offsets[0] = pos;
        for (u32 i = 0; i < chunk_count; ++i) {
            chunk_offsets[i + 1] = chunk_offsets[i] + chunk_sizes[i];
        }

        uncompressed_size = read_le<u32>(map.subspan(map.size() - 4));

        for (auto& c : cache) {
            c.data.resize(chunk_length);
        }
    }

    [[nodiscard]] size_t read(std::span<char> buf, u64 seek) {
        size_t bytes_read = 0;

        while (bytes_read < buf.size()) {
            u64 current_seek = seek + bytes_read;
            u32 chunk_idx = static_cast<u32>(current_seek / chunk_length);
            u32 chunk_offset = static_cast<u32>(current_seek % chunk_length);

            auto chunk_data = get_chunk(chunk_idx);
            if (chunk_data.empty() || chunk_offset >= chunk_data.size()) break;

            size_t available = chunk_data.size() - chunk_offset;
            size_t to_copy = std::min(buf.size() - bytes_read, available);

            std::ranges::copy_n(chunk_data.data() + chunk_offset, to_copy, reinterpret_cast<u8*>(buf.data()) + bytes_read);
            bytes_read += to_copy;
        }

        return bytes_read;
    }

    [[nodiscard]] u32 get_uncompressed_size() const noexcept {
        return uncompressed_size;
    }
};
