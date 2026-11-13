//! Linux socket FFI; packet storage and ancillary buffers remain stack/preallocated.
use crate::{monotonic_ns, Error, Received};
use std::{
    io,
    mem::{size_of, size_of_val, zeroed},
    net::Ipv4Addr,
    os::fd::{AsRawFd, FromRawFd, OwnedFd},
};
#[derive(Default)]
pub struct RxStats {
    pub packets: u64,
    pub bytes: u64,
    pub first_rx: u64,
    pub last_rx: u64,
    pub kernel_drops: u64,
    pub ring_drops: u64,
    pub truncated: u64,
    pub high_watermark: usize,
    pub error: String,
}
#[derive(Clone, Copy)]
pub enum Status {
    Idle,
    Packet,
    Truncated,
}
pub struct Receiver {
    fd: OwnedFd,
    pub port: u16,
    pub buffer: i32,
    last_dropped: u32,
}
fn os_error(operation: &str) -> Error {
    format!("{}: {}", operation, io::Error::last_os_error())
}
impl Receiver {
    pub fn new(
        group: Ipv4Addr,
        interface: Ipv4Addr,
        port: u16,
        buffer: i32,
        idle_ms: u32,
    ) -> Result<Self, Error> {
        // SAFETY: socket has no pointer arguments; OwnedFd assumes sole ownership on success.
        let raw = unsafe { libc::socket(libc::AF_INET, libc::SOCK_DGRAM, 0) };
        if raw < 0 {
            return Err(os_error("socket"));
        }
        let mut receiver = Self {
            fd: unsafe { OwnedFd::from_raw_fd(raw) },
            port: 0,
            buffer: 0,
            last_dropped: 0,
        };
        receiver.set(libc::SOL_SOCKET, libc::SO_REUSEADDR, &1i32)?;
        receiver.set(libc::SOL_SOCKET, libc::SO_RCVBUF, &buffer)?;
        let timeout = libc::timeval {
            tv_sec: 0,
            tv_usec: (idle_ms.min(50) * 1000) as libc::suseconds_t,
        };
        receiver.set(libc::SOL_SOCKET, libc::SO_RCVTIMEO, &timeout)?;
        receiver.set(libc::SOL_SOCKET, libc::SO_RXQ_OVFL, &1i32)?;
        receiver.set(libc::IPPROTO_IP, libc::IP_MULTICAST_ALL, &0i32)?;
        // SAFETY: sockaddr_in is a C plain-data type whose zero representation is valid.
        let mut local: libc::sockaddr_in = unsafe { zeroed() };
        local.sin_family = libc::AF_INET as libc::sa_family_t;
        local.sin_port = port.to_be();
        // SAFETY: local is initialized and the passed length matches its representation.
        if unsafe {
            libc::bind(
                raw,
                (&local as *const libc::sockaddr_in).cast(),
                size_of_val(&local) as libc::socklen_t,
            )
        } < 0
        {
            return Err(os_error("bind"));
        }
        let membership = libc::ip_mreq {
            imr_multiaddr: libc::in_addr {
                s_addr: u32::from_ne_bytes(group.octets()),
            },
            imr_interface: libc::in_addr {
                s_addr: u32::from_ne_bytes(interface.octets()),
            },
        };
        receiver.set(libc::IPPROTO_IP, libc::IP_ADD_MEMBERSHIP, &membership)?;
        let mut length = size_of_val(&local) as libc::socklen_t;
        // SAFETY: writable local and length cover the declared sockaddr_in.
        if unsafe {
            libc::getsockname(
                raw,
                (&mut local as *mut libc::sockaddr_in).cast(),
                &mut length,
            )
        } < 0
        {
            return Err(os_error("getsockname"));
        }
        receiver.port = u16::from_be(local.sin_port);
        let mut actual = 0i32;
        length = size_of_val(&actual) as libc::socklen_t;
        // SAFETY: writable integer option buffer and valid length pointer.
        if unsafe {
            libc::getsockopt(
                raw,
                libc::SOL_SOCKET,
                libc::SO_RCVBUF,
                (&mut actual as *mut i32).cast(),
                &mut length,
            )
        } < 0
        {
            return Err(os_error("getsockopt"));
        }
        receiver.buffer = actual;
        Ok(receiver)
    }
    fn set<T>(&self, level: i32, option: i32, value: &T) -> Result<(), Error> {
        // SAFETY: value points to initialized option data of the declared size.
        if unsafe {
            libc::setsockopt(
                self.fd.as_raw_fd(),
                level,
                option,
                (value as *const T).cast(),
                size_of::<T>() as libc::socklen_t,
            )
        } < 0
        {
            Err(os_error("setsockopt"))
        } else {
            Ok(())
        }
    }
    pub fn receive(&mut self, packet: &mut Received, stats: &mut RxStats) -> Result<Status, Error> {
        let mut io = libc::iovec {
            iov_base: packet.bytes.as_mut_ptr().cast(),
            iov_len: packet.bytes.len(),
        };
        // usize alignment covers cmsghdr; 32 bytes cover one u32 SO_RXQ_OVFL record on x86_64.
        let mut control = [0usize; 4];
        // SAFETY: msghdr is plain C data; pointers below cover live writable buffers.
        let mut message: libc::msghdr = unsafe { zeroed() };
        message.msg_iov = &mut io;
        message.msg_iovlen = 1;
        message.msg_control = control.as_mut_ptr().cast();
        message.msg_controllen = size_of_val(&control);
        // SAFETY: all buffers remain live for the call; MSG_TRUNC reports the full datagram size.
        let received = unsafe { libc::recvmsg(self.fd.as_raw_fd(), &mut message, libc::MSG_TRUNC) };
        if received < 0 {
            let error = io::Error::last_os_error();
            if matches!(error.raw_os_error(), Some(libc::EINTR) | Some(libc::EAGAIN)) {
                return Ok(Status::Idle);
            }
            return Err(format!("recvmsg: {}", error));
        }
        packet.rx_ns = monotonic_ns(); // Same RX boundary as C++: immediately after successful recvmsg.
        packet.len = received as usize;
        if stats.packets == 0 {
            stats.first_rx = packet.rx_ns;
        }
        stats.packets += 1;
        stats.last_rx = packet.rx_ns;
        stats.bytes += received as u64;
        let truncated = message.msg_flags & libc::MSG_TRUNC != 0;
        if truncated {
            stats.truncated += 1;
        }
        if message.msg_flags & libc::MSG_CTRUNC != 0 {
            return Err("Truncated socket control data".into());
        }
        // SAFETY: libc walks kernel-produced cmsghdrs inside the aligned control buffer.
        unsafe {
            let mut item = libc::CMSG_FIRSTHDR(&message);
            while !item.is_null() {
                if (*item).cmsg_level == libc::SOL_SOCKET
                    && (*item).cmsg_type == libc::SO_RXQ_OVFL
                    && (*item).cmsg_len >= libc::CMSG_LEN(4) as usize
                {
                    let dropped = std::ptr::read_unaligned(libc::CMSG_DATA(item).cast::<u32>());
                    stats.kernel_drops += dropped.wrapping_sub(self.last_dropped) as u64;
                    self.last_dropped = dropped;
                }
                item = libc::CMSG_NXTHDR(&message, item);
            }
        }
        Ok(if truncated {
            Status::Truncated
        } else {
            Status::Packet
        })
    }
}
