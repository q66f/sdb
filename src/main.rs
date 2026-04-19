mod dictzip;
mod render;
mod stardict;
mod utils;

use anyhow::Result;
use regex::bytes::RegexBuilder;
use rustyline::{Config, Editor, history::DefaultHistory};
use std::env;
use std::io::Write;
use std::io::{IsTerminal, stdin, stdout};
use std::path::PathBuf;
use std::process::{Command, Stdio};

use crate::stardict::Dictionary;

const LATIN1_FOLD: &[u8; 64] =
    b"AAAAAAACEEEEIIIIDNOOOOO\0OUUUUY\0\0aaaaaaaceeeeiiiidnooooo\0ouuuuy\0y";

#[derive(Default)]
pub struct ConfigData {
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
         \x20 -c dir  Use dictionaries in given dir.\n\
         \x20 -d dict Use the given dictionary.\n\
         \x20 -a      Accurate accent matching.\n\
         \x20 -l      List available dictionaries.\n\
         \x20 -r      Use regex matching.\n\
         \x20 -s      Strict case matching.\n\
         \x20 -u      Unicode case lowercasing.\n\
         \x20 -D      Disable html rendering.\n\
         \x20 --      Stop option parsing."
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
            && n < max
        {
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
        } else if cfg!(unix) {
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
            && let Ok(Some(d)) = Dictionary::load(&path, &cfg)
        {
            dicts.push(d);
        }
    }

    if cfg.list_only {
        for d in &dicts {
            println!("{}\t(words: {})", d.bookname, d.idx.word_count);
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
