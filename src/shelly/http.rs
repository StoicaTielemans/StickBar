//! A minimal HTTP/1.0 `GET`, for the plugs' local API (`docs/modules/shelly.md`).
//!
//! A Shelly answers plain HTTP on the LAN, with a few hundred bytes of JSON,
//! so this is `std::net::TcpStream` and a dozen lines of parsing: no HTTP
//! crate, no TLS stack, and no curl child per request (cheaper than the
//! Proxmox module's curl, which needs TLS and a pinned key).
//!
//! **Blocking**: it only ever runs on the `shelly-req` worker threads, never
//! on the event loop. Every step has a timeout ([`TIMEOUT`]), so a plug that
//! vanished mid-answer can't hold a thread for long.
//!
//! HTTP/1.0 with `Connection: close` means the server sends the body as-is
//! (no chunked encoding) and closes when done, so reading to EOF is the
//! whole answer. A chunked answer is still decoded, in case a firmware
//! update ever starts sending one anyway.

use std::{
    io::{self, Read, Write},
    net::{SocketAddr, TcpStream},
    time::Duration,
};

/// Connecting, and each read and write. A plug on the LAN answers in a few
/// ms; one that's unplugged never does.
const TIMEOUT: Duration = Duration::from_millis(1500);
/// The most of an answer read. A `Switch.GetStatus` answer is ~0.5 KB with
/// its headers; anything near this is not a plug.
const MAX_ANSWER: u64 = 16 * 1024;

/// Why a request got no HTTP answer.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Failure {
    /// Nothing answered (refused, no route, timed out): unplugged or away.
    NoAnswer,
    /// Something answered, but not with HTTP; what.
    Garbled(String),
}

/// `GET path` from `addr`: the status code and the body.
pub(crate) fn get(addr: SocketAddr, path: &str) -> Result<(u16, String), Failure> {
    let mut stream =
        TcpStream::connect_timeout(&addr, TIMEOUT).map_err(|_refused| Failure::NoAnswer)?;
    let timeouts = stream
        .set_read_timeout(Some(TIMEOUT))
        .and_then(|()| stream.set_write_timeout(Some(TIMEOUT)));
    timeouts.map_err(|err| Failure::Garbled(err.to_string()))?;
    let request = format!(
        "GET {path} HTTP/1.0\r\nHost: {}\r\nConnection: close\r\n\r\n",
        addr.ip()
    );
    stream
        .write_all(request.as_bytes())
        .map_err(|err| failure(&err))?;
    let mut answer = Vec::new();
    let _read = stream
        .take(MAX_ANSWER)
        .read_to_end(&mut answer)
        .map_err(|err| failure(&err))?;
    parse(&String::from_utf8_lossy(&answer))
}

/// A timeout while talking means nothing's answering; anything else is a
/// real error.
fn failure(err: &io::Error) -> Failure {
    // A read timeout is `WouldBlock` on Linux, `TimedOut` elsewhere.
    let kind = err.kind();
    if kind == io::ErrorKind::TimedOut || kind == io::ErrorKind::WouldBlock {
        Failure::NoAnswer
    } else {
        Failure::Garbled(err.to_string())
    }
}

/// Splits an answer into its status code and body.
fn parse(answer: &str) -> Result<(u16, String), Failure> {
    let (head, body) = answer
        .split_once("\r\n\r\n")
        .ok_or_else(|| Failure::Garbled("an answer with no end of headers".to_owned()))?;
    let mut lines = head.lines();
    let status = lines
        .next()
        .and_then(|line| line.strip_prefix("HTTP/"))
        .and_then(|rest| rest.split(' ').nth(1))
        .and_then(|code| code.parse::<u16>().ok())
        .ok_or_else(|| Failure::Garbled("not an HTTP answer".to_owned()))?;
    let chunked = lines.any(|line| {
        line.split_once(':').is_some_and(|(name, value)| {
            name.trim().eq_ignore_ascii_case("transfer-encoding")
                && value.trim().eq_ignore_ascii_case("chunked")
        })
    });
    let body = if chunked {
        dechunk(body).ok_or_else(|| Failure::Garbled("a broken chunked body".to_owned()))?
    } else {
        body.to_owned()
    };
    Ok((status, body))
}

/// Joins a chunked body's chunks: `<hex size>\r\n<data>\r\n`, ending at size 0.
fn dechunk(mut body: &str) -> Option<String> {
    let mut out = String::new();
    loop {
        let (size_line, rest) = body.split_once("\r\n")?;
        // A size may carry `;extensions` after it.
        let size_hex = size_line.split(';').next().unwrap_or(size_line).trim();
        let size = usize::from_str_radix(size_hex, 16).ok()?;
        if size == 0 {
            return Some(out);
        }
        out.push_str(rest.get(..size)?);
        body = rest.get(size..)?.strip_prefix("\r\n")?;
    }
}

#[cfg(test)]
mod tests {
    use super::{Failure, parse};

    #[test]
    fn splits_status_and_body() {
        let answer = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\n{\"output\":1}\n";
        assert_eq!(parse(answer), Ok((200, "{\"output\":1}\n".to_owned())));
        let refused = "HTTP/1.0 401 Unauthorized\r\n\r\n";
        assert_eq!(parse(refused), Ok((401, String::new())));
    }

    #[test]
    fn decodes_chunked_bodies() {
        let answer = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n\
                      5\r\n{\"a\":\r\n2\r\n1}\r\n0\r\n\r\n";
        assert_eq!(parse(answer), Ok((200, "{\"a\":1}".to_owned())));
    }

    #[test]
    fn rejects_what_isnt_http() {
        assert!(matches!(parse("garbage"), Err(Failure::Garbled(_))));
        assert!(matches!(parse("SSH-2.0\r\n\r\n"), Err(Failure::Garbled(_))));
    }
}
