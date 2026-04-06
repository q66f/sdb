use std::io::Write;

fn decode_entity(ent: &str) -> Option<char> {
    match ent.trim().to_ascii_lowercase().as_str() {
        "lt" => Some('<'),
        "gt" => Some('>'),
        "amp" => Some('&'),
        "quot" => Some('"'),
        "apos" => Some('\''),
        "nbsp" => Some('\u{00A0}'),
        s if s.starts_with("#x") => u32::from_str_radix(&s[2..], 16)
            .ok()
            .and_then(char::from_u32),
        s if s.starts_with('#') => s[1..].parse().ok().and_then(char::from_u32),
        _ => None,
    }
}

struct Renderer<'a, W: Write + ?Sized> {
    out: &'a mut W,
    is_tty: bool,
    buffer: String,
    list_stack: Vec<i32>,
    indent: usize,
    newlines: usize,
    last_was_space: bool,
}

impl<'a, W: Write + ?Sized> Renderer<'a, W> {
    fn new(is_tty: bool, out: &'a mut W) -> Self {
        Self {
            out,
            is_tty,
            buffer: String::with_capacity(4096),
            list_stack: Vec::new(),
            indent: 0,
            newlines: 2,
            last_was_space: true,
        }
    }

    fn flush(&mut self) {
        if !self.buffer.is_empty() {
            let _ = self.out.write_all(self.buffer.as_bytes());
            self.buffer.clear();
        }
    }

    fn ensure_newline(&mut self, n: usize) {
        while self.newlines < n {
            self.buffer.push('\n');
            self.newlines += 1;
        }
        self.last_was_space = true;
    }

    fn push_char(&mut self, c: char) {
        if c.is_whitespace() {
            if !self.last_was_space {
                self.buffer.push(' ');
                self.last_was_space = true;
            }
        } else {
            if self.newlines > 0 {
                for _ in 0..self.indent {
                    self.buffer.push(' ');
                }
            }
            self.buffer.push(c);
            self.last_was_space = false;
            self.newlines = 0;
        }
        if self.buffer.len() >= 4000 {
            self.flush();
        }
    }

    fn handle_tag(&mut self, tag: &str) {
        let name = tag
            .split_whitespace()
            .next()
            .unwrap_or("")
            .trim_end_matches('/')
            .to_ascii_lowercase();

        match name.as_str() {
            "b" | "strong"
                if self.is_tty => {
                    self.buffer.push_str("\x1b[1m");
                }
            "/b" | "/strong"
                if self.is_tty => {
                    self.buffer.push_str("\x1b[22m");
                }
            "i" | "em"
                if self.is_tty => {
                    self.buffer.push_str("\x1b[3m");
                }
            "/i" | "/em"
                if self.is_tty => {
                    self.buffer.push_str("\x1b[23m");
                }
            "u"
                if self.is_tty => {
                    self.buffer.push_str("\x1b[4m");
                }
            "/u"
                if self.is_tty => {
                    self.buffer.push_str("\x1b[24m");
                }
            "p" | "div" | "h1" | "h2" | "h3" | "h4" | "h5" | "h6" | "blockquote" | "tr" => {
                self.ensure_newline(2);
            }
            "br" => self.ensure_newline(1),
            "ul" | "ol" => {
                self.ensure_newline(2);
                self.list_stack.push(if name == "ul" { -1 } else { 0 });
            }
            "/ul" | "/ol" => {
                self.list_stack.pop();
                self.indent = self.list_stack.len() * 3;
                self.ensure_newline(2);
            }
            "li" => {
                self.ensure_newline(1);
                if let Some(top) = self.list_stack.last_mut() {
                    let marker = if *top >= 0 {
                        *top += 1;
                        format!("{}. ", top)
                    } else {
                        "• ".to_string()
                    };
                    self.indent = (self.list_stack.len() - 1) * 3;
                    for _ in 0..self.indent {
                        self.buffer.push(' ');
                    }
                    self.buffer.push_str(&marker);

                    self.indent += marker.chars().count();
                    self.newlines = 0;
                    self.last_was_space = true;
                }
            }
            _ => {}
        }
    }
}

pub fn render_html_to<W: Write + ?Sized>(data: &[u8], is_tty: bool, out: &mut W) {
    let mut r = Renderer::new(is_tty, out);
    let s = String::from_utf8_lossy(data);
    let mut it = s.chars().peekable();

    while let Some(c) = it.next() {
        match c {
            '<' => {
                let mut t = String::new();
                let mut iq = false;
                while let Some(&n) = it.peek() {
                    it.next();
                    if n == '"' || n == '\'' {
                        iq = !iq;
                    }
                    if n == '>' && !iq {
                        break;
                    }
                    t.push(n);
                }
                r.handle_tag(&t);
            }
            '&' => {
                let mut e = String::new();
                while let Some(&n) = it.peek() {
                    it.next();
                    if n == ';' {
                        break;
                    }
                    e.push(n);
                }
                if let Some(d) = decode_entity(&e) {
                    r.push_char(d);
                }
            }
            _ => r.push_char(c),
        }
    }

    r.ensure_newline(1);
    if r.is_tty {
        r.buffer.push_str("\x1b[0m");
    }
    r.flush();
}
