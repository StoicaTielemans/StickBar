//! Network status and rates (the C bar's `network/network_client.c`; why each
//! figure is read the way it is: `docs/modules/network.md`).
//!
//! Everything comes from plain files (no `NetworkManager` or any other D-Bus
//! service), re-read on the same 5 s timer as sysmon:
//!
//! - `poll` (every 5 s, what the bar box needs): the default route and its
//!   gateway, `operstate`, and the rx/tx byte counters from one read of
//!   `/proc/net/dev`, turned into rates.
//! - `refresh_details` (dropdown only, and on an interface change): MAC, MTU,
//!   link speed and duplex, the IPv4 address and the DNS servers.

use std::{fs, net::Ipv4Addr, time::Instant};

use nix::sys::socket::SockaddrIn;

use crate::util::history::History2;

/// At most this many DNS servers are listed (glibc only uses the first few).
const MAX_DNS: usize = 8;

/// Everything known about the active connection.
#[derive(Debug, Default)]
pub(crate) struct Network {
    /// The default route's interface, e.g. `enp8s0` ("" with no default route).
    pub(crate) iface: String,
    /// Whether that interface is up.
    pub(crate) connected: bool,
    /// Whether it's Wi-Fi (listed in `/proc/net/wireless`), read on interface change.
    pub(crate) wifi: bool,
    /// The default gateway, e.g. `192.168.0.1`.
    pub(crate) gateway: String,
    /// The interface's IPv4 address. Dropdown only.
    pub(crate) ip: String,
    /// The MAC address. Dropdown only.
    pub(crate) mac: String,
    /// The MTU. Dropdown only.
    pub(crate) mtu: String,
    /// Link speed in Mbps, "" if unknown (Wi-Fi reports none). Dropdown only.
    pub(crate) speed: String,
    /// "full"/"half". Dropdown only.
    pub(crate) duplex: String,
    /// The DNS servers from `/etc/resolv.conf`. Dropdown only.
    pub(crate) dns: Vec<String>,
    /// Download rate, bytes/s, over the last poll interval.
    pub(crate) rx_rate: f64,
    /// Upload rate, bytes/s.
    pub(crate) tx_rate: f64,
    /// Bytes received since boot (the interface counter).
    pub(crate) rx_total: f64,
    /// Bytes sent since boot.
    pub(crate) tx_total: f64,
    /// The previous poll's counters and when they were read, for the rates.
    prev: Option<(f64, f64, Instant)>,
    /// rx (a) and tx (b) rates, one sample per poll; cleared when the
    /// interface changes or goes away.
    pub(crate) history: History2,
}

impl Network {
    /// One 5 s poll.
    pub(crate) fn poll(&mut self) {
        let route = fs::read_to_string("/proc/net/route")
            .ok()
            .and_then(|t| default_route(&t));
        let route_iface = route.as_ref().map_or("", |(name, _)| name.as_str());
        let changed = route_iface != self.iface;
        if changed {
            // A new interface: its counters have nothing to do with the old
            // one's, so no rate until the next poll, and a fresh history.
            route_iface.clone_into(&mut self.iface);
            self.prev = None;
            self.history = History2::default();
        }
        let Some((iface, gateway_hex)) = route else {
            self.connected = false;
            self.wifi = false;
            self.gateway.clear();
            self.ip.clear();
            self.rx_rate = 0.0;
            self.tx_rate = 0.0;
            self.history = History2::default();
            return;
        };
        self.gateway = hex_to_ip(&gateway_hex).unwrap_or_default();
        self.connected = read_trimmed(&format!("/sys/class/net/{iface}/operstate")) == "up";
        if changed {
            self.wifi = fs::read_to_string("/proc/net/wireless").is_ok_and(|t| is_wifi(&t, &iface));
            self.refresh_details();
        }

        let counters = fs::read_to_string("/proc/net/dev")
            .ok()
            .and_then(|t| dev_counters(&t, &iface));
        let now = Instant::now();
        if let (Some((rx, tx)), Some((prev_rx, prev_tx, then))) = (counters, self.prev) {
            let elapsed = now.duration_since(then).as_secs_f64();
            if elapsed > 0.0 {
                // A counter can only go backwards on a reset: show 0, never negative.
                self.rx_rate = ((rx - prev_rx) / elapsed).max(0.0);
                self.tx_rate = ((tx - prev_tx) / elapsed).max(0.0);
            }
        }
        if let Some((rx, tx)) = counters {
            self.rx_total = rx;
            self.tx_total = tx;
            self.prev = Some((rx, tx, now));
        }
        self.history.push(self.rx_rate, self.tx_rate);
    }

    /// The dropdown-only details for the current interface.
    pub(crate) fn refresh_details(&mut self) {
        if self.iface.is_empty() {
            return;
        }
        let sys = |file: &str| read_trimmed(&format!("/sys/class/net/{}/{file}", self.iface));
        self.mac = sys("address");
        self.mtu = sys("mtu");
        let speed = sys("speed");
        // Wi-Fi and unplugged links report -1 or nothing.
        self.speed = if speed.parse::<i64>().is_ok_and(|s| s > 0) {
            speed
        } else {
            String::new()
        };
        self.duplex = sys("duplex");
        self.dns = fs::read_to_string("/etc/resolv.conf")
            .map(|t| nameservers(&t))
            .unwrap_or_default();
        self.ip = ipv4_address(&self.iface)
            .map(|ip| ip.to_string())
            .unwrap_or_default();
    }
}

/// A small file's contents without surrounding whitespace ("" if unreadable).
fn read_trimmed(path: &str) -> String {
    fs::read_to_string(path)
        .map(|t| t.trim().to_owned())
        .unwrap_or_default()
}

/// The default route from `/proc/net/route`: the row whose destination is
/// `00000000`, as (interface, gateway in the kernel's hex).
fn default_route(route: &str) -> Option<(String, String)> {
    route.lines().skip(1).find_map(|line| {
        let mut fields = line.split_whitespace();
        let (iface, destination, gateway) = (fields.next()?, fields.next()?, fields.next()?);
        (destination == "00000000").then(|| (iface.to_owned(), gateway.to_owned()))
    })
}

/// The kernel's 8-hex-digit IPv4 (the address bytes in reverse order, as
/// stored in memory): `0100A8C0` → `192.168.0.1`.
fn hex_to_ip(hex: &str) -> Option<String> {
    let value = u32::from_str_radix(hex, 16)
        .ok()
        .filter(|_| hex.len() == 8)?;
    let [a, b, c, d] = value.to_le_bytes();
    Some(Ipv4Addr::new(a, b, c, d).to_string())
}

/// (rx bytes, tx bytes) of `iface` from `/proc/net/dev`'s text: after
/// `iface:`, the 1st field is received bytes and the 9th sent bytes.
fn dev_counters(dev: &str, iface: &str) -> Option<(f64, f64)> {
    dev.lines().find_map(|line| {
        let (name, values) = line.split_once(':')?;
        if name.trim() != iface {
            return None;
        }
        let mut fields = values.split_whitespace();
        let rx: f64 = fields.next()?.parse().ok()?;
        let tx: f64 = fields.nth(7)?.parse().ok()?;
        Some((rx, tx))
    })
}

/// Whether `iface` is listed in `/proc/net/wireless` (i.e. is Wi-Fi). The
/// signal columns aren't used, as in the reference bar.
fn is_wifi(wireless: &str, iface: &str) -> bool {
    wireless.lines().any(|line| {
        line.trim_start()
            .strip_prefix(iface)
            .is_some_and(|rest| rest.starts_with(':'))
    })
}

/// Every `nameserver` from `/etc/resolv.conf`'s text, at most `MAX_DNS`.
fn nameservers(resolv: &str) -> Vec<String> {
    resolv
        .lines()
        .filter_map(|line| line.trim_start().strip_prefix("nameserver"))
        .filter_map(|rest| rest.split_whitespace().next())
        .take(MAX_DNS)
        .map(str::to_owned)
        .collect()
}

/// `iface`'s IPv4 address, through `getifaddrs()`: one libc call over
/// kernel-resident data, instead of running `ip addr` (no subprocess, and no
/// interface name pasted into a command).
fn ipv4_address(iface: &str) -> Option<Ipv4Addr> {
    nix::ifaddrs::getifaddrs()
        .ok()?
        .filter(|entry| entry.interface_name == iface)
        .find_map(|entry| entry.address.as_ref()?.as_sockaddr_in().map(SockaddrIn::ip))
}

/// A rate as the reference bar shows it: `12.3 MB/s`, `4.5 KB/s` or `512 B/s`.
pub(crate) fn format_rate(bytes_per_sec: f64) -> String {
    if bytes_per_sec >= 1024.0 * 1024.0 {
        format!("{:.1} MB/s", bytes_per_sec / 1024.0 / 1024.0)
    } else if bytes_per_sec >= 1024.0 {
        format!("{:.1} KB/s", bytes_per_sec / 1024.0)
    } else {
        format!("{bytes_per_sec:.0} B/s")
    }
}

/// A byte total: `1.23 GB`, `4.56 MB`, `7.89 KB` or `512 B`.
pub(crate) fn format_bytes(bytes: f64) -> String {
    const KIB: f64 = 1024.0;
    if bytes >= KIB * KIB * KIB {
        format!("{:.2} GB", bytes / KIB / KIB / KIB)
    } else if bytes >= KIB * KIB {
        format!("{:.2} MB", bytes / KIB / KIB)
    } else if bytes >= KIB {
        format!("{:.2} KB", bytes / KIB)
    } else {
        format!("{bytes:.0} B")
    }
}

#[cfg(test)]
mod tests {
    use super::{
        default_route, dev_counters, format_bytes, format_rate, hex_to_ip, is_wifi, nameservers,
    };

    #[test]
    fn route_and_gateway() {
        let route = "Iface\tDestination\tGateway\tFlags\n\
                     enp8s0\t0000A8C0\t00000000\t0001\n\
                     enp8s0\t00000000\t0100A8C0\t0003\n";
        assert_eq!(
            default_route(route),
            Some(("enp8s0".to_owned(), "0100A8C0".to_owned()))
        );
        assert_eq!(hex_to_ip("0100A8C0").as_deref(), Some("192.168.0.1"));
        assert_eq!(hex_to_ip("123"), None);
    }

    #[test]
    fn counters_wifi_and_dns() {
        let dev = "Inter-|   Receive\n face |bytes packets\n\
                   enp8s0: 1000 10 0 0 0 0 0 0 2000 20 0 0 0 0 0 0\n";
        assert_eq!(dev_counters(dev, "enp8s0"), Some((1000.0, 2000.0)));
        assert_eq!(dev_counters(dev, "enp8s"), None); // no prefix matches
        assert!(is_wifi(
            " face | tus |\nwlan0: 0000   70.  -40.  -256\n",
            "wlan0"
        ));
        assert!(!is_wifi(" face | tus |\nwlan0: 0000\n", "wlan"));
        let resolv = "# comment\nnameserver 192.168.0.1\n  nameserver 1.1.1.1 # x\nsearch lan\n";
        assert_eq!(nameservers(resolv), vec!["192.168.0.1", "1.1.1.1"]);
    }

    #[test]
    fn formats() {
        assert_eq!(format_rate(512.0), "512 B/s");
        assert_eq!(format_rate(12.6 * 1024.0), "12.6 KB/s");
        assert_eq!(format_rate(3.0 * 1024.0 * 1024.0), "3.0 MB/s");
        assert_eq!(format_bytes(1.5 * 1024.0 * 1024.0 * 1024.0), "1.50 GB");
        assert_eq!(format_bytes(100.0), "100 B");
    }
}
