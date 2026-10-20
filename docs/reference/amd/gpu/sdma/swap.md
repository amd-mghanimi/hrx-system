# SDMA buffer exchange

`COPY_LINEAR_SWAP` exchanges the contents of two memory ranges while keeping
their virtual addresses unchanged. For equal-sized, independently owned ranges
`A` and `B`, the intended result is `A_after = B_before` and
`B_after = A_before`. Both operands are read and written. The operation can
exchange resident and backing data without a third application-managed payload
buffer; it does not update a residency table or remap either allocation.
[Operation contract][descriptor] [Packet definitions][classic-layout]

This operation is distinct from the `src_sw` and `dst_sw` byte-order fields
present in several SDMA copy layouts. Its opcode is `SDMA_OP_COPY` (1) and
its suboperation is `SDMA_SUBOP_COPY_SWAP` (9). Addresses and byte length are
carried in the packet. [Constants][constants]
[Indirect copies](indirect-copy.md) have a separate execution-time address
mechanism; the HIP batch entry point rejects combining swap and indirect flags.
[HIP admission][hip-entry]

## Applicability and caller shape

ROCr and CLR use compiler ISA predicates for admission. These predicates are
runtime policy, not a complete native SDMA-IP or firmware support table.

| Runtime selection | Exchange path and alignment |
| --- | --- |
| ISA major 9, minor at least 4 | ROCr sets `swap_supported_`; the batch path uses the classic seven-DWORD packet and requires both initial addresses to be 64-byte aligned. |
| ISA major 12, minor at least 5 | ROCr sets `is_gfx125plus_`; the batch path uses the fused wait/signal form and requires both initial addresses to be 32-byte aligned. |
| Other ISA versions | Neither predicate admits exchange through this ROCr path. In particular, ordinary copy support on gfx11, gfx115x, or gfx120x does not establish swap support. |

CLR's `sdma_swap_supported_` uses the same two predicates.
[ROCr initialization][initialize] [ROCr admission][admission]
[CLR setting][settings]

The classic packet comment says SDMA5.2+ while separately naming gfx94/gfx95X
address alignment. The actual caller admits the major-9 targets above; that
header comment alone does not establish a physical IP threshold. Compiler
targets, native SDMA IPs and platform transports retain their separate
[identities](../architectures.md). [Classic definition][classic-layout]

`HSA_AMD_MEMORY_COPY_OP_LINEAR_SWAP` has two descriptor forms:

| Form | Operands consumed by the caller |
| --- | --- |
| Scalar, `num_entries == 0` | `src`, `src_agent`, `dst`, `dst_agent`, `src_size`, `dst_size`. Both sizes must be positive, and `DmaCopySwap` rejects unequal sizes. |
| Multiple pairs, `num_entries > 0` | `src_list`, `dst_list`, `dst_agent_list`, a common routing `src_agent`, and `size_list`. Each pair has one positive length, used for both operands. |

The scalar form becomes a one-entry list before reaching `DmaCopyFanOutOp`.
The two size vectors accepted by `SubmitFusedCoordinator` contain identical
per-pair lengths, and `BuildWaitSignalSwapCommand` emits only one count field.
Their argument names do not define asymmetric exchange semantics.
[Descriptor][descriptor] [Validation][validate] [Scalar conversion][convert]
[Coordinator inputs][coordinator-inputs] [Fused builder][fused-build]

The descriptor's raw `wait`, `signal`, and `traffic_class` members are not
forwarded by `DmaCopySwap`. Its builders receive the HSA dependency signals
and operation completion signal instead. The fixed fused operands below
describe that caller, rather than arbitrary raw comparisons or QoS settings.
[Descriptor fields][descriptor-fields] [Caller][convert]

The public header describes up to 65536 pairs, but `num_entries` is `uint16_t`:
positive representable counts stop at 65535, and zero selects the scalar form.
CLR narrows its grouped vector length to that field without splitting it in
the inspected caller. This is a descriptor/caller disagreement, not a packet
destination-count field. [Descriptor fields][descriptor-fields]
[CLR grouping][clr-group]

## Plain packet representations

Both plain forms occupy seven DWORDs, or 28 bytes. For a positive chunk length
`L`, `count = L - 1`. The 30-bit field represents lengths through
`0x40000000` bytes, independently of the smaller runtime cap below.
[Classic layout][classic-layout] [GFX1250 layout][scoped-layout]
[Builder][plain-build]

| DWORD | `SDMA_PKT_COPY_LINEAR_SWAP` fields |
| --- | --- |
| 0 | `op` bits 7:0 = 1; `sub_op` bits 15:8 = 9; `extra_info` bits 31:16. |
| 1 | `count` bits 29:0; bits 31:30 reserved. |
| 2 | `dst_sw` bits 17:16; `dst_cache_policy` bits 20:18; `src_sw` bits 25:24; `src_cache_policy` bits 28:26. Bits 15:0, 23:21 and 31:29 reserved. |
| 3–4 | `addr_a_31_6` in low-word bits 31:6, then `addr_a_63_32`; low address bits 5:0 reserved. |
| 5–6 | `addr_b_31_6` in low-word bits 31:6, then `addr_b_63_32`; low address bits 5:0 reserved. |

ROCr zeroes `extra_info`, both byte-order fields, both cache-policy fields,
and reserved fields. The three-bit cache policies are not the two-bit scope
fields of the next layout. Its builder receives `dst` as address A and `src`
as address B; both remain read/write operands. [Builder][plain-build]
[Body caller][bodies]

| DWORD | `SDMA_PKT_COPY_LINEAR_SWAP_GFX1250` fields |
| --- | --- |
| 0 | `op` bits 7:0 = 1; `sub_op` bits 15:8 = 9; `tmz` bit 18. Bits 17:16 and 31:19 reserved. |
| 1 | `count` bits 29:0; bits 31:30 reserved. |
| 2 | `scope_b` bits 19:18; `temporal_hint_b` bits 22:20; `scope_a` bits 27:26; `temporal_hint_a` bits 30:28. Bits 17:0, 25:23 and 31 reserved. |
| 3–4 | `addr_a_31_5` in low-word bits 31:5, then `addr_a_63_32`; low address bits 4:0 reserved. |
| 5–6 | `addr_b_31_5` in low-word bits 31:5, then `addr_b_63_32`; low address bits 4:0 reserved. |

The plain builder selects this form when both `scopeFields` and
`is_gfx125plus_` are true. It sets both scopes to SYS (3), leaving temporal
hints, TMZ and reserved fields zero. The ordinary batch caller selects the
fused path on `IsGfx125Plus`, however; the existence of this plain builder
branch does not make it the batch's active GFX1250 lowering.
[Constants][constants] [Builder][plain-build]
[Batch selection][fanout-selection] [Body selection][bodies]

### Chunk limits and alignment

All three ROCr swap structures declare `kMaxSize_ = 0x3fffffe0` bytes,
1 GiB minus 32 bytes. The builders emit `min(remaining, kMaxSize_)`, advance
both addresses by that length, and subtract one for the packet count. This
cap differs from ordinary linear copy's selected cap and is independent of
the packet field's representable maximum. [Definitions][classic-layout]
[Scoped definition][scoped-layout] [Fused definition][fused-layout]
[Builders][plain-build] [Fused builder][fused-build]

The classic source has an unresolved alignment inconsistency: its declared
alignment is 64 bytes, but its chunk cap is only a multiple of 32 bytes.
Admission checks the original pointers, and the builder advances by the cap
without restoring 64-byte alignment between chunks. The inspected caller
contains no separate chunk-size correction. These facts do not establish the
hardware's behavior for a split classic request. A multi-packet contract needs
an alignment-preserving chunk policy or authoritative evidence changing the
stated alignment. The GFX1250 cap preserves its stated 32-byte alignment.
[Admission][admission] [Chunk planning][bodies] [Builder][plain-build]

ROCr validates address alignment but has no corresponding length-modulus
check in these paths. A count field's ability to represent a short length
does not, by itself, specify exchange-tail granularity. The cited definitions
and builders also supply no overlapping-range semantics. Exclusive, disjoint
read/write ranges avoid depending on an unspecified overlap protocol.
[Validation][validate] [Admission][admission] [Layouts][classic-layout]

## Fused wait, exchange and signal

`SDMA_PKT_COPY_LINEAR_SWAP_WAITSIGNAL_GFX1250` adds optional control blocks.
The source C structure describes the maximum 19-DWORD form; serialization
omits absent blocks. Its length is `7 + 7W + 5S` DWORDs for Boolean presence
values `W` and `S`. [Layout][fused-layout] [Serialization][fused-build]

| Block | Representation |
| --- | --- |
| Header, one DWORD | `op` bits 7:0 = 1; `sub_op` bits 15:8 = 9; `tmz` bit 18; `wait` bit 30; `signal` bit 31. Bits 17:16 and 29:19 reserved. |
| Optional WAIT, seven DWORDs | `wait_function` bits 2:0, `wait_scope` bits 19:18, `wait_temporal_hint` bits 22:20; remaining bits reserved. Then wait address, reference and mask, each as low/high DWORDs. Address low bits 2:0 are reserved. |
| Exchange, six DWORDs | Count, parameter, address A low/high, address B low/high. Count and parameter fields match the plain GFX1250 form. Fused address low fields are full 32-bit words; the separate 32-byte address-alignment requirement still applies. |
| Optional SIGNAL, five DWORDs | `signal_operation` bits 6:0, `signal_scope` bits 19:18, `signal_temporal_hint` bits 22:20; remaining bits reserved. Then signal address and data, each low/high. Address low bits 2:0 are reserved. |

The first exchange DWORD is at offset `1 + 7W`; a present SIGNAL starts at
`7 + 7W`. The four packet lengths are 7, 14, 12 and 19 DWORDs for neither,
WAIT only, SIGNAL only and both. The control cells are eight-byte aligned.
ROCr emits WAIT function 3 (equal), reference zero, an all-ones 64-bit mask,
and SYS scope. SIGNAL uses operation `0x70` (64-bit subtract), operand 1,
and SYS scope. Both payload scopes are SYS; hints, TMZ and reserved fields
remain zero. [Builder][fused-build]

Within an engine group, only the first chunk of the first entry carries WAIT,
and only the last chunk of the last entry carries SIGNAL. Additional
dependencies use separate 64-bit polls. For `K` exchange chunks and an optional
first wait, the exchange packets occupy `7K + 7W + 5` DWORDs; separate polls,
cache operations, timestamps, coordinator work and ring padding add their own
storage. This group completion differs from multicast's per-chunk decrement.
[Bodies][bodies] [Coordinator][coordinator]
[Multicast comparison](fanout.md#chunk-completion-and-profiling)

## Execution, completion and ownership

`DmaCopyFanOutOp` groups pairs by selected engine. Its
[engine assignment policy](fanout.md#ordinary-copy-fan-out-and-engine-joins)
also applies to exchange: entries can execute on several engines, and large
GFX1250 batches can group eight entries per engine. Descriptor order therefore
does not establish an ordering edge between different pairs.
[Assignment][fanout-selection]

The classic path publishes a dependency/cache prologue, engine bodies that
wait on its internal start signal, and a coordinator epilogue. Each body
performs its exchanges and contributes either an atomic decrement or a
separate body flag when platform atomics are unavailable. The epilogue joins
all bodies before final completion. The GFX1250 path uses first/last fused
control blocks; profiling adds a prologue, and an epilogue performs any
selected cache release, end timestamp and notification. With no GCR,
profiling or mailbox, ROCr removes the epilogue and adjusts the output count
so the final body decrement reaches zero. [Fan-out owner][fanout-owner]
[Classic bodies][bodies] [Fused coordinator][coordinator]

The atomic and fused paths assume a dedicated operation counter initialized
to one, then increased by the number of engine groups. The classic path with
separate body flags keeps the output at one until its final store. These are
not general shared batch-counter recipes. CLR's separate completion per merged
operation and the public API's differing shared-counter description are detailed in
[batch completion](fanout.md#completion-and-descriptor-controls). The same
[batch boundary](fanout.md#batch-composition-and-descriptor-ownership)
separates pair counts from packet and completion counts across the operation
family.

The packet comments describe exchange as atomic, but do not name its
granularity, participating observers, or a whole-range linearization point.
ROCr can split a range into packets and run different pairs on separate
engines. Its completion joins establish the intended software ownership
boundary; the comments do not establish an all-or-nothing snapshot for a CPU,
shader, peer GPU, or NPU concurrently reading the ranges.
[Packet comments][classic-layout] [Chunk builder][plain-build]
[Engine owner][fanout-owner]

| Resource | Final user and reuse boundary |
| --- | --- |
| Both payload ranges | Every exchange engine reading and writing the pair. The next consumer waits for the complete operation and applies the directed memory edge's acquire/visibility protocol. |
| Host descriptor and address/size lists | The submitting runtime reads these while constructing and publishing its queue commands. CLR owns its temporary grouped lists across the ROCr call; the engine receives values, not those host list pointers. |
| Input dependency signals | All engine polls or fused waits referring to them; an asynchronous API return does not retire these reads. |
| Internal start/body signals | The fan-out owner arranges asynchronous cleanup after the output reaches zero. |
| Output signal and event mailbox | Device completion, notification tail and host/runtime observers have separate final accesses. Observing payload completion alone does not retire every notification resource. |
| Command-ring bytes | The queue's read/retirement frontier controls reuse, independently of the payload ownership edge. |

[Descriptor serialization][clr-group] [Body publication][bodies]
[Internal cleanup][fanout-cleanup]
[Notification ownership](../notifications.md)
[Queue retirement](publication.md)
[Directed visibility](../../interop/README.md#the-six-directed-handoffs)

## HIP and CLR caller policy

HIP exposes exchange as `hipMemcpyFlagExtOpSwap` (`0x200`) in batch-copy
attributes. The inspected implementation admits tracked device-to-device,
device-to-pinned-host, pinned-host-to-device and peer-device operations. It
rejects host-only memory pairs and the pageable-host staging paths for this flag.
These are HIP routing choices, not additional packet address bits.
[Flag][hip-flags] [Classification and admission][hip-admission]

HIP builds command-owned descriptors, preserves prior stream dependencies
when dispatching through another device's queue, and joins those commands back
into the original stream. CLR classifies H2D, D2H and D2D groups and merges
their swap entries into multi-pair ROCr descriptors. Both scalar sizes come
from the same `BatchCopyOp::size`. Peer routing selects the calling GPU's SDMA
engines; it does not change which operand gets overwritten. Extra completion
signals are joined before the next stream operation can depend on the batch.
[Command owner][command-owner] [Queue dependencies][hip-enqueue]
[Stream join][hip-join] [Agent routing][clr-operands]
[Grouping][clr-group] [Completion join][clr-join]

Both allocations remain writable and live through completion even though the
API calls one `src`. The generic source-access-order attributes do not supply
an early release for an exchange operand: this path enqueues a
`BatchCopyMemoryCommand`, while the explicit DuringApiCall snapshot branch
only handles pageable `BatchWriteMemoryOp` entries. The exact interaction
between that attribute and exchange needs an explicit API contract.
[Attribute definition][hip-flags] [Enqueue implementation][hip-enqueue]

If the SDMA batch fails, CLR detects swap entries and reports failure instead
of falling back to its one-directional shader copy. Neither that error return
nor the multi-engine submission code defines transactional rollback of work
already published to another engine. Recovery retains the native queue's
progress and ownership rules. [Failure handling][clr-failure]
[Submission owner][fanout-owner]

## Residency exchange sequence and accounting

For a resident slot `R` and backing slot `B`, a caller can compose the mechanism
as follows. This sequence uses independent, aligned, equal-sized ranges and
explicit ownership dependencies; it does not require a host wait between
device stages.

1. Establish mappings that give the selected exchange engine read/write access
   to both slots. The resident and backing allocations keep their addresses
   and placement for the entire operation.
2. Wait for the last readers and writers of both slots. Publish both input
   contents with the release operations for their
   [directed memory edges](../../interop/README.md#the-six-directed-handoffs).
3. Publish exchange commands with the supported packet form and a
   size/alignment policy consistent with that form. Independent pairs can
   share a batch; a dependency between pairs needs an explicit ordering edge.
4. Join every participating engine. The next resident consumer acquires `R`;
   a consumer of the evicted contents independently acquires `B`. Completion
   of one group does not return all pairs to their owners.
5. Publish any software residency-table change after the required completion
   and visibility edges. Reuse control cells and command storage only after
   their own final accesses retire.

[Descriptor contract][descriptor] [Execution owner][fanout-owner]
[Queue publication](publication.md)

One `N`-byte pair describes two `N`-byte inputs and two `N`-byte results.
ROCr's `total_bytes`/`bytes_queued_` progress accounting adds `N`, not `2N`,
for that pair. It is logical operation accounting, not a measurement of link
or memory traffic. A bandwidth report must name whether its numerator counts
one operand, both exchanged payloads, or observed bus transactions. The
internal buffering and physical traffic cannot be recovered from the packet
size alone. [Body accounting][bodies] [Coordinator accounting][coordinator]

Profiling changes the surrounding prologue and epilogue while preserving the
fused exchange selection on GFX1250. The sampled interval includes the
selected synchronization and engine join. The
[timestamp contract](timing.md) supplies clock and ordering details; command
DWORD counts alone supply no exchange latency or throughput bound.
[Coordinator][coordinator]

[constants]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L47-L86
[classic-layout]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L244-L310
[scoped-layout]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L1127-L1195
[fused-layout]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L1768-L1931
[initialize]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L204-L218
[settings]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocsettings.cpp#L156-L159
[descriptor]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2296-L2309
[descriptor-fields]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2331-L2390
[validate]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ext_amd.cpp#L679-L701
[convert]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2033-L2058
[admission]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1667-L1679
[plain-build]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2377-L2445
[fused-build]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2868-L2943
[coordinator-inputs]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1724-L1816
[fanout-selection]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1547-L1723
[fanout-owner]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1724-L1902
[fanout-cleanup]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1868-L1902
[coordinator]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1078-L1348
[bodies]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1351-L1577
[hip-flags]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/hip/include/hip/driver_types.h#L454-L493
[hip-entry]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/hipamd/src/hip_memory.cpp#L3245-L3314
[hip-admission]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/hipamd/src/hip_memory.cpp#L3085-L3169
[hip-enqueue]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/hipamd/src/hip_memory.cpp#L2959-L3009
[hip-join]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/hipamd/src/hip_memory.cpp#L3198-L3242
[command-owner]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/platform/command.hpp#L1232-L1272
[clr-operands]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocblit.cpp#L647-L717
[clr-group]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocblit.cpp#L732-L879
[clr-join]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L3726-L3792
[clr-failure]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocblit.cpp#L3160-L3280
