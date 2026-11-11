//! System figures for the CPU, GPU and memory modules (the C bar's
//! `sysmon/sysmon_client.c`; why each figure is read the way it is:
//! `docs/modules/sysmon.md`).
//!
//! No connections and no threads: every 5 s poll (`timer.rs`) re-reads small
//! `/proc` files from scratch, on the main loop. Each poll also pushes one
//! sample per history, so a dropdown's graph is already full when it opens.
//! Ported so far: memory (M7a) and CPU (M7b). GPU joins in M7c.

pub(crate) mod cpu;
pub(crate) mod memory;
pub(crate) mod timer;

use crate::util::history::History2;
use cpu::Cpu;
use memory::Memory;

/// Everything sysmon knows.
#[derive(Debug)]
pub(crate) struct Sysmon {
    /// CPU figures.
    pub(crate) cpu: Cpu,
    /// Usage % (a) and temperature °C (b), one sample per poll.
    pub(crate) cpu_history: History2,
    /// Memory figures.
    pub(crate) mem: Memory,
    /// Used % (a) and swap % (b), one sample per poll.
    pub(crate) mem_history: History2,
}

impl Sysmon {
    /// Reads the static facts (CPU model, cores, P/E split). No poll yet.
    pub(crate) fn new() -> Self {
        Self {
            cpu: Cpu::new(),
            cpu_history: History2::default(),
            mem: Memory::default(),
            mem_history: History2::default(),
        }
    }

    /// One 5 s poll: fresh figures, then one history sample each.
    pub(crate) fn poll(&mut self) {
        self.cpu.poll();
        self.mem.poll();
        self.cpu_history
            .push(self.cpu.percent, f64::from(self.cpu.temp_c));
        self.mem_history.push(
            f64::from(self.mem.percent),
            f64::from(self.mem.swap_percent),
        );
    }
}
