use memmap2::Mmap;
use std::{cmp::Ordering, fs::File, path::Path};

pub fn try_mmap_file(path: &Path) -> Result<Mmap, std::io::Error> {
    unsafe { Mmap::map(&File::open(path)?) }
}

pub fn compare(a: &[u8], b: &[u8], strict: bool) -> Ordering {
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

pub fn starts_with_ignore_case(val: &[u8], prefix: &[u8], strict: bool) -> bool {
    val.len() >= prefix.len() && compare(&val[..prefix.len()], prefix, strict) == Ordering::Equal
}
