# SDMA cache maintenance and transport

SDMA cache commands request writeback and invalidation around transfers. A
dependency wait orders work after a producer's published signal; cache
maintenance supplies writeback or invalidation for the next access. Neither
action alone publishes the command bytes, selects memory mappings, or retires every user of
the payload and command storage.

ROCr's V5 ordinary KFD copy path emits `USER_GCR` inside its user ring. Linux's
scheduled-IB path can instead emit `GCR_REQ` on a kernel ring in response to
submission flags. HDP maintenance is a separate operation for the host data
path. Their packet forms and surrounding operations follow the native
transport; a matching opcode does not make the queue contracts interchangeable.
[ROCr stream][rocr-submit] [Linux wrapper][linux-submit]

## Packet forms and fields

The published definitions contain three GCR representations. A DWORD is
32 bits; bit positions below are relative to the containing DWORD. The base
and limit fields omit address bits 6:0, giving 128-byte address granularity.
That representation alone does not define inclusive/exclusive range endpoints,
all range-selector values, or firmware permissions for a particular queue.

### Classic five-DWORD GCR

ROCr's `SDMA_PKT_GCR` and PAL's GFX10 `SDMA_PKT_GCR_REQ` share these aggregate
fields. ROCr selects opcode 17 / suboperation 1 for its user command; Linux's
kernel builder selects opcode 17 / suboperation 0.
[ROCr fields][rocr-fields] [PAL fields][pal10-fields]
[ROCr builder][rocr-gcr] [Linux builder][linux-gcr]

| DWORD | Bits | Field |
| --- | --- | --- |
| 0 | 7:0; 15:8; 31:16 | `op`; `sub_op`; reserved. |
| 1 | 6:0; 31:7 | Reserved; `base_va_31_7` / `BaseVA_LO`. |
| 2 | 15:0; 31:16 | `base_va_47_32` / `BaseVA_HI`; `gcr_control_15_0`. |
| 3 | 2:0; 6:3; 31:7 | `gcr_control_18_16`; reserved; `limit_va_31_7` / `LimitVA_LO`. |
| 4 | 15:0; 23:16; 27:24; 31:28 | `limit_va_47_32` / `LimitVA_HI`; reserved; `vmid`; reserved. |

The VMID member belongs to the generic/request representation. ROCr leaves it
zero when emitting USER_GCR; that does not establish a user-controlled VMID
selection rule. PAL's separately named user form below reserves those bits.

### PAL GFX12 five-DWORD GCR

PAL's `SDMA_PKT_GCR_REQ` and `SDMA_PKT_GCR_USER` retain the classic address
positions and header. The following fields differ; all other fields retain
the positions above. `SDMA_SUBOP_GCR_USER` is 1.
[Suboperation][pal-subop] [Complete request/user definitions][pal-user]

| DWORD | Bits | GFX12 representation |
| --- | --- | --- |
| 1 | 0; 1; 6:2 | Reserved; `broadcast`; reserved. |
| 3 | 3:0; 6:4 | `gcr_control_19_16`; reserved. The aggregate control is 20 bits. |
| 4, request | 15:0; 23:16; 27:24; 31:28 | Limit high address; reserved; `vmid`; reserved. |
| 4, user | 15:0; 31:16 | Limit high address; reserved. There is no VMID operand. |

These generated structs name the extra control bit and broadcast field but
do not give an ordinary inline caller or a complete range/broadcast contract.
PAL's Linux submission uses the separate kernel-wrapper request described
below. Its GFX12 field layout does not establish the encoding of those bits
for a GFX11 user queue.

### Six-DWORD GCR

ROCr's `SDMA_PKT_GCR_GFX1250` moves the control word and expands the base and
limit. Linux SDMA7.1 uses a corresponding six-DWORD aggregate request layout.
The control meanings require the source-specific comparison below.
[ROCr definition][rocr125-fields] [Linux field macros][linux71-fields]
[Linux emitter][linux71]

| DWORD | Bits | Field |
| --- | --- | --- |
| 0 | 7:0; 15:8; 31:16 | `op`; `sub_op`; reserved. |
| 1 | 6:0; 31:7 | Reserved; `base_va_31_7`. |
| 2 | 24:0; 31:25 | ROCr `BaseVA_HI`, named `base_va_56_32` in Linux; reserved. |
| 3 | 18:0; 22:19; 31:23 | `gcr_control_18_0`; reserved; `limit_va_15_7`. |
| 4 | 31:0 | `limit_va_47_16`. |
| 5 | 8:0; 25:9; 29:26; 31:30 | `limit_va_56_48`; reserved; `vmid`; reserved. |

Linux's `BASE_VA_56_32` macro masks its argument with `0x00ffffff`, only
24 bits, although the name spans 25 bits and ROCr's field has width 25.
Linux emits zero there, so its ordinary emitter does not resolve the
high-address discrepancy. Both selected kernel emitters and ROCr's helper
leave range addresses zero; neither is a nonzero-range caller witness.

## Control words and source differences

The following table gives logical control-bit positions, before packing them
into a packet, alongside the full ROCr member and Linux constant names.
The Linux column applies to its Navi10, SDMA6 and SDMA7.1 headers, including the six-DWORD emitter.
[ROCr classic][rocr-fields] [ROCr GFX1250][rocr125-fields]
[Linux Navi10][linux5-controls] [Linux SDMA6][linux6-controls]
[Linux SDMA7.1][linux71-controls]

| Control name | ROCr classic bits | Linux bits | ROCr GFX1250 bits |
| --- | --- | --- | --- |
| `GCR_CONTROL_GLI_INV` / `SDMA_GCR_GLI_INV` | 1:0 | 1:0 | 1:0 |
| `GCR_CONTROL_GL1_RANGE` / `SDMA_GCR_GL1_RANGE` | 3:2 | 3:2 | 3:2 |
| `GCR_CONTROL_GLM_WB` / `SDMA_GCR_GLM_WB` | 4 | 4 | Absent. |
| `GCR_CONTROL_GLM_INV` / `SDMA_GCR_GLM_INV` | 5 | 5 | Absent. |
| `GCR_CONTROL_GL2_SCOPE` | Absent. | Absent. | 5:4 |
| `GCR_CONTROL_GLK_WB` / `SDMA_GCR_GLK_WB` | 6 | 6 | Absent. |
| `GCR_CONTROL_GLV_WB` | Absent. | Absent. | 6 |
| `GCR_CONTROL_GLK_INV` / `SDMA_GCR_GLK_INV` | 7 | 7 | 7 |
| `GCR_CONTROL_GLV_INV` / `SDMA_GCR_GLV_INV` | 8 | 8 | 8 |
| `GCR_CONTROL_GL1_INV` / `SDMA_GCR_GL1_INV` | 9 | 9 | Reserved bit 9. |
| `GCR_CONTROL_GL2_US` / `SDMA_GCR_GL2_US` | 10 | 10 | 10 |
| `GCR_CONTROL_GL2_RANGE` / `SDMA_GCR_GL2_RANGE` | 12:11 | 12:11 | 12:11 |
| `GCR_CONTROL_GL2_DISCARD` / `SDMA_GCR_GL2_DISCARD` | 13 | 13 | 13 |
| `GCR_CONTROL_GL2_INV` / `SDMA_GCR_GL2_INV` | 14 | 14 | 14 |
| `GCR_CONTROL_GL2_WB` / `SDMA_GCR_GL2_WB` | 15 | 15 | 15 |
| `GCR_CONTROL_SEQ` / `SDMA_GCR_SEQ` | 18:17 | 17:16 | 17:16 |
| `GCR_CONTROL_RANGE_IS_PA` / `SDMA_GCR_RANGE_IS_PA` | 16 | 18 | 18 |

Two disagreements affect a general-purpose encoder. First, classic ROCr and
Linux place `SEQ` and `RANGE_IS_PA` differently. Their cited ordinary builders
leave both zero, so that use does not distinguish the layouts. Second,
Linux SDMA7.1 retains the old cache-client names and emits control `0xc3a1`,
including bits 5 and 9. ROCr's GFX1250 definition instead makes those positions
part of `GL2_SCOPE` and a reserved bit. The published sources establish these
different definitions and emitted values; they do not establish one shared
firmware interpretation for both native transports.
[Linux mask and six-DWORD packing][linux71] [ROCr builder][rocr-gcr]

ROCr's GFX1250 helper would emit `0x4010` for acquire and `0x8010` for release:
`GL2_SCOPE=1` plus GL2 invalidation or writeback. The helper comments call
that scope system scope. That value is not the numeric scope encoding of a
different SDMA packet. Its normal factory selects V6 with `useGCR=false`
on this target, so the helper is a definition rather than an active ordinary
GCR path. The classic masks below do have selected ordinary callers.
[Builder][rocr-gcr] [Factory][rocr-select]

## Ordinary user-queue composition

ROCr's V5 blit implementation creates a normal KFD SDMA user queue and emits
dependency polls, applicable HDP maintenance, USER_GCR acquire, the copy body,
USER_GCR release and completion. It then publishes the finished ring extent
through the write pointer and doorbell. Cache commands sit inside the
caller-owned queue; no scheduled kernel IB wrapper runs for each such copy.
[Queue creation][rocr-init], [stream construction][rocr-submit],
[completion][rocr-completion], [publication][rocr-publish]

The classic USER_GCR builder clears the packet before setting these operations:

| Position in the copy stream | ROCr's selected control |
| --- | --- |
| Acquire before copy | `0xc3c0`: GL2/GLK writeback and GL2/GL1/GLV/GLK invalidation. |
| Release after copy | `0x8040`: GL2/GLK writeback. |

Base, limit, range selectors, sequence and other operands remain zero. In
particular, the builder's comment about discarding lines does not set the
distinct GL2_DISCARD field. These are the complete masks emitted by that
builder. [Fields][rocr-fields] [Builder][rocr-gcr]

ROCr's factory selects the composition using ISA and transport, rather than
discovered SDMA IP:

| Source predicate | Selected behavior |
| --- | --- |
| Major 9 | V4: no GCR or per-copy scope fields; applicable HDP work remains separate. |
| Major 10, or major 11/12 with minor below 5; non-DXG | V5: USER_GCR acquire/release. |
| Major 11/12 with minor at least 5; non-DXG | V6: per-packet scope fields, no GCR. |
| DXG on the major-10/11/12 paths | V4; the source attributes GCR wrapping to its underlying driver. |

Initialization excludes HSA full-profile agents. The V6 comment describes
OSS7.1/DACC, which alone does not explain the broader major-11.5 factory
predicate. For non-DXG gfx11.5 the factory selects V6 without USER_GCR. The
DXG wrapping statement belongs to that driver transport, not every Windows
driver interface. The enclosing `InitDma` policy can select a shader blit
instead of creating an SDMA blit at all.
[Factory][rocr-select] [Variant definitions][rocr-variants]
[Executor selection][rocr-blit-select]

Each packet builder still owns the scope-field encoding. ROCr's
[rectangular-copy branch](rectangular-copy.md#rectangular-specific-scope-boundary)
sets those fields only in its GFX12-or-later layout, so its gfx11.5 rectangular
path does not inherit the ordinary linear-copy scope writes.

### Maintenance placement by entry point

ROCr has separate ordinary, fanout and fused stream constructors. Their GCR
and HDP placement is not one shared prologue applied to every transfer.
The table lists all direct maintenance-call owners; HDP additionally requires
the setting and platform conditions described below.

| Stream constructor | Selected maintenance and ordering |
| --- | --- |
| `SubmitCommand` | Dependencies, optional start sample, applicable HDP, V5 GCR acquire, body, V5 GCR release, optional end sample, gang joins, output completion and optional notification. |
| `SubmitPrologue` | Classic fanout coordinator waits for dependencies, samples start if requested, performs applicable HDP and V5 GCR acquire, then clears its private prologue signal. |
| `SubmitEpilogue` | Classic fanout coordinator joins body completion, performs V5 GCR release, samples end if requested, completes the output and optionally notifies. |
| `SubmitLinearCopyBodyWaitSignal` | Fused single-copy service is selected for major 12/minor at least 5. Both factory choices there have GCR disabled; no HDP call exists in this constructor. |
| `SubmitFusedCoordinator` | On that fused service, a profiling prologue waits for dependencies and emits applicable HDP before releasing workers. Its GCR arms are disabled by the selected template. Without profiling, the owner creates no prologue. |
| `SubmitLinearCopyMulticastCommand` | Profiling delegates to ordinary `SubmitCommand`. Its separate unprofiled fused path has no HDP and no selected GCR emission. |

[Ordinary stream][rocr-submit] [Ordinary tail][rocr-tail]
[Classic prologue][rocr-prologue] [Classic epilogue][rocr-epilogue]
[Single fused constructor][rocr-single] [Coordinator][rocr-coordinator]
[Multicast constructor][rocr-multicast] [Actual fused/fanout owner][rocr-fanout]
[Single-copy owner][rocr-single-owner]

Classic fanout workers wait for the coordinator's private prologue signal
before their bodies. The epilogue joins either an aggregate output or private
body signals before release and final completion. `SubmitBodies` has no GCR
or HDP call. This is one coordinator-owned cache envelope, not one repeated
by every worker; the source placement alone does not specify the physical
cache instances covered by that request. Ordinary gang copy instead submits
each stream through the ordinary envelope.
[Worker bodies][rocr-bodies] [Classic fanout owner][rocr-fanout-classic]
[Ordinary gang owner][rocr-gang]

The ordinary packet-building linear, broadcast and rectangular copy paths
use `SubmitCommand`; the fused single-copy constructor is separate. ROCr's
DTIF fast-copy branches use CPU `memcpy` without emitting this stream.
The SDMA fill helper would also use it, but ROCr's public `DmaFill` selects
the shader blit. That helper's presence is not an active USER_GCR fill
service. The [fill chapter](fill.md#applicability-and-caller-selection)
describes the selected executor.
[Ordinary copy callers][rocr-copy-callers] [Rectangular caller][rocr-rect-caller]
[Fill helper][rocr-fill-helper] [Fill owner][rocr-fill-owner]

## Kernel submission and generation differences

PAL's Linux `Queue::AddIb` requests `AMDGPU_IB_FLAG_EMIT_MEM_SYNC` on the
first IB. Both its raw and legacy submission conversions preserve that flag.
Linux's `amdgpu_ib_schedule` tests the first IB's flag and the ring's
`emit_mem_sync` callback, then emits the cache operation before HDP flush
and the IB sequence. After the IBs it calls HDP invalidate, emits applicable
user/native completion fences, and commits the ring. This is a conditional
prefix operation, not a release automatically appended after every payload.
[PAL request][pal-submit] [Raw flag propagation][pal-raw]
[Legacy propagation][pal-legacy] [First-IB selection][linux-first-ib]
[Kernel sequence][linux-submit]

`AMDGPU_IB_FLAG_EMIT_MEM_SYNC` is bit 6; bit 3,
`AMDGPU_IB_FLAG_TC_WB_NOT_INVALIDATE`, is a separate fence-policy request.
The kernel allocator initially sets MEM_SYNC on its own allocated IBs. The
userspace CS parser replaces those flags with the submitted flags; the
allocator default is not silently added to every UMD submission. Kernel
`amdgpu_copy_buffer` is an actual consumer of the allocated-IB default.
[Flag definitions][linux-ib-flags] [Allocation default][linux-ib-get]
[CS parser][linux-cs-flags] [Kernel copy owner][linux-copy-owner]
[Job/IB allocation][linux-job-allocation]

### Registered Linux engine backends

The table describes installed callbacks at the cited revision. An absent GCR
callback means the wrapper supplies no GCR through that hook; it does not
specify the coherence of every mapping used by the engine. HDP remains a
separate callback and fallback path.

| Ring backend | GCR callback | Ring HDP flush |
| --- | --- | --- |
| SI DMA | Absent. | No ring callback; device fallback. [Table][linux-si] |
| CIK SDMA | Absent. | Six-DWORD request/done register poll. [Emitter][linux-cik-hdp] [Table][linux-cik] |
| SDMA2.4 | Absent. | Six-DWORD HDP poll. [Emitter][linux24-hdp] [Table][linux24] |
| SDMA3.0 | Absent. | Six-DWORD HDP poll. [Emitter][linux30-hdp] [Table][linux30] |
| SDMA4.0 | Absent in ordinary and paging tables. | Shared HDP poll helper. [Emitter][linux40-hdp] [Tables][linux40] |
| SDMA4.4.2 | Absent in ordinary and paging tables. | Shared HDP poll helper. [Emitter][linux442-hdp] [Tables][linux442] |
| SDMA5.0 | Five-DWORD GCR_REQ. | HDP poll. [Emitters][linux50] [Table][linux50-table] |
| SDMA5.2 | Five-DWORD GCR_REQ. | HDP poll for engines 0/1; generic HDP fallback for higher engine indices. [Emitters][linux52] [Table][linux52-table] |
| SDMA6.0 implementation | Five-DWORD GCR_REQ. | HDP poll. [Emitters][linux60] [Table and installation][linux60-table] |
| SDMA7.0 | Five-DWORD GCR_REQ. | HDP poll. [Emitters][linux70] [Table][linux70-table] |
| SDMA7.1 | Six-DWORD GCR_REQ. | No HDP poll callback; installed register-write callback permits the generic HDP path. [GCR][linux71] [Table][linux71-table] |

Discovery selects the SDMA6.0 implementation for native SDMA IP
6.0.0–6.0.3, 6.1.0–6.1.4 and 6.4.0. These versions therefore receive the
same nonempty cache callbacks. The other selected groups are 5.0.0/1/2/5
for SDMA5.0, 5.2.0–5.2.7 for SDMA5.2, 7.0.0/1 for SDMA7.0 and 7.1.0
for SDMA7.1. Native SDMA and HDP versions are separate selectors.
[SDMA IP selection][linux-discovery]

All five GCR emitters select suboperation 0, zero base/limit and VMID zero.
Their logical control is `0xc3a1`: Linux-named GLI_INV value 1, GLM/GLK/GLV/
GL1/GL2 invalidation and GL2 writeback. GLM/GLK writeback remain zero.
The six-DWORD control-name disagreement above remains material even though
the numerical mask is unchanged. These are kernel-owned request packets;
the wrapper does not establish a USER_GCR range or context-selection rule.

### PAL and Mesa supply different surrounding work

PAL's DMA barrier methods emit event waits and applicable overlap-hazard NOPs,
alongside image transitions. Its Linux submission supplies the MEM_SYNC
request separately. A generic barrier comment about DMA having no caches
does not remove those native submission operations.
[PAL DMA barriers][pal-barriers] [Combined barrier][pal-combined]

RADV's `ib_to_info` starts IB flags at zero; its DRM submission path adds
other flags but not MEM_SYNC. Gallium's MEM_SYNC assignment is conditional
on a GFX or COMPUTE IB, not an SDMA IB. Mesa also removes SDMA from its
direct-userq mask at this revision. Neither audited SDMA submission path
inherits PAL's kernel GCR request.
[RADV initial flags][mesa-ib-flags] [RADV submission flags][mesa-submit-flags]
[Final DRM conversion][mesa-drm-flags] [Gallium engine predicate][mesa-gallium]
[Userq selection][mesa-userq]

RADV's SDMA-to-ACE image-copy fallback instead assigns cache work to the
actual shader consumer and producer. ACE waits for the SDMA progress signal
and requests L2 invalidation before its copy; its return signal requests
GL2 writeback on GFX10+ before SDMA proceeds. Its progress-allocation policy
selects a separate BO for noncoherent gang members, with L2 bypass under
that predicate; the alternate path uses upload storage. The queue-wide final
join has a separate owner. [Progress mapping selection][mesa-progress-mapping]
Those operations are detailed in the
[PM4/SDMA handoff](../recipes/pm4-sdma.md) and
[terminal-join sequence](poll.md#mesa-progress-and-terminal-joins).
[Actual copy fallback][mesa-copy] [Progress and cache operations][mesa-progress]

## HDP and command publication

ROCr's `SDMA_PKT_HDP_FLUSH` is a parameterless six-DWORD template:
`{0x8, 0, 0x80000000, 0, 0, 0}`. `BuildHdpFlushCommand` copies those words;
it does not configure the ordinary dependency-poll builder. The shared opcode
and extent do not give this template the meaning of an equality memory poll.
The header names gfx9 microcode `0x1a5` as its introduction version, but the
blit initialization and builder do not compare that constant to reported
firmware. The comment is not an enforced runtime version gate.
[Template][rocr-hdp-template] [Builder][rocr-hdp-builder]
[Initialization][rocr-init]

The enable setting defaults on unless `HSA_ENABLE_SDMA_HDP_FLUSH` is the
string `"0"`; DXG discovery explicitly disables it. Initialization samples
that effective setting. The blit's separate support predicate requires ISA
major at least 9 except 10.1, and a non-XGMI link from this GPU to the first
CPU agent. Its three HDP callsites apply both enable and support, with the
additional prologue/profiling conditions in the caller table above.
[Environment setting][rocr-hdp-flag] [DXG override][rocr-dxg-hdp]
[Sampled setting][rocr-hdp-setting] [Platform predicate][rocr-init]

### Kernel HDP request and dispatch

Linux's ring HDP operation is a register request/done protocol. In the classic
`POLL_REGMEM` representation, its six words are:

| DWORD | Emitted operand |
| --- | --- |
| 0 | Opcode 8, suboperation 0, HDP/extra-operation bit 26 set, equality function 3 in bits 30:28; memory-poll bit 31 clear. |
| 1 | The selected done-register index shifted left 2. |
| 2 | The selected request-register index shifted left 2. This is not the high half of a memory address. |
| 3 | Engine-specific reference value. |
| 4 | The same engine-specific mask. |
| 5 | Retry count `0xfff` in bits 27:16; interval 10 decimal (`0x0a`) in bits 15:0. |

[CIK operation spelling][linux-cik-poll] [SDMA6 emitter][linux60-hdp]
[Poll fields](poll.md#classic-memory-equality)

CIK, SDMA2.4 and SDMA3.0 select SDMA0/1 reference bits. SDMA5.0 uses the
corresponding NBIO reference values. SDMA4.0, SDMA6.0 and SDMA7.0 shift the
SDMA0 mask by `ring->me`; SDMA4.4.2 uses that index modulo
`num_inst_per_aid`. SDMA5.2 takes the generic HDP path above engine 1.
The emitters in the backend table own these choices; they are not
caller-selected memory-poll operands.

`amdgpu_device_flush_hdp` and `amdgpu_device_invalidate_hdp` skip their
operations for an APU without passthrough under `CONFIG_X86_64`, and for
`xgmi.connected_to_cpu` on all builds. Other XGMI connectivity alone does not
select that skip. Flush prefers a ring HDP callback; without one it delegates
to ASIC/HDP dispatch. Invalidate delegates directly. The generic flush can
emit an in-stream register write through `emit_wreg`, or perform the CPU
register operation when that path is not available. Callback absence does
not mean no HDP maintenance. A no-ring SR-IOV runtime call first attempts
the wrapper's KIQ flush and returns on success; only a nonzero result falls
through to generic dispatch.
[Device predicates][linux-hdp-device] [ASIC/HDP dispatch][linux-hdp-dispatch]

The HDP-IP callbacks further distinguish flush from invalidate:

| HDP implementation | Selected behavior after the device predicates |
| --- | --- |
| 4.0 | Generic flush. Invalidate returns immediately for HDP IP 4.4.0/4.4.2/4.4.5; otherwise it writes `HDP_READ_CACHE_INVALIDATE`. [Invalidate][linux-hdp40] [Callbacks][linux-hdp40-table] |
| 5.0 | Generic flush and a separate read-cache invalidation write. This implementation also serves HDP IP 5.2.0. [Invalidate][linux-hdp50] [Callbacks][linux-hdp50-table] |
| 5.2 | Its own remapped flush-register path, with CPU readback selected by virtualization mode or an in-stream write. No invalidate callback; selected for HDP IP 5.2.1. [Flush][linux-hdp52] [Callbacks][linux-hdp52-table] |
| 6.0 | Generic flush and no invalidate callback. Also selected for HDP 6.1.0/6.1.1/6.4.0. [Callbacks][linux-hdp60] |
| 7.0 | Generic flush and no invalidate callback. [Callbacks][linux-hdp70] |

[HDP IP selection][linux-hdp-discovery] supplies the exact version groups.
Older SI/CIK/VI ASIC callbacks take priority over HDP-IP callbacks and supply
their own flush/invalidate writes. A missing HDP invalidate hook, an explicit
platform skip, and an absent SDMA GCR hook are different mechanisms.
[SI callbacks][linux-si-hdp] [CIK callbacks][linux-cik-asic-hdp]
[VI callbacks][linux-vi-hdp]

## Complete payload and command handoff

The public HSA asynchronous-copy contract requires system-coherent buffers:
the sending device supplies its release before copy and the receiving device
supplies the applicable system acquire before use. Dependency readiness and
the completion-signal update are additional obligations. The presence of a
GCR builder does not transfer those responsibilities to the packet format.
[HSA copy contract][hsa-copy]

CLR provides a concrete composition:

1. `DmaBlitManager::hsaCopy` obtains the mapped addresses and owning agents,
   then calls `releaseGpuMemoryFence`. Pending dispatch, dirty fence state or
   external dependencies cause that helper to emit a system-scope AQL
   barrier. [Copy caller][clr-copy] [Release owner][clr-release]
   [Barrier header][clr-barrier]
2. `rocrCopyBuffer` passes dependency and completion signals to the selected
   HSA copy API. On success it marks the next queue header for system scope.
   `adjustHeader` applies system acquire/release to that subsequent device
   work. [Copy completion policy][clr-copy-scope] [Consumer header][clr-scope]
3. The destination consumer still waits for copy completion before access.
   Signal reuse also waits for its final waiter; a payload-ready observation
   need not retire a later mailbox/trap or another engine's control access.
   [Signal and waiter lifetime](poll.md#signal-lifetime-and-actual-callers)
   [Completion tails](fence.md#rocr-signal-stores)

Command storage has its own publication and retirement. ROCr constructs the
ring bytes before ordered WPTR/doorbell publication; RPTR governs reusable
ring capacity. Linux pads its ring and executes `mb()` before publishing WPTR.
An in-stream HDP or GCR operation cannot make its own previously unpublished
command bytes fetchable. Nor does that operation retire an independently
referenced signal, image, or payload mapping.
[ROCr publication][rocr-publish] [ROCr capacity][rocr-capacity]
[Linux ring publication][linux-ring-publish]
[Command ownership](publication.md#completion-and-storage-ownership)

For scheduled IBs, native fences and the driver's completion-backed allocation
owners remain the retirement mechanism. For multiple engines, their terminal
join must cover all command users before replay or reuse. System-memory
placement, CPU access through a VRAM aperture, GPU cache policy and the actual
peer route remain separate inputs to the directed visibility recipe.
[Native completion](fence.md#linux-ring-completion-and-user-fences)
[Directed memory paths](../recipes/local-memory.md)

[rocr-submit]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L516-L578
[linux-submit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c#L231-L351
[linux-first-ib]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c#L124-L131
[mesa-progress-mapping]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1855-L1939
[rocr-fields]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L1002-L1059
[pal10-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L2259-L2315
[rocr-gcr]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2992-L3026
[linux-gcr]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L293-L315
[pal-subop]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L71
[pal-user]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L2410-L2526
[rocr125-fields]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L1061-L1125
[linux71-fields]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L5164-L5225
[linux71]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c#L289-L310
[linux5-controls]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L77-L91
[linux6-controls]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L79-L93
[linux71-controls]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L79-L93
[rocr-select]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L852-L904
[rocr-init]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L156-L260
[rocr-completion]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L614-L650
[rocr-publish]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1997-L2046
[rocr-variants]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_blit_sdma.h#L579-L590
[rocr-blit-select]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L974-L1029
[rocr-tail]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L579-L663
[rocr-prologue]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L806-L940
[rocr-epilogue]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L942-L1073
[rocr-single]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L666-L804
[rocr-coordinator]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1075-L1346
[rocr-multicast]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1697-L1856
[rocr-fanout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1706-L1809
[rocr-single-owner]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1418-L1436
[rocr-bodies]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1348-L1574
[rocr-fanout-classic]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1812-L1895
[rocr-gang]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1234-L1343
[rocr-copy-callers]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1576-L1695
[rocr-rect-caller]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1858-L1931
[rocr-fill-helper]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1933-L1947
[rocr-fill-owner]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2263-L2265
[pal-submit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuQueue.cpp#L1938-L1983
[pal-raw]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuQueue.cpp#L2037-L2053
[pal-legacy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuQueue.cpp#L2319-L2333
[linux-ib-flags]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/drm/amdgpu_drm.h#L1001-L1016
[linux-ib-get]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c#L64-L87
[linux-cs-flags]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_cs.c#L397-L415
[linux-copy-owner]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L2464-L2559
[linux-job-allocation]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_job.c#L252-L277
[linux-si]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/si_dma.c#L736-L758
[linux-cik-hdp]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cik_sdma.c#L247-L264
[linux-cik]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cik_sdma.c#L1236-L1260
[linux24-hdp]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v2_4.c#L274-L292
[linux24]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v2_4.c#L1123-L1148
[linux30-hdp]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v3_0.c#L451-L469
[linux30]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v3_0.c#L1504-L1529
[linux40-hdp]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_0.c#L827-L871
[linux40]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_0.c#L2426-L2487
[linux442-hdp]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L392-L437
[linux442]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L2132-L2193
[linux50]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c#L464-L508
[linux50-table]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c#L1930-L1962
[linux52]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v5_2.c#L312-L358
[linux52-table]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v5_2.c#L1935-L1969
[linux60]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L300-L342
[linux60-table]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L1741-L1784
[linux70]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_0.c#L302-L344
[linux70-table]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_0.c#L1673-L1704
[linux71-table]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c#L1638-L1667
[linux-discovery]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c#L2785-L2845
[pal-barriers]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L243-L305
[pal-combined]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L387-L445
[mesa-ib-flags]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L156-L166
[mesa-submit-flags]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L1020-L1149
[mesa-drm-flags]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L1752-L1790
[mesa-gallium]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp#L833-L859
[mesa-userq]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L1499-L1504
[mesa-copy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_copy.c#L106-L138
[mesa-progress]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1955-L2037
[rocr-hdp-template]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L988-L1000
[rocr-hdp-builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2986-L2990
[rocr-hdp-flag]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/util/flag.h#L231-L232
[rocr-dxg-hdp]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_topology.cpp#L319-L334
[rocr-hdp-setting]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L256-L260
[linux-cik-poll]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cikd.h#L495-L542
[linux60-hdp]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L325-L342
[linux-hdp-device]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c#L6541-L6575
[linux-hdp-dispatch]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_hdp.c#L51-L84
[linux-hdp40]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/hdp_v4_0.c#L39-L54
[linux-hdp40-table]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/hdp_v4_0.c#L171-L177
[linux-hdp50]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/hdp_v5_0.c#L30-L40
[linux-hdp50-table]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/hdp_v5_0.c#L208-L214
[linux-hdp52]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/hdp_v5_2.c#L30-L52
[linux-hdp52-table]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/hdp_v5_2.c#L202-L206
[linux-hdp60]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/hdp_v6_0.c#L140-L144
[linux-hdp70]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/hdp_v7_0.c#L128-L132
[linux-hdp-discovery]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c#L3513-L3549
[linux-si-hdp]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/si.c#L1492-L1511
[linux-cik-asic-hdp]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cik.c#L1859-L1878
[linux-vi-hdp]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vi.c#L1311-L1330
[hsa-copy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2090-L2136
[clr-copy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocblit.cpp#L618-L644
[clr-release]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L2347-L2365
[clr-barrier]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L71-L74
[clr-copy-scope]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocblit.cpp#L581-L603
[clr-scope]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L1624-L1633
[rocr-capacity]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2087-L2094
[linux-ring-publish]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c#L169-L189
