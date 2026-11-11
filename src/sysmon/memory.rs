//! Memory figures (the memory half of the C bar's `sysmon/sysmon_client.c`;
//! the reasoning behind each figure is in `docs/modules/sysmon.md`).
//!
//! `poll` reads `/proc/meminfo` (every 5 s, for the bar box and the graph).
//! `refresh_details` adds what only the dropdown shows: pressure (PSI), zram,
//! and the top apps. It runs when the dropdown opens and on each poll while
//! it's open, never while it's closed.

use std::fs;

/// How many apps the dropdown lists.
pub(crate) const MAX_PROCS: usize = 5;

/// KiB per GiB.
const KIB_PER_GIB: f64 = 1024.0 * 1024.0;
/// Bytes per MiB.
const BYTES_PER_MIB: f64 = 1024.0 * 1024.0;

/// zram swap figures (from `/sys/block/zramN/`).
#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct Zram {
    /// The active compression algorithm (the `[bracketed]` one), e.g. "zstd".
    pub(crate) algorithm: String,
    /// Data stored, uncompressed, in MiB.
    pub(crate) orig_mb: f64,
    /// The same data compressed, in MiB.
    pub(crate) compr_mb: f64,
    /// RAM zram really uses (with its overhead), in MiB.
    pub(crate) used_mb: f64,
}

/// Everything known about memory.
#[derive(Debug, Default)]
pub(crate) struct Memory {
    /// Used % (total − available), what the bar box shows.
    pub(crate) percent: i32,
    /// What apps hold: total − `MemAvailable`, in GiB.
    pub(crate) used_gb: f64,
    /// `MemTotal`, in GiB.
    pub(crate) total_gb: f64,
    /// `MemAvailable`: free plus what the kernel can reclaim, in GiB.
    pub(crate) avail_gb: f64,
    /// `MemFree`: truly unused, usually small, in GiB.
    pub(crate) free_gb: f64,
    /// Shmem, in GiB.
    pub(crate) shared_gb: f64,
    /// Dirty (waiting to be written to disk), in MiB.
    pub(crate) dirty_mb: f64,
    /// Swap in use, in GiB.
    pub(crate) swap_used_gb: f64,
    /// Swap size, in GiB (0 = no swap).
    pub(crate) swap_total_gb: f64,
    /// Swap used %.
    pub(crate) swap_percent: i32,
    /// Memory pressure, the three `some` averages (10 s / 60 s / 5 min) in %;
    /// `None` on kernels without PSI. Dropdown only.
    pub(crate) psi: Option<[f64; 3]>,
    /// zram figures, if swap is on zram. Dropdown only.
    pub(crate) zram: Option<Zram>,
    /// The biggest apps by private memory: (name, GiB), largest first, at
    /// most `MAX_PROCS`. Dropdown only.
    pub(crate) procs: Vec<(String, f64)>,
}

impl Memory {
    /// Reclaimable cache: available − free, in GiB. With `used_gb` and
    /// `free_gb` it adds up to the total, which explains a tiny "free".
    pub(crate) fn cache_gb(&self) -> f64 {
        self.avail_gb - self.free_gb
    }

    /// Re-reads `/proc/meminfo`. On a read or parse failure the previous
    /// figures stay.
    pub(crate) fn poll(&mut self) {
        if let Ok(text) = fs::read_to_string("/proc/meminfo") {
            self.apply_meminfo(&text);
        }
    }

    /// Fills the totals from `/proc/meminfo`'s text.
    fn apply_meminfo(&mut self, text: &str) {
        let (Some(total_kb), Some(avail_kb)) = (
            meminfo_kb(text, "MemTotal:"),
            meminfo_kb(text, "MemAvailable:"),
        ) else {
            return;
        };
        if total_kb <= 0.0 {
            return;
        }
        let used_kb = total_kb - avail_kb;
        self.percent = round_i32(100.0 * used_kb / total_kb);
        self.used_gb = used_kb / KIB_PER_GIB;
        self.total_gb = total_kb / KIB_PER_GIB;
        self.avail_gb = avail_kb / KIB_PER_GIB;
        self.free_gb = meminfo_kb(text, "MemFree:").unwrap_or(0.0) / KIB_PER_GIB;
        self.shared_gb = meminfo_kb(text, "Shmem:").unwrap_or(0.0) / KIB_PER_GIB;
        self.dirty_mb = meminfo_kb(text, "Dirty:").unwrap_or(0.0) / 1024.0;
        if let (Some(swap_total_kb), Some(swap_free_kb)) = (
            meminfo_kb(text, "SwapTotal:"),
            meminfo_kb(text, "SwapFree:"),
        ) {
            let swap_used_kb = swap_total_kb - swap_free_kb;
            self.swap_total_gb = swap_total_kb / KIB_PER_GIB;
            self.swap_used_gb = swap_used_kb / KIB_PER_GIB;
            self.swap_percent = if swap_total_kb > 0.0 {
                round_i32(100.0 * swap_used_kb / swap_total_kb)
            } else {
                0
            };
        }
    }

    /// Fresh totals plus the dropdown-only details, so everything the
    /// dropdown shows is from one moment. History isn't touched.
    pub(crate) fn refresh_details(&mut self) {
        self.poll();
        self.psi = fs::read_to_string("/proc/pressure/memory")
            .ok()
            .and_then(|t| parse_psi(&t));
        self.zram = read_zram();
        self.procs = top_private_memory();
    }
}

/// Rounds half away from zero (like C's `lround`) and converts to `i32`.
fn round_i32(value: f64) -> i32 {
    crate::render::whole_px(value.round())
}

/// The number after `key` in `/proc/meminfo`'s text (in KiB), if present.
fn meminfo_kb(text: &str, key: &str) -> Option<f64> {
    let start = text.find(key)? + key.len();
    let rest = text.get(start..)?.trim_start();
    let digits: &str = rest.split(|c: char| !c.is_ascii_digit()).next()?;
    // Parsed straight to f64: exact for any count below 2^53, and it avoids
    // an integer -> float conversion (there's no lossless `From<u64>`).
    digits.parse::<f64>().ok()
}

/// `/proc/pressure/memory`'s first line: `some avg10=0.00 avg60=0.00 avg300=0.00 total=N`.
fn parse_psi(text: &str) -> Option<[f64; 3]> {
    let line = text.lines().find(|l| l.starts_with("some "))?;
    let value = |key: &str| {
        line.split_whitespace()
            .find_map(|field| field.strip_prefix(key))
            .and_then(|v| v.parse::<f64>().ok())
    };
    Some([value("avg10=")?, value("avg60=")?, value("avg300=")?])
}

/// The zram device swap is on, from `/proc/swaps` (e.g. "zram0"), if any.
fn zram_device(swaps: &str) -> Option<&str> {
    swaps.lines().find_map(|line| {
        let device = line.split_whitespace().next()?.strip_prefix("/dev/")?;
        device.starts_with("zram").then_some(device)
    })
}

/// zram figures, if swap is on a zram device whose stats can be read.
fn read_zram() -> Option<Zram> {
    let swaps = fs::read_to_string("/proc/swaps").ok()?;
    let device = zram_device(&swaps)?;
    let mm_stat = fs::read_to_string(format!("/sys/block/{device}/mm_stat")).ok()?;
    let mut fields = mm_stat.split_whitespace().map(|v| v.parse::<f64>().ok());
    let (Some(Some(orig)), Some(Some(compr)), Some(Some(used))) =
        (fields.next(), fields.next(), fields.next())
    else {
        return None;
    };
    let algorithms =
        fs::read_to_string(format!("/sys/block/{device}/comp_algorithm")).unwrap_or_default();
    Some(Zram {
        algorithm: active_algorithm(&algorithms).unwrap_or_default().to_owned(),
        orig_mb: orig / BYTES_PER_MIB,
        compr_mb: compr / BYTES_PER_MIB,
        used_mb: used / BYTES_PER_MIB,
    })
}

/// The `[bracketed]` entry of `comp_algorithm`, e.g. "lzo [zstd] lz4" → "zstd".
fn active_algorithm(list: &str) -> Option<&str> {
    let open = list.find('[')?;
    let rest = list.get(open + 1..)?;
    rest.get(..rest.find(']')?)
}

/// A process's name from its `/proc/PID/stat` text: from the first `(` to
/// the **last** `)`, since a name can itself contain either.
pub(crate) fn proc_comm(stat: &str) -> Option<&str> {
    let open = stat.find('(')?;
    let close = stat.rfind(')')?;
    stat.get(open + 1..close)
}

/// The biggest apps by private memory (statm resident − shared, i.e. what
/// only that process holds), summed by name so a browser's many processes are
/// one line. Private memory, not RSS or PSS: summed RSS counts shared
/// libraries once per process (7.0 GB for the browser here); PSS is exact but
/// took ~215 ms and other users' processes can't be read; private memory was
/// 3.5 GB in a few ms and readable for all (docs/modules/sysmon.md).
fn top_private_memory() -> Vec<(String, f64)> {
    let page_gib = f64::from(u32::try_from(rustix::param::page_size()).unwrap_or(4096))
        / (KIB_PER_GIB * 1024.0);
    let mut groups: Vec<(String, f64)> = Vec::new();
    let Ok(entries) = fs::read_dir("/proc") else {
        return groups;
    };
    for entry in entries.flatten() {
        let pid = entry.file_name();
        let Some(pid) = pid
            .to_str()
            .filter(|p| p.bytes().all(|b| b.is_ascii_digit()))
        else {
            continue;
        };
        let Some(private_pages) = fs::read_to_string(format!("/proc/{pid}/statm"))
            .ok()
            .and_then(|t| private_pages(&t))
        else {
            continue; // exited meanwhile, or a kernel thread (no memory of its own)
        };
        let Ok(stat) = fs::read_to_string(format!("/proc/{pid}/stat")) else {
            continue;
        };
        let Some(name) = proc_comm(&stat) else {
            continue;
        };
        let gib = private_pages * page_gib;
        match groups.iter_mut().find(|(n, _)| n == name) {
            Some((_, total)) => *total += gib,
            None => groups.push((name.to_owned(), gib)),
        }
    }
    // Largest first; a stable sort keeps first-seen order among equals, as C.
    groups.sort_by(|a, b| b.1.total_cmp(&a.1));
    groups.truncate(MAX_PROCS);
    groups
}

/// resident − shared pages from `/proc/PID/statm`, or `None` if there are
/// none (kernel threads) or the text doesn't parse.
fn private_pages(statm: &str) -> Option<f64> {
    let mut fields = statm
        .split_whitespace()
        .skip(1)
        .map(|v| v.parse::<f64>().ok());
    let (Some(Some(resident)), Some(Some(shared))) = (fields.next(), fields.next()) else {
        return None;
    };
    (resident > shared).then_some(resident - shared)
}

#[cfg(test)]
mod tests {
    use super::{Memory, active_algorithm, parse_psi, private_pages, proc_comm, zram_device};

    const MEMINFO: &str = "MemTotal:       32000000 kB\nMemFree:         2000000 kB\n\
                           MemAvailable:   20000000 kB\nShmem:            500000 kB\n\
                           Dirty:              2048 kB\nSwapTotal:       8000000 kB\nSwapFree:        6000000 kB\n";

    #[test]
    fn meminfo_figures_add_up() {
        let mut m = Memory::default();
        m.apply_meminfo(MEMINFO);
        assert_eq!(m.percent, 38); // 12/32 = 37.5 %, rounded half away from zero
        assert_eq!(m.swap_percent, 25);
        assert!((m.used_gb + m.cache_gb() + m.free_gb - m.total_gb).abs() < 1e-9);
        assert!((m.dirty_mb - 2.0).abs() < 1e-9);
    }

    #[test]
    fn psi_and_zram_parsing() {
        let psi = "some avg10=1.50 avg60=0.25 avg300=0.00 total=12\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=3\n";
        assert_eq!(parse_psi(psi), Some([1.5, 0.25, 0.0]));
        let swaps = "Filename\tType\tSize\tUsed\tPriority\n/dev/zram0  partition  8G  1G  100\n";
        assert_eq!(zram_device(swaps), Some("zram0"));
        assert_eq!(zram_device("Filename\n/swapfile file 8G 0 -2\n"), None);
        assert_eq!(active_algorithm("lzo lzo-rle lz4 [zstd]\n"), Some("zstd"));
    }

    #[test]
    fn process_names_and_private_memory() {
        assert_eq!(
            proc_comm("1234 (Web Content (x)) S 1 2 3"),
            Some("Web Content (x)")
        );
        assert_eq!(private_pages("1000 600 200 1 0 50 0"), Some(400.0));
        assert_eq!(private_pages("0 0 0 0 0 0 0"), None); // a kernel thread
    }
}
