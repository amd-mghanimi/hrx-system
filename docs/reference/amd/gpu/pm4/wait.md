# PM4 register and memory waits

`WAIT_REG_MEM` stalls the selected command-processing path until a comparison
passes. Its memory form reads a 32-bit control value; `WAIT_REG_MEM64` carries
a 64-bit reference and mask. The wait connects an observable control value to
later commands. The surrounding protocol supplies producer completion, fresh
control reads, payload cache visibility, independent progress and storage reuse.

## Applicability

PAL declares `PM4_MEC_WAIT_REG_MEM`, `PM4_ME_WAIT_REG_MEM` and
`PM4_PFP_WAIT_REG_MEM`, and corresponding `WAIT_REG_MEM64` structures, in both
its `gfx9` and `gfx12` families. The earlier directory includes GFX10/GFX11
definitions; its name is not a physical architecture predicate. The engine
views have different operation enums and polling controls even where their
packet lengths agree. [Earlier MEC][p-mec] [ME][p-me] [PFP][p-pfp]
[GFX12 MEC][p12-mec] [ME][p12-me] [PFP][p12-pfp]

Declarations and actual selection are separate. PAL's earlier command utility
implements both widths, but its GFX12 utility implements the 32-bit builder.
The ordinary compute memory and bus-marker methods use that 32-bit form.
Register and native queue-control uses also depend on their driver's access
and scheduling context; their field definitions do not establish a user-queue
programming sequence. [Earlier builders][p-builders]
[GFX12 builder][p12-builder] [Earlier callers][p-callers]
[GFX12 callers][p12-callers]

## Representation

Packets contain little-endian DWORDs. Word numbers here are zero-based; PAL's
`ordinal1` is word 0. The [type-3 header](memory-commands.md#representation)
has opcode `0x3c` for the seven-DWORD `WAIT_REG_MEM`, giving count `5`.
The nine-DWORD `WAIT_REG_MEM64` has opcode `0x93` and count `7`.
[Opcodes][p-opcodes] [32/64-bit construction][p-builders]

The following control fields occur in both widths:

| Word and field | Representation |
| --- | --- |
| 1, `function` 2:0 | Comparison function; values below. Value 7 is absent from the cited enums. |
| 1, `mem_space` 5:4 | Register space `0`, memory space `1`; values 2 and 3 are absent from the cited enums. |
| 1, `operation` 7:6 | Engine-specific operation, separate from the comparison and polling offload controls. |
| 1, ME/PFP `engine_sel` 9:8 | ME declares micro engine `0`; PFP declares micro engine `0` and prefetch parser `1`. MEC reserves these bits. |
| 1, `mes_intr_pipe` 23:22 | Two-bit MES interrupt-pipe field. |
| 1, `mes_action` 24 | MES action field. |
| 1, earlier PAL `cache_policy` 26:25 | `LRU=0`, `STREAM=1`, `NOA=2`, `BYPASS=3`. |
| 1, PAL GFX12 `temporal` 26:25 | `RT=0`, `NT=1`, `HT=2`, `LU=3`; these are distinct meanings from the earlier policy enum. |
| Final word, `poll_interval` 15:0 | Encoded polling interval. The field definition alone supplies no wall-clock duration. |
| Final word, MEC `optimize_ace_offload_mode` 31 | ACE offload control. ME/PFP reserve this bit. |

[Earlier MEC fields][p-mec] [ME fields][p-me] [PFP fields][p-pfp]
[GFX12 MEC fields][p12-mec] [ME fields][p12-me] [PFP fields][p12-pfp]

The remaining fields identify the operand and comparison values:

| Operand | `WAIT_REG_MEM` | `WAIT_REG_MEM64` |
| --- | --- | --- |
| Memory `mem_poll_addr_lo` | Word 2 bits 31:2; bits 1:0 reserved, giving four-byte alignment. | Word 2 bits 31:3; bits 2:0 reserved, giving eight-byte alignment. |
| Memory `mem_poll_addr_hi` | All of word 3. | All of word 3. |
| Register `reg_poll_addr` | Word 2 bits 17:0. | Word 2 bits 17:0. |
| Alternate `reg_write_addr1` | Word 2 bits 17:0. | Word 2 bits 17:0. |
| Alternate `reg_write_addr2` | Word 3 bits 17:0. | Word 3 bits 17:0. |
| `reference`, `reference_hi` | Word 4; no high word. | Words 4–5, low then high. |
| `mask`, `mask_hi` | Word 5; no high word. | Words 6–7, low then high. |
| Polling controls | Word 6. | Word 8. |

The memory words encode the byte address, rather than a shifted address placed
into all 32 bits of the low word. Register views reserve their other operand
bits. The named fields above exhaust these PAL structures; their other bits
are reserved. The representable address width does not define a process VM's
usable addresses or make a 64-bit observation atomic with respect to its
producer. [Complete operand views][p-mec] [GFX12 views][p12-mec]

### Comparison and operation values

| `function` | Named comparison against the reference |
| --- | --- |
| 0 | Always pass. |
| 1 | Less than. |
| 2 | Less than or equal. |
| 3 | Equal. |
| 4 | Not equal. |
| 5 | Greater than or equal. |
| 6 | Greater than. |

PAL's public memory-wait contract applies the mask to the memory value before
comparison. Its builder copies the supplied reference and mask without
normalizing them. The enums do not specify full-width signedness for every
relational comparison or settle how out-of-mask reference bits participate.
An unsigned C parameter is not that hardware contract.
[Memory-wait contract][p-api] [Builder operands][p-builders]

The operation enums are the same between the cited PAL generations and widths,
but differ by engine:

| `operation` | MEC | ME | PFP |
| --- | --- | --- | --- |
| 0 | `wait_reg_mem` | `wait_reg_mem` | `wait_reg_mem` |
| 1 | `wr_wait_wr_reg` | — | `wr_wait_wr_reg` |
| 2 | — | `wait_reg_mem_cond` | — |
| 3 | `wait_mem_preemptable` | `wait_mem_preemptable` | `wait_mem_preemptable` |

A dash means the cited enum lacks the named value. The ME conditional operation
is not [COND_EXEC or COND_INDIRECT_BUFFER](conditional.md), and a preemptable
operation is not the MEC offload bit. A header naming either does not promise
that an independently queued producer can run under every scheduling policy.
[Earlier operation enums][p-mec] [ME enum][p-me] [PFP enum][p-pfp]
[GFX12 operation enums][p12-mec] [ME enum][p12-me] [PFP enum][p12-pfp]

### Linux field views

Linux's `soc15d.h` and `nvd.h` supply corresponding `PACKET3_WAIT_REG_MEM__*`
macros. Their named register operands are 18 bits; `nvd.h` names both
`CACHE_POLICY` and `TEMPORAL` at bits 26:25. These aliases do not identify one
common policy for all devices. The legacy `WAIT_REG_MEM_ENGINE` macro in
`soc15d.h` places its selector at bit 8, separately from the MEC-style macro
set. [SOC15 macros][l-soc15] [Combined macros][l-nvd]

The GC12.1 header has another specific view:

| Word and full macro name | GC12.1 representation |
| --- | --- |
| 1, `PACKET3_WAIT_REG_MEM__MODE` 11:10 | Local XCD `0`, remote/local AID `1`, remote XCD `2`, remote MID `3`. |
| 1, `PACKET3_WAIT_REG_MEM__MID_DIE_ID` 13:12 | Two-bit MID identifier. |
| 1, `PACKET3_WAIT_REG_MEM__XCD_DIE_ID` 17:14 | Four-bit XCD identifier. |
| 1, `PACKET3_WAIT_REG_MEM__TEMPORAL` 26:25 | `RT=0`, `NT=1`, `HT=2`, `LU=3`. |
| 2, `PACKET3_WAIT_REG_MEM__REG_POLL_ADDR` | Full low DWORD. |
| 3, `PACKET3_WAIT_REG_MEM__REG_POLL_ADDR_HI` 13:0 | Fourteen high bits, giving a 46-bit register-poll representation. |
| 2/3, `PACKET3_WAIT_REG_MEM__REG_WRITE_ADDR1` / `REG_WRITE_ADDR2` 17:0 | Alternate write-register operands remain 18 bits each. |

The memory-address, reference, mask, MES and final polling fields retain the
32-bit-wait positions above. This view has no `ENGINE` macro and does not
extend PAL's register view by implication. The GC12.1 header names opcode
`WAIT_REG_MEM64` separately without supplying an equivalent complete macro
layout for it. [GC12.1 fields][l121-fields] [GC12.1 opcodes][l121-opcodes]

ROCm's libhsakmt header has two more named 64-bit structures. Its `gfx9`
view reserves word 1 bits 31:8 and polling-word bits 31:16; its `gfx10` view
adds the MES/cache and offload fields at the earlier PAL positions. Both have
the nine-word operand/reference/mask layout above. These declarations do not
show an actual 64-bit wait in the WDDM barrier translator described below.
[ROCm earlier view][r64-old] [ROCm later view][r64-new]

## Selected construction

PAL zero-initializes the packet, selects function, memory space and operation,
copies the low/high address, reference and mask, and uses polling interval
`Device::PollInterval`, which is `10`. The 32-bit helper defaults to ordinary
operation `0`; the earlier 64-bit helper always selects ordinary operation.
Graphics-capable engines receive the passed engine selector. Compute receives
MEC `optimize_ace_offload_mode=1`; MES fields and policy/temporal bits stay zero.
[Earlier construction][p-builders] [Default operation][p-default]
[GFX12 construction][p12-builder] [Polling constant][p-poll]

The 32-bit helper asserts four-byte memory alignment or the 18-bit register
view as selected. The earlier 64-bit helper asserts the three low address bits
are zero unconditionally, even though it accepts a memory-space argument.
Its existence does not demonstrate a usable 64-bit register-wait caller.
These are builder assertions, not recoverable public input checks or an
all-transport access rule. [Address construction][p-builders]

PAL's ordinary memory-wait API requires the caller to transition the control
location for `PipelineStagePostPrefetch` and `CoherCp`, and documents four-byte
alignment. `CmdWaitMemoryValue` itself emits the wait; it does not add that
transition. `CmdWaitBusAddressableMemoryMarker` obtains the memory object's
marker address and forwards the same comparison to the memory-wait method.
[API preconditions][p-api] [Earlier compute methods][p-callers]
[GFX12 compute methods][p12-callers]

Mesa's `ac_emit_cp_wait_mem` fixes memory space to `1`, copies the caller's
`flags` into the control word, and emits polling interval `4`. It does not add
a comparison, offload, preemption or cache operation. RADV's wrapper accepts
EQ, NE and GE comparisons; graphics and compute streams use PM4, while an
SDMA stream receives an SDMA wait. A shared wrapper name does not make those
the same command-processor protocol. [Mesa builder][m-builder]
[RADV transport selection][m-wrapper]

PAL translates its public `CompareFunc` through a table rather than copying
the enum ordinal. `Never` is forbidden by its API and asserted against; the
table's placeholder entry is always-pass, not a never-satisfying hardware
operation. [Comparison translation][p-compare] [Public contract][p-api]

## Execution and cache dependencies

The ordinary memory protocol is:

```text
producer work → join the actual producer → release payload → publish control
                                                              ↓
                                      fresh control read → wait passes
                                                              ↓
                                      acquire payload → consumer work
                                                              ↓
                                      final consumer completion
```

The producer and consumer mappings must make the control update observable
before the consumer reaches its payload acquire. A later invalidate cannot
repair a poll that is already reading stale control state. Likewise, a wait
does not join unrelated producer work, invalidate shader caches, or retire
payload still used by a following dispatch. [Event release/acquire contract][p-event-api]
[Cross-queue handoff](handoff.md) describes the native mapping and directed
visibility premises; [cache control](cache.md) owns the generation-specific
maintenance operations.

PAL's `AcquireEvent` is a concrete decomposition: it emits full-mask equality
waits on each bound event slot, then calls `AcquireInternal` for visibility
and layout work. Both earlier and GFX12 implementations retain that order.
Its compute event writer selects RELEASE_MEM when compute or bottom-of-pipe
stages participate, and otherwise a CP WRITE_DATA. Asynchronous CP DMA has
its own join. A CP store merely following a dispatch is not a shader-result
release. [Earlier event acquire][p-event-acquire]
[GFX12 event acquire][p12-event-acquire] [Compute event producer][p-event-write]

PAL's non-PWS release-token path uses a different comparison: full-mask GE
waits against its command-buffer fence values. The PWS path uses ACQUIRE_MEM
counter synchronization instead. When a graphics dependency must hold the
PFP, PAL places PFP_SYNC_ME after the ME waits and cache work so the earlier
parser cannot resume too soon. A wait's engine/stage is part of its meaning.
[Release-token selection][p-token-wait] [Final parser synchronization][p-parser]

The [compute-completion overview](memory-commands.md#compute-completion-and-firmware)
retains the CS_PARTIAL_FLUSH firmware predicates and the owned
RELEASE_MEM/equality-wait alternative. Their shader-completion role does not
change the independent cache or final-storage obligations.

### Event storage and host participation

PAL binds explicit event memory. CPU-accessible bindings are mapped and reset
when bound; the host `Set` and `Reset` methods write that mapped DWORD.
GPU-only events admit invisible memory and leave initial state to the client.
The constructor's event-state rule and the mapping's CPU-access property are
separate from the dependency that makes a later reset safe.
[Event contract][p-event-storage-api] [Binding and host access][p-event-storage]

RADV creates an eight-byte, eight-byte-aligned event allocation with GL2
bypass. Device-only events use VRAM without CPU access; other events use GTT
with a CPU mapping. Its host event methods read or write a 64-bit mapped value,
while its PM4 wait compares the low DWORD against `1` with a full 32-bit mask.
That particular 0/1 protocol does not prove that arbitrary 64-bit updates can
be observed atomically by either wait width. `CmdWaitEvents2` adds each event
allocation to the command stream, emits the waits, then applies the dependency
barriers. [RADV event allocation and host methods][m-events]
[RADV event wait and barrier][m-event-wait]

An event object and its allocation remain live for every submitted use. Seeing
the producer's set value does not show that the consumer has read it, and
resetting it early can remove the condition on which the consumer depends.
Reusable control state needs a last-reader edge or an ordered value protocol
whose publication/reuse rules preserve every pending observation.
[Command-storage and retained-use ownership](command-buffers.md)

### Gang scheduling, preemptable entry and final join

RADV's queue-level gang semaphore uses two distinct DWORDs: leader-to-follower
entry and follower-to-leader completion. Its allocation always requests GL2
bypass because the entry preamble can run before the normal cache flush, and
because its selected CP/SDMA routes can be non-coherent. That control allocation
is different from the command buffer's own per-phase gang semaphore.
[Queue-level allocation and protocol][m-gang-queue]

The queue-level sequence is:

1. The leader preamble writes `1` to the entry cell. The ACE preamble uses GE
   against `1`, full mask and operation `wait_mem_preemptable`; after passing,
   ACE clears that cell.
2. Each stream performs its recorded work. The ACE postamble publishes `1`
   to the completion cell through a bottom-of-pipe release.
3. The leader postamble uses an ordinary wait on the completion cell and clears
   it. A graphics leader selects PFP; an SDMA leader uses an SDMA wait instead
   of PM4.
4. The native leader completion can then cover the joined gang. RADV explains
   that without this postamble join, the kernel's leader fence could allow
   command-buffer reuse while the follower still executes.

[Selected preambles/postambles][m-gang-queue] [Submitted gang wrappers][m-gang-submit]

This real preemptable-wait use comes with gang scheduling, allocation and
submission owners. It is not a standalone fairness guarantee. Ordinary
per-phase gang waits use GE too, but use the ordinary wait operation through
the transport wrapper. Their publisher is an SDMA fence or CP release; the
CP-to-SDMA path selects the required cache writeback. Separate semaphore
storage is chosen for dedicated-VRAM placement or non-coherent participants;
that allocation requests GL2 bypass only for the non-coherent case. The
coherent path may use initialized upload storage. At command finalization,
each reader clears the opposite stream's cell for the next completed reuse.
[Per-phase allocation and publishers][m-gang-phases]
[Per-phase waits and final clear][m-gang-final]

## Native ring synchronization

Linux's ordinary pipeline-sync wait compares the ring's own
`fence_drv.sync_seq` against its fence writeback location, with equality,
full mask and interval `4`. The GFX6/7/8, GC9.4.3, GFX10/11, GFX12 and
GC12.1 implementations use this shape. The GFX7/8, GFX10/11 and GFX12.0
callers select PFP for a graphics ring and ME for compute. GFX6 fixes its
selector to PFP; GC9.4.3 passes ME. GC12.1 supplies no encoded engine selector.
The GFX9.0 implementation instead emits shader partial-flush and
memory-synchronization operations;
its function name does not imply a memory wait.
[GFX6][l6-pipeline] [GFX7][l7-pipeline] [GFX8][l8-pipeline]
[GFX9.0 distinction][l9-pipeline] [GC9.4.3][l943-pipeline]
[GFX10][l10-pipeline] [GFX11][l11-pipeline] [GFX12][l12-pipeline]
[GC12.1][l121-pipeline]

This is ring-owned control storage. Ring initialization obtains a writeback
slot and CPU/GPU aliases; the fence driver installs those addresses and
initializes the value. Fence emission increments its 32-bit sequence and
selects the native fence writer with an interrupt request. Submission requests
pipeline synchronization for an explicit dependency, context switch or VM
condition; the VM sequence can consume that request before the IB sequence.
The payload IBs, final fence and remaining native wrappers are recorded before
the padded ring is published. A CPU fence observer and scheduler/suballocation
owners account for completion and IB reuse separately from the packet wait.
[Ring storage][l-ring-storage] [Fence initialization][l-fence-init]
[Fence publication][l-fence-emit] [Submission sequence][l-submit]
[Ring publication][l-ring-publish] [CPU observer][l-fence-observe]
[IB retirement][l-job-retire] [IB release][l-ib-free]
[Suballocation retirement][l-suballoc]

Native register polling uses a different operand contract. The GFX11 helper
emits ordinary register equality with interval `0x20`; its combined
register-write/wait callback selects operation `1` and two register operands.
GC12.1 normalizes register offsets before emission and leaves its new routing
fields zero. Its retained `eng_sel` parameter is not encoded by that helper.
[GFX11 helper][l11-builder] [GFX11 callers][l11-register]
[GC12.1 helper][l121-builder]
[GC12.1 register callers][l121-register]

### Firmware selection for combined register operations

The Linux GFX9 callback uses separate ME and MEC eligibility flags. Graphics
rings require the listed ME and PFP conditions; other rings use MEC. Each
pair below is minimum firmware image version / minimum feature version, and
all conditions within a cell apply. These are that driver's selection rules,
not conditions on an ordinary user-memory wait.

| GC IP selected by the GFX9 implementation | Graphics: ME and PFP | Compute: MEC |
| --- | --- | --- |
| 9.0.1 | ME `0x9c / 42`, PFP `0xb1 / 42` | `0x193 / 42` |
| 9.2.1 | ME `0x9c / 44`, PFP `0xb2 / 44` | `0x196 / 44` |
| 9.4.0 | ME `0x9c / 44`, PFP `0xb2 / 44` | `0x197 / 44` |
| 9.1.0 or 9.2.2 | ME `0x9c / 42`, PFP `0xb1 / 42` | `0x192 / 42` |
| The switch's default branch | Enabled. | Enabled. |

The preceding old-firmware warning has a different predicate and does not
replace this selection table. [GFX9 flags][l9-firmware]
[GFX9 operation selection][l9-register]

The GFX10 implementation has one combined flag. Its GC10.1.1, 10.1.2,
10.1.3, 10.1.4 and 10.1.10 cases require ME `0x46 / 27`, PFP `0x68 / 27`
and MEC `0x5b / 27` together. Its GC10.3.0 through 10.3.7 cases enable the
flag; the switch's default leaves it disabled. The callback selects operation
`1` when that flag is set. [GFX10 flags][l10-firmware]
[GFX10 operation selection][l10-register]

When the GFX9/GFX10 flag is clear, a helper emits two operations: write `ref`
to `reg0`, then ordinary register wait on `reg1` with reference **`mask`** and
mask **`mask`**. The write value is not copied into that wait's reference.
GC9.4.3 always uses this helper; GFX11, GFX12.0 and GC12.1 directly select the
combined operation in their callbacks. These source-specific choices do not
turn the combined operation into an ordinary memory dependency.
[Separate-operation helper][l-register-helper] [GC9.4.3 selection][l943-register]
[GFX11 selection][l11-register] [GFX12 selection][l12-register]
[GC12.1 selection][l121-register]

## Runtime-selected wait paths

ROCr's PC-sampling path selected by `!LargeBarEnabled()` uses an ordinary
32-bit wait with equality, full mask, interval `4` and MEC offload. After
switching the active buffer, it waits for the old buffer's written count,
copies the samples, then resets that count with confirmed WRITE_DATA.
The producer trap code completes sample stores before incrementing the
written count. For ISA Major 12, Minor 0 or 5, the copy stream additionally
emits GL2 writeback after the wait and before DMA. The counter observation
and that payload-cache step remain distinct. [Path selection][r-pcs-select]
[Active-buffer switch and count observation][r-pcs-switch]
[Wait/copy/reset sequence][r-pcs] [Earlier producer][r-trap]
[GFX12 producer][r12-trap]

The normal host path requests SYSTEM release on `ExecutePM4` and acquires
its completion before advancing the host-buffer write offset. On GFX9 and
newer, supplying that signal selects asynchronous submission; the command and
sample buffers must span the later observation. The readiness wait alone is
not the copy's completion or host-result retirement. The [inline-write
chapter](write.md#selected-memory-callers) and [AQL carrier](../aql/transfers.md#carrier-representation-and-publication)
describe the corresponding reset and command-storage owners.
[Host observer][r-pcs] [Carrier completion][r-carrier]

CLR has another actual PAL consumer: `CL_COMMAND_WAIT_SIGNAL_AMD` records a
bus-addressable marker wait with GE and full mask, retains the memory in its
VM/command references, and records a GPU event. Its write-signal path flushes
L2 and waits for the associated buffer's prior GPU event before publishing
the marker. This is the bus-marker protocol, not a declaration that all CLR
stream waits use PM4. Its `KernelBlitManager::streamOpsWait` implements both
32- and 64-bit stream operands with a one-work-item shader and synchronizes
that operation. [Bus-marker caller][r-clr-marker]
[Shader stream-wait implementation][r-clr-stream]

ROCm's DXG/WDDM AQL barrier translator also separates its API name from the
hardware packet. It resolves dependency signals on the CPU: AND waits on
each non-null signal; a nonempty OR set is repeatedly inspected until one
signal is zero. An empty dependency set bypasses those waits. When a completion
signal is present, the translator records its cache/completion work; it then
records read-pointer progress regardless of that signal's presence. The header's
64-bit PM4 wait structures are not evidence that this translator emits them.
[WDDM dependency consumer][r-wddm-barrier]

### Waiting on a ready word inside a wider result

RADV's graphics/compute query-copy paths illustrate why result width and wait
width differ. With `VK_QUERY_RESULT_WAIT_BIT`, the following observers all use
32-bit memory waits and full masks before a separate result-processing shader:

| Query result | Source-selected observation |
| --- | --- |
| Occlusion when `radv_occlusion_query_use_l2` is false | GE against `0x80000000` in the upper word of the last enabled render-backend entry. |
| Pipeline statistics | EQ against a separate availability DWORD containing `1`; ACE plus emulated mesh-query use additionally waits on the start/stop upper words with GE against `0x80000000`. |
| Transform feedback | GE against `0x80000000` in all four result upper words. |
| Timestamp | NE against `TIMESTAMP_NOT_READY >> 32` in the high DWORD, because the low DWORD can legitimately equal `0xffffffff`. |
| Primitives generated | GE against `0x80000000` in result upper words at byte offsets 4 and 20; emulated queries below GFX11 also use offsets 36 and 44. |
| Mesh primitives | GFX11+ uses a separate availability DWORD EQ `1`; the earlier path waits on upper words at offsets 4 and 12 with GE against `0x80000000`. |

[Occlusion][m-query-occ] [Pipeline statistics][m-query-pipe]
[Transform feedback][m-query-tfb] [Timestamp][m-query-time]
[Primitives generated][m-query-prims] [Mesh primitives][m-query-mesh]

Those are the selected observers of each producer's result format, not an
atomic multiword comparison or a general interpretation of every relational
input. The result-processing shader requests its own cache invalidation.
After dispatch, RADV records compute/cache work in `active_query_flush_bits`
so a later command-buffer query reset joins those readers before overwriting
the pool. Result availability, result-copy completion and pool reuse are
three different observations. [Shader reader][m-query-shader]
[Reset dependency][m-query-reset]

RadeonSI selects its shader-query path for GFX11+ primitive and streamout
queries. Shader atomics update the counters; a separate bottom-of-pipe release
publishes the last emitted record's fence when a query ends. CPU result
retrieval uses buffer mapping synchronization, whereas resource-result
retrieval launches a compute reader. The resource path does not inherit a
mandatory CPU wait from the query API.
[Query selection][m-si-query-select] [Counter producer][m-si-query-counter]
[Record fence and CPU reader][m-si-query-producer] [Resource-result API][m-query-api]

The resource caller's `PIPE_QUERY_WAIT` branch passes reference `1`, mask `1`
and **flags `0`** to the common helper. Because that helper preserves the
flags, this selects always-pass, despite the comment describing an availability
wait. Before dispatch, the internal-operation helper requests VMEM invalidation
and consults buffer idleness and binding history. A busy buffer requests PS
synchronization when fragment-stage history is present, otherwise VS; compute
history adds CS synchronization. In the ordinary branch without a CB/DB flush,
stage joins retained after barrier reduction become partial-flush events;
cache acquisition follows. Those selected packets contain no comparison
against the record fence.
[Query caller][m-si-query] [Buffer-idle predicate][m-si-query-idle]
[Internal-operation dependencies][m-si-query-barriers]
[Barrier reduction and lowering][m-si-query-lowering]

The result shader samples each visited record's fence once and stops that
record scan on zero. Its final store depends on the computed missing-data
state; availability mode writes a Boolean instead. Chained buffers pass a
summary between dispatches, and each visited record assigns that state again.
The shader does not spin until readiness. Shader completion, cache visibility
and observation of the separately produced record marker remain distinct.
The emitted always-pass packet supplies no readiness test; any guarantee must
come from the surrounding producer and synchronization protocol.
[Result shader][m-si-query-shader]

## Lifetime of a reusable wait protocol

| Resource | Owner and final use |
| --- | --- |
| Wait packet, reference and mask | Captured in command storage; retained through command consumption and any native submission tail that still reads it. |
| Control allocation | Mapped for the publisher and poller; retained through their last accesses. A satisfying value is not the last reader's acknowledgment. |
| Producer inputs, code and arguments | Retained through producer execution, independently of which packet later signals control. |
| Released payload | Retained through the consumer's actual accesses; a passed wait starts that consumer interval rather than ending it. |
| Rearm/reset write | Ordered after prior readers and before the next use; otherwise an old value can satisfy a new wait or an early reset can hide a pending one. |
| Native completion and notification storage | Retained through their separate final writers/readers, including work after the dependency signal. |

A complete repeated handoff establishes the native mappings and initial
non-satisfying state, publishes the producer/release and consumer/wait/acquire
sequences, observes the final dependent completion, and joins any independent
tails before rearming or releasing storage. [Queue publication](publication.md),
[cross-queue handoff](handoff.md) and [command buffers](command-buffers.md)
describe those surrounding owners. An ordinary poll has no timeout field;
the interval does not supply a deadline, cancellation path or queue-retirement
observation. [Polling representation][p-mec]

[p-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L2286-L2569
[p-me]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_me_pm4_packets.h#L3165-L3460
[p-pfp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L3774-L4071
[p12-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L2612-L2895
[p12-me]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_me_pm4_packets.h#L2931-L3226
[p12-pfp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L4462-L4759
[p-opcodes]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_pm4_it_opcodes.h#L65-L130
[p-builders]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4439-L4594
[p-default]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.h#L745-L763
[p12-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L1992-L2077
[p-poll]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/device.h#L1020-L1025
[p-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L4254-L4308
[p-callers]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1097-L1147
[p12-callers]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L604-L629
[l-soc15]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/soc15d.h#L177-L225
[l-nvd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/nvd.h#L163-L224
[l121-fields]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1_pkt.h#L152-L217
[l121-opcodes]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1_pkt.h#L545-L548
[m-builder]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L89-L102
[m-wrapper]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cs.h#L172-L186
[p-compare]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4919-L4941
[p-event-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L2818-L2862
[p-event-acquire]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L2614-L2656
[p12-event-acquire]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L1717-L1754
[p-event-write]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1327-L1384
[p-token-wait]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L1640-L1733
[p-parser]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L2027-L2055
[p-event-storage-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palGpuEvent.h#L54-L89
[p-event-storage]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/gpuEvent.cpp#L84-L244
[m-events]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_event.c#L31-L129
[m-event-wait]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L16502-L16526
[m-gang-queue]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1347-L1500
[m-gang-submit]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1718-L1804
[m-gang-phases]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1851-L2013
[m-gang-final]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L2015-L2086
[r64-old]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/include/impl/pm4_cmds.h#L774-L824
[r64-new]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/include/impl/pm4_cmds.h#L952-L1006
[l6-pipeline]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v6_0.c#L2382-L2397
[l7-pipeline]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v7_0.c#L3105-L3120
[l8-pipeline]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v8_0.c#L6085-L6100
[l9-pipeline]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_0.c#L5655-L5663
[l943-pipeline]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_4_3.c#L3019-L3028
[l10-pipeline]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c#L8754-L8762
[l11-pipeline]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L6109-L6117
[l12-pipeline]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c#L4596-L4604
[l121-pipeline]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1.c#L3771-L3778
[l-ring-storage]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c#L197-L419
[l-fence-init]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_fence.c#L407-L432
[l-fence-emit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_fence.c#L101-L146
[l-submit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c#L209-L350
[l-ring-publish]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c#L169-L189
[l-fence-observe]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_fence.c#L207-L249
[l-job-retire]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_job.c#L296-L312
[l-ib-free]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c#L97-L100
[l-suballoc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_sa.c#L101-L109
[l11-builder]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L544-L565
[l11-register]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L6387-L6401
[l121-builder]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1.c#L354-L379
[l121-register]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1.c#L3869-L3881
[l9-firmware]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_0.c#L1293-L1358
[l9-register]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_0.c#L5973-L5988
[l10-firmware]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c#L4140-L4174
[l10-register]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c#L9038-L9054
[l-register-helper]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c#L426-L439
[l943-register]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_4_3.c#L3145-L3151
[l12-register]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c#L4733-L4747
[r-pcs-select]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3758-L3795
[r-pcs-switch]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4735-L4795
[r-pcs]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4831-L4953
[r-trap]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/trap_handler/trap_handler.s#L645-L659
[r12-trap]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/trap_handler/trap_handler_gfx12.s#L968-L1005
[r-carrier]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1609-L1759
[r-clr-marker]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/pal/palvirtual.cpp#L3221-L3268
[r-clr-stream]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/pal/palblit.cpp#L2585-L2620
[r-wddm-barrier]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/dxg/wddm/queue.cpp#L790-L877
[m-query-occ]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L362-L389
[m-query-pipe]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L758-L800
[m-query-tfb]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L1018-L1042
[m-query-time]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L1150-L1174
[m-query-prims]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L1424-L1456
[m-query-mesh]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L1636-L1679
[m-query-shader]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L1783-L1835
[m-query-reset]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L2561-L2601
[m-si-query]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/gfx11_query.c#L268-L418
[m-si-query-select]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_query.c#L1202-L1219
[m-si-query-counter]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/gfx/si_nir_lower_abi.c#L497-L510
[m-si-query-producer]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/gfx11_query.c#L151-L266
[m-query-api]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/docs/gallium/context.rst#L430-L440
[m-si-query-idle]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_pipe.h#L2151-L2156
[m-si-query-lowering]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_barrier.c#L40-L319
[m-si-query-shader]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/gfx/si_shaderlib_nir.c#L766-L977
[m-si-query-barriers]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_barrier.c#L540-L654
