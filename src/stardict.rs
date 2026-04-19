use anyhow::Result;
use memchr::memchr;
use memmap2::Mmap;
use regex::bytes::Regex;
use std::cell::RefCell;
use std::cmp::Ordering;
use std::env;
use std::fs::File;
use std::io::{BufRead, BufReader, BufWriter, Write};
use std::path::{Path, PathBuf};

use crate::ConfigData;
use crate::dictzip::Dictzip;
use crate::render::render_html_to;
use crate::utils::{compare, starts_with_ignore_case, try_mmap_file};

const ENTR_PER_PAGE: u32 = 32;
const OFT_HEADER_STR: &[u8; 30] = b"StarDict's Cache, Version: 0.2";
const OFT_MAGIC: u32 = 0x51a4d1c1;
const OFT_HEADER_SIZE: usize = 34;

struct IndexEntry<'a> {
    word: &'a [u8],
    ptr: usize,
}

#[derive(Clone)]
pub struct SearchResult {
    pub(crate) dict_idx: usize,
    pub(crate) matched_word: String,
    pub(crate) real_word: String,
    offset: u64,
    size: u32,
}

#[derive(Default)]
pub(crate) struct Index {
    mapping: Option<Mmap>,
    page_offsets: Vec<u32>,
    pub(crate) word_count: u32,
    trailer_size: u8,
}

impl Index {
    fn entries(&self) -> impl Iterator<Item = IndexEntry<'_>> {
        let map = self.mapping.as_deref().unwrap_or(&[]);
        let trailer = 1 + self.trailer_size as usize;
        let mut curr = 0;
        std::iter::from_fn(move || {
            if curr >= map.len() {
                return None;
            }
            let len = memchr(b'\0', &map[curr..]).unwrap_or(map.len() - curr);
            let entry = IndexEntry {
                word: &map[curr..curr + len],
                ptr: curr + len + 1,
            };
            curr += len + trailer;
            Some(entry)
        })
    }

    fn cache_dir_oft_path(idx_path: &Path) -> PathBuf {
        let base = env::var_os("XDG_CACHE_HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                PathBuf::from(env::var_os("HOME").unwrap_or_default()).join(".cache")
            });
        let name = idx_path.file_name().unwrap_or_default().to_string_lossy();
        base.join("sdcv").join(format!("{name}.oft"))
    }

    fn ensure_oft(&self, oft_path: &Path, idx_path: &Path) -> Result<()> {
        if self.word_count == 0 {
            return Ok(());
        }
        let n = 2 + (self.word_count - 1) / ENTR_PER_PAGE;
        let expected_size = OFT_HEADER_SIZE as u64 + (n as u64 * 4);

        if let (Ok(om), Ok(im)) = (oft_path.metadata(), idx_path.metadata())
            && om.len() == expected_size
            && om.modified()? >= im.modified()?
        {
            return Ok(());
        }

        if let Some(parent) = oft_path.parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        let mut f = BufWriter::new(File::create(oft_path)?);
        f.write_all(OFT_HEADER_STR)?;
        f.write_all(&OFT_MAGIC.to_ne_bytes())?;

        let map = self.mapping.as_deref().unwrap();
        let (mut curr, trailer) = (0, 1 + self.trailer_size as usize);
        for i in 0..self.word_count {
            if i % ENTR_PER_PAGE == 0 {
                f.write_all(&(curr as u32).to_ne_bytes())?;
            }
            let slice = map.get(curr..).unwrap_or(&[]);
            curr += memchr(b'\0', slice).unwrap_or(slice.len()) + trailer;
        }
        f.write_all(&(curr as u32).to_ne_bytes())?;
        Ok(f.flush()?)
    }

    fn load(&mut self, path: &Path, trailer_size: u8) -> Result<()> {
        self.mapping = Some(try_mmap_file(path)?);
        self.trailer_size = trailer_size;
        if self.word_count == 0 {
            return Ok(());
        }

        let ext = path.extension().and_then(|e| e.to_str()).unwrap_or("idx");
        let mut oft = path.with_extension(format!("{ext}.oft"));

        if self.ensure_oft(&oft, path).is_err() {
            let cache_oft = Self::cache_dir_oft_path(path);
            if self.ensure_oft(&cache_oft, path).is_ok() {
                oft = cache_oft;
            }
        }

        if let Ok(o) = try_mmap_file(&oft) {
            let n = (2 + (self.word_count - 1) / ENTR_PER_PAGE) as usize;
            if o.len() == OFT_HEADER_SIZE + (n * 4)
                && o.starts_with(OFT_HEADER_STR)
                && o.get(30..34)
                    .and_then(|b| b.try_into().ok())
                    .map(u32::from_ne_bytes)
                    == Some(OFT_MAGIC)
            {
                self.page_offsets = (0..n - 1)
                    .map(|i| {
                        let p = OFT_HEADER_SIZE + i * 4;
                        o.get(p..p + 4)
                            .and_then(|b| b.try_into().ok())
                            .map(u32::from_ne_bytes)
                            .unwrap_or(0)
                    })
                    .collect();
                return Ok(());
            }
        }
        self.page_offsets = self
            .entries()
            .step_by(ENTR_PER_PAGE as usize)
            .map(|e| e.ptr as u32 - (e.word.len() as u32 + 1))
            .collect();
        Ok(())
    }

    fn read_entry(&self, ptr: usize) -> (u64, u32) {
        let map = self.mapping.as_deref().unwrap_or(&[]);
        let get_u32 = |p| {
            map.get(p..p + 4)
                .and_then(|b| b.try_into().ok())
                .map(u32::from_be_bytes)
                .unwrap_or(0)
        };
        (get_u32(ptr) as u64, get_u32(ptr + 4))
    }

    fn syn_offset(&self, ptr: usize) -> u64 {
        self.mapping
            .as_deref()
            .and_then(|m| m.get(ptr..ptr + 4))
            .and_then(|b| b.try_into().ok())
            .map(u32::from_be_bytes)
            .unwrap_or(0) as u64
    }

    fn entries_from(&self, n: u32) -> impl Iterator<Item = IndexEntry<'_>> {
        let page_idx = (n / ENTR_PER_PAGE) as usize;
        let remaining = (n % ENTR_PER_PAGE) as usize;
        let map = self.mapping.as_deref().unwrap_or(&[]);
        let mut curr = self.page_offsets.get(page_idx).copied().unwrap_or(0) as usize;
        let trailer = 1 + self.trailer_size as usize;
        let mut it = std::iter::from_fn(move || {
            if curr >= map.len() {
                return None;
            }
            let len = memchr(b'\0', &map[curr..]).unwrap_or(map.len() - curr);
            let entry = IndexEntry {
                word: &map[curr..curr + len],
                ptr: curr + len + 1,
            };
            curr += len + trailer;
            Some(entry)
        });
        if remaining > 0 {
            it.nth(remaining - 1);
        }
        it
    }
}

#[derive(Default)]
enum DictData {
    #[default]
    None,
    Raw(Mmap),
    Dz(RefCell<Dictzip>),
}

#[derive(Default)]
pub struct Dictionary {
    pub(crate) bookname: String,
    pub(crate) idx: Index,
    syn: Index,
    data: DictData,
    sametypesequence: String,
}

impl Dictionary {
    pub fn load(path: &Path, cfg: &ConfigData) -> Result<Option<Self>> {
        let mut d = Dictionary::default();

        let file = File::open(path)?;
        let reader = BufReader::new(file);

        for line in reader.lines().map_while(std::result::Result::ok) {
            if let Some((k, v)) = line.split_once('=') {
                match k.trim() {
                    "bookname" => d.bookname = v.trim().to_string(),
                    "wordcount" => d.idx.word_count = v.trim().parse().unwrap_or(0),
                    "synwordcount" => d.syn.word_count = v.trim().parse().unwrap_or(0),
                    "sametypesequence" => d.sametypesequence = v.trim().to_string(),
                    _ => {}
                }
            }
        }
        if !cfg.filter_dicts.is_empty() && !cfg.filter_dicts.contains(&d.bookname) {
            return Ok(None);
        }
        if cfg.list_only {
            return Ok(Some(d));
        }

        let b = path.with_extension("");
        d.idx.load(&b.with_extension("idx"), 8)?;

        if d.syn.word_count > 0 {
            let _ = d.syn.load(&b.with_extension("syn"), 4);
        }
        d.data = Dictzip::new(&b.with_extension("dict.dz"))
            .map(|dz| DictData::Dz(RefCell::new(dz)))
            .or_else(|_| try_mmap_file(&b.with_extension("dict")).map(DictData::Raw))
            .unwrap_or(DictData::None);

        Ok(Some(d))
    }

    pub(crate) fn lookup_exact(
        &self,
        target: &str,
        re: Option<&Regex>,
        dict_idx: usize,
        cfg: &ConfigData,
    ) -> Vec<SearchResult> {
        let mut res = self.search_in_index(target, re, false, false, dict_idx, cfg);
        if self.syn.word_count > 0 {
            res.extend(self.search_in_index(target, re, true, false, dict_idx, cfg));
        }
        res
    }

    pub(crate) fn lookup_prefix(
        &self,
        target: &str,
        dict_idx: usize,
        cfg: &ConfigData,
    ) -> Vec<SearchResult> {
        let mut res = self.search_in_index(target, None, false, true, dict_idx, cfg);
        if self.syn.word_count > 0 {
            res.extend(self.search_in_index(target, None, true, true, dict_idx, cfg));
        }
        res
    }

    fn search_in_index(
        &self,
        target: &str,
        re: Option<&Regex>,
        is_syn: bool,
        is_prefix: bool,
        dict_idx: usize,
        cfg: &ConfigData,
    ) -> Vec<SearchResult> {
        let index = if is_syn { &self.syn } else { &self.idx };
        let mut results = Vec::new();

        if let Some(regex) = re {
            for e in index.entries().filter(|e| regex.is_match(e.word)).take(100) {
                if is_syn {
                    if let Some(re) = self.idx.entries_from(index.syn_offset(e.ptr) as u32).next() {
                        let (o, s) = self.idx.read_entry(re.ptr);
                        results.push(SearchResult {
                            dict_idx,
                            matched_word: String::from_utf8_lossy(e.word).into(),
                            real_word: String::from_utf8_lossy(re.word).into(),
                            offset: o,
                            size: s,
                        });
                    }
                } else {
                    let (o, s) = index.read_entry(e.ptr);
                    results.push(SearchResult {
                        dict_idx,
                        matched_word: String::from_utf8_lossy(e.word).into(),
                        real_word: String::from_utf8_lossy(e.word).into(),
                        offset: o,
                        size: s,
                    });
                }
            }
        } else {
            let tb = target.as_bytes();
            let it = index.page_offsets.partition_point(|&off| {
                let w = index
                    .mapping
                    .as_deref()
                    .and_then(|m| m.get(off as usize..))
                    .unwrap_or(&[]);
                compare(tb, &w[..memchr(b'\0', w).unwrap_or(0)], cfg.strict_case) != Ordering::Less
            });

            let start_idx = (it.saturating_sub(1) * ENTR_PER_PAGE as usize) as u32;

            for e in index.entries_from(start_idx) {
                let ord = compare(tb, e.word, cfg.strict_case);
                if is_prefix {
                    if starts_with_ignore_case(e.word, tb, cfg.strict_case) {
                        if is_syn {
                            if let Some(re) =
                                self.idx.entries_from(index.syn_offset(e.ptr) as u32).next()
                            {
                                let (o, s) = self.idx.read_entry(re.ptr);
                                results.push(SearchResult {
                                    dict_idx,
                                    matched_word: String::from_utf8_lossy(e.word).into(),
                                    real_word: String::from_utf8_lossy(re.word).into(),
                                    offset: o,
                                    size: s,
                                });
                            }
                        } else {
                            let (o, s) = index.read_entry(e.ptr);
                            results.push(SearchResult {
                                dict_idx,
                                matched_word: String::from_utf8_lossy(e.word).into(),
                                real_word: String::from_utf8_lossy(e.word).into(),
                                offset: o,
                                size: s,
                            });
                        }
                        if results.len() > 30 {
                            break;
                        }
                    } else if ord == Ordering::Less {
                        break;
                    }
                } else if ord == Ordering::Equal {
                    if is_syn {
                        if let Some(re) =
                            self.idx.entries_from(index.syn_offset(e.ptr) as u32).next()
                        {
                            let (o, s) = self.idx.read_entry(re.ptr);
                            results.push(SearchResult {
                                dict_idx,
                                matched_word: String::from_utf8_lossy(e.word).into(),
                                real_word: String::from_utf8_lossy(re.word).into(),
                                offset: o,
                                size: s,
                            });
                        }
                    } else {
                        let (o, s) = index.read_entry(e.ptr);
                        results.push(SearchResult {
                            dict_idx,
                            matched_word: String::from_utf8_lossy(e.word).into(),
                            real_word: String::from_utf8_lossy(e.word).into(),
                            offset: o,
                            size: s,
                        });
                    }
                } else if ord == Ordering::Less && e.word.cmp(tb) == Ordering::Greater {
                    break;
                }
            }
        }
        results
    }

    pub(crate) fn print(&self, res: &SearchResult, cfg: &ConfigData, out: &mut dyn Write) {
        let mut buf = vec![0; res.size as usize];
        let content = match &self.data {
            DictData::Dz(dz) if dz.borrow_mut().read(&mut buf, res.offset) == res.size as usize => {
                &buf[..]
            }
            DictData::Raw(r) => r
                .get(res.offset as usize..res.offset as usize + res.size as usize)
                .unwrap_or(&[]),
            _ => return,
        };

        let (cyan, reset) = if cfg.is_tty {
            ("\x1b[1;36m", "\x1b[0m")
        } else {
            ("", "")
        };

        if res.matched_word == res.real_word || res.real_word.is_empty() {
            let _ = writeln!(
                out,
                "{cyan}--- {}[{}] ---{reset}",
                self.bookname, res.matched_word
            );
        } else {
            let _ = writeln!(
                out,
                "{cyan}--- {}[{} -> {}] ---{reset}",
                self.bookname, res.matched_word, res.real_word
            );
        }

        for (typ, seg) in (Segments {
            data: content,
            seq: self.sametypesequence.as_bytes(),
            use_seq: !self.sametypesequence.is_empty(),
        }) {
            if typ.is_ascii_lowercase() {
                if !cfg.disable_html && (typ == b'h' || typ == b'x') {
                    render_html_to(seg, cfg.is_tty, out);
                } else {
                    let _ = out.write_all(seg);
                    if !seg.ends_with(b"\n") {
                        let _ = out.write_all(b"\n");
                    }
                }
            } else {
                let _ = writeln!(
                    out,
                    "[binary format: '{}', {} bytes]",
                    typ as char,
                    seg.len()
                );
            }
        }
        let _ = writeln!(out);
    }
}

struct Segments<'a> {
    data: &'a [u8],
    seq: &'a [u8],
    use_seq: bool,
}

impl<'a> Iterator for Segments<'a> {
    type Item = (u8, &'a [u8]);
    fn next(&mut self) -> Option<Self::Item> {
        if self.data.is_empty() {
            return None;
        }
        let (typ, is_last) = if self.use_seq {
            let (&t, rest) = self.seq.split_first()?;
            self.seq = rest;
            (t, rest.is_empty())
        } else {
            let (&t, rest) = self.data.split_first()?;
            self.data = rest;
            (t, false)
        };
        let (seg, next_data) = if typ.is_ascii_lowercase() {
            let pos = if is_last {
                self.data.iter().rposition(|&b| b != 0).map_or(0, |i| i + 1)
            } else {
                memchr(b'\0', self.data).unwrap_or(self.data.len())
            };
            (
                &self.data[..pos],
                self.data
                    .get(pos + if is_last { 0 } else { 1 }..)
                    .unwrap_or(&[]),
            )
        } else if typ.is_ascii_uppercase() {
            let sz = self
                .data
                .get(..4)
                .and_then(|b| b.try_into().ok())
                .map(u32::from_be_bytes)
                .unwrap_or(0) as usize;
            let sz = sz.min(self.data.len().saturating_sub(4));
            (
                &self.data[4..4 + sz],
                self.data.get(4 + sz..).unwrap_or(&[]),
            )
        } else {
            (self.data, [].as_slice())
        };
        self.data = next_data;
        Some((typ, seg))
    }
}
