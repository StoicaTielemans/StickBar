//! CPU figures (the CPU half of the C bar's `sysmon/sysmon_client.c`; the
//! reasoning behind each is in `docs/modules/sysmon.md`, "CPU dropdown").
//!
//! - `poll` (every 5 s): usage from `/proc/stat` jiffies deltas, overall and
//!   per thread, and the package temperature.
//! - `new` (once): the model, core count, max clock and P/E-core split.
//! - `refresh_details` (dropdown only): per-thread clocks, load average, task
//!   counts, uptime, and the top processes.

use std::{collections::HashMap, fs, time::Instant};

use super::memory::proc_comm;

/// How many processes the dropdown lists.
pub(crate) const MAX_PROCS: usize = 5;

/// Where the CPU package temperature is read (millidegrees). Zone 1 is the
/// package sensor (`x86_pkg_temp`) on this machine.
const TEMP_PATH: &str = "/sys/class/thermal/thermal_zone1/temp";

/// One `/proc/stat` cpu line's (idle, total) jiffies, from the previous poll.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct Jiffies {
    /// Idle jiffies: the 4th field only, so iowait counts as busy (the
    /// reference bar's `percentFromJiffies()`; a documented, kept quirk).
    idle: u64,
    /// The sum of the first 10 fields.
    total: u64,
}

/// Everything known about the CPU.
#[derive(Debug, Default)]
pub(crate) struct Cpu {
    /// Overall usage %, since the previous poll (0 on the very first).
    pub(crate) percent: f64,
    /// Package temperature, °C.
    pub(crate) temp_c: i32,
    /// Per-thread usage %, by thread number.
    pub(crate) cores: Vec<f64>,
    /// The aggregate line's jiffies from the previous poll.
    prev_total: Option<Jiffies>,
    /// Each thread line's jiffies from the previous poll.
    prev_cores: Vec<Option<Jiffies>>,
    /// e.g. "12th Gen Intel(R) Core(TM) i5-12600K" (read once).
    pub(crate) model: String,
    /// Physical cores (`cpu cores`), 0 if unknown (read once).
    pub(crate) physical_cores: u32,
    /// Max clock, GHz (0 if unknown; read once).
    pub(crate) max_ghz: f64,
    /// Which threads are E-cores, by thread number (read once).
    pub(crate) efficient: Vec<bool>,
    /// Whether the chip has both P- and E-cores (both sysfs lists exist).
    pub(crate) hybrid: bool,
    /// Per-thread current clock, GHz (0 = unknown). Dropdown only.
    pub(crate) core_ghz: Vec<f64>,
    /// Average clock from `/proc/cpuinfo`, only used when no thread has a
    /// cpufreq reading. Dropdown only.
    pub(crate) fallback_ghz: f64,
    /// "0.52 0.61 0.70" (1/5/15 min). Dropdown only.
    pub(crate) load: String,
    /// Running tasks. Dropdown only.
    pub(crate) tasks_running: u32,
    /// All tasks. Dropdown only.
    pub(crate) tasks_total: u32,
    /// Seconds since boot. Dropdown only.
    pub(crate) uptime_s: f64,
    /// Top processes by CPU %: (name, % of the whole CPU), largest first.
    pub(crate) procs: Vec<(String, f64)>,
    /// `procs` holds a real measurement (it takes two scans).
    pub(crate) procs_ready: bool,
    /// The previous scan's utime+stime per PID, and when it was taken.
    proc_sample: Option<(HashMap<u32, u64>, Instant)>,
}

impl Cpu {
    /// A CPU with its static facts read.
    pub(crate) fn new() -> Self {
        let mut cpu = Self {
            load: "0.00 0.00 0.00".to_owned(),
            ..Self::default()
        };
        if let Ok(info) = fs::read_to_string("/proc/cpuinfo") {
            cpuinfo_field(&info, "model name")
                .unwrap_or_default()
                .clone_into(&mut cpu.model);
            cpu.physical_cores = cpuinfo_field(&info, "cpu cores")
                .and_then(|v| v.parse().ok())
                .unwrap_or(0);
        }
        cpu.max_ghz = read_number("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq")
            .map_or(0.0, |khz| khz / 1e6);
        // Hybrid Intel chips list their P-core and E-core threads here;
        // neither file exists on AMD or older Intel (one "CORES" group then).
        let p_cores = fs::read_to_string("/sys/devices/cpu_core/cpus")
            .ok()
            .map(|t| parse_cpulist(&t));
        let e_cores = fs::read_to_string("/sys/devices/cpu_atom/cpus")
            .ok()
            .map(|t| parse_cpulist(&t));
        cpu.hybrid = p_cores.is_some() && e_cores.is_some();
        for thread in e_cores.unwrap_or_default() {
            if cpu.efficient.len() <= thread {
                cpu.efficient.resize(thread + 1, false);
            }
            if let Some(flag) = cpu.efficient.get_mut(thread) {
                *flag = true;
            }
        }
        cpu
    }

    /// Whether thread `index` is an E-core.
    pub(crate) fn is_efficient(&self, index: usize) -> bool {
        self.efficient.get(index).copied().unwrap_or(false)
    }

    /// One 5 s poll: usage since the previous poll, and the temperature.
    pub(crate) fn poll(&mut self) {
        if let Ok(stat) = fs::read_to_string("/proc/stat") {
            self.apply_stat(&stat);
        }
        if let Some(milli) = read_number(TEMP_PATH) {
            self.temp_c = crate::render::whole_px((milli / 1000.0).round());
        }
    }

    /// Updates usage from `/proc/stat`'s text: each cpu line diffed against
    /// the same line's previous sample.
    fn apply_stat(&mut self, stat: &str) {
        let mut cores = Vec::new();
        // The "cpu"/"cpuN" lines always come first, contiguously.
        for (label, now) in stat.lines().map_while(parse_stat_line) {
            if label == "cpu" {
                self.percent = usage(self.prev_total, now);
                self.prev_total = Some(now);
            } else {
                let index = cores.len();
                if self.prev_cores.len() <= index {
                    self.prev_cores.resize(index + 1, None);
                }
                let prev = self.prev_cores.get(index).copied().flatten();
                cores.push(usage(prev, now));
                if let Some(slot) = self.prev_cores.get_mut(index) {
                    *slot = Some(now);
                }
            }
        }
        self.cores = cores;
    }

    /// The dropdown-only figures: clocks, load, tasks, uptime, top processes.
    pub(crate) fn refresh_details(&mut self) {
        self.core_ghz = (0..self.cores.len())
            .map(|i| {
                read_number(&format!(
                    "/sys/devices/system/cpu/cpu{i}/cpufreq/scaling_cur_freq"
                ))
                .map_or(0.0, |khz| khz / 1e6)
            })
            .collect();
        let have_core_clocks = self.core_ghz.iter().any(|&ghz| ghz > 0.0);
        self.fallback_ghz = if have_core_clocks {
            0.0
        } else {
            cpuinfo_avg_ghz()
        };
        if let Some((load, running, total)) = fs::read_to_string("/proc/loadavg")
            .ok()
            .as_deref()
            .and_then(parse_loadavg)
        {
            self.load = load;
            self.tasks_running = running;
            self.tasks_total = total;
        } else {
            self.tasks_running = 0;
            self.tasks_total = 0;
        }
        self.uptime_s = fs::read_to_string("/proc/uptime")
            .ok()
            .and_then(|t| t.split_whitespace().next().and_then(|v| v.parse().ok()))
            .unwrap_or(0.0);
        self.sample_processes();
    }

    /// Forgets the previous process scan, so the next one only primes. The
    /// dropdown does this on open: without it, the first figures would average
    /// over however long it was closed.
    pub(crate) fn reset_process_sample(&mut self) {
        self.proc_sample = None;
        self.procs_ready = false;
        self.procs.clear();
    }

    /// One pass over every process: CPU ticks used since the previous pass,
    /// as % of the whole CPU, summed by name, top `MAX_PROCS`. The first pass
    /// after a reset only records ticks (a difference needs two).
    fn sample_processes(&mut self) {
        let now = Instant::now();
        let threads = f64::from(u32::try_from(self.cores.len().max(1)).unwrap_or(1));
        let ticks_per_s =
            f64::from(u32::try_from(rustix::param::clock_ticks_per_second()).unwrap_or(100));
        let previous = self.proc_sample.take();
        let denominator = previous.as_ref().map_or(0.0, |(_, then)| {
            now.duration_since(*then).as_secs_f64() * ticks_per_s * threads
        });

        let mut current = HashMap::new();
        let mut groups: Vec<(String, f64)> = Vec::new();
        for (pid, name, ticks) in all_process_ticks() {
            let _previous_entry: Option<u64> = current.insert(pid, ticks);
            let Some((old, _)) = &previous else {
                continue; // priming: nothing to diff against yet
            };
            let Some(&old_ticks) = old.get(&pid) else {
                continue; // new since the last scan
            };
            if ticks <= old_ticks || denominator <= 0.0 {
                continue; // idle
            }
            let used = f64::from(u32::try_from(ticks - old_ticks).unwrap_or(u32::MAX));
            let percent = used * 100.0 / denominator;
            match groups.iter_mut().find(|(n, _)| *n == name) {
                Some((_, total)) => *total += percent,
                None => groups.push((name, percent)),
            }
        }
        if previous.is_some() {
            // Largest first; a stable sort keeps first-seen order among equals.
            groups.sort_by(|a, b| b.1.total_cmp(&a.1));
            groups.truncate(MAX_PROCS);
            self.procs = groups;
            self.procs_ready = true;
        }
        self.proc_sample = Some((current, now));
    }
}

/// (pid, name, utime + stime) for every process that could be read.
fn all_process_ticks() -> Vec<(u32, String, u64)> {
    let Ok(entries) = fs::read_dir("/proc") else {
        return Vec::new();
    };
    entries
        .flatten()
        .filter_map(|entry| {
            let pid: u32 = entry.file_name().to_str()?.parse().ok()?;
            let stat = fs::read_to_string(format!("/proc/{pid}/stat")).ok()?;
            let name = proc_comm(&stat)?.to_owned();
            let ticks = stat_ticks(&stat)?;
            Some((pid, name, ticks))
        })
        .collect()
}

/// utime + stime from a `/proc/PID/stat` line: fields 14 and 15, counted
/// from after the `)` that ends the name (which may contain spaces).
fn stat_ticks(stat: &str) -> Option<u64> {
    let after_name = stat.get(stat.rfind(')')? + 1..)?;
    // After the name: state(3) ppid pgrp session tty tpgid flags minflt
    // cminflt majflt cmajflt utime(14) stime(15).
    let mut fields = after_name.split_whitespace().skip(11);
    let utime: u64 = fields.next()?.parse().ok()?;
    let stime: u64 = fields.next()?.parse().ok()?;
    Some(utime + stime)
}

/// A `/proc/stat` "cpu"/"cpuN" line as (label, jiffies), or `None` for any
/// other line (which ends the cpu block).
fn parse_stat_line(line: &str) -> Option<(&str, Jiffies)> {
    let mut parts = line.split_whitespace();
    let label = parts.next().filter(|l| l.starts_with("cpu"))?;
    let fields: Vec<u64> = parts.take(10).filter_map(|v| v.parse().ok()).collect();
    let idle = *fields.get(3)?;
    Some((
        label,
        Jiffies {
            idle,
            total: fields.iter().sum(),
        },
    ))
}

/// Busy % between two samples of one cpu line (0 without a previous one).
fn usage(prev: Option<Jiffies>, now: Jiffies) -> f64 {
    let Some(prev) = prev else {
        return 0.0;
    };
    let total = now.total.saturating_sub(prev.total);
    let idle = now.idle.saturating_sub(prev.idle);
    if total == 0 {
        return 0.0;
    }
    let to_f64 = |v: u64| f64::from(u32::try_from(v).unwrap_or(u32::MAX));
    to_f64(total.saturating_sub(idle)) * 100.0 / to_f64(total)
}

/// The kernel's cpulist format, e.g. "0-11,13" → [0..=11, 13].
fn parse_cpulist(text: &str) -> Vec<usize> {
    text.trim()
        .split(',')
        .filter_map(|part| {
            let (low, high) = part.split_once('-').unwrap_or((part, part));
            Some(low.trim().parse::<usize>().ok()?..=high.trim().parse::<usize>().ok()?)
        })
        .flatten()
        .collect()
}

/// A `key : value` field's value from `/proc/cpuinfo` (the first occurrence).
fn cpuinfo_field<'a>(info: &'a str, key: &str) -> Option<&'a str> {
    info.lines().find_map(|line| {
        let (name, value) = line.split_once(':')?;
        (name.trim() == key).then(|| value.trim())
    })
}

/// The average `cpu MHz` in `/proc/cpuinfo`, in GHz (0 if none). A fallback
/// for when per-thread cpufreq isn't exposed; it's 28 KB, so dropdown only.
fn cpuinfo_avg_ghz() -> f64 {
    let Ok(info) = fs::read_to_string("/proc/cpuinfo") else {
        return 0.0;
    };
    let mhz: Vec<f64> = info
        .lines()
        .filter(|line| line.starts_with("cpu MHz"))
        .filter_map(|line| line.split_once(':')?.1.trim().parse().ok())
        .collect();
    if mhz.is_empty() {
        return 0.0;
    }
    mhz.iter().sum::<f64>() / f64::from(u32::try_from(mhz.len()).unwrap_or(1)) / 1000.0
}

/// `/proc/loadavg` → ("0.52 0.61 0.70", running, total).
fn parse_loadavg(text: &str) -> Option<(String, u32, u32)> {
    let mut parts = text.split_whitespace();
    let loads: Vec<f64> = parts
        .by_ref()
        .take(3)
        .filter_map(|v| v.parse().ok())
        .collect();
    let (running, total) = parts.next()?.split_once('/')?;
    let [one, five, fifteen] = loads.as_slice() else {
        return None;
    };
    Some((
        format!("{one:.2} {five:.2} {fifteen:.2}"),
        running.parse().ok()?,
        total.parse().ok()?,
    ))
}

/// A file holding one number (e.g. a sysfs value), as f64.
fn read_number(path: &str) -> Option<f64> {
    fs::read_to_string(path).ok()?.trim().parse().ok()
}

/// The model without the marketing: "12th Gen Intel(R) Core(TM) i5-12600K" →
/// "i5-12600K"; "AMD Ryzen 7 5800X 8-Core Processor" → "AMD Ryzen 7 5800X".
pub(crate) fn short_model(model: &str) -> String {
    let mut short = match model.find("Core(TM) ") {
        Some(at) => model
            .get(at + "Core(TM) ".len()..)
            .unwrap_or(model)
            .to_owned(),
        None if model.is_empty() => "CPU".to_owned(),
        None => model.to_owned(),
    };
    if let Some(at) = short.find(" Processor") {
        short.truncate(at);
    }
    // Drop a trailing " N-Core" word.
    if let Some(at) = short.find("-Core") {
        let word_start = short.get(..at).and_then(|s| s.rfind(' ')).unwrap_or(0);
        if word_start > 0 {
            short.truncate(word_start);
        }
    }
    short
}

#[cfg(test)]
mod tests {
    use super::{
        Jiffies, parse_cpulist, parse_loadavg, parse_stat_line, short_model, stat_ticks, usage,
    };

    #[test]
    fn stat_lines_and_usage() {
        let (label, a) = parse_stat_line("cpu0 100 0 50 800 50 0 0 0 0 0").expect("cpu line");
        assert_eq!(
            (label, a),
            (
                "cpu0",
                Jiffies {
                    idle: 800,
                    total: 1000
                }
            )
        );
        assert!(parse_stat_line("intr 12345 0 0").is_none());
        let b = Jiffies {
            idle: 1600,
            total: 2000,
        }; // 800 idle of 1000 more
        assert!((usage(Some(a), b) - 20.0).abs() < 1e-9);
        assert_eq!(usage(None, b), 0.0); // the first poll reads 0
    }

    #[test]
    fn cpulists() {
        assert_eq!(parse_cpulist("12-15\n"), vec![12, 13, 14, 15]);
        assert_eq!(parse_cpulist("0-2,5"), vec![0, 1, 2, 5]);
    }

    #[test]
    fn process_ticks_skip_the_name() {
        let stat = "42 (my (odd) name) S 1 42 42 0 -1 4194304 100 0 0 0 250 35 0 0 20 0";
        assert_eq!(stat_ticks(stat), Some(285));
    }

    #[test]
    fn loadavg_and_models() {
        assert_eq!(
            parse_loadavg("0.52 0.61 0.70 2/1234 5678\n"),
            Some(("0.52 0.61 0.70".to_owned(), 2, 1234))
        );
        assert_eq!(
            short_model("12th Gen Intel(R) Core(TM) i5-12600K"),
            "i5-12600K"
        );
        assert_eq!(
            short_model("AMD Ryzen 7 5800X 8-Core Processor"),
            "AMD Ryzen 7 5800X"
        );
        assert_eq!(short_model(""), "CPU");
    }
}
