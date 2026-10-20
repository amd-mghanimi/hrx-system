# AQL signal-value waits

AMD's `HSA_AMD_PACKET_TYPE_BARRIER_VALUE` makes the packet processor wait for
a masked comparison on one HSA signal before processing later packets. It
uses AMD vendor format 2 inside an AQL slot. The comparison can express an
exact value, a changed value, or an ordered threshold; ordinary BARRIER_AND
and BARRIER_OR instead wait for zero-valued dependencies. [Vendor packet
contract][vendor] · [Standard barriers](barriers.md)

## Applicability and runtime selection

The AMD extension names kernel-dispatch queues created from AMD GPU agents as
the consumers of this packet. It does not enumerate firmware revisions or
physical GPU IPs. CLR's automatic dependency path has a narrower selector:
`major == 9 && minor == 0 && stepping == 10`, or `major == 9 && minor >= 4 &&
stepping in {0, 1, 2}`. This is the pinned runtime's `barrier_value_packet_`
policy, not a general generation boundary inferred from the packet layout.
[Extension applicability][vendor] · [CLR device policy][device-policy]

CLR also has a stream-memory-operation caller controlled by
`GPU_STREAMOPS_CP_WAIT`. The flag defaults to false; the default caller
dispatches a wait shader. With the flag true, it submits BARRIER_VALUE and
requires the memory object to carry `ROCCLR_MEM_HSA_SIGNAL_MEMORY`. That branch
does not consult `barrier_value_packet_`. The override and the automatic
device selector therefore describe different choices; the override's presence
does not establish firmware support on another target. [Flag default][flag]
· [Memory admission][admission] · [Submission branches][stream-wait]

## Packet representation

The following layout uses HSA's large machine model, where
`hsa_signal_value_t` is `int64_t`. A packet occupies 64 bytes; a DWORD is
32 bits. The AMD header refers to the standard AQL header-field definitions.
[Signal types][signal-types] · [Vendor layout][vendor] · [Header
fields](barriers.md#header-and-barrier-representation)

| Byte offset | DWORD and bits | Native field and meaning |
| --- | --- | --- |
| 0–1 | DWORD 0, bits 15:0 | `header.header`: type bits 7:0 are `HSA_PACKET_TYPE_VENDOR_SPECIFIC` = 0; bit 8 requests ordering after preceding packets; bits 10:9 and 12:11 select acquire and release scope. Bits 15:13 are zero. |
| 2 | DWORD 0, bits 23:16 | `header.AmdFormat`: `HSA_AMD_PACKET_TYPE_BARRIER_VALUE` = 2. |
| 3 | DWORD 0, bits 31:24 | `header.reserved`: zero. |
| 4–7 | DWORD 1 | `reserved0`: zero. |
| 8–15 | DWORDs 2–3 | `signal`: 64-bit dependency handle; zero denotes an already-satisfied dependency. |
| 16–23 | DWORDs 4–5 | `value`: signed 64-bit comparison reference. |
| 24–31 | DWORDs 6–7 | `mask`: 64-bit mask applied to the observed signal value. |
| 32–35 | DWORD 8 | `cond`: 32-bit `hsa_signal_condition32_t`, carrying an HSA condition enum value. |
| 36–39 | DWORD 9 | `reserved1`: zero. |
| 40–47 | DWORDs 10–11 | `reserved2`: zero. |
| 48–55 | DWORDs 12–13 | `reserved3`: zero. |
| 56–63 | DWORDs 14–15 | `completion_signal`: 64-bit handle; zero requests no completion notification. |

For a non-null signal, let `x` be its signed 64-bit value and `m` the mask.
The reference `v` is not masked by the packet. The condition enum is distinct
from PM4 wait-function encodings. [Comparison contract][vendor] · [HSA
condition enum][conditions]

| `cond` | Native spelling | Satisfied predicate |
| --- | --- | --- |
| 0 | `HSA_SIGNAL_CONDITION_EQ` | `(x & m) == v` |
| 1 | `HSA_SIGNAL_CONDITION_NE` | `(x & m) != v` |
| 2 | `HSA_SIGNAL_CONDITION_LT` | `(x & m) < v` |
| 3 | `HSA_SIGNAL_CONDITION_GTE` | `(x & m) >= v` |

The large-model signal type makes LT and GTE signed comparisons in this ABI.
Clearing bit 63 in the mask restricts the observed operand to nonnegative
values, but does not change a negative reference. A null dependency bypasses
the comparison; it does not remove the header's ordering or fence requests.
[Signal type][signal-types] · [Null dependency and header contract][vendor]

With the barrier bit set and both scopes SYSTEM, the header is `0x1500` and
the first DWORD is `0x00021500`. CLR starts its stream wait with those fields.
Its dependency builder also has a NONE/NONE header template. Before
publication, its stream-ordering optimizer can clear the barrier bit when its
tracked queue ordering already supplies that edge. Fence scope, same-queue
ordering, and the signal predicate retain separate roles. [Header
templates][headers] · [Builder][builder] · [Ordering optimizer][ordering]

## Signal handle, value address and ownership

A native AMD USER signal has a 64-byte, 64-byte-aligned ABI block: kind 1 at
byte 0, signed value at byte 8, and additional event, timestamp and queue
fields. An HSA signal handle names the signal object. A pointer to its value
names only the scalar; those two operands are not interchangeable in an AQL
packet. The surrounding runtime object carries further ownership state.
[Native layout][signal-layout] · [Signal ownership](barriers.md#native-signal-storage-and-last-consumers)

The concrete HIP allocation flow makes that distinction visible:

1. `hipExtMallocWithFlags(..., 8, hipMallocSignalMemory)` selects fine-grained,
   atomic signal memory. This allocation flag requires an eight-byte request.
2. CLR creates an HSA signal initialized to 1 with
   `HSA_AMD_SIGNAL_AMD_GPU_ONLY`, retains its handle, and asks
   `hsa_amd_signal_value_pointer` for the scalar address exposed by the memory
   object. The allocation's eight-byte API size does not describe the full
   native signal object.
3. The CP-wait caller retrieves the retained handle with `Buffer::getSignal()`.
   The shader caller receives a memory address and a width instead.
4. CLR destroys the retained signal when it releases that signal-memory
   allocation. All producers and pending waiters must have stopped using it
   before that destruction.

[HIP allocation][allocation] · [Signal creation and value pointer][signal-create]
· [CP caller][stream-wait] · [Shader launch][shader-launch] · [Signal
destruction][signal-destroy]

The value-pointer API explicitly leaves ownership and lifetime with the
signal. It requires an AMD_GPU_ONLY or IPC-attributed signal and accesses
that are atomic on the platform and respect the signal's usage restrictions.
Extracting the pointer supplies neither a new owner nor permission for
arbitrary ordinary loads and stores. CLR also clears its direct-host-access
flag on this allocation so its memory writes take the blit path. [Pointer
contract][value-pointer] · [Allocation policy][signal-create]

## Stream-memory predicate lowering

With `GPU_STREAMOPS_CP_WAIT` true, CLR lowers HIP's four stream-wait operations
to the following fields. Here `v` and `m` are the API inputs; `x` is the signal
value. The predicates for AND and NOR are algebraic consequences of the
emitted NE packet, not additional native condition codes. [Lowering
switch][stream-wait]

| HIP operation | Packet `value` | Packet `mask` | Packet `cond` | Resulting predicate |
| --- | --- | --- | --- | --- |
| `hipStreamWaitValueGte` | `v` | `m` | GTE | `(x & m) >= v`, with the native signed comparison. |
| `hipStreamWaitValueEq` | `v` | `m` | EQ | `(x & m) == v`. |
| `hipStreamWaitValueAnd` | `0` | `v & m` | NE | `(x & v & m) != 0`. |
| `hipStreamWaitValueNor` | `n = ~v & m` | `n` | NE | `(x & n) != n`, equivalent to `(~(x \| v) & m) != 0`. |

The 32-bit HIP entry point promotes its `uint32_t` reference and mask to
64 bits. The CP packet still names the complete native signal; the promoted
mask excludes its upper 32 value bits. BARRIER_VALUE has no width or byte-offset
field; CLR's CP branch retrieves the allocation's signal handle instead of
adding the API pointer's offset. The default shader path instead selects a
32-bit or 64-bit atomic pointer at that offset. [HIP width handling][width] ·
[CP operands][stream-wait] · [Shader arguments][shader-launch]

### Masked NOR disagreement

The HIP API header prints NOR as `~((x & m) | (v & m)) != 0`. That expression
differs from the CP lowering for a partial mask: complementing after masking
makes every bit outside the mask a possible reason for success. For example,
with `x = 1`, `v = 0`, and `m = 1`, the printed expression is true while the
emitted NE comparison is false. [API formula][api-predicates] · [CP
lowering][stream-wait]

The separately pinned device-library implementation agrees with the CP
lowering: its shader keeps waiting while `((x | v) & m) == m`. Thus both source
implementations confine NOR success to a bit selected by `m`; the printed API
formula does not. This is a disagreement between public source contracts, not
an observed packet-processor failure. A zero effective mask yields no
satisfying AND or NOR state in these implementations. [Shader predicates][shader]

### Shader comparison and ordering

The default stream wait launches one work-item. Its device-library function
polls with relaxed atomic loads at `memory_scope_all_svm_devices` and sleeps
between unsuccessful observations. CLR separately requests system-scope AQL
fences, with a source comment explaining that the shader's atomics can bypass
L2 on some hardware. The atomic load's scope does not replace the surrounding
payload-visibility protocol. [Launch geometry][shader-launch] · [Polling
loop][shader] · [Packet scope policy][stream-wait]

The shader's GTE loop casts the masked left operand to `int` or `long`, but
compares it with an unsigned right operand of the same rank, `uint` or
`ulong`. Usual arithmetic conversion therefore makes that source comparison
unsigned. This is a source-level distinction from the native signed 64-bit
signal contract. With all bits selected, `x = 0x8000000000000000` and `v = 1`
satisfy unsigned GTE but not signed GTE. The 32-bit CP route avoids that
particular sign-bit distinction because it promotes both API operands to
nonnegative 64-bit values. [Shader expressions][shader] · [Native signal
type][signal-types] · [32-bit promotion][width]

The CLR and device-library citations identify separate source revisions. They
establish the displayed lowering and comparison mechanisms, not that every
deployed runtime links that exact library or that firmware implements an
undocumented unsigned variant of the packet.

## Producer, waiter and final reuse

A complete device-to-device dependency can retain one signal across several
epochs without host rearming. Consider producer queue `P`, consumer queue
`C`, dependency signal `S`, a payload, and a separate terminal signal that the
host is allowed to observe:

1. The host allocates and maps the payload, initializes code and arguments,
   initializes `S` to a positive epoch `E`, and initializes the terminal
   signal to 1. It publishes those resources with SYSTEM release before
   publishing their packets.
2. `C` receives a BARRIER_VALUE on `S` with reference `E`, full mask and LT,
   NONE/NONE scopes, a set header barrier bit and no completion signal,
   followed by its consumer dispatch. The wait's header ordering covers any
   preceding work on `C`; its dependency on `P` comes from `S`.
3. `P` receives a producer dispatch with SYSTEM acquire for the host's inputs,
   AGENT release for this same-agent consumer, and completion signal `S`.
   Its release fence publishes the payload before completion decrements `S`
   to `E - 1`. The wait can now pass. `P` remains able to progress independently
   of the waiting consumer.
4. The consumer dispatch performs the matching AGENT acquire before reading
   the payload. A following standard AND barrier with no dependencies and its
   header barrier bit set joins preceding consumer work. NONE acquire and
   SYSTEM release on that barrier publish the result before decrementing the
   terminal signal.
5. The host acquires terminal completion before examining output. Producer
   and consumer last-use observations protect the dependency signal, payload,
   arguments and executable. Ring-slot reuse separately follows the queue's
   read-index protocol.

This composition uses the vendor predicate for readiness, ordinary dispatch
release/acquire for payload visibility, and standard completion for the final
join. It follows the vendor wait contract and HSA's dispatch, fence and
completion rules. [Vendor dependency][vendor] · [HSA System Architecture 1.2,
§§2.9, 3.3.3.1 and 3.3.8][hsa] · [Publication and slot lifetime](publication.md)

An epoch identifies the intended producer only when those producers complete
in epoch order. Independent, unordered producers decrementing one signal
supply a count rather than that identity. A header barrier on each successive
producer dispatch can establish the required same-queue completion order.
Signal reuse also does not return ownership of a payload slot: its previous
consumer must finish before a later producer overwrites it, whether through a
device credit dependency or a terminal host join. These are separate edges in
the lifetime graph. [HSA ordering and completion, §§2.9.1–2][hsa] · [Independent
worksets](barriers.md#fan-in-and-independent-worksets)

The next epoch compares against the next intended value; monotonic descending
values preserve earlier LT predicates until their waiters finish. Rearming,
wraparound and changing an EQ/NE predicate do not inherit that property.
Observing a producer update alone does not retire a pending waiter that still
borrows `S`. A wait's own completion signal can identify its completion, but
does not join later consumer work. [Packet fields][vendor] · [Signal
last-consumer rules](barriers.md#native-signal-storage-and-last-consumers)

CLR's dependency-resolution builder uses reference `kInitSignalValueOne` = 1,
mask `INT64_MAX`, and LT for the first pending signal. It emits ordinary AND
packets for additional dependencies before publishing that value-wait packet.
Its choice of LT for this protocol does not restrict the packet's other
conditions. [Reference constant][initial-value] · [Dependency builder][builder]

Return to [AQL](README.md) or the [primary source map](../../sources.md).

[vendor]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L117-L229
[signal-types]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L1382-L1401
[conditions]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L2005-L2025
[device-policy]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocsettings.cpp#L148-L153
[flag]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/utils/flags.hpp#L224-L225
[admission]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/hipamd/src/hip_stream_ops.cpp#L45-L90
[stream-wait]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L4242-L4308
[headers]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L91-L99
[builder]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L2234-L2309
[initial-value]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.hpp#L42-L46
[ordering]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L1485-L1498
[signal-layout]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/amd_hsa_signal.h#L49-L77
[allocation]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/hipamd/src/hip_memory.cpp#L828-L857
[signal-create]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocmemory.cpp#L972-L991
[signal-destroy]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocmemory.cpp#L770-L783
[value-pointer]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L1397-L1426
[width]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/hipamd/src/hip_stream_ops.cpp#L126-L142
[api-predicates]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/hip/include/hip/hip_runtime_api.h#L3391-L3464
[shader-launch]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocblit.cpp#L3571-L3609
[shader]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/amd/device-libs/opencl/src/misc/amdblit.cl#L852-L921
[hsa]: https://hsafoundation.com/wp-content/uploads/2021/02/HSA-SysArch-1.2.pdf
