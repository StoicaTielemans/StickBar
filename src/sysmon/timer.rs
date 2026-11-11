//! The 5 s sysmon poll (the C bar's `create_sysmon_timer()` in `main.c`),
//! matching the reference bar's `Timer { interval: 5000 }`. A plain
//! monotonic timer: unlike the clock, it doesn't need wall-clock alignment.

use std::time::Duration;

use calloop::{
    LoopHandle,
    timer::{TimeoutAction, Timer},
};

use crate::wayland::App;

/// Time between polls.
const INTERVAL: Duration = Duration::from_secs(5);

/// Starts polling every 5 s.
pub(crate) fn start(handle: &LoopHandle<'static, App>) -> Result<(), String> {
    handle
        .insert_source(Timer::from_duration(INTERVAL), |_deadline, (), app| {
            app.sysmon_poll();
            TimeoutAction::ToDuration(INTERVAL)
        })
        .map(|_token| ())
        .map_err(|err| format!("couldn't start the 5 s sysmon timer: {}", err.error))
}
