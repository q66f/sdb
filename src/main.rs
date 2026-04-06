mod dictzip;
mod render;

use anyhow::Result;
use memchr::memchr;
use memmap2::Mmap;
use regex::bytes::{Regex, RegexBuilder};
use rustyline::{Config, Editor, history::DefaultHistory};
use std::cell::RefCell;
use std::cmp::Ordering;
use std::env;
use std::fs::File;
use std::io::{BufRead, BufReader, BufWriter, IsTerminal, Write, stdin, stdout};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use dictzip::Dictzip;
use render::render_html_to;

const LATIN1_FOLD: &[u8; 64] =
    b"AAAAAAACEEEEIIIIDNOOOOO\0OUUUUY\0\0aaaaaaaceeeeiiiidnooooo\0ouuuuy\0y";
const ENTR_PER_PAGE: u32 = 32;
const OFT_HEADER_STR: &[u8; 30] = b"StarDict's Cache, Version: 0.2";
const OFT_MAGIC: u32 = 0x51a4d1c1;
const OFT_HEADER_SIZE: usize = 34;

fn try_mmap_file(path: &Path) -> Result<Mmap, std::io::Error> {
    unsafe { Mmap::map(&File::open(path)?) }
}

#[derive(Default)]
struct ConfigData {
    accurate_accent: bool,
    strict_case: bool,
    unicode_lower: bool,
    list_only: bool,
    use_regex: bool,
    disable_html: bool,
    is_tty: bool,
    is_stdin_tty: bool,
    base_dir: Option<PathBuf>,
    filter_dicts: Vec<String>,
    patterns: Vec<String>,
}

struct IndexEntry<'a> {
    word: &'a [u8],
    ptr: usize,
}

#[derive(Clone)]
struct SearchResult {
    dict_idx: usize,
    matched_word: String,
    real_word: String,
    offset: u64,
    size: u32,
}

#[derive(Default)]
struct Index {
    mapping: Option<Mmap>,
    page_offsets: Vec<u32>,
    word_count: u32,
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
            && om.len() == expected_size && om.modified()? >= im.modified()? {
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

    fn load(&mut self, path: &Path, count: u32, trailer_size: u8) -> Result<()> {
        self.mapping = Some(try_mmap_file(path)?);
        self.word_count = count;
        self.trailer_size = trailer_size;
        if count == 0 {
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
            let n = (2 + (count - 1) / ENTR_PER_PAGE) as usize;
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
        let get_u64 = |p| {
            map.get(p..p + 8)
                .and_then(|b| b.try_into().ok())
                .map(u64::from_be_bytes)
                .unwrap_or(0)
        };
        if self.trailer_size == 12 {
            (get_u64(ptr), get_u32(ptr + 8))
        } else {
            (get_u32(ptr) as u64, get_u32(ptr + 4))
        }
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
struct Dictionary {
    bookname: String,
    idx: Index,
    syn: Index,
    data: DictData,
    sametypesequence: String,
}

impl Dictionary {
    fn load(path: &Path, cfg: &ConfigData) -> Result<Option<Self>> {
        let mut d = Dictionary::default();
        let (mut wc, mut sc, mut is_64) = (0, 0, false);

        let file = File::open(path)?;
        let reader = BufReader::new(file);

        for line in reader.lines().map_while(std::result::Result::ok) {
            if let Some((k, v)) = line.split_once('=') {
                match k.trim() {
                    "bookname" => d.bookname = v.trim().to_string(),
                    "wordcount" => wc = v.trim().parse().unwrap_or(0),
                    "synwordcount" => sc = v.trim().parse().unwrap_or(0),
                    "idxoffsetbits" => is_64 = v.trim() == "64",
                    "sametypesequence" => d.sametypesequence = v.trim().to_string(),
                    _ => {}
                }
            }
        }
        if !cfg.filter_dicts.is_empty() && !cfg.filter_dicts.contains(&d.bookname) {
            return Ok(None);
        }
        if cfg.list_only {
            d.idx.word_count = wc;
            return Ok(Some(d));
        }

        let b = path.with_extension("");
        d.idx
            .load(&b.with_extension("idx"), wc, if is_64 { 12 } else { 8 })?;

        if sc > 0 {
            let _ = d.syn.load(&b.with_extension("syn"), sc, 4);
        }
        d.data = Dictzip::new(&b.with_extension("dict.dz"))
            .map(|dz| DictData::Dz(RefCell::new(dz)))
            .or_else(|_| try_mmap_file(&b.with_extension("dict")).map(DictData::Raw))
            .unwrap_or(DictData::None);

        Ok(Some(d))
    }

    fn lookup_exact(
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

    fn lookup_prefix(&self, target: &str, dict_idx: usize, cfg: &ConfigData) -> Vec<SearchResult> {
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

    fn print(&self, res: &SearchResult, cfg: &ConfigData, out: &mut dyn Write) {
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

fn compare(a: &[u8], b: &[u8], strict: bool) -> Ordering {
    if strict {
        return a.cmp(b);
    }
    a.iter()
        .zip(b)
        .find_map(|(ca, cb)| {
            let (la, lb) = (ca.to_ascii_lowercase(), cb.to_ascii_lowercase());
            (la != lb).then(|| la.cmp(&lb))
        })
        .unwrap_or_else(|| a.len().cmp(&b.len()))
}

fn starts_with_ignore_case(val: &[u8], prefix: &[u8], strict: bool) -> bool {
    val.len() >= prefix.len() && compare(&val[..prefix.len()], prefix, strict) == Ordering::Equal
}

fn normalize_word(word: &str, cfg: &ConfigData) -> String {
    if cfg.accurate_accent && !cfg.unicode_lower {
        return word.into();
    }
    let mut out = String::with_capacity(word.len());
    for mut c in word.chars() {
        if !cfg.accurate_accent && (0xc0..=0xff).contains(&(c as u32)) {
            let f = LATIN1_FOLD[(c as u32 - 0xc0) as usize];
            if f != 0 {
                c = f as char;
            }
        }
        if cfg.unicode_lower {
            out.extend(c.to_lowercase());
        } else {
            out.push(c);
        }
    }
    out
}

fn usage() -> ! {
    eprintln!(
        "usage: sdb [-c dir] [-d dict] [-alrsuD] [--] [pattern]\n\
         \x20   -c dir  Use dictionaries in given dir.\n\
         \x20   -d dict Use the given dictionary.\n\
         \x20   -a      Accurate accent matching.\n\
         \x20   -l      List available dictionaries.\n\
         \x20   -r      Use regex matching.\n\
         \x20   -s      Strict case matching.\n\
         \x20   -u      Unicode case lowercasing.\n\
         \x20   -D      Disable html rendering.\n\
         \x20   --      Stop option parsing."
    );
    std::process::exit(1);
}

fn parse_args() -> ConfigData {
    let mut cfg = ConfigData {
        is_tty: stdout().is_terminal(),
        is_stdin_tty: stdin().is_terminal(),
        ..Default::default()
    };
    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        if arg == "--" {
            cfg.patterns.extend(args);
            break;
        }
        if let Some(opts) = arg.strip_prefix('-') {
            let mut chars = opts.chars();
            while let Some(c) = chars.next() {
                match c {
                    'a' => cfg.accurate_accent = true,
                    'l' => cfg.list_only = true,
                    'r' => cfg.use_regex = true,
                    's' => cfg.strict_case = true,
                    'u' => cfg.unicode_lower = true,
                    'D' => cfg.disable_html = true,
                    'c' | 'd' => {
                        let val = if !chars.as_str().is_empty() {
                            chars.as_str().to_string()
                        } else {
                            args.next().unwrap_or_else(|| usage())
                        };
                        if c == 'c' {
                            cfg.base_dir = Some(PathBuf::from(val));
                        } else {
                            cfg.filter_dicts.push(val);
                        }
                        break;
                    }
                    _ => usage(),
                }
            }
        } else {
            cfg.patterns.push(arg);
        }
    }
    cfg
}

fn parse_selection(input: &str, max: usize) -> Vec<usize> {
    let mut sel = Vec::new();
    for part in input.split(|c: char| c.is_whitespace() || c == ',') {
        if part.is_empty() {
            continue;
        }
        if let Some((s_str, e_str)) = part.split_once('-') {
            if let (Ok(s), Ok(e)) = (s_str.parse::<usize>(), e_str.parse::<usize>()) {
                for i in s..=e.min(max.saturating_sub(1)) {
                    sel.push(i);
                }
            }
        } else if let Ok(n) = part.parse::<usize>()
            && n < max {
                sel.push(n);
            }
    }
    sel
}

fn main() -> Result<()> {
    let cfg = parse_args();
    let paths = cfg.base_dir.clone().map(|b| vec![b]).unwrap_or_else(|| {
        let mut p = Vec::new();
        if let Ok(dir) = env::var("STARDICT_DATA_DIR") {
            p.push(PathBuf::from(dir).join("dic"));
        } else {
            if let Ok(xdg) = env::var("XDG_DATA_HOME") {
                p.push(PathBuf::from(xdg).join("stardict/dic"));
            } else if let Ok(h) = env::var("HOME") {
                p.push(PathBuf::from(h).join(".local/share/stardict/dic"));
            }
            if let Ok(dirs) = env::var("XDG_DATA_DIRS") {
                p.extend(
                    dirs.split(':')
                        .filter(|s| !s.is_empty())
                        .map(|s| PathBuf::from(s).join("stardict/dic")),
                );
            } else {
                p.extend([PathBuf::from("/usr/share/stardict/dic")]);
            }
        }
        p
    });

    let mut dicts = Vec::new();
    let mut stack: Vec<_> = paths.into_iter().filter(|p| p.exists()).collect();
    while let Some(path) = stack.pop() {
        if path.is_dir() {
            if let Ok(e) = std::fs::read_dir(path) {
                stack.extend(e.flatten().map(|e| e.path()));
            }
        } else if path.extension().is_some_and(|e| e == "ifo")
            && let Ok(Some(d)) = Dictionary::load(&path, &cfg) {
                dicts.push(d);
            }
    }

    if cfg.list_only {
        for d in &dicts {
            println!("{0}\t(words: {1})", d.bookname, d.idx.word_count);
        }
        return Ok(());
    }

    let hist_size = env::var("SDCV_HISTSIZE")
        .ok()
        .and_then(|s| s.parse::<usize>().ok())
        .unwrap_or(2000);

    let hist_file = env::var_os("SDCV_HISTFILE")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            let xdg_data = env::var_os("XDG_DATA_HOME")
                .map(PathBuf::from)
                .unwrap_or_else(|| {
                    PathBuf::from(env::var_os("HOME").unwrap_or_default()).join(".local/share")
                });
            xdg_data.join("sdcv_history")
        });

    let mut rl = if cfg.is_stdin_tty && cfg.is_tty {
        let rl_config = Config::builder()
            .max_history_size(hist_size)
            .unwrap_or_default()
            .build();
        let mut editor = Editor::<(), DefaultHistory>::with_config(rl_config).ok();
        if let Some(e) = editor.as_mut() {
            let _ = e.load_history(&hist_file);
        }
        editor
    } else {
        None
    };

    let mut pattern_iter = cfg.patterns.iter();

    macro_rules! render {
        ($iter:expr) => {{
            let mut pager = if cfg.is_tty {
                let pager_cmd = env::var("SDCV_PAGER")
                    .or_else(|_| env::var("PAGER"))
                    .unwrap_or_else(|_| "less -R -F -X".to_string());
                Command::new("sh")
                    .arg("-c")
                    .arg(&pager_cmd)
                    .stdin(Stdio::piped())
                    .spawn()
                    .ok()
            } else {
                None
            };
            {
                let mut out: Box<dyn Write> = match pager.as_mut() {
                    Some(child) => Box::new(child.stdin.take().unwrap()),
                    None => Box::new(stdout().lock()),
                };
                for res in $iter {
                    dicts[res.dict_idx].print(res, &cfg, &mut *out);
                }
                let _ = out.flush();
            }
            if let Some(mut child) = pager {
                let _ = child.wait();
            }
        }};
    }

    loop {
        let w = if let Some(p) = pattern_iter.next() {
            p.to_string()
        } else if cfg.patterns.is_empty() {
            if let Some(editor) = rl.as_mut() {
                match editor.readline("> ") {
                    Ok(line) => {
                        let trimmed = line.trim();
                        if trimmed.is_empty() {
                            continue;
                        }
                        let _ = editor.add_history_entry(trimmed);
                        trimmed.to_string()
                    }
                    _ => break,
                }
            } else {
                let mut input = String::new();
                if stdin().read_line(&mut input).unwrap_or(0) == 0 {
                    break;
                }
                let trimmed = input.trim();
                if trimmed.is_empty() {
                    continue;
                }
                trimmed.to_string()
            }
        } else {
            break;
        };

        let p = if cfg.use_regex {
            w.clone()
        } else {
            normalize_word(&w, &cfg)
        };

        let re = if cfg.use_regex {
            match RegexBuilder::new(&p)
                .case_insensitive(!cfg.strict_case)
                .build()
            {
                Ok(r) => Some(r),
                Err(e) => {
                    eprintln!("Invalid regex: {e}");
                    continue;
                }
            }
        } else {
            None
        };

        let mut all_results = Vec::new();
        let mut is_exact_pass = true;

        if let Some(re_val) = re.as_ref() {
            for (idx, d) in dicts.iter().enumerate() {
                all_results.extend(d.lookup_exact(&p, Some(re_val), idx, &cfg));
            }
            is_exact_pass = false;
        } else {
            for (idx, d) in dicts.iter().enumerate() {
                all_results.extend(d.lookup_exact(&p, None, idx, &cfg));
            }

            if all_results.is_empty() && p.len() > 1 {
                is_exact_pass = false;
                for (idx, d) in dicts.iter().enumerate() {
                    all_results.extend(d.lookup_prefix(&p, idx, &cfg));
                }
            }
        }

        if all_results.is_empty() {
            if cfg.is_tty {
                println!("\x1b[1;31mNo matches found for '{w}'\x1b[0m");
            }
            continue;
        }

        if (is_exact_pass && re.is_none()) || (!is_exact_pass && all_results.len() < 4) {
            render!(all_results.iter());
        } else if cfg.is_stdin_tty && cfg.is_tty {
            for (i, res) in all_results.iter().enumerate() {
                let display = if res.matched_word == res.real_word {
                    res.matched_word.clone()
                } else {
                    format!("{} -> {}", res.matched_word, res.real_word)
                };
                println!("{:2}: [{}] {}", i, dicts[res.dict_idx].bookname, display);
            }

            let choice_input = if let Some(editor) = rl.as_mut() {
                editor
                    .readline("Select (e.g. 1 3, 0-5, 'a'): ")
                    .unwrap_or_default()
            } else {
                let mut ci = String::new();
                let _ = stdin().read_line(&mut ci);
                ci
            };

            let s = choice_input.trim();
            if s == "a" {
                render!(all_results.iter());
            } else {
                let sel = parse_selection(s, all_results.len());
                if !sel.is_empty() {
                    render!(sel.into_iter().map(|i| &all_results[i]));
                }
            }
        } else {
            render!(all_results.iter());
        }
    }

    if let Some(editor) = rl.as_mut() {
        if let Some(parent) = hist_file.parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        let _ = editor.save_history(&hist_file);
    }

    Ok(())
}
