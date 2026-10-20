# SDMA atomic operations and signaling

SDMA has several ordinary signaling protocols with different owners and memory
contracts. ROCr uses `ATOMIC` to decrement copy-completion signals. PAL uses
`MEM_INCR` to retire command storage. Legacy Radeon SI and CIK drivers use
different semaphore encodings to join rings. An operation's packet layout,
payload visibility and final storage use are separate parts of each protocol.

## Applicability

The protocols below describe distinct native consumers. The detailed sections
state their engine, transport, memory, and ownership conditions.

| Protocol | Encoding | Native consumer |
| --- | --- | --- |
| ROCr completion decrement | Opcode 10, operation 47 (`ADD64`), eight DWORDs. | An HSA signal observer or dependent operation. |
| PAL command retirement | Opcode 7, suboperation 1 (`MEM_INCR`), three DWORDs. | The command allocator's root-chunk busy tracker. |
| SI ring handshake | Legacy opcode 5 at header bits 31:28 (`DMA_PACKET_SEMAPHORE`), three DWORDs. | A waiting ring using the Radeon semaphore owner. |
| CIK ring handshake | Opcode 7, suboperation 0 (`SEMAPHORE`), three DWORDs. | A waiting ring, with semaphore storage retained through the consumer fence. |
| gfx125 fused copy signaling | COPY_LINEAR with optional WAIT/SIGNAL blocks. | The selected copy path's dependency and completion signals. |

## Copy completion through ADD64

The HSA async-copy API requires both agents to access both buffers, valid
nonoverlapping extents and completion of all dependency signals before the
copy. Completion decrements the output signal; negative values represent
asynchronous errors. The caller must ensure system-level coherent buffers
because the DMA engines may sit outside the same coherency domain. In general,
this requires a sending-device SYSTEM release before the copy and a recipient
SYSTEM acquire before consuming its output. An atomic control update does not
replace those payload obligations. [Public copy contract][copy-api]

ROCr's classic submission builds this complete flow:

```text
dependency polls → selected HDP/cache acquire → copy → selected cache release
  → gang joins, if any → output decrement/store → optional mailbox FENCE/TRAP
```

[Stream construction][copy-stream] [Notification tail][completion]

`BlitSdma::Initialize` selects atomic completion from the directed link between
the GPU node and `cpu_agents()[0]`: `atomic_support_64bit`, except gfx701
explicitly disables it. Initialization rejects a non-GPU agent and the FULL
HSA profile. This is the actual source predicate, not a guarantee for every
integrated GPU, memory attachment or atomic operation. Topology initialization
applies the link's `Override` and `NoAtomics64bit` flags. Its comment about
disallowing XGMI overrides disagrees with the unconditional `if (Override)`
code; the predicate follows the code. Linux derives directional PCIe atomic
flags separately and exempts XGMI from that check.
[Runtime selection][atomic-gate] [Link interpretation][link-flags]
[Kernel directionality][linux-links]

When the platform predicate is true, the output update is ADD64 with operand
`UINT64_MAX`. Otherwise ROCr computes `LoadRelaxed() - 1` on the CPU and emits
the necessary high-word FENCE followed by low-word FENCE. Its source explicitly
uses FENCE because consecutive copy/write commands may overlap. That alternate
path supplies neither a generic concurrent read-modify-write nor an indivisible
64-bit store. [Completion construction][completion] [Fence choice][fence-choice]

The implementation has four direct calls to `BuildAtomicDecrementCommand`:

| Caller | Updated cell and preceding dependency |
| --- | --- |
| Ordinary `SubmitCommand`, gang leader | Each internal gang cell, after polling it to one. This is the leader's final access before the cell can be destroyed. |
| Ordinary `SubmitCommand`, completion | The operation's output cell, after its selected payload maintenance and gang joins. |
| Classic `SubmitBodies` | The shared fan-out output cell, once per engine group after that group's copy packets. The no-platform-atomics branch instead fences its private body cell to zero. |
| Classic `SubmitEpilogue` | The fan-out output cell after joining body completion. The no-platform-atomics branch instead stores zero. |

The ordinary wrapper also serves rectangular copy, fill, broadcast and
profiled multicast callers. The fan-out owner raises the shared counter for
the number of participating engine groups when using atomic completion;
its no-platform-atomics path allocates one private body cell per group.
Each protocol owns its counter initialization and final join.
[Ordinary calls][gang-commands] [Ordinary output][completion]
[Body completion][atomic-body] [Epilogue completion][atomic-epilogue]
[Fan-out signal selection][fanout-signal-selection]
[Broadcast wrapper][atomic-copy-wrappers]
[Profiled multicast wrapper][atomic-profiled-multicast]
[Rectangular and fill wrappers][atomic-rect-fill]
[Blocking wrapper][atomic-blocking]

### Representation

`SDMA_PKT_ATOMIC` occupies eight DWORDs. Its body carries the target address,
source operand and comparison operand; it has no separate returned-value
address. The common body and source-specific header views are:

| DWORD and bits | Field and representation |
| --- | --- |
| 0, 7:0 | `op`: 10, ATOMIC. |
| 0, 16 | `loop` / ROCr `l`: loop control. |
| 0, 31:25 | `atomic_op` / ROCr `operation`: seven-bit operation selector. |
| 1–2, 31:0 each | `addr_31_0`, `addr_63_32`: target byte address in the classic and ROCr layouts. PAL GFX12 represents the low word as `addr_31_2` at bits 31:2, with bits 1:0 unnamed. |
| 3–4, 31:0 each | `src_data_31_0`, `src_data_63_32`: source operand. |
| 5–6, 31:0 each | `cmp_data_31_0`, `cmp_data_63_32`: comparison operand. |
| 7, 12:0 | `loop_interval`; bits 31:13 are unnamed/reserved in the structures. |

[ROCr layout][atomic-layout] [PAL GFX10 layout][pal-atomic-layout]
[PAL GFX12 layout][pal12-atomic-layout]

| Source layout | Additional DWORD 0 fields |
| --- | --- |
| Linux Tonga | No additional named fields; in particular, its macros do not name bits 15:8. |
| Linux Vega10 / Navi10; PAL GFX10 base view | `tmz` at bit 18. PAL leaves bits 15:8 unnamed. |
| Linux SDMA6.0 / SDMA7.1; PAL `gfx103Plus` view | `tmz` at bit 18, `cache_policy` at bits 22:20 and `cpv` at bit 24. |
| PAL GFX12 | `sub_op` at bits 15:8, `tmz` at bit 18 and `mall_policy` at bits 23:22; bits 17, 21:19 and 24 are unnamed. |
| ROCr | `sub_op` at bits 15:8, `tmz` at bit 18, `scope` at bits 21:20 and `temporal_hint` at bits 24:22; bits 17 and 19 are reserved. |

[Tonga][linux-tonga-atomic] [Vega10][linux-vega-atomic]
[Navi10][linux-navi-atomic] [SDMA6.0][linux-atomic]
[SDMA7.1][linux71-atomic] [PAL GFX10][pal-atomic-layout]
[PAL GFX12][pal12-atomic-layout] [ROCr][atomic-layout]

These are complete named fields in the cited source views, not interchangeable
policy encodings. An absent macro names no hardware meaning. A full low-address
word also does not establish byte-aligned atomic operands; the operation and
its storage owner supply the alignment contract.

The ordinary ROCr decrement zeroes the complete packet, selects ADD64
(operation 47), sets both source words to `0xffffffff`, and leaves comparison,
loop and interval zero. The scoped template additionally writes SYS scope (3);
the temporal hint remains zero. The signal owner provides a 64-byte-aligned
structure with its signed 64-bit value at byte offset 8, giving this target
eight-byte alignment. The observer reads the updated cell rather than an old
value returned elsewhere. This caller supplies no looping or interval-unit
contract and establishes no other operation/width combinations.
[Builder][atomic-builder] [Signal storage][signal-layout]
[Operation and scope constants][atomic-constants]

PAL's public `CmdMemoryAtomic` contract includes 32- and 64-bit operations,
but its DMA implementation inherits the base unsupported method rather than
an SDMA ATOMIC builder. That API's operation list and shader-atomic contract
therefore do not establish SDMA operation eligibility.
[Public API][pal-atomic-api] [Base implementation][pal-atomic-base]

### Architecture and transport differences

The ROCr factory selects these templates. The aliases determine whether the
builder emits USER_GCR and scope fields. Cache maintenance for the copied
payload remains independent of the signal update. [Factory][factory] [Template aliases][aliases]

| Source predicate | Template behavior |
| --- | --- |
| gfx9, including gfx942 | V4: no USER_GCR and no scope fields. The directed atomic-link predicate still applies. |
| gfx10 outside DXG | V5: USER_GCR, without scope fields. |
| gfx11/12 outside DXG, minor < 5 | V5. |
| gfx11/12 outside DXG, minor >= 5 | V6: scope fields, without USER_GCR. |
| gfx10/11/12 on DXG | V4; the factory assigns surrounding GCR work to the underlying driver. |

This factory has explicit major-version cases 9, 10, 11 and 12; the default
asserts and returns null. The initializer's retained gfx701 special case
therefore does not establish a selected gfx7 path through this factory.
[Complete factory][factory]

The V6 alias comment describes DACC/OSS7.1, but the factory also selects it for
gfx11.5. This matters for ATOMIC: the pinned Linux SDMA6 and PAL gfx103Plus
layouts name header bits 22:20 **cache policy** and bit 24 **CPV**, whereas
ROCr names bits 21:20 **scope** and bits 24:22 **temporal hint**. Matching bit
positions do not resolve that semantic discrepancy. The gfx11.5 factory choice
and shared opcode leave that difference unresolved; they do not identify which
interpretation applies to a particular SDMA6 firmware interface. The newer
ROCm revision `105dd4ff35798f95646353bc08f6c885416ae17e` retains that ordinary
factory predicate; its separate fused-operation selection does not resolve it.
[Linux policy fields][linux-atomic] [PAL policy fields][pal-atomic]
[Newer factory][current-factory]

HDP work has another predicate: the runtime setting, gfx major >= 9 excluding
gfx10.1, and a non-XGMI link. That test neither establishes a universal HDP
requirement nor supplies control-cell atomic reach. ROCr selects atomic
completion using the ISA, template, transport, and link conditions above. Its
separate HDP setting is sampled during initialization and checked again when
constructing the copy stream.
[HDP support][atomic-gate] [HDP setting][hdp-setting]
[HDP emission][hdp-emission]

### Memory and lifetime

ROCr's busy-wait and interrupt-signal implementations compare the full 64-bit
value. Their acquire waits perform the relaxed wait followed by an acquire
fence; the interrupt path rechecks the value after an event wake. That
observation does not make two FENCE32 stores indivisible. The AMD signal
layout and the public copy contract remain the source of the value and
payload-visibility obligations, respectively.
[Busy-wait observer][signal-observer] [Interrupt observer][interrupt-observer]

ROCr appends any mailbox FENCE/TRAP after the completion decrement. Observing
the data-completion value therefore does not establish completion of that
notification tail. Ring availability is independently calculated from the
native read index and serialized producer commits. Ring-byte retirement does
not release payload or control storage still borrowed by another queue.
[Notification tail][completion] [Ring publication][ring-publication]
[Read-pointer capacity][ring-capacity]

The gang path makes the last-consumer rule concrete: the leader polls each
internal signal to one, then performs its final decrement/store so that signal
destruction cannot race the poll. For one fan-out operation with output
initialized to one, the successful path joins body completions and ties
internal-signal destruction to output zero. Its [completion
ownership](fanout.md#completion-ownership) differs from the public batch API's
shared-counter description. Each owner keeps signal reuse behind its final
reader. [Gang ownership][gang-owner] [Gang commands][gang-commands]
[Gang destruction][gang-handler] [Batch ownership][batch-owner]

Host object reference counting is another lifetime boundary. The runtime
retains a signal while an async handler watches it and releases that reference
when the handler retires; destroying a signal is not a generic GPU drain.
The gang acknowledgment and fan-out join above supply the final-reader
ordering for their private cells. CLR's reusable tracker separately waits
for the current and next signal before resetting a slot, preserving a cell
that the next operation may still be reading.
[Handler retention][handler-retention] [Handler retirement][handler-retirement]
[Signal destruction][signal-destruction] [CLR slot reuse][clr-slot-reuse]

## MEM_INCR and command allocator retirement

PAL's SDMA `AddPostamble` emits an increment when the command stream has a
nonzero GPU busy-tracker address. The tracker belongs to the first command
chunk and is updated after command execution. The operation uses opcode
7/suboperation 1 with a header and full target address, checks eight-byte
alignment, and supplies no explicit operand, comparison, retry count or
returned-value address.
[PAL postamble][pal-postamble]

The allocator enables `TrackBusyChunks` only when `autoMemoryReuse` is set
and `disableBusyChunkTracking` is clear. The first chunk then initializes
the root tracker. With tracking disabled, the default done-count pointer
aliases the CPU submit count and `IsIdleOnGpu` reports idle; the client owns
the completion obligation before returning chunks. Without automatic reuse,
the allocator's public contract recycles memory at `Reset`. These are distinct
ownership modes, not evidence that every SDMA command buffer emits MEM_INCR.
[Allocator flags][pal-allocator-flags] [Selected policy][pal-allocator-policy]
[First-chunk initialization][pal-tracker-selection]
[Tracker reset and observation][pal-tracker-reset]

The root tracker reserves eight bytes, or uses separate writable storage for
read-only command streams. PAL compares only its low 32-bit count because
engine counter widths differ, under an explicit no-wrap assumption. Submit
counts cover root and nested uses. In PAL's automatic tracked reuse path,
`ReuseChunks` consults `IsIdleOnGpu`: matching submit/done counts or a retired
root generation permits the chunks to return to the free list. This establishes
PAL's GPU-increment/CPU-observation/storage-reuse flow, not a general full-width
MEM_INCR counter or CPU-concurrent arithmetic contract.
[Tracker construction and idleness][pal-tracker]
[Submission count][pal-submit] [Allocator reuse][pal-reuse]

The complete `SDMA_PKT_MEM_INCR` representation is:

| DWORD and bits | Field and representation |
| --- | --- |
| 0, 7:0 / 15:8 | `op` / `sub_op`: 7 / 1. |
| 0, 31:16 | Unnamed in the PAL GFX10 base view; the generation-specific policy views below use some of these bits. |
| 1, 31:0 | `addr_31_0` in PAL GFX10 and Linux SDMA6.0/7.1. PAL GFX12 instead represents `addr_31_3` at bits 31:3, leaving the bottom three bits zero. |
| 2, 31:0 | `addr_63_32`. |

| Policy view | DWORD 0 fields |
| --- | --- |
| PAL `gfx103Plus`; Linux SDMA6.0/7.1 | `l2_policy` at bits 25:24, `llc_policy` at bit 26 and `cpv` at bit 28. |
| PAL GFX12 | `mall_policy` at bits 27:26; bits 25:16 and 31:28 unnamed. |

These policy fields occupy different positions from ATOMIC. Linux's SDMA7.1
definitions retain the L2/LLC/CPV representation rather than PAL GFX12's MALL
view. None of these three-DWORD forms carries an explicit increment amount,
comparison value, retry interval or returned-value address.
[PAL GFX10][pal-increment-layout] [PAL GFX12][pal12-layout]
[Linux SDMA6.0][linux-increment] [Linux SDMA7.1][linux71-increment]

The gfx10 emitter writes L2, LLC and CPV fields only when MALL is supported;
otherwise its zero-initialized packet leaves them zero. Its MALL-bypass choice
depends on Navi2x and settings. CPV additionally requires a non-default policy
setting and valid KMD L2 policy. GFX12 uses `GetMallPolicy(false)`: the
destination MALL setting when MALL is supported, otherwise policy zero.
These are actual policy owners, not a universal packet override. Payload cache
visibility remains independent of allocator retirement.
[gfx10 postamble][pal-postamble] [gfx103Plus fields][pal-increment-layout]
[Policy predicates][pal-policy] [gfx12 postamble][pal12-postamble]
[gfx12 fields][pal12-layout] [gfx12 policy selection][pal12-policy]

## SI semaphore encoding

Radeon's SI ring table selects `r600_dma_semaphore_ring_emit`, shared by
earlier engines. It emits a three-DWORD `DMA_PACKET_SEMAPHORE` through the
legacy four-argument `DMA_PACKET` macro:

| DWORD and bits | Emitted value |
| --- | --- |
| 0, 31:28 | Opcode 5. |
| 0, 22 | `s`: one for signal, zero for wait. The macro's `t` bit 23, count bits 15:0 and all other bits are zero for this caller. |
| 1, 31:0 | Address low word masked with `0xfffffffc`. |
| 2, 7:0 | Address bits 39:32; the upper 24 bits are zero. |

The emitter uses a 40-bit address representation, distinct from CIK's full
64-bit pair. The shared semaphore allocator still provides eight-byte-aligned
storage; masking two low address bits is not a different allocator contract.
This kernel ring operation is selected by the SI ring callbacks, rather than
by the existence of the opcode name in a userspace packet header.
[SI selection][si-selection] [Legacy emitter][si-semaphore]
[Legacy macro][si-semaphore-header] [Cell owner][cik-owner]

The SI paging-copy callback uses the same successful dependency/consumer-fence
ownership flow described below, with its own COPY encoding. Its semaphore
cell comes from the cached GTT IB pool; the CIK branch of that pool instead
requests GTT with write-combined CPU mapping. Shared protocol ownership does
not make their packet or mapping representations identical.
[Copy selection][si-copy-selection] [SI copy][si-copy]
[IB pool placement][semaphore-pool]

## Classic SEMAPHORE and ring handoff

`SDMA_PKT_SEMAPHORE` has the same three-DWORD size as MEM_INCR, but selects
suboperation zero and different controls. The named fields agree in Linux's
Iceland, Tonga, Vega10, Navi10, SDMA6.0 and SDMA7.1 headers and PAL GFX10/GFX12:

| DWORD and bits | Field and representation |
| --- | --- |
| 0, 7:0 / 15:8 | `op` / `sub_op`: 7 / 0. |
| 0, 28:16 | Unnamed in the PAL structures. |
| 0, 29 | `write_one`; CIK calls this `O`: zero selects increment, one selects write-one. |
| 0, 30 | `signal`; CIK calls this `S`: zero selects wait, one selects signal. |
| 0, 31 | `mailbox`; CIK calls this `M`. |
| 1–2, 31:0 each | `addr_31_0`, `addr_63_32`: target byte address. |

[Iceland][linux-iceland-semaphore] [Tonga][linux-tonga-semaphore]
[Vega10][linux-vega-semaphore] [Navi10][linux-navi-semaphore]
[SDMA6.0][linux-semaphore] [SDMA7.1][linux71-semaphore]
[PAL GFX10][semaphore-layout] [PAL GFX12][pal12-semaphore]
[CIK control definitions][cik-controls]

The retained Radeon CIK emitter sets only `S` for signaling and clears it for
waiting. `O` and `M` remain zero, so its signal uses increment rather than
write-one or mailbox mode. The owner allocates a private eight-byte,
eight-aligned cell initialized to zero, and the emitter masks the low three
address bits. Neither the storage size nor these control names specifies a
general 64-bit arithmetic result or the exact wait-consumption algorithm.
[CIK emitter][cik-emitter] [Cell owner][cik-owner]

The successful TTM paging-copy path finds producer dependencies, emits a
signal on the producer ring and a wait on the consumer ring, then performs
its copy and signals the consumer fence. Semaphore storage is freed behind that final
consumer fence. The software waiter count tracks emitted wait/signal balance;
it is not an exposed CPU arithmetic protocol for the cell. This is a complete
kernel-managed CIK handoff, with different VM/ring/storage ownership from
KFD user queues. [Copy caller][cik-copy] [Dependency and free][cik-sync]

RADV's `gang_sem_bo` illustrates why names do not identify packet semantics.
Its control allocation uses GL2_BYPASS. The SDMA gang leader waits through
POLL_REGMEM for the member's confirmed shader EOP store before KMD completion
allows command reuse. That protocol does not emit classic SEMAPHORE or create
an HSA signal object. [RADV gang owner][radv-gang]
[SDMA memory poll][mesa-poll]

## gfx125 fused wait/copy/signal

ROCr's selected on-engine path uses a distinct fused form when the ISA major
is 12 and minor >= 5. `DmaCopyOnEngine` reaches
`SubmitLinearCopyBodyWaitSignal`; this is an actual caller, not merely a packet
definition. [On-engine caller][fused-caller]

The fan-out caller selects this path with `IsGfx125Plus`, independently of
`PlatformAtomicSupport`. Direct, indirect, swap and multicast copy builders
all carry their own inline SIGNAL block using operation `0x70`. That selector
belongs to the fused form, not the standalone ATOMIC packet's operation 47.
The ordinary ADD64 link predicate cannot be used as the applicability rule for
this distinct representation. [Fused owner][fused-owner]
[Direct form][fused-builder] [Indirect form][fused-indirect]
[Swap form][fused-swap] [Multicast form][fused-multicast]

The linear packet consists of a header, an optional seven-DWORD WAIT block,
six COPY DWORDs and an optional five-DWORD SIGNAL block. Absent blocks are omitted,
not zero padded. The builder selects an eight-aligned EQ64-to-zero wait with
full mask and SYS scope, a bytes-minus-one copy count with SYS source/destination
scopes, and signal operation `0x70`, 64-bit subtraction with operand one.
[Fused builder][fused-builder]

The five-DWORD SIGNAL block has the same fields in the four fused forms.
Word numbers below are relative to that block; its packet offset depends on
the preceding optional WAIT and copy body:

| SIGNAL word and bits | Field and selected value |
| --- | --- |
| 0, 6:0 | `signal_operation`: `0x70`. |
| 0, 19:18 / 22:20 | `signal_scope`: SYS (3); `signal_temporal_hint`: zero. Bits 17:7 and 31:23 are reserved. |
| 1, 31:3 | `signal_addr_31_3`: target low address bits; bits 2:0 are reserved and zero. |
| 2, 31:0 | `signal_addr_63_32`: target high address. |
| 3–4, 31:0 each | `signal_data_31_0`, `signal_data_63_32`: one and zero. |

[Linear fields][fused-signal-fields] [Indirect fields][fused-indirect-fields]
[Swap fields][fused-swap-fields] [Multicast fields][fused-multicast-fields]

The single-copy and unprofiled multicast callers add `chunk_count - 1` to the
initial output value, then signal once per chunk, giving one net decrement.
The fan-out callers instead signal once per engine group; their embedded
wait is on the first chunk of the first entry and the signal on the last
chunk of the last entry. For its isolated initial-one protocol, the fan-out
owner adds the group count and the coordinator chooses the final completion:

| Coordinator condition | Final completion owner |
| --- | --- |
| No GCR, profiling or mailbox tail | The CPU subtracts the extra one before publishing the coordinator; the final group SIGNAL reaches zero. |
| Any of those tail obligations | The coordinator polls the shared value to one, performs selected GCR/end sampling, then stores zero with FENCE_64B before any mailbox FENCE/TRAP. |

The selected wide stores are in `SubmitFusedCoordinator`; the separate
`SubmitEpilogue` function's wide branch is not reached by this fan-out caller.
Completion values still do not cover later notification packets. In the
single-copy path they also precede the optional final timestamp, giving a
distinct [sample-readiness contract](timing.md#fused-single-copy-readiness).
[Chunk accounting][fused-count] [Multicast accounting][fused-multicast-count]
[Actual fan-out selection][fused-owner] [Coordinator condition][fused-tail-selection]
[Coordinator calls][fused-coordinator-calls] [Body calls][fused-body-calls]
[CPU adjustment][fused-tail-adjustment] [Coordinator final store][fused-tail]
[Classic caller][classic-epilogue-caller]

The generation has separate POLL_MEM_64B (opcode 8/suboperation 5,
retry zero meaning infinite) and FENCE_64B (opcode 5/suboperation 2, SYS/MTYPE3)
builders. Neither is the classic poll's `0xfff` retry spelling or two ordered
FENCE32 stores.
[64-bit poll/fence][wide-controls]

[copy-api]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2090-L2153
[copy-stream]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L521-L635
[atomic-constants]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L50-L81
[atomic-gate]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L156-L204
[link-flags]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_topology.cpp#L220-L289
[linux-links]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_topology.c#L1210-L1236
[completion]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L614-L663
[fence-choice]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L469-L479
[atomic-builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2944-L2958
[atomic-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L873-L938
[signal-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/amd_hsa_signal.h#L49-L77
[factory]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L852-L904
[aliases]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_blit_sdma.h#L578-L590
[linux-atomic]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L4797-L4879
[pal-atomic]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L94-L126
[ring-publication]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1954-L2050
[gang-owner]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1283-L1332
[gang-commands]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L580-L611
[batch-owner]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1827-L1903
[pal-postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L175-L211
[pal-tracker]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStreamAllocation.cpp#L404-L479
[pal-submit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStream.cpp#L628-L649
[pal-reuse]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdAllocator.cpp#L703-L739
[pal-increment-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L2480-L2520
[pal-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L371-L449
[pal12-postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L184-L213
[pal12-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L2645-L2679
[semaphore-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L2939-L2973
[cik-emitter]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/radeon/cik_sdma.c#L217-L242
[cik-owner]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/radeon/radeon_semaphore.c#L34-L106
[cik-copy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/radeon/cik_sdma.c#L565-L633
[cik-sync]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/radeon/radeon_sync.c#L121-L204
[radv-gang]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1376-L1457
[mesa-poll]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L36-L57
[fused-caller]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1409-L1434
[fused-builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2714-L2796
[fused-count]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L696-L788
[wide-controls]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2663-L2711

[hdp-setting]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L256-L260
[hdp-emission]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L548-L558
[pal12-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L346-L384

[pal-atomic-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L94-L183
[pal12-atomic-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L84-L168
[pal-atomic-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L3936-L3957
[pal-atomic-base]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdBuffer.h#L536-L541
[pal12-semaphore]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L3152-L3186
[cik-controls]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/radeon/cikd.h#L2000-L2031
[current-factory]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L866-L918
[linux-tonga-atomic]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/tonga_sdma_pkt_open.h#L2034-L2098
[linux-vega-atomic]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L2725-L2795
[linux-navi-atomic]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L4113-L4183
[linux71-atomic]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L4797-L4879
[linux-increment]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L3999-L4040
[linux71-increment]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L3999-L4040
[linux-iceland-semaphore]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/iceland_sdma_pkt_open.h#L1695-L1736
[linux-tonga-semaphore]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/tonga_sdma_pkt_open.h#L1695-L1736
[linux-vega-semaphore]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L2128-L2169
[linux-navi-semaphore]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L3411-L3452
[linux-semaphore]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L3949-L3990
[linux71-semaphore]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L3949-L3990

[pal-allocator-flags]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdAllocator.h#L44-L70
[pal-allocator-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdAllocator.cpp#L110-L117
[pal-tracker-selection]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStream.cpp#L399-L412
[pal-tracker-reset]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStreamAllocation.cpp#L389-L479

[si-selection]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/radeon/radeon_asic.c#L1901-L1914
[si-semaphore]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/radeon/r600_dma.c#L302-L325
[si-semaphore-header]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/radeon/r600d.h#L644-L657
[si-copy-selection]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/radeon/radeon_asic.c#L1963-L1970
[si-copy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/radeon/si_dma.c#L230-L282
[semaphore-pool]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/radeon/radeon_ib.c#L195-L222

[atomic-body]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1523-L1560
[atomic-epilogue]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L990-L1056
[fanout-signal-selection]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1734-L1866
[fused-owner]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1706-L1757
[fused-indirect]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2799-L2864
[fused-swap]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2867-L2941
[fused-multicast]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2281-L2373

[atomic-copy-wrappers]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1637-L1695
[atomic-rect-fill]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1858-L1947
[atomic-blocking]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L364-L394
[signal-observer]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/default_signal.cpp#L59-L121
[interrupt-observer]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/interrupt_signal.cpp#L94-L211
[handler-retention]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L963-L986
[handler-retirement]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L2018-L2032
[signal-destruction]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/signal.h#L284-L292
[clr-slot-reuse]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L746-L845
[fused-signal-fields]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L1271-L1430
[fused-indirect-fields]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L1435-L1600
[fused-swap-fields]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L1772-L1931
[fused-multicast-fields]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L1606-L1766
[fused-multicast-count]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1697-L1856
[fused-tail-selection]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1089-L1110
[fused-tail-adjustment]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1194-L1197
[fused-tail]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1291-L1335
[classic-epilogue-caller]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1811-L1865
[atomic-profiled-multicast]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1697-L1738
[ring-capacity]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2087-L2094
[gang-handler]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1223-L1232
[fused-coordinator-calls]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1255-L1276
[fused-body-calls]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1428-L1452
