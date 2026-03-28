#include <algorithm>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <ranges>
#include <span>
#include <strings.h>
#include <vector>

#include "types.hh"
#include "utf/utf.h"
#include "mmap.hh"
#include "dictzip.hh"

using namespace types;
namespace fs = std::filesystem;

constexpr u32 ENTR_PER_PAGE = 64;

template <typename CharT>
inline size_t get_str_len(const CharT* ptr, size_t max_len) {
    std::span s{ptr, max_len};
    auto it = std::ranges::find(s, CharT{0});
    return std::ranges::distance(s.begin(), it);
}

class Index {
public:
    MmapView mapping;
    std::vector<u32> page_offsets;
    u32 word_count = 0;
    u8 trailer_size = 8;

    bool load(const std::string& path, u32 count, u8 ts) {
        auto res = MmapView::map_file(path);
        if (!res) return false;
        mapping = std::move(*res);
        word_count = count;
        trailer_size = ts;
        build_page_offsets();
        return true;
    }

private:
    void build_page_offsets() {
        auto span = mapping.span();
        const u8* data = span.data();
        const u8* end = data + span.size();
        const u8* curr = data;

        page_offsets.reserve(word_count / ENTR_PER_PAGE + 1);

        for (u32 i = 0; i < word_count && curr < end; ++i) {
            if (i % ENTR_PER_PAGE == 0) {
                page_offsets.push_back(static_cast<u32>(curr - data));
            }
            curr += get_str_len(curr, std::distance(curr, end)) + 1 + trailer_size;
        }
    }
};

struct Config {
    bool accurate_accent = false;
    bool strict_case = false;
    bool unicode_lower = false;
    bool list_only = false;
    fs::path base_dir;
    std::vector<std::string> filter_dicts;
};

class Dictionary {
public:
    std::string bookname;
    Index idx;
    Index syn;
    MmapView dict_raw;
    std::unique_ptr<Dictzip> dz;
    bool is_compressed = false;
    std::string sametypesequence;

    Dictionary() = default;

    static std::expected<Dictionary, std::string> load(const fs::path& ifo_path) {
        Dictionary d;
        std::ifstream ifo(ifo_path);
        if (!ifo) return std::unexpected("Cannot open .ifo");

        std::string line;
        fs::path base = ifo_path;
        base.replace_extension("");

        u32 wc = 0, sc = 0;
        bool is_64 = false;

        while (std::getline(ifo, line)) {
            auto pos = line.find('=');
            if (pos == std::string::npos) continue;
            
            std::string_view sv = line;
            auto key = sv.substr(0, pos);
            auto val = sv.substr(pos + 1);
            if (val.ends_with('\r')) val.remove_suffix(1);

            if (key == "bookname") d.bookname = val;
            else if (key == "wordcount") std::from_chars(val.data(), val.data() + val.size(), wc);
            else if (key == "synwordcount") std::from_chars(val.data(), val.data() + val.size(), sc);
            else if (key == "idxoffsetbits") is_64 = (val == "64");
            else if (key == "sametypesequence") d.sametypesequence = val;
        }

        if (!d.idx.load(base.string() + ".idx", wc, is_64 ? 12 : 8)) return std::unexpected("No .idx");
        if (sc > 0) d.syn.load(base.string() + ".syn", sc, 4);

        if (auto m = MmapView::map_file(base.string() + ".dict")) {
            d.dict_raw = std::move(*m);
        } else if (std::string dz_path = base.string() + ".dict.dz"; fs::exists(dz_path)) {
            try {
                d.dz = std::make_unique<Dictzip>(dz_path.c_str());
                d.is_compressed = true;
            } catch (const std::exception &e) {
                return std::unexpected(std::string("Dictzip error: ") + e.what());
            }
        }
        return d;
    }

    void lookup(std::string_view target, const Config& cfg) const {
        search_in_index(target, idx, false, cfg);
        if (syn.word_count > 0) search_in_index(target, syn, true, cfg);
    }

private:
    template<typename T>
    static T read_be(const u8* p) {
        T v;
        std::memcpy(&v, p, sizeof(T));
        return std::endian::native == std::endian::little ? std::byteswap(v) : v;
    }

    void search_in_index(std::string_view target, const Index& index, bool is_syn, const Config& cfg) const {
        if (index.page_offsets.empty()) return;

        auto proj = [&](u32 offset) {
            const char* entry = reinterpret_cast<const char*>(index.mapping.span().data() + offset);
            return std::string_view(entry, get_str_len(entry, index.mapping.size() - offset));
        };

        auto comp = [&](std::string_view a, std::string_view b) {
            if (cfg.strict_case) return a < b;
            size_t min_len = std::min(a.size(), b.size());
            int c = strncasecmp(a.data(), b.data(), min_len);
            return c == 0 ? a.size() < b.size() : c < 0;
        };

        auto it = std::ranges::upper_bound(index.page_offsets, target, comp, proj);

        size_t low = (it == index.page_offsets.begin()) ? 0 : std::distance(index.page_offsets.begin(), it) - 1;

        const u8* curr = index.mapping.span().data() + index.page_offsets[low];
        const u8* end = index.mapping.span().data() + index.mapping.size();

        while (curr < end) {
            size_t word_len = get_str_len(curr, end - curr);
            std::string_view entry_word(reinterpret_cast<const char*>(curr), word_len);
            
            int cmp = 0;
            if (cfg.strict_case) {
                cmp = target.compare(entry_word);
            } else {
                size_t min_len = std::min(target.size(), entry_word.size());
                cmp = strncasecmp(target.data(), entry_word.data(), min_len);
                if (cmp == 0) {
                    if (target.size() < entry_word.size()) cmp = -1;
                    else if (target.size() > entry_word.size()) cmp = 1;
                }
            }

            if (cmp == 0) {
                const u8* data_ptr = curr + word_len + 1;
                if (data_ptr + (index.trailer_size == 12 ? 12 : 8) > end) break;

                if (is_syn) {
                    resolve_real_word(entry_word, read_be<u32>(data_ptr));
                } else {
                    u64 off = (index.trailer_size == 12) ? read_be<u64>(data_ptr) : read_be<u32>(data_ptr);
                    u32 sz = read_be<u32>(data_ptr + (index.trailer_size == 12 ? 8 : 4));
                    print_content(entry_word, entry_word, off, sz);
                }
            } else if (cmp < 0) break;
            curr += word_len + 1 + index.trailer_size;
        }
    }

    void resolve_real_word(std::string_view matched_syn, u32 n) const {
        u32 page = n / ENTR_PER_PAGE;
        u32 skip = n % ENTR_PER_PAGE;
        if (page >= idx.page_offsets.size()) return;

        auto span = idx.mapping.span();
        const u8* ptr = span.data() + idx.page_offsets[page];
        const u8* end = span.data() + span.size();

        while (skip-- && ptr < end) {
            ptr += get_str_len(ptr, end - ptr) + 1 + idx.trailer_size;
        }
        if (ptr >= end) return;

        size_t word_len = get_str_len(ptr, end - ptr);
        std::string_view real_word(reinterpret_cast<const char*>(ptr), word_len);
        
        const u8* tr = ptr + word_len + 1;
        if (tr + (idx.trailer_size == 12 ? 12 : 8) > end) return;

        u64 off = (idx.trailer_size == 12) ? read_be<u64>(tr) : read_be<u32>(tr);
        u32 sz = read_be<u32>(tr + (idx.trailer_size == 12 ? 8 : 4));
        print_content(matched_syn, real_word, off, sz);
    }

    void print_content(std::string_view matched_word, std::string_view real_word, u64 off, u32 sz) const {
        const char* output_ptr = nullptr;
        std::vector<char> buffer;

        if (is_compressed) {
            buffer.resize(sz);
            if (dz->read(buffer, off) != sz) return;
            output_ptr = buffer.data();
        } else {
            if (off + sz > dict_raw.size()) return;
            output_ptr = reinterpret_cast<const char*>(dict_raw.span().data() + off);
        }

        if (matched_word == real_word || real_word.empty()) {
            std::println("--- {} [{}] ---", bookname, matched_word);
        } else {
            std::println("--- {}[{} -> {}] ---", bookname, matched_word, real_word);
        }
        
        const char* p = output_ptr;
        const char* end = p + sz;

        auto print_segment =[](char type, std::string_view data) {
            if (std::isalpha(type)) std::println("{}", data);
            else std::println("[binary data: type '{}', {} bytes]", type, data.size());
        };

        auto next_segment =[](char type, bool is_last, const char*& p, const char* end) -> std::string_view {
            const char* seg_data = p;
            size_t seg_sz = 0;
            if (std::isalpha(type)) {
                if (is_last) {
                    seg_sz = end - p;
                    while (seg_sz > 0 && p[seg_sz - 1] == '\0') seg_sz--; // '\0'
                    p = end;
                } else {
                    std::span s{p, static_cast<size_t>(end - p)};
                    auto it = std::ranges::find(s, '\0');
                    seg_sz = std::ranges::distance(s.begin(), it);
                    p = (it == s.end()) ? end : p + seg_sz + 1;
                }
            } else {
                if (is_last) {
                    seg_sz = end - p;
                    p = end;
                } else {
                    if (end - p >= 4) {
                        seg_sz = read_be<u32>(reinterpret_cast<const u8*>(p));
                        p += 4;
                        seg_data = p;
                        seg_sz = std::min<size_t>(seg_sz, end - p);
                        p += seg_sz;
                    } else {
                        seg_sz = end - p;
                        p = end;
                    }
                }
            }
            return {seg_data, seg_sz};
        };

        if (!sametypesequence.empty()) {
            for (auto [i, type] : std::views::enumerate(sametypesequence)) {
                if (p >= end) break;
                bool is_last = (static_cast<size_t>(i) == sametypesequence.size() - 1);
                print_segment(type, next_segment(type, is_last, p, end));
            }
        } else {
            while (p < end) {
                char type = *p++;
                print_segment(type, next_segment(type, false, p, end));
            }
        }
        std::println("");
    }
};

static void normalize_word(std::string& word, const Config& cfg) {
    if (cfg.accurate_accent && !cfg.unicode_lower) return;

    static constexpr char latin1_fold[] = {
        'A','A','A','A','A','A','A','C','E','E','E','E','I','I','I','I',
        'D','N','O','O','O','O','O', 0 ,'O','U','U','U','U','Y', 0 , 0 ,
        'a','a','a','a','a','a','a','c','e','e','e','e','i','i','i','i',
        'd','n','o','o','o','o','o', 0 ,'o','u','u','u','u','y', 0 ,'y',
    };

    std::string processed;
    processed.reserve(word.size());
    const char* p = word.data();
    Rune r;

    while (*p) {
        int n = chartorune(&r, p);
        if (!cfg.accurate_accent && r >= 0xc0 && r <= 0xff && latin1_fold[r - 0xc0]) {
            r = latin1_fold[r - 0xc0];
        }
        if (cfg.unicode_lower) {
            r = tolowerrune(r);
        }
        char buf[10];
        int ln = runetochar(buf, &r);
        processed.append(buf, ln);
        p += n;
    }
    word = std::move(processed);
}

[[noreturn]] static void usage() {
    std::println(stderr,
        "usage: sdb[-c dir] [-d dict] [-alsu] [--] [pattern]\n"
        "    -c dir  Use dictionaries in given dir.\n"
        "    -d dict Use the given dictionary.\n"
        "    -a      Accurate accent matching.\n"
        "    -l      List available dictionaries.\n"
        "    -s      Strict case matching.\n"
        "    -u      Unicode case lowercasing.\n"
        "    --      Stop option parsing.");
    std::exit(1);
}

int main(int argc, char** argv) {
    std::span args{argv, static_cast<size_t>(argc)};
    Config cfg;
    size_t i = 1;

    for (; i < args.size() && args[i][0] == '-'; ++i) {
        std::string_view arg = args[i];
        if (arg == "--") { i++; break; }
        if (arg.size() < 2) usage();

        for (size_t j = 1; j < arg.size(); ++j) {
            char c = arg[j];
            if (c == 'a') cfg.accurate_accent = true;
            else if (c == 'l') cfg.list_only = true;
            else if (c == 's') cfg.strict_case = true;
            else if (c == 'u') cfg.unicode_lower = true;
            else if (c == 'c' || c == 'd') {
                std::string_view val = (j + 1 < arg.size()) ? arg.substr(j + 1) : (++i < args.size() ? args[i] : (usage(), ""));
                if (c == 'c') cfg.base_dir = val;
                else cfg.filter_dicts.emplace_back(val);
                break;
            } else {
                usage();
            }
        }
    }

    std::vector<fs::path> paths;
    if (!cfg.base_dir.empty()) {
        paths.push_back(cfg.base_dir);
    } else {
        if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
            paths.push_back(fs::path(xdg) / "stardict/dic");
        } else if (const char* home = std::getenv("HOME")) {
            paths.push_back(fs::path(home) / ".local/share/stardict/dic");
        }
        paths.emplace_back("/usr/share/stardict/dic");
    }

    std::vector<Dictionary> dicts;
    for (const auto& p : paths) {
        if (!fs::exists(p)) continue;
        for (const auto& entry : fs::recursive_directory_iterator(p)) {
            if (entry.path().extension() == ".ifo") {
                if (auto d = Dictionary::load(entry.path())) {
                    bool keep = cfg.filter_dicts.empty() || std::ranges::any_of(cfg.filter_dicts, [&](std::string_view f) {
                        return d->bookname == f;
                    });
                    if (keep) dicts.push_back(std::move(*d));
                }
            }
        }
    }

    if (cfg.list_only) {
        for (const auto& d : dicts) {
            std::println("{}\t(words: {})", d.bookname, d.idx.word_count);
        }
        return 0;
    }

    auto query = [&](std::string_view w) {
        std::string proc(w);
        normalize_word(proc, cfg);
        for (const auto& d : dicts) d.lookup(proc, cfg);
    };

    if (i < args.size()) {
        for (; i < args.size(); i++) query(args[i]);
    } else {
        std::string line;
        while (std::getline(std::cin, line)) query(line);
    }
    return 0;
}
