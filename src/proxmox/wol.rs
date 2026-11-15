//! Wake-on-LAN: the "magic packet" that powers the server on, sent from the
//! bar itself. It does what `wakeonlan <mac>` does, without a subprocess or
//! the package:
//!
//! - **The packet:** 6 bytes of `0xFF`, then the MAC 16 times (102 bytes). The
//!   network card, powered down but listening, wakes the machine when it sees
//!   its own MAC repeated like this anywhere in a frame.
//! - **Where to:** a UDP broadcast to `255.255.255.255:9` (the discard port,
//!   `wakeonlan`'s default), so it reaches the sleeping card without an IP
//!   (it has none while off, and its ARP entry may be gone).
//! - **On the main thread:** one `sendto` of 102 bytes on a fresh UDP socket
//!   returns at once. UDP has no handshake to wait for.
//!
//! Whether it worked can only be seen by the server answering later: the
//! module shows "waking" and polls more often until it does
//! (`docs/modules/proxmox.md`).

use std::net::UdpSocket;

/// Where the packet goes (`wakeonlan`'s default too).
const BROADCAST: &str = "255.255.255.255:9";
/// Replaces [`BROADCAST`] (`host:port`), so a test catches the packet on
/// localhost instead of broadcasting on the user's LAN.
const ADDR_VAR: &str = "STICKBAR_TEST_PROXMOX_WOL_ADDR";
/// The packet's length: the 6-byte sync stream plus 16 copies of the MAC.
pub(crate) const PACKET_LEN: usize = 6 + 16 * 6;

/// The magic packet for `mac`.
pub(crate) fn magic_packet(mac: [u8; 6]) -> [u8; PACKET_LEN] {
    let mut packet = [0xFF_u8; PACKET_LEN];
    // Every 6-byte chunk after the first (the sync stream) is the MAC.
    for chunk in packet.as_chunks_mut::<6>().0.iter_mut().skip(1) {
        *chunk = mac;
    }
    packet
}

/// Sends the magic packet for `mac`. In test mode only to the test's
/// address: no address, nothing sent (a test bar never broadcasts on the LAN).
pub(crate) fn send(mac: [u8; 6], test_mode: bool) -> Result<(), String> {
    let target = match std::env::var(ADDR_VAR) {
        Ok(addr) if !addr.is_empty() => addr,
        _ if test_mode => return Err(format!("test mode without {ADDR_VAR}: not sent")),
        _ => BROADCAST.to_owned(),
    };
    let socket = UdpSocket::bind("0.0.0.0:0").map_err(|e| format!("UDP socket: {e}"))?;
    // Without SO_BROADCAST the kernel refuses a broadcast address (EACCES).
    socket
        .set_broadcast(true)
        .map_err(|e| format!("SO_BROADCAST: {e}"))?;
    let sent = socket
        .send_to(&magic_packet(mac), &target)
        .map_err(|e| format!("sending to {target}: {e}"))?;
    if sent == PACKET_LEN {
        Ok(())
    } else {
        Err(format!("only {sent} of {PACKET_LEN} bytes sent"))
    }
}

#[cfg(test)]
mod tests {
    use super::{PACKET_LEN, magic_packet};

    #[test]
    fn the_packet_is_sync_then_sixteen_macs() {
        let mac = [0x7c, 0x05, 0x07, 0xb0, 0xc4, 0xc0];
        let packet = magic_packet(mac);
        assert_eq!(packet.len(), PACKET_LEN);
        assert_eq!(packet.get(..6), Some(&[0xFF_u8; 6][..]));
        let copies = packet.get(6..).unwrap().chunks(6);
        assert_eq!(copies.len(), 16);
        assert!(packet.get(6..).unwrap().chunks(6).all(|c| c == mac));
    }
}
