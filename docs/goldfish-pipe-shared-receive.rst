Goldfish pipe segmented shared receive
=====================================

The graphics QemuPipeStream client uses a mandatory shared-memory receive
transport. The host, guest module and client are released together. There is
no capability negotiation or fallback to READ. Other pipe services retain
their existing transport. Graphics requests use the independent shared
transmit transport described below; legacy clients retain WRITE descriptors.

Layout and ownership
--------------------

PipeSharedReceiveProtocol.h is the fixed little-endian ABI, mirrored in the
guest module and Mesa connection manager. All shared counters are aligned
32-bit integers, not C++ atomic objects. The transport uses 4096-byte pages.

The guest module owns two control pages and a pool of 64 KiB segments. Each
segment consists of sixteen independently allocated RAM pages. Four segments
are registered initially. Growth doubles the pool up to 64 segments (4 MiB),
subject to a device-wide 256 MiB page budget on both guest and host. The guest
maps the maximum virtual range once; new pages become accessible through the
VMA fault handler only after allocation. Host mappings persist until CLOSE.

Page zero contains PipeRxHost and segment descriptors. Page one contains
PipeRxGuest::released[]. Segment data starts at page two. The full mapping is
read-only to userspace. Only a separate mapping of page one is writable.
Private mappings, executable mappings, and permission upgrades of host-owned
pages are rejected. Mappings are not inherited by fork. File references held
by VMAs retain the pipe until the last mapping and descriptor are released.

Only the host changes segment generation, committed bytes and next ID. Only
the guest changes released generations. Generation zero denotes an unused
segment. Reuse increments the generation and skips zero. The host never reuses
its current segment or a segment whose generation the consumer has not released.

Publishing and consuming
------------------------

The renderer's writeToGuest path drains replies into the shared receive sink.
It copies bytes before a release store to committed. The consumer uses an
acquire load before copying those bytes. One logical consumer is serialized
by the client. Vulkan wire bytes are unchanged and may span any number of
segments; readFully consumes and releases incrementally.

When a segment fills, the host initializes a free segment, sets the previous
segment's next ID, then release-publishes PIPE_RX_SEALED in committed. The
consumer reads the next ID only after observing this seal, switches to the
next generation, then release-publishes the previous generation in released.
No marker occupies payload space. Published links remain immutable until
release. A reply can be larger than the entire pool.

When no segment is free, the host publishes NEED_SEGMENT, executes a full
barrier, and rechecks released generations. The consumer releases a segment,
executes a full barrier, and checks NEED_SEGMENT. Together these checks prevent
a missed space notification. The client grows the pool while permitted; at
the cap, or after an allocation limit, it sends CREDIT instead. The renderer
retains the untransferred prefix offset and remaining reply buffers. Its
existing bounded buffer queue applies backpressure; the sink never blocks
while holding the channel lock or BQL waiting for guest progress.

Commands and waiting
--------------------

The command page keeps its existing buffer-array layout:

* 13 RX_SETUP: two control-page addresses.
* 14 RX_ADD_SEGMENT: sixteen data-page addresses; consumed_size supplies the ID.
* 15 RX_ACTIVATE: enable the registered receiver.
* 16 RX_CREDIT: notify the consumer that reply space or segments are available.
* 17 RX_ARM: buffers_count carries the observed event sequence.

The guest exposes INIT/GROW/CREDIT/WAIT ioctls at p:0x81 through p:0x84.
INIT and GROW return the registered segment count. WAIT takes a pointer to
the observed 32-bit event sequence. Initialization failure is fatal to the
graphics connection; partially registered pages remain owned until CLOSE.

Normal reads use userspace loads and copies only. Empty reads poll for up to
100 us total per readFully, then enter WAIT. The driver sets its wake bit and
submits RX_ARM. Under its publication lock the host either arms a one-shot
IRQ or immediately wakes if the sequence already changed. The waitqueue
rechecks sequence, closed state and its wake bit before sleeping. Publication
increments event_seq and consumes the IRQ arm. Signal interruption is exposed
by WAIT; the graphics client retries EINTR. Close and error state end reads.

Lifetime and snapshots
----------------------

Guest close stops channel callbacks before the host releases RAM mappings.
The producer marks written payload and control bytes dirty through the device
callback. All mapping addresses are validated as RAM, page-aligned, bounded,
and nonduplicated within a pipe.

Snapshot pipe payload format 3 saves RX/TX registrations, activation and
doorbell registration state. VM stop synchronously disables RX publication
and TX consumption before RAM serialization. In-flight copies finish before
the run-state callback returns. Replies completed while stopped remain in
the channel, including its partially consumed first buffer, for device-state
serialization. VM resume enables the shared queues and wakes the consumer.
Restore reconstructs mappings before enabling consumption. Closed pipes skip
doorbell registration; unavailable doorbells leave TX kicks on the MMIO path.

Verification
------------

Host tests cover partial transfers, growth, concurrent reuse, sequence and
generation wraparound, one-shot IRQ arming, and restored partial segments.
Native guest integration tests should include large query-result replies
above 4 MiB, guard bytes, concurrent small replies, allocation limits, mapping
permissions, EINTR, process exit and mapped-file lifetime. shared_rx_stats
reports allocated pages, IRQ waits, credit calls, growth calls and rejected
legacy reads. Counters are device-wide diagnostic snapshots.

WHPX transmit notification
--------------------------

After receive activation the guest module can enable command 18. On WHPX,
the host registers each pipe ID as a 32-bit doorbell match value at device
offset 0x1000, on a separate MMIO page. The event directly wakes the existing
RenderThread. Notification carries no payload or completion sequence. A host
without doorbell support handles the same store through ordinary MMIO.

The write_doorbell parameter selects WHPX event notification or an MMIO
TX_KICK command. Both consume the same shared payload. Closing a pipe removes
its event registration before the channel destroys the event handle.

RX_CREDIT also signals the consumer event. It never copies replies on the
vCPU thread. The consumer drains queued replies on credits, before waiting
for input and while applying reply-queue backpressure. Notifications do not
acquire the channel's copy lock. Each RX/TX copy holds its locks for at most
64 KiB of payload; draining multiple chunks releases the channel lock between
chunks. Interrupt arming can therefore only wait behind one bounded copy.

Encoder direct shared transmit
------------------------------

PipeSharedTransmitProtocol.h defines an independent guest-to-host queue.
The graphics client initializes RX, allocates TX, and activates TX before
sending renderer stream flags. Matching host, kernel and client builds are
required; TX initialization failure ends the connection. It does not switch
payload transport back to WRITE.

The kernel owns two control pages and initially four 64 KiB data segments.
The guest can map each segment as a contiguous virtual range even though its
physical pages are allocated separately. PipeTxGuest contains producer-owned
generation, committed-byte count and next-segment ID. PipeTxHost contains the
consumer cursor, released generations, capacities and interrupt state. The
host-owned page is read-only to userspace, including mprotect upgrades. All
TX mappings prohibit execution and fork inheritance and retain the file.

QemuPipeStream::allocBuffer returns a reservation in this mapped TX storage.
VulkanStreamGuest::reserve therefore lets the encoder write packet bytes
directly into shared RAM. Ordinary preferred reservations are capped at
64 KiB; a larger individual packet gets a sufficiently large contiguous
segment. Sizes normally round up to powers of two, with page rounding near
the allocation limit. At most four segments capable of satisfying the current
reservation are provisioned before waiting for reuse. This prevents a slow
consumer from filling all 64 descriptor slots with small segments before a
large packet arrives. Guest and host enforce a device-wide 256 MiB TX page
budget, including control pages. Allocated segments persist until close.

Commit release-publishes only the encoded bytes, then sends TX_KICK. On WHPX,
this ioctl rings the existing per-pipe doorbell event without pinning caller
pages, supplying WRITE descriptors, or waiting for copy completion. Other
notification configurations submit the same kick through ordinary MMIO.
Notification selection does not change the shared payload path.

The RenderThread consumes persistent TX mappings directly into ReadBuffer.
This removes the intermediate ChannelBuffer allocation, copy and queue entry.
One host payload copy remains, from shared pages to contiguous decoder input.
writeFully still copies data that was already constructed outside TX into TX;
it does not make arbitrary application buffers zero-copy.

The producer initializes the next segment, stores its ID in the old segment,
then seals the old committed-byte count. The host releases a sealed generation
only after copying its last byte. The guest must acquire that release before
reusing the storage. Neither links nor markers consume payload bytes, and
ReadBuffer joins commands across segment boundaries. A single contiguous
encoder reservation must fit the available TX budget. A longer byte stream
can reuse segments indefinitely. Space exhaustion polls for up to 100 us,
then arms a one-shot WRITE interrupt using an event-sequence recheck. Normal
client destruction flushes and seals its final nonempty segment, then waits
for every published generation to be released, including when the current
segment is empty. Allocation failure suppresses further growth attempts only
within the current reservation; later requests can retry after memory is freed.

Commands 19 through 24 are TX_SETUP, TX_ADD_PAGES, TX_FINISH_SEGMENT,
TX_ACTIVATE, TX_KICK and TX_ARM. INIT/GROW/ACTIVATE/KICK/WAIT use ioctls
p:0x85 through p:0x89. The mmap offset range starts at page 65536 and each
segment has a fixed virtual offset stride of PIPE_TX_MAX_PAGES pages. Only
allocated extents may be mapped. Registration validates RAM addresses,
alignment, count, budget and duplicates before consumption.

The channel lock serializes consumption with detach. Channel stop also cancels
decoder sequence waits, since a closed producer cannot supply a missing
cross-thread command. This cancellation is nonblocking and precedes process
resource cleanup. Queue consumption pauses
when the VM stops, and resume wakes the existing consumer event. Snapshot
format 3 saves TX registrations, activation and partially registered segments;
shared RAM preserves data, links, cursor and released generations. Restore
reconstructs the mappings before consumption resumes.

shared_tx_stats reports current allocated pages, cumulative segment
allocations, kicks, interrupt waits and rejected legacy writes. Host tests
cover publication, page boundaries, generation wrap, invalid descriptors,
partial snapshot restore, pause/resume, interrupt races, concurrent reuse and
channel closure with a missing decoder sequence. The guest writer test delays
consumption until a space wait, then checks small-to-large growth, recycling,
payload order and draining on close.
