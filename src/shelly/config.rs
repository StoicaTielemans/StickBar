//! The smart-plug card's settings file (`docs/modules/shelly.md`):
//! `~/.config/stickBar/shelly.conf`, kept **outside the repo** because it
//! lists the user's LAN addresses. No file means no card.
//!
//! One plug per line, `name = address`, in the order the card shows them;
//! `#` starts a comment line and blank lines are skipped. The address is an
//! IP, optionally with a port (the tests' fake plugs listen on localhost
//! ports; a real Shelly answers on 80):
//!
//! ```text
//! desk    = 192.168.1.51
//! tv      = 192.168.1.52
//! kitchen = 192.168.1.50:80
//! ```
//!
//! Only IP addresses, never host names: a name would need a DNS lookup,
//! which can hang far longer than the plugs' own timeouts.

use std::{
    net::{IpAddr, SocketAddr},
    path::PathBuf,
};

/// Replaces the settings file's path (absolute), in any mode. **In test mode
/// only this is read**, never the real file, so a test bar can't switch the
/// user's real plugs; the tests' helpers point every bar they start at a file
/// that doesn't exist, and `test_shelly.py` at its fakes.
const CONF_VAR: &str = "STICKBAR_TEST_SHELLY_CONF";
/// The file's name, inside `$XDG_CONFIG_HOME/stickBar/` (`~/.config/stickBar/`).
const CONF_NAME: &str = "shelly.conf";
/// The port a Shelly's web server listens on.
const HTTP_PORT: u16 = 80;
/// The most plugs the card shows (`docs/limitations.md`): each one is a row,
/// and the card sits in a column of a fixed-size dropdown.
pub(crate) const MAX_PLUGS: usize = 12;
/// The longest name accepted, in characters (longer ones are truncated
/// when drawn anyway; this just catches a pasted mistake).
const MAX_NAME_CHARS: usize = 32;

/// One configured plug.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct PlugConfig {
    /// What the card calls it.
    pub(crate) name: String,
    /// Where its web server is.
    pub(crate) addr: SocketAddr,
}

/// Reads the settings: `Ok(None)` when there's no file (no card), `Err` when
/// there is one but it's unusable (the reason goes to stderr and there's no
/// card).
pub(crate) fn load(test_mode: bool) -> Result<Option<Vec<PlugConfig>>, String> {
    let Some(path) = path(test_mode) else {
        return Ok(None);
    };
    let text = match std::fs::read_to_string(&path) {
        Ok(text) => text,
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(err) => return Err(format!("{}: {err}", path.display())),
    };
    parse(&text)
        .map(Some)
        .map_err(|err| format!("{}: {err}", path.display()))
}

/// Where the settings file is: the test variable if set; otherwise nowhere
/// in test mode, and `$XDG_CONFIG_HOME/stickBar/shelly.conf` (falling back to
/// `~/.config`) outside it. The same rule as `proxmox::config`.
fn path(test_mode: bool) -> Option<PathBuf> {
    let overridden = std::env::var_os(CONF_VAR)
        .filter(|v| !v.is_empty())
        .map(PathBuf::from);
    if overridden.is_some() || test_mode {
        return overridden;
    }
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".config")))?;
    Some(base.join("stickBar").join(CONF_NAME))
}

/// Parses the file's text. A bad line is an error (so a typo doesn't
/// silently drop a plug), as are a repeated name, no plugs at all, and more
/// than [`MAX_PLUGS`].
pub(crate) fn parse(text: &str) -> Result<Vec<PlugConfig>, String> {
    let mut plugs: Vec<PlugConfig> = Vec::new();
    for (number, line) in text.lines().enumerate() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let line_no = number + 1;
        let Some((name, value)) = line.split_once('=') else {
            return Err(format!("line {line_no}: expected `name = address`"));
        };
        let name = name.trim();
        // A trailing `# comment`, after whitespace.
        let value = value.split(" #").next().unwrap_or(value).trim();
        if name.is_empty() || name.chars().count() > MAX_NAME_CHARS {
            return Err(format!(
                "line {line_no}: a plug's name must be 1 to {MAX_NAME_CHARS} characters"
            ));
        }
        let addr = parse_addr(value)
            .ok_or_else(|| format!("line {line_no}: `{value}` isn't an IP or IP:port"))?;
        if plugs.iter().any(|p| p.name == name) {
            return Err(format!("line {line_no}: `{name}` is listed twice"));
        }
        plugs.push(PlugConfig {
            name: name.to_owned(),
            addr,
        });
    }
    if plugs.is_empty() {
        return Err("no plugs listed".to_owned());
    }
    if plugs.len() > MAX_PLUGS {
        return Err(format!("{} plugs listed; at most {MAX_PLUGS}", plugs.len()));
    }
    Ok(plugs)
}

/// `192.168.1.51` (port 80) or `192.168.1.51:8080` (IPv6 as `[::1]:80`).
fn parse_addr(text: &str) -> Option<SocketAddr> {
    text.parse::<SocketAddr>().ok().or_else(|| {
        text.parse::<IpAddr>()
            .ok()
            .map(|ip| SocketAddr::new(ip, HTTP_PORT))
    })
}

#[cfg(test)]
mod tests {
    use super::{MAX_PLUGS, parse};

    #[test]
    fn parses_names_and_addresses_in_order() {
        let text = "# plugs\n\
                    desk = 192.168.1.51\n\
                    \n\
                    living room = 192.168.1.52:8080   # the TV\n";
        let plugs = parse(text).unwrap();
        assert_eq!(plugs.len(), 2);
        assert_eq!(plugs[0].name, "desk");
        assert_eq!(plugs[0].addr.to_string(), "192.168.1.51:80");
        assert_eq!(plugs[1].name, "living room");
        assert_eq!(plugs[1].addr.to_string(), "192.168.1.52:8080");
    }

    #[test]
    fn the_repos_template_parses() {
        // examples/shelly.conf, the file users copy: it must stay valid.
        let plugs = parse(include_str!("../../examples/shelly.conf")).unwrap();
        assert_eq!(plugs.len(), 3);
    }

    /// Whether `text` fails to parse.
    fn fails(text: &str) -> bool {
        parse(text).is_err()
    }

    #[test]
    fn rejects_what_would_go_wrong_later() {
        assert!(fails(""));
        assert!(fails("desk 192.168.1.51\n"));
        assert!(fails("desk = shelly.lan\n"));
        assert!(fails(" = 192.168.1.51\n"));
        assert!(fails("a = 10.0.0.1\na = 10.0.0.2\n"));
        let many = (0..=MAX_PLUGS)
            .map(|i| format!("p{i} = 10.0.0.{i}\n"))
            .collect::<Vec<_>>()
            .concat();
        assert!(fails(&many));
    }
}
