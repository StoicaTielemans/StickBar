//! The Proxmox module's settings file (`docs/modules/proxmox.md`):
//! `~/.config/stickBar/proxmox.conf`, kept **outside the repo** because it
//! holds an API token and the user's LAN addresses. No file means no module:
//! the box never appears.
//!
//! The format is one `key = value` per line; `#` starts a comment line and
//! blank lines are skipped. Values aren't quoted.
//!
//! ```text
//! url   = https://192.168.0.50:8006
//! token = root@pam!stickbar=0b3c...      # read-only, role PVEAuditor
//! pin   = sha256//kG2J...=                # the server key's hash
//! mac   = 7c:05:07:b0:c4:c0              # for Wake-on-LAN
//! ```
//!
//! `pin` is optional but needed with Proxmox's self-signed certificate:
//! without it curl verifies the certificate the normal way, which fails for a
//! self-signed one (and for any IP not in its SAN list).

use std::{os::unix::fs::PermissionsExt, path::PathBuf};

/// Replaces the settings file's path (absolute), in any mode, so a test bar
/// reads a test file (or, pointed at a file that doesn't exist, none: what
/// the tests' helpers do for every bar they start, including the ones outside
/// test mode). **In test mode only this is read**, never the real file: a
/// test bar must not grow a Proxmox box from the user's own settings (it would
/// shift every box the other tests measure) or poll the user's server.
const CONF_VAR: &str = "STICKBAR_TEST_PROXMOX_CONF";
/// The file's name, inside `$XDG_CONFIG_HOME/stickBar/` (`~/.config/stickBar/`).
const CONF_NAME: &str = "proxmox.conf";

/// The parsed settings.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Config {
    /// The API's base URL, e.g. `https://192.168.0.50:8006`, no trailing `/`.
    pub(crate) url: String,
    /// The API token, `user@realm!tokenid=secret` (the part after `PVEAPIToken=`).
    pub(crate) token: String,
    /// curl's `--pinnedpubkey`, `sha256//<base64>`, if set.
    pub(crate) pin: Option<String>,
    /// The server's network card, for Wake-on-LAN.
    pub(crate) mac: [u8; 6],
}

impl Config {
    /// The host part of [`Self::url`] (for the "no answer from …" line).
    pub(crate) fn host(&self) -> &str {
        let rest = self
            .url
            .split_once("://")
            .map_or(self.url.as_str(), |(_scheme, rest)| rest);
        rest.split([':', '/']).next().unwrap_or(rest)
    }
}

/// Reads the settings: `Ok(None)` when there's no file (the module is off),
/// `Err` when there is one but it's unusable (the reason goes to stderr and
/// the module stays off).
pub(crate) fn load(test_mode: bool) -> Result<Option<Config>, String> {
    let Some(path) = path(test_mode) else {
        return Ok(None);
    };
    let text = match std::fs::read_to_string(&path) {
        Ok(text) => text,
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(err) => return Err(format!("{}: {err}", path.display())),
    };
    // The token is a password: say so if others can read it (it still works).
    if let Ok(meta) = std::fs::metadata(&path)
        && meta.permissions().mode() & 0o077 != 0
    {
        eprintln!(
            "stickBar: proxmox: {} is readable by other users; it holds an API token \
             (chmod 600 it)",
            path.display()
        );
    }
    parse(&text)
        .map(Some)
        .map_err(|err| format!("{}: {err}", path.display()))
}

/// Where the settings file is: the test variable if set; otherwise nowhere
/// in test mode, and `$XDG_CONFIG_HOME/stickBar/proxmox.conf` (falling back
/// to `~/.config`) outside it.
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

/// Parses the file's text. `url`, `token` and `mac` are required; an unknown
/// key is an error, so a typo doesn't silently drop a setting.
pub(crate) fn parse(text: &str) -> Result<Config, String> {
    let (mut url, mut token, mut pin, mut mac) = (None, None, None, None);
    for (number, line) in text.lines().enumerate() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let line_no = number + 1;
        let Some((key, value)) = line.split_once('=') else {
            return Err(format!("line {line_no}: expected `key = value`"));
        };
        // Only the first `=` splits: a token's own `=` stays in its value.
        let (key, value) = (key.trim(), value.trim());
        // A trailing `# comment`, after whitespace (`#` is never in a value).
        let value = value.split(" #").next().unwrap_or(value).trim();
        if value.is_empty() {
            return Err(format!("line {line_no}: `{key}` has no value"));
        }
        match key {
            "url" => url = Some(value.trim_end_matches('/').to_owned()),
            "token" => token = Some(value.to_owned()),
            "pin" => pin = Some(value.to_owned()),
            "mac" => {
                mac = Some(
                    parse_mac(value).ok_or_else(|| format!("line {line_no}: bad MAC `{value}`"))?,
                );
            }
            _ => return Err(format!("line {line_no}: unknown key `{key}`")),
        }
    }
    let url = url.ok_or("missing `url`")?;
    if !url.starts_with("https://") {
        return Err("`url` must start with https://".to_owned());
    }
    let token = token.ok_or("missing `token`")?;
    // It goes inside a double-quoted curl config value: keep it to the
    // characters a Proxmox token is made of, so nothing needs escaping.
    if !token
        .chars()
        .all(|c| c.is_ascii_alphanumeric() || "@!=-_.".contains(c))
    {
        return Err("`token` has characters a Proxmox token never has".to_owned());
    }
    Ok(Config {
        url,
        token,
        pin,
        mac: mac.ok_or("missing `mac`")?,
    })
}

/// Parses `7c:05:07:b0:c4:c0` (or with `-` between the bytes).
pub(crate) fn parse_mac(text: &str) -> Option<[u8; 6]> {
    let mut mac = [0_u8; 6];
    let mut parts = text.split([':', '-']);
    for byte in &mut mac {
        let part = parts.next()?;
        if part.len() != 2 {
            return None;
        }
        *byte = u8::from_str_radix(part, 16).ok()?;
    }
    // Exactly six parts.
    parts.next().is_none().then_some(mac)
}

#[cfg(test)]
mod tests {
    use super::{parse, parse_mac};

    #[test]
    fn parses_a_full_file() {
        let text = "# my server\n\
                    url = https://192.168.0.50:8006/\n\
                    token = root@pam!stickbar=0b3c-11aa   # read only\n\
                    \n\
                    pin = sha256//abc+/def=\n\
                    mac = 7c:05:07:b0:c4:c0\n";
        let config = parse(text).unwrap();
        assert_eq!(config.url, "https://192.168.0.50:8006");
        assert_eq!(config.token, "root@pam!stickbar=0b3c-11aa");
        assert_eq!(config.pin.as_deref(), Some("sha256//abc+/def="));
        assert_eq!(config.mac, [0x7c, 0x05, 0x07, 0xb0, 0xc4, 0xc0]);
        assert_eq!(config.host(), "192.168.0.50");
    }

    /// Whether `text` fails to parse.
    fn fails(text: &str) -> bool {
        parse(text).is_err()
    }

    #[test]
    fn rejects_what_would_go_wrong_later() {
        let ok = "url = https://h:8006\ntoken = a@pam!b=c\nmac = 00:11:22:33:44:55\n";
        assert!(!fails(ok));
        assert!(fails(&ok.replace("https", "http")));
        assert!(fails(&format!("{ok}colour = red\n")));
        assert!(fails(&ok.replace("a@pam!b=c", "a\"b")));
        assert!(fails("url = https://h\nmac = 00:11:22:33:44:55\n"));
    }

    #[test]
    fn the_repos_template_parses() {
        // examples/proxmox.conf, the file users copy: it must stay valid.
        let config = parse(include_str!("../../examples/proxmox.conf")).unwrap();
        assert_eq!(config.host(), "192.168.0.50");
        assert!(config.pin.is_some());
    }

    #[test]
    fn parses_macs() {
        assert_eq!(
            parse_mac("7C-05-07-b0-c4-c0"),
            Some([0x7c, 5, 7, 0xb0, 0xc4, 0xc0])
        );
        assert_eq!(parse_mac("7c:05:07:b0:c4"), None);
        assert_eq!(parse_mac("7c:05:07:b0:c4:c0:11"), None);
        assert_eq!(parse_mac("7c:5:07:b0:c4:c0"), None);
        assert_eq!(parse_mac("zz:05:07:b0:c4:c0"), None);
    }
}
