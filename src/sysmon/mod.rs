//! System figures for the CPU, GPU and memory modules (the C bar's
//! `sysmon/sysmon_client.c`; why each figure is read the way it is:
//! `docs/modules/sysmon.md`).
//!
//! No connections and no threads: every 5 s poll (`timer.rs`) re-reads small
//! `/proc` files from scratch, on the main loop. Each poll also pushes one
//! sample per history, so a dropdown's graph is already full when it opens.
//! Ported so far: memory (M7a). CPU and GPU join in M7b/M7c.

pub(crate) mod memory;
pub(crate) mod timer;

use crate::util::history::History2;
use memory::Memory;

/// Everything sysmon knows.
#[derive(Debug, Default)]
pub(crate) struct Sysmon {
    /// Memory figures.
    pub(crate) mem: Memory,
    /// Used % (a) and swap % (b), one sample per poll.
    pub(crate) mem_history: History2,
}

impl Sysmon {
    /// One 5 s poll: fresh figures, then one history sample each.
    pub(crate) fn poll(&mut self) {
        self.mem.poll();
        self.mem_history.push(
            f64::from(self.mem.percent),
            f64::from(self.mem.swap_percent),
        );
    }
}
