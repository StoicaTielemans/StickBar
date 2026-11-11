//! A ring of the last `HISTORY_LEN` samples of two series (the C bar's
//! `util/history.h`): usage and temperature, rx and tx, used and swap. One
//! sample per 5 s poll, recorded always (not only while a dropdown is open),
//! so a graph is already full the moment it opens: two stores per poll, on a
//! timer that runs anyway.

/// How many samples are kept: 10 minutes at one per 5 s. The graphs show as
/// much of it as fits their card.
pub(crate) const HISTORY_LEN: usize = 120;

/// The two series. Index 0 of `a()`/`b()` is the oldest sample still held.
#[derive(Debug)]
pub(crate) struct History2 {
    /// First series.
    a: [f64; HISTORY_LEN],
    /// Second series.
    b: [f64; HISTORY_LEN],
    /// The slot the *next* sample goes in.
    head: usize,
    /// How many samples are held (up to `HISTORY_LEN`).
    len: usize,
}

impl Default for History2 {
    fn default() -> Self {
        Self {
            a: [0.0; HISTORY_LEN],
            b: [0.0; HISTORY_LEN],
            head: 0,
            len: 0,
        }
    }
}

impl History2 {
    /// Adds one sample to each series, dropping the oldest once full.
    pub(crate) fn push(&mut self, a: f64, b: f64) {
        if let (Some(slot_a), Some(slot_b)) = (self.a.get_mut(self.head), self.b.get_mut(self.head))
        {
            *slot_a = a;
            *slot_b = b;
        }
        self.head = (self.head + 1) % HISTORY_LEN;
        self.len = (self.len + 1).min(HISTORY_LEN);
    }

    /// How many samples are held.
    pub(crate) const fn len(&self) -> usize {
        self.len
    }

    /// The ring slot of sample `index` (0 = oldest still held): it sits at
    /// `head` once the ring has wrapped, at slot 0 before that.
    const fn slot(&self, index: usize) -> usize {
        let oldest = if self.len < HISTORY_LEN { 0 } else { self.head };
        (oldest + index) % HISTORY_LEN
    }

    /// Sample `index` of the first series (0 = oldest); 0.0 out of range.
    pub(crate) fn a(&self, index: usize) -> f64 {
        if index < self.len {
            self.a.get(self.slot(index)).copied().unwrap_or(0.0)
        } else {
            0.0
        }
    }

    /// Sample `index` of the second series (0 = oldest); 0.0 out of range.
    pub(crate) fn b(&self, index: usize) -> f64 {
        if index < self.len {
            self.b.get(self.slot(index)).copied().unwrap_or(0.0)
        } else {
            0.0
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{HISTORY_LEN, History2};

    #[test]
    fn oldest_first_before_wrapping() {
        let mut h = History2::default();
        h.push(1.0, 10.0);
        h.push(2.0, 20.0);
        assert_eq!(h.len(), 2);
        assert_eq!((h.a(0), h.a(1), h.b(1)), (1.0, 2.0, 20.0));
        assert_eq!(h.a(2), 0.0); // out of range
    }

    #[test]
    fn oldest_first_after_wrapping() {
        let mut h = History2::default();
        for i in 0..HISTORY_LEN + 3 {
            h.push(f64::from(u32::try_from(i).unwrap()), 0.0);
        }
        assert_eq!(h.len(), HISTORY_LEN);
        assert_eq!(h.a(0), 3.0); // the first three were pushed out
        assert_eq!(
            h.a(HISTORY_LEN - 1),
            f64::from(u32::try_from(HISTORY_LEN + 2).unwrap())
        );
    }
}
