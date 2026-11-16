# Reading the Rust recovery implementation

Start with [`recovery.rs`](../processors/rust/src/recovery.rs), then follow
`receive` into `drain` and [`Processor::process_sampled`](../processors/rust/src/lib.rs).
The [recovery contract](recovery.md) describes behavior and experiment commands.
The two languages share the same network tests and observable state transitions.

## One owner replaces shared mutation

`Recovery::receive(&mut self, p: &mut Processor, payload: &[u8], ...)` temporarily
borrows the recovery object and processor exclusively. `&mut` is an exclusive
borrow, not a request to take a mutex. The caller cannot access either object
through another reference while that borrow is active. A `&[u8]` is a borrowed
slice: a pointer and length, with no ownership of the packet's allocation.

The RX thread owns the ring producer. The main thread owns the consumer, recovery
object and processor. Packet bytes stay alive while the ring's read guard is
held. In-order processing reads those bytes directly; it does not clone a Vec or
share the book through `Arc<Mutex<_>>`. C++ enforces the same design through
conventions and the ring API; Rust additionally checks many of the ownership
relationships at compile time.

## Copy only when a packet must outlive its slot

Once a ring read guard is released, RX can reuse that slot. Recovery therefore
cannot store a slice into it. Buffered messages are copied into an owned
`Entry { bytes: [u8; 255], length: u8, rx: u64 }`. The fixed array holds the maximum
PITCH message length representable by its one-byte Length field. The buffer is
allocated before RX starts using a bounded Vec of these entries.

The Vec owns its allocation; array indexing borrows one entry. `length == 0`
means a free slot because valid messages are at least two bytes. `head` and the
distance from `expected` determine where a future message belongs. Neither
`HashMap` allocation nor a tree insertion is needed just to hold an out-of-order
message. Book/trade maps can still allocate when that message is eventually
applied; this is not a blanket allocation-free processor claim.

In `drain`, the code first borrows an entry immutably, creates a short stack
packet for the existing processor API, and applies it. The immutable borrow's
last use is the processing call. Rust's non-lexical lifetimes then permit
`self.entries[self.head].length = 0`. Without that separation, attempting to hold
an entry reference while mutating the same buffer can produce a borrow error.
The small packet copy occurs on recovery/catch-up, not the normal live path.

## `Option`, `Box`, and temporarily moving a TCP session

`Option<T>` represents a value that may be absent. Pattern matching or `if let`
handles that absence explicitly. A TCP session is stored in `Option<Box<Session>>`:
`Box` uniquely owns heap storage, and `Option` lets `tick` temporarily move that
ownership out of the recovery object.

```rust
if let Some(mut grp) = self.grp.take() {
    let result = grp.tick(now, |m| self.control(p, m, false, now));
    self.grp = Some(grp);
    result?;
}
```

`take()` replaces the field with `None` and returns its previous value. The
closure then mutably borrows `self` while the local `grp` owns the session.
Keeping `&mut self.grp` borrowed while calling `self.control` would attempt two
overlapping mutable borrows of `self`. Moving the Box solves that ownership
conflict without copying the session or adding a lock. Restoring it before
`result?` also keeps it available for clean shutdown if polling returns an error.
The C++ implementation permits a callback through a Session pointer without
this compiler-enforced separation; its owner must still avoid invalidating that
pointer during the callback.

## Errors are values until the caller propagates them

Most fallible functions return `Result<T, Error>`. `Ok(value)` is success;
`Err(error)` is failure. Here `Error` is the project's existing String alias.
`?` returns early from the current function on Err and otherwise extracts the
Ok value. It does not throw a C++ exception.

A nonblocking TCP read returning `WouldBlock` is ordinary control flow: there is
no data yet. A read returning zero means the peer closed the connection. A
partial frame remains in the bounded input buffer until more bytes arrive.
The parser checks frame length, message count, message boundaries and login
state before dispatch. TCP writes likewise retain unsent bytes after a short
write. No assumption is made that one TCP read equals one PITCH frame.

A rejected GRP request is a valid protocol response that can select spin.
Malformed framing is fatal. Both languages retain that distinction; neither
silently turns malformed input into a successful recovery.

## Concurrency and `unsafe`

The new recovery module uses safe standard-library `TcpStream`, byte slices,
owned buffers and exclusive borrows. Linux `recvmsg`, ancillary data, polling
and the existing ring still have small FFI/unsafe boundaries elsewhere.
The `SAFETY` comments explain pointer validity and ownership for those calls.
Safe recovery code does not make UDP authenticated or establish live-exchange
interoperability.

The existing ring uses Release when publishing and Acquire when observing a
publication. That establishes the happens-before relationship for packet bytes.
Simply using Relaxed indices everywhere would not establish that relationship.
`Arc<AtomicBool>` supplies shared ownership of the RX stop/finished flags; Arc's
reference count keeps the allocation alive, while the atomic operation supplies
the synchronization. Arc alone does not make an arbitrary mutable book safe to
share. Book state remains owned by one thread.

## Reading the tests as a learner

The shared Python suite is useful because it observes sockets, reported books,
trades and validity rather than Rust-specific private fields. Read the scripted
overlap case: a replay packet contains an already-applied Add, one missing Add,
and two already-buffered updates. The final quantity and retained execution
count prove that the receiver filled the gap without applying the execution
twice. The rollover case verifies the same contract across `u32::MAX -> 1`.

Rust's type system helps enforce ownership, but it does not prove sequence math,
wire offsets or exchange semantics. Those still need protocol review and
behavioral tests. That distinction is a useful part of the C++/Rust comparison.
