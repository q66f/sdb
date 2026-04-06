use anyhow::{Result, ensure};
use flate2::{Decompress, FlushDecompress};
use memchr::memchr;
use memmap2::Mmap;
use std::fs::File;
use std::path::Path;

#[derive(Default, Clone)]
struct Cache {
    last_used: u64,
    chunk_idx: u32,
    data: Vec<u8>,
    d_len: usize,
    valid: bool,
}

pub struct Dictzip {
    file_map: Mmap,
    chunk_length: u32,
    chunk_count: u32,
    chunk_offsets: Vec<usize>,
    cache: [Cache; 4],
    access_counter: u64,
    z_stream: Decompress,
}

impl Dictzip {
    pub fn new(path: &Path) -> Result<Self> {
        let map = unsafe { Mmap::map(&File::open(path)?)? };
        ensure!(
            map.len() >= 18 && map.starts_with(b"\x1f\x8b") && map[3] & 0x04 != 0,
            "Invalid dictzip"
        );

        let get_u16 = |p| {
            map.get(p..p + 2)
                .and_then(|b| b.try_into().ok())
                .map(u16::from_le_bytes)
                .unwrap_or(0)
        };

        let mut pos = 12;
        let end = (pos + get_u16(10) as usize).min(map.len());
        let (mut clen, mut ccnt) = (0, 0);
        let mut csz = Vec::new();

        while pos + 4 <= end {
            let sublen = get_u16(pos + 2) as usize;
            if map.get(pos..pos + 2) == Some(b"RA") {
                clen = get_u16(pos + 6) as u32;
                ccnt = get_u16(pos + 8) as u32;
                csz = (0..ccnt as usize)
                    .map(|i| get_u16(pos + 10 + i * 2) as usize)
                    .collect();
                break;
            }
            pos += 4 + sublen;
        }

        ensure!(ccnt > 0, "Missing dictzip chunks");
        pos = end;

        if map[3] & 0x08 != 0 {
            pos += map
                .get(pos..)
                .and_then(|s| memchr(b'\0', s))
                .map_or(0, |p| p + 1);
        }
        if map[3] & 0x10 != 0 {
            pos += map
                .get(pos..)
                .and_then(|s| memchr(b'\0', s))
                .map_or(0, |p| p + 1);
        }
        if map[3] & 0x02 != 0 {
            pos += 2;
        }

        let mut coff = Vec::with_capacity(ccnt as usize + 1);
        coff.push(pos);
        for &sz in &csz {
            pos += sz;
            coff.push(pos);
        }

        Ok(Self {
            file_map: map,
            chunk_length: clen,
            chunk_count: ccnt,
            chunk_offsets: coff,
            cache: Default::default(),
            access_counter: 0,
            z_stream: Decompress::new(false),
        })
    }

    pub fn read(&mut self, mut buf: &mut [u8], mut seek: u64) -> usize {
        let mut read = 0;
        while !buf.is_empty() {
            let idx = (seek / self.chunk_length as u64) as u32;
            let off = (seek % self.chunk_length as u64) as usize;
            if idx >= self.chunk_count {
                break;
            }

            let (data, d_len) = if let Some(i) = self
                .cache
                .iter()
                .position(|c| c.valid && c.chunk_idx == idx)
            {
                self.access_counter += 1;
                self.cache[i].last_used = self.access_counter;
                (&self.cache[i].data, self.cache[i].d_len)
            } else {
                let lru = self
                    .cache
                    .iter()
                    .enumerate()
                    .min_by_key(|(_, c)| if c.valid { c.last_used } else { 0 })
                    .map(|(i, _)| i)
                    .unwrap();

                self.z_stream.reset(false);
                let entry = &mut self.cache[lru];
                entry.data.resize(self.chunk_length as usize, 0);

                let range = self.chunk_offsets[idx as usize]
                    ..self
                        .chunk_offsets
                        .get(idx as usize + 1)
                        .copied()
                        .unwrap_or(self.file_map.len());

                let _ = self.z_stream.decompress(
                    self.file_map.get(range).unwrap_or(&[]),
                    &mut entry.data,
                    FlushDecompress::Sync,
                );

                entry.d_len = self.z_stream.total_out() as usize;
                entry.valid = true;
                entry.chunk_idx = idx;
                self.access_counter += 1;
                entry.last_used = self.access_counter;
                (&entry.data, entry.d_len)
            };

            let copy = buf.len().min(d_len.saturating_sub(off));
            if copy == 0 {
                break;
            }

            buf[..copy].copy_from_slice(&data[off..off + copy]);
            buf = &mut buf[copy..];
            read += copy;
            seek += copy as u64;
        }
        read
    }
}
