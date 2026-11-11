//! NVIDIA GPU figures through NVML (the NVML half of the C bar's
//! `sysmon/sysmon_client.c`; `docs/modules/sysmon.md` has the reasoning).
//!
//! NVML is the library `nvidia-smi` itself is a front end for. Calling it in
//! process replaced running `nvidia-smi` every 5 s, which blocked the event
//! loop (`test_no_subprocess.py` guards that). The `nvml-wrapper` crate loads
//! `libnvidia-ml.so.1` at runtime, so the bar still starts without the
//! driver: the GPU box then just shows `--`. The crate also uses the
//! `_v2` memory call, whose `used` matches `nvidia-smi` (v1 also counts ~300 MiB
//! of driver-reserved memory), and the `_v3` process queries.
//!
//! **Threads:** NVML starts a couple of idle threads of its own. They inherit
//! the blocked SIGUSR1 because the signal source is set up first in `main`;
//! without that, the power-menu keybind killed the C bar (BUG.md).
//!
//! **Cost:** NVML pulls in `libcuda` and its buffers, about 25 MB more
//! resident memory. That was judged worth it in the C bar for the CPU and latency win.
//!
//! - `poll` (every 5 s): only what the bar box shows, usage and temperature
//!   (~0.01 ms each).
//! - `refresh_details` (dropdown only): VRAM, fan, power, P-state, clocks,
//!   encoder/decoder and the processes. The fan query alone took 0.8-5.3 ms
//!   per call, ~90 % of what a poll would cost, so none of it runs while
//!   the dropdown is closed.

use std::fs;

use nvml_wrapper::{
    Device, Nvml,
    enum_wrappers::device::{Clock, PerformanceState, TemperatureSensor},
    enums::device::UsedGpuMemory,
    struct_wrappers::device::ProcessInfo,
};

/// How many apps the dropdown lists.
pub(crate) const MAX_PROCS: usize = 5;

/// Bytes per MiB.
const MIB: u64 = 1024 * 1024;

/// Everything known about the GPU.
#[derive(Default)]
pub(crate) struct Gpu {
    /// The loaded library; `None` if NVML isn't usable this run.
    nvml: Option<Nvml>,
    /// At least one poll succeeded: the figures below are real.
    pub(crate) available: bool,
    /// Usage % (kernel time over the last sample period).
    pub(crate) percent: u32,
    /// Die temperature, °C.
    pub(crate) temp_c: u32,
    /// e.g. `NVIDIA GeForce RTX 3080` (read once).
    pub(crate) name: String,
    /// e.g. "580.95.05" (read once).
    pub(crate) driver: String,
    /// The enforced power limit, W (0 = unknown; read once).
    pub(crate) power_limit_w: f64,
    /// The max graphics clock, MHz (0 = unknown; read once).
    pub(crate) clock_max_mhz: u32,
    /// VRAM in use, MiB. Dropdown only.
    pub(crate) mem_used_mb: u64,
    /// VRAM size, MiB. Dropdown only.
    pub(crate) mem_total_mb: u64,
    /// Fan speed, %. Dropdown only.
    pub(crate) fan_percent: u32,
    /// Power draw, W. Dropdown only.
    pub(crate) power_w: f64,
    /// Performance state 0-15 (P0 = max performance), if known. Dropdown only.
    pub(crate) pstate: Option<u8>,
    /// Current graphics clock, MHz. Dropdown only.
    pub(crate) clock_mhz: u32,
    /// Current memory clock, MHz. Dropdown only.
    pub(crate) mem_clock_mhz: u32,
    /// NVENC load, %. Dropdown only.
    pub(crate) encoder_percent: u32,
    /// NVDEC load, %. Dropdown only.
    pub(crate) decoder_percent: u32,
    /// The biggest VRAM users: (name, MiB), largest first. Dropdown only.
    pub(crate) procs: Vec<(String, u64)>,
}

impl std::fmt::Debug for Gpu {
    /// `Nvml` has no `Debug`, so it's shown as loaded or not.
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Gpu")
            .field("nvml_loaded", &self.nvml.is_some())
            .field("available", &self.available)
            .field("percent", &self.percent)
            .field("temp_c", &self.temp_c)
            .finish_non_exhaustive()
    }
}

impl Gpu {
    /// Loads NVML and reads the static facts. If anything is missing, says so
    /// once on stderr and leaves the GPU unavailable for this run.
    pub(crate) fn new() -> Self {
        let mut gpu = Self::default();
        let nvml = match Nvml::init() {
            Ok(nvml) => nvml,
            Err(err) => {
                warn(&format!("NVML unavailable ({err}; no NVIDIA driver?)"));
                return gpu;
            }
        };
        let Ok(device) = nvml.device_by_index(0) else {
            warn("NVML found no GPU at index 0");
            return gpu;
        };
        // All optional: a missing value just leaves that field empty.
        gpu.name = device.name().unwrap_or_default();
        gpu.driver = nvml.sys_driver_version().unwrap_or_default();
        gpu.power_limit_w = device
            .enforced_power_limit()
            .map_or(0.0, |mw| f64::from(mw) / 1000.0);
        gpu.clock_max_mhz = device.max_clock_info(Clock::Graphics).unwrap_or(0);
        gpu.nvml = Some(nvml);
        gpu
    }

    /// The device handle (looked up each time: a `Device` borrows the
    /// `Nvml`, so it can't be stored next to it; the lookup is cheap).
    fn device(&self) -> Option<Device<'_>> {
        self.nvml.as_ref()?.device_by_index(0).ok()
    }

    /// One 5 s poll: usage and temperature only.
    pub(crate) fn poll(&mut self) {
        let Some((percent, temp_c)) = self.device().and_then(|device| {
            let usage = device.utilization_rates().ok()?;
            Some((
                usage.gpu,
                device.temperature(TemperatureSensor::Gpu).unwrap_or(0),
            ))
        }) else {
            return;
        };
        self.percent = percent;
        self.temp_c = temp_c;
        self.available = true;
    }

    /// Fresh usage/temperature plus every dropdown-only figure, so the
    /// dropdown shows one consistent moment.
    pub(crate) fn refresh_details(&mut self) {
        self.poll();
        let Some(device) = self.device() else {
            return;
        };
        let fan = device.fan_speed(0).unwrap_or(0);
        let power_w = device
            .power_usage()
            .map_or(0.0, |mw| f64::from(mw) / 1000.0);
        let memory = device
            .memory_info()
            .ok()
            .map(|m| (m.used / MIB, m.total / MIB));
        let pstate = device.performance_state().ok().and_then(pstate_number);
        let clock = device.clock_info(Clock::Graphics).unwrap_or(0);
        let mem_clock = device.clock_info(Clock::Memory).unwrap_or(0);
        let encoder = device.encoder_utilization().map_or(0, |u| u.utilization);
        let decoder = device.decoder_utilization().map_or(0, |u| u.utilization);
        let graphics = device.running_graphics_processes().unwrap_or_default();
        let compute = device.running_compute_processes().unwrap_or_default();

        self.fan_percent = fan;
        self.power_w = power_w;
        if let Some((used, total)) = memory {
            self.mem_used_mb = used;
            self.mem_total_mb = total;
        }
        self.pstate = pstate;
        self.clock_mhz = clock;
        self.mem_clock_mhz = mem_clock;
        self.encoder_percent = encoder;
        self.decoder_percent = decoder;
        self.procs = top_processes(&graphics, &compute);
    }
}

/// Logs why the GPU module is unavailable (once: `new` runs once).
fn warn(why: &str) {
    eprintln!("stickBar: {why} -- GPU module will show \"unavailable\" for this run");
}

/// A performance state as its number (P0 = 0 ... P15 = 15).
fn pstate_number(state: PerformanceState) -> Option<u8> {
    use PerformanceState as P;
    match state {
        P::Zero => Some(0),
        P::One => Some(1),
        P::Two => Some(2),
        P::Three => Some(3),
        P::Four => Some(4),
        P::Five => Some(5),
        P::Six => Some(6),
        P::Seven => Some(7),
        P::Eight => Some(8),
        P::Nine => Some(9),
        P::Ten => Some(10),
        P::Eleven => Some(11),
        P::Twelve => Some(12),
        P::Thirteen => Some(13),
        P::Fourteen => Some(14),
        P::Fifteen => Some(15),
        P::Unknown => None,
    }
}

/// The two process lists merged by PID (an app doing graphics and compute is
/// in both, with the same figure: the larger is kept), as (PID, bytes).
/// "Not available" counts as 0.
fn merge_processes(graphics: &[ProcessInfo], compute: &[ProcessInfo]) -> Vec<(u32, u64)> {
    let mut merged: Vec<(u32, u64)> = Vec::new();
    for process in graphics.iter().chain(compute) {
        let used = match process.used_gpu_memory {
            UsedGpuMemory::Used(bytes) => bytes,
            UsedGpuMemory::Unavailable => 0,
        };
        match merged.iter_mut().find(|(pid, _)| *pid == process.pid) {
            Some((_, best)) => *best = (*best).max(used),
            None => merged.push((process.pid, used)),
        }
    }
    merged
}

/// The biggest VRAM users, named from `/proc/PID/comm` (so a browser's GPU
/// process reads "helium", not its command line), largest first.
fn top_processes(graphics: &[ProcessInfo], compute: &[ProcessInfo]) -> Vec<(String, u64)> {
    let mut merged = merge_processes(graphics, compute);
    merged.sort_by_key(|&(_, bytes)| std::cmp::Reverse(bytes));
    merged.truncate(MAX_PROCS);
    merged
        .into_iter()
        .map(|(pid, bytes)| {
            let name = fs::read_to_string(format!("/proc/{pid}/comm"))
                .map_or_else(|_| format!("pid {pid}"), |comm| comm.trim().to_owned());
            (name, bytes / MIB)
        })
        .collect()
}

/// The name without the brand: `NVIDIA GeForce RTX 3080` → `RTX 3080`.
pub(crate) fn short_name(name: &str) -> &str {
    let name = name.strip_prefix("NVIDIA ").unwrap_or(name);
    let name = name.strip_prefix("GeForce ").unwrap_or(name);
    if name.is_empty() { "NVIDIA GPU" } else { name }
}

#[cfg(test)]
mod tests {
    use nvml_wrapper::{enums::device::UsedGpuMemory, struct_wrappers::device::ProcessInfo};

    use super::{merge_processes, short_name};

    fn process(pid: u32, used: UsedGpuMemory) -> ProcessInfo {
        ProcessInfo {
            pid,
            used_gpu_memory: used,
            gpu_instance_id: None,
            compute_instance_id: None,
        }
    }

    #[test]
    fn processes_merge_by_pid_keeping_the_larger() {
        let graphics = [
            process(10, UsedGpuMemory::Used(300)),
            process(11, UsedGpuMemory::Unavailable),
        ];
        let compute = [
            process(10, UsedGpuMemory::Used(500)),
            process(12, UsedGpuMemory::Used(50)),
        ];
        assert_eq!(
            merge_processes(&graphics, &compute),
            vec![(10, 500), (11, 0), (12, 50)]
        );
    }

    #[test]
    fn names_lose_the_brand() {
        assert_eq!(short_name("NVIDIA GeForce RTX 3080"), "RTX 3080");
        assert_eq!(short_name("NVIDIA RTX A4000"), "RTX A4000");
        assert_eq!(short_name(""), "NVIDIA GPU");
    }
}
