# SDMA timestamps

`TIMESTAMP_GET_GLOBAL` records a 64-bit GPU clock sample in memory. PAL's
GFX10 and GFX12 builders state that it completes preceding commands before
sampling, without a preceding FENCE. The sample therefore locates progress
through one SDMA stream. Its meaning across several engines depends on the
dependencies placed around it; host visibility, clock conversion and storage
retirement have their own owners. [GFX10 builder][pal10-builder]
[GFX12 builder][pal12-builder] [Public timestamp contract][pal-contract]

## Representation and alignment

`SDMA_PKT_TIMESTAMP_GET_GLOBAL`, called `SDMA_PKT_TIMESTAMP` by ROCr,
occupies three DWORDs. The baseline header is `0x0000020d`.

| DWORD | Fields and units |
| --- | --- |
| 0 | `op = 13` at bits 7:0; `sub_op = 2` at 15:8; layout-specific policy fields below. Other header bits are reserved and zero in the cited builders. |
| 1 | Destination byte address bits 31:3 in the corresponding bit positions; bits 2:0 are reserved in the Linux/PAL layouts. ROCr names the entire word `addr_31_0`. |
| 2 | Destination byte address bits 63:32. |

The Linux generated low-address macro shifts its argument by three, so that
macro consumes `address >> 3`; PAL, Mesa and ROCr instead assign the whole
low address DWORD. The stored sample is eight bytes, independent of the
12-byte command size. Two timestamp packets occupy 24 command bytes and
write 16 result bytes, before result alignment, surrounding commands or ring
padding. These sizes do not establish timestamp latency or store atomicity.
[Linux address field][linux-iceland] [PAL fields][pal10-layout]
[ROCr fields][rocr-layout] [Mesa builder][mesa-builder]

The address representation permits eight-byte alignment. Allocation contracts
can be stronger: ROCr's signal owner records a 32-byte requirement for gfx7/8
and an eight-byte requirement for gfx9, then places both samples at separate
32-byte-aligned offsets in its 128-byte `SharedSignal`. PAL's cited GFX9 and
GFX12 DMA-engine properties report an eight-byte minimum. Those properties do
not supersede the earlier ROCr requirement. [ROCr storage][rocr-storage]
[PAL GFX9 properties][pal9-properties] [PAL GFX12 properties][pal12-properties]

`TIMESTAMP_SET = 0` and `TIMESTAMP_GET = 1` are different suboperations.
The global-sampling builders use suboperation 2; the common opcode does not
make all three operations equivalent. [Opcode constants][linux-constants]

## Policy layouts and actual selection

Every field in this table belongs to DWORD 0. A source's family name identifies
its declaration, not a universal version test for the other implementations.

| Declaration | Fields above bit 15 |
| --- | --- |
| Linux Iceland, Tonga, Vega10 and Navi10 | No policy fields declared for GET_GLOBAL. [Iceland][linux-iceland] [Tonga][linux-tonga] [Vega10][linux-vega] [Navi10][linux-navi] |
| Linux SDMA 6.0 and 7.1 | `l2_policy[25:24]`, `llc_policy[26]`, `cpv[28]`. [6.0][linux6] [7.1][linux71] |
| PAL GFX10 base / `gfx103Plus` overlay | Base reserves the upper header; overlay names `l2_policy[25:24]`, `llc_policy[26]`, `cpv[28]`. [Fields][pal10-layout] |
| PAL GFX12 | `mall_policy[27:26]`. [Fields][pal12-layout] |
| ROCr scoped template | `scope[25:24]`, `temporal_hint[28:26]`. [Fields][rocr-layout] |

PAL's GFX10 builder selects the overlay when `supportsMall` is set. Its
write-policy helper supplies L2 policy and MALL bypass; the latter requires
`IsNavi2x` and the write-bypass setting. CPV requires a nondefault bypass
setting and valid native L2 policies. The helper's L2 encodings are LRU 0,
stream 1, no allocation 2, and uncached/bypass 3; only its low two bits fit
the packet's L2 field. PAL's GFX12 builder instead selects the destination
MALL setting when MALL is supported, otherwise zero. That two-bit field uses
regular temporal 0, non-temporal 1, high-priority temporal 2 and last-use 3.
[GFX10 emission][pal10-builder] [GFX10 selectors][pal10-policy]
[GFX12 emission][pal12-builder] [GFX12 selector][pal12-policy]

ROCr zeroes the packet and sets `scope = SYS (3)` only when `scopeFields` is
true; its temporal hint remains zero. `CreateBlitSdma` selects that template
for non-DXG ISA major 11 or 12 with minor at least 5. ISA major 9 and the
selected DXG paths have neither explicit GCR nor scope fields; non-DXG major
10 and major 11/12 with minor below 5 use GCR without scope fields. The
separate fused-copy predicate is exactly major 12 with minor at least 5.
Thus the selected fused paths have no explicit GCR, even though their generic
helpers contain conditional GCR branches. The factory attributes DXG's GCR
wrapper to the underlying driver, outside the runtime's sample placement.
[Builder][rocr-builder] [Scope value][rocr-constants]
[Template aliases][rocr-aliases] [Factory][rocr-factory]
[Fused predicate][rocr-fused-predicate]

Mesa emits the baseline header without policy bits. In particular, bits
25:24 cannot be interpreted as both L2 policy and scope for one packet merely
because two headers give them different names. The actual engine, native
transport and selected format determine that interpretation. Cache policy
also does not replace the [payload release/acquire protocol](cache.md).
[Mesa builder][mesa-builder]

## Execution point and sampled interval

PAL's public timestamp contract permits only bottom-of-pipe timestamps on
SDMA. Its shared DMA command-buffer method receives `stageMask` but passes
only the destination address to the hardware builder. RADV likewise emits
GET_GLOBAL for its transfer queue and returns without interpreting `stage`.
Its optional `flush_before_timestamp_write` setting adds an SDMA NOP, whose
emitter documents a pending-copy drain. Neither caller implements a separate
top-of-pipe SDMA sampling mode. [PAL contract][pal-contract]
[PAL caller][pal-caller] [RADV query caller][mesa-query-write]
[Mesa drain and timestamp builders][mesa-builder]

RADV's transfer family requires native SDMA support and queues, transfer
opt-in, an enabled compute queue, GFX9 or later, and no transfer-disable flag.
Transfer commands on a general or compute queue use a different timestamp
path. A command's name alone therefore does not identify the sampled engine.
[Queue-family selection][mesa-family] [Timestamp paths][mesa-query-write]

### ROCr copy envelopes

ROCr's asynchronous-copy profiling setting enables GET_GLOBAL pairs in its
SDMA submission helpers. The following intervals follow the actual emitted
commands, rather than assuming every pair brackets the same operation.
Optional HDP and GCR work retains the [cache chapter's selectors](cache.md#ordinary-user-queue-composition).
[Profiling enable][rocr-enable] [Ordinary producer][rocr-ordinary]

| Selected flow | Sample placement | Meaning of the difference |
| --- | --- | --- |
| Ordinary `SubmitCommand` with no gang | Dependency waits → start → selected HDP/GCR acquire → body → selected GCR writeback → end → user completion → notification. | This stream's body and emitted cache work; excludes prior dependency waiting and the final completion/notification. |
| Gang copy through `SubmitCommand` | Only the leader writes samples. Its end precedes polling/acknowledging followers and publishing user completion. | Leader-local work, not the interval covering all engines. There is no common start gate or min/max over their samples. |
| Isolated fanout through prologue, bodies and epilogue | Coordinator waits dependencies, samples start and opens a common gate; body groups run; coordinator joins every group, performs selected writeback, samples end, then publishes final completion. | One operation's joined body interval, including gate/join/cache work, in the coordinator's clock. |
| Profiled fused fanout | Profiling retains the common gate and final epilogue; coordinator end follows the body join and precedes the final zero FENCE64. | The same joined-operation interpretation, including the coordinator's own body, rather than a sum of engine busy times. |

[Ordinary and gang command order][rocr-ordinary]
[Gang caller and ownership][rocr-gang]
[Fanout start][rocr-prologue] [Fanout join/end][rocr-epilogue]
[Fused coordinator start][rocr-coordinator-start]
[Fused coordinator join/end][rocr-coordinator-end]
[Fanout caller][rocr-fanout]

The fanout rows describe one operation with an initial output count of one.
Its atomic join waits for the absolute value one, and its final completion
decrements or fences that value to zero. A shared counter for several public
batch descriptors has a different contract; the isolated-operation sequence
does not prove such a batch's completion or interval. The [batch composition
chapter](fanout.md#batch-composition-and-descriptor-ownership) traces that
boundary. [Fanout join][rocr-epilogue] [Fused join][rocr-coordinator-end]

Rectangular copies and the plain broadcast builder use the ordinary envelope.
The multicast helper specifically selects a plain multicast body through
`SubmitCommand` when profiling is enabled. Its unprofiled fused path therefore
has a different command shape; a profiling comparison can change the work
being measured. [Rectangular submission][rocr-rect]
[Broadcast submission][rocr-broadcast] [Multicast selection][rocr-multicast]

### Fused single-copy readiness

`DmaCopyOnEngine` selects `SubmitLinearCopyBodyWaitSignal` for an SDMA blit
satisfying the separate fused-copy predicate, including when profiling is
enabled. Its selected command order is:

```text
wait dependency[1..], if present → start
  → {wait dependency[0] if present, copy chunk, subtract completion} for each chunk
  → end → optional notification
```

The helper adds `chunk_count - 1` to the completion count before publication.
Each chunk waits on the first dependency, when present, and performs a
SYS-scoped 64-bit subtract of one. The first dependency's wait is therefore inside this sample
interval, unlike the ordinary envelope. The final payload completion update
is before the ending timestamp, with no second completion update after it.
[Caller selection][rocr-fused-caller] [Sample placement][rocr-fused-single]
[Fused wait/signal fields][rocr-fused-fields]

This source order does not establish that observing the copy's completion
value makes the ending sample ready. The later optional mailbox/TRAP does
not change the order of ordinary signal-value observation, and the profiling
getter adds no wait. This is a source-level readiness mismatch in the cited
path, not a measured failure rate or a general hardware timestamp limitation.
[Sample and notification order][rocr-fused-single]
[Profiling getter][rocr-getter]

## Result publication and storage lifetime

An explicit same-stream measurement has a complete ownership sequence:

1. Establish writable sample mappings with the native alignment/cache policy,
   retain them, and satisfy payload and command dependencies.
2. Record start, the intended work, and end. If the intended interval spans
   other engines, place their common start dependency and completion join
   inside that envelope.
3. Publish sample/payload visibility through the transport's required release
   and a completion operation ordered after the ending sample.
4. Observe that completion and acquire the result before reading either
   sample. Preserve the sample's device, clock domain and storage epoch.
5. Copy out the result before resetting its slots. Release each allocation
   only after its final device and host user; a sample's readiness does not
   retire later commands or another consumer's payload use.

This sequence composes the preceding sampling rule with [completion
stores](fence.md), [cache maintenance](cache.md), and [command
retirement](command-buffers.md). Actual runtime owners supply different
readiness mechanisms:

| Owner | Readiness and retrieval |
| --- | --- |
| PAL `GpaSession` timing sample | Begin/end write separate aligned slots. Session `End` records a performance-data barrier, a bottom-of-pipe event, and an event-data barrier. `IsReady` observes that event and, when queue timing is enabled, timed-queue fences. The result getter copies raw values and adds no wait. [Allocation and start][pal-sample-start] [End sample][pal-sample-end] [Completion publisher][pal-session-end] [Readiness][pal-ready] [Result copy][pal-result] |
| RADV timestamp query | One mapped eight-byte slot starts at `TIMESTAMP_NOT_READY = UINT64_MAX` after reset. GET_GLOBAL replaces that sentinel; no separate SDMA availability store follows it. Host retrieval atomically reads the slot and derives availability from a changed value. WAIT polls within the driver's query timeout; a non-waiting unavailable result reports `VK_NOT_READY`, and timeout reports device loss. [Pool storage][mesa-query-storage] [Reset][mesa-query-reset] [Write][mesa-query-write] [Read][mesa-query-read] |
| RADV utrace | The offset-zero read waits for the timestamp BO to become idle, then subsequent samples share that completed buffer. A failed wait is logged but does not stop the callback, so only the successful wait establishes this readiness path. [Reader][mesa-utrace-read] |
| ROCr async-copy signal | The getter uses the copy agent stored in the signal and reads its embedded pair without waiting. Readiness must come from the selected submission's completion protocol; the fused single-copy exception above matters. [Storage][rocr-storage] [Getter][rocr-getter] |

RADV query availability is a concrete consumer protocol, not a proof that
every native GET_GLOBAL store is an atomic 64-bit publication primitive.
Its query-copy path uses a CP wait and shader, rather than an SDMA result-copy
opcode. Pool destruction frees storage without a GPU wait; submitted writes
and result-copy reads must already have relinquished it.
[Query copy][mesa-query-copy] [Pool destruction entry][mesa-query-destroy]
[Storage release][mesa-query-free]

RADV utrace preserves execution-specific storage for reusable recordings:
one-time traces transfer ownership, while reusable traces are cloned through
an appended copy command buffer with an explicit memory-write-to-transfer-read
barrier. When no command-stream words have advanced, several trace events can
also share one timestamp. Event count and hardware sample count consequently
differ. [Clone and barrier][mesa-utrace-copy]
[Submission ownership][mesa-utrace-submit] [Sample reuse][mesa-utrace-write]

ROCr's `async_copy_agent` resets the signal's one pair through `CopyPrep`.
The public batch API requires all descriptors to share one completion signal,
and `DmaCopyBatch` dispatches descriptors individually through paths that
prepare that signal again. There is no aggregate batch pair or retained pair
per descriptor. A batch spanning copy agents also lacks a separate saved
clock owner for each overwritten pair. A completed measurement must be copied
out before reusing that signal's sample/agent epoch.
[Pair reset][rocr-storage] [Agent setter][rocr-agent-setter]
[Batch contract][rocr-batch-contract] [Descriptor dispatch][rocr-batch]

## Clock units and measurement effects

Raw GET_GLOBAL results are GPU ticks. PAL's `timestampFrequency` is in Hz;
RADV queries return raw values and expose a period of
`1,000,000 / clock_crystal_freq` nanoseconds per tick, with that frequency in
kHz. RADV utrace performs this conversion inside its reader. Its advertised
64 valid bits describe the API result; the physical clock's effective width,
wrap and reset behavior still need the device-specific clock source.
[PAL frequency][pal-frequency] [RADV period][mesa-period]
[Native frequency input][mesa-frequency] [Frequency units][mesa-frequency-units]
[Transfer width][mesa-width]
[Utrace conversion][mesa-utrace-read]

`hsa_amd_profiling_get_async_copy_time` returns HSA system-clock ticks, not
the stored GPU ticks. The executing copy agent translates both samples using
its correlated clock state, end first to reduce clock-measurement jitter.
Each scalar translation locks that state separately; the pair has no shared
calibration snapshot. A zero sample or one before its initial GPU
counter produces a zero pair through the getter's success path. The raw
signal reader's `sdma_end_ts != 0` test selects SDMA versus shader storage;
it supplies neither a wait nor an independent validity guarantee.
[API units][rocr-time-contract] [Pair selection][rocr-storage]
[Getter][rocr-getter] [Pair translation][rocr-translation]
[Scalar clock state][rocr-scalar-translation]

For a valid pair in one non-wrapping clock epoch, subtracting ticks before
conversion preserves precision. CPU correlation, cross-device comparisons
and their uncertainty need the separate [clock observation
contract](../observability.md#clock-domains-and-conversion). A GET_GLOBAL
interval excludes host work before its first sample. It can include cache
maintenance, gates and joins, and its drain can remove overlap that exists
without instrumentation. Packet byte size alone predicts neither that
perturbation nor elapsed time.

[pal10-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L107-L137
[pal12-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L124-L146
[pal-contract]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L4024-L4046
[pal-caller]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L1236-L1245
[pal10-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L3057-L3095
[pal12-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L3224-L3260
[pal10-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L355-L450
[pal12-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L345-L385
[pal9-properties]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Device.cpp#L6789-L6808
[pal12-properties]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Device.cpp#L550-L566
[pal-sample-start]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSession.cpp#L1821-L1855
[pal-sample-end]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSession.cpp#L1982-L2005
[pal-session-end]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSession.cpp#L1537-L1621
[pal-ready]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSession.cpp#L2033-L2062
[pal-result]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSessionPerfSample.cpp#L804-L854
[pal-frequency]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palDevice.h#L1030-L1037
[linux-iceland]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/iceland_sdma_pkt_open.h#L2093-L2121
[linux-constants]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/iceland_sdma_pkt_open.h#L38-L43
[linux-tonga]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/tonga_sdma_pkt_open.h#L2166-L2194
[linux-vega]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L2863-L2891
[linux-navi]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L4251-L4279
[linux6]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L4965-L5011
[linux71]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L4965-L5011
[mesa-builder]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L16-L33
[mesa-family]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L109-L126
[mesa-query-write]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L2751-L2826
[mesa-query-storage]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L1934-L2027
[mesa-query-reset]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L1836-L1867
[mesa-query-read]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L2074-L2125
[mesa-query-copy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L1150-L1174
[mesa-query-destroy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L2038-L2048
[mesa-query-free]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L1869-L1882
[mesa-utrace-write]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/layers/radv_utrace_layer.c#L63-L111
[mesa-utrace-read]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/layers/radv_utrace_layer.c#L113-L135
[mesa-utrace-copy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/layers/radv_utrace_layer.c#L196-L235
[mesa-utrace-submit]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/layers/radv_utrace_layer.c#L257-L350
[mesa-period]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L1911-L1912
[mesa-frequency]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L1208-L1212
[mesa-frequency-units]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L1948-L1952
[mesa-width]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L3039-L3048
[rocr-constants]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L51-L81
[rocr-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L940-L968
[rocr-builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2961-L2974
[rocr-storage]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/signal.h#L146-L225
[rocr-agent-setter]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/signal.h#L476-L482
[rocr-aliases]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_blit_sdma.h#L579-L590
[rocr-factory]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L852-L889
[rocr-fused-predicate]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L198-L210
[rocr-enable]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ext_amd.cpp#L863-L877
[rocr-ordinary]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L448-L650
[rocr-gang]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1234-L1339
[rocr-prologue]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L872-L929
[rocr-epilogue]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L998-L1047
[rocr-coordinator-start]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1200-L1241
[rocr-coordinator-end]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1294-L1335
[rocr-fanout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1706-L1865
[rocr-rect]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1864-L1930
[rocr-broadcast]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1638-L1694
[rocr-multicast]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1698-L1738
[rocr-fused-caller]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1413-L1425
[rocr-fused-single]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L732-L793
[rocr-fused-fields]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2714-L2795
[rocr-batch-contract]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2396-L2429
[rocr-batch]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2137-L2232
[rocr-getter]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ext_amd.cpp#L928-L954
[rocr-time-contract]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L1156-L1171
[rocr-translation]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3214-L3230
[rocr-scalar-translation]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3240-L3310
