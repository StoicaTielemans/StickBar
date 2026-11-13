//! The clock's tick: a timer that fires at the start of every wall-clock
//! second (the C bar's `arm_clock_timer()` in `main.c` + `util/timer.c`).
//!
//! Why not a plain "every second" timer: one counted from startup ticks at
//! whatever fraction of a second the bar happened to start at, so the shown
//! time lagged reality by up to a second (BUG.md, "Clock seconds lagged real
//! time"; guarded by `test_clock_alignment.py`). Instead:
//!
//! - **`CLOCK_REALTIME` + `ABSTIME`, first expiry at the next whole second.**
//!   An absolute periodic timer keeps every later expiry on `.000` too, and a
//!   timerfd never fires early, so the time read at expiry is the new second.
//! - **`CANCEL_ON_SET`:** if the wall clock is *set* (NTP step, manual change,
//!   resume from suspend), the next read fails with `ECANCELED` instead of the
//!   timer silently keeping the old alignment, and it gets re-armed.
//!
//! calloop's own `Timer` can't do either: it only knows the monotonic clock.
//! It ticks every second even though seconds are hidden; the clock only
//! repaints when its text changes (once a minute).

use std::os::fd::{AsFd, OwnedFd};

use calloop::{Interest, LoopHandle, Mode, PostAction, generic::Generic};
use rustix::{
    io::Errno,
    time::{
        ClockId, Itimerspec, TimerfdClockId, TimerfdFlags, TimerfdTimerFlags, Timespec,
        clock_gettime, timerfd_create, timerfd_settime,
    },
};

use crate::wayland::App;

/// One second, as the timer's repeat interval.
const ONE_SECOND: Timespec = Timespec {
    tv_sec: 1,
    tv_nsec: 0,
};

/// Creates the timer, arms it, and adds it to the event loop.
pub(crate) fn start(handle: &LoopHandle<'static, App>) -> Result<(), String> {
    let fd = timerfd_create(
        TimerfdClockId::Realtime,
        TimerfdFlags::CLOEXEC | TimerfdFlags::NONBLOCK,
    )
    .map_err(|err| format!("timerfd_create(clock): {err}"))?;
    arm(&fd).map_err(|err| format!("timerfd_settime(clock): {err}"))?;
    let source = Generic::new(fd, Interest::READ, Mode::Level);
    handle
        .insert_source(source, |_readiness, timer, app| {
            on_expiry(timer, app);
            Ok(PostAction::Continue)
        })
        .map(|_token| ())
        .map_err(|err| format!("couldn't watch the clock timer: {}", err.error))
}

/// Arms `fd` to fire at the next whole wall-clock second, then every second.
fn arm(fd: impl AsFd) -> rustix::io::Result<()> {
    let now = clock_gettime(ClockId::Realtime);
    let first = Timespec {
        tv_sec: now.tv_sec + 1,
        tv_nsec: 0,
    };
    let spec = Itimerspec {
        it_interval: ONE_SECOND,
        it_value: first,
    };
    timerfd_settime(
        fd,
        TimerfdTimerFlags::ABSTIME | TimerfdTimerFlags::CANCEL_ON_SET,
        &spec,
    )
    .map(|_previous| ())
}

/// The timer fired (or the wall clock was set): re-arm if needed, then tick.
fn on_expiry(fd: &OwnedFd, app: &mut App) {
    // Reading clears the "readable" state. The 8 bytes are how many expiries
    // happened since the last read, which doesn't matter here.
    let mut expirations = [0_u8; 8];
    match rustix::io::read(fd, &mut expirations) {
        Err(Errno::CANCELED) => {
            // The wall clock was set: line the timer up with the new time.
            if let Err(err) = arm(fd) {
                eprintln!("stickBar: re-arming the clock timer failed: {err}");
            }
        }
        // Nothing to read after all (a spurious wake-up): no tick.
        Err(Errno::AGAIN) => return,
        Err(err) => eprintln!("stickBar: reading the clock timer failed: {err}"),
        Ok(_count) => {}
    }
    app.clock_tick();
}
