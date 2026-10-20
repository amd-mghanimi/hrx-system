# Compute context save and restore

Compute wave save/restore (CWSR) lets the native scheduler take a compute
queue off hardware while preserving unfinished waves for resumption. The
command processor and context-save handler retain control, register and LDS
state in queue-owned storage. Saved waves still depend on their executable,
arguments, payloads and private scratch. Suspension releases execution
resources; completion and final storage ownership remain separate events.
[Native save/remap sequence][queue-update] · [Storage calculation][native-sizing]

## Applicability and owners

This chapter describes Linux KFD compute queues and the ROCr thunk's backing
at the [source-map revisions](../sources.md). KFD handles both
`KFD_IOC_QUEUE_TYPE_COMPUTE` and `KFD_IOC_QUEUE_TYPE_COMPUTE_AQL` as compute
queues, with separate PM4 and AQL formats. SDMA does not use this compute
save-area contract. [Queue classification][queue-classification] ·
[Compute-only acquisition][native-acquisition]

Three owners establish different parts of the mechanism:

| Owner | Responsibility |
| --- | --- |
| KFD process/device owner | Select and install the context-save handler and construct native queue state. |
| User queue owner | Allocate, initialize and retain the mapped context-save area and any referenced error-signal payload. |
| Dispatch/runtime owner | Preserve code, arguments, payloads and private scratch needed by unfinished work, including work saved off hardware. |

KFD's handler selection requires both `cwsr_enable` and the device's
`supports_cwsr` predicate. Its physical GC-IP selection is independent of the
compiler target used for storage sizing. For example, GC12.1.0 maps to
`gfx_target_version = 120500` (`gfx1250`). The
[trap and context owner flow](pm4/dispatch.md#runtime-trap-and-context-state)
traces installation and runtime handler chaining. Nonzero topology
`cwsr_size` or `ctl_stack_size` describes storage, without proving that this
handler is installed. [Handler selection][handler-selection] ·
[GC12.1.0 target mapping][gc1250] · [Sizing inputs][native-sizing]

Other [native transports](architectures.md#native-queue-ownership) have their own
context and retirement owners. These KFD sizes and user-memory layouts do
not specify DRM scheduled-IB, DRM user-queue or Windows WDDM save storage.

## Save-area representation

The queue's create request contains three context-save fields. Offsets below
are bytes within the 96-byte `kfd_ioctl_create_queue_args` at the cited UAPI
revision. The address names the first XCC's header.
[Create-queue representation][create-layout]

| Byte offset | Field | Width | Meaning |
| --- | --- | --- | --- |
| 72 | `ctx_save_restore_address` | 64 bits | Base virtual address `A` of the mapped area. |
| 80 | `ctx_save_restore_size` | 32 bits | Bytes `S` for one XCC's header, control stack and wave-save storage; excludes debugger storage. |
| 84 | `ctl_stack_size` | 32 bits | Bytes `T` from that XCC's header to its wave-save area; includes header space and control-stack capacity. |

Let `X` be the queue node's XCC count and `D` its debugger bytes per XCC.
The thunk constructs `X` consecutive `S`-byte context regions, followed by
one combined `X*D`-byte debugger region:

```text
A             XCC 0: header | control-stack space | wave-save space
A + S         XCC 1: header | control-stack space | wave-save space
...
A + (X-1)*S   XCC X-1: header | control-stack space | wave-save space
A + X*S       debugger storage for all XCCs, X*D bytes
```

For each ordinal `i`, the thunk places its header at `A+i*S`, writes
`DebugOffset = (X-i)*S`, and sets `DebugSize = X*D`. All headers therefore
refer to the same trailing debugger region. The per-XCC stride is `S`, even
though total backing is page-rounded `X*(S+D)`. KFD's multi-XCC MQD
constructors for GC9.4.3, GC9.4.4, GC9.5.0 and GC12.1.0 independently use
`A+i*S` for the per-XCC base.
[Thunk headers][header-initialization] · [Allocation extent][thunk-allocation] ·
[V9 branch selection][mqd9-xcc-selection] · [V9 bases][mqd9-xcc] ·
[GC12.1.0 bases][mqd121-xcc]

### Header fields

The 64-bit thunk's `HsaUserContextSaveAreaHeader` occupies 40 bytes at each
context-region base. Linux's `kfd_context_save_area_header` has the matching
field layout, with its first four words grouped under `wave_state`. Their
C type alignments differ: the thunk forces four-byte packing, while the
native header has eight-byte alignment on the 64-bit ABI. The allocation's
page alignment satisfies both. Offsets are relative to the current XCC
header, including `DebugOffset`. [Thunk declaration][header-layout] ·
[Thunk packing][header-packing] · [Native declaration][native-header]

| Byte offset | Thunk field | Width | Value or interpretation |
| --- | --- | --- | --- |
| 0 | `ControlStackOffset` | 32 bits | Offset of the saved control-stack top; 4-byte aligned. |
| 4 | `ControlStackSize` | 32 bits | Saved control-stack bytes; a multiple of 4. |
| 8 | `WaveStateOffset` | 32 bits | Wave-state offset; the public comment and native snapshot writer differ as explained under [inspection](#inspecting-saved-state). |
| 12 | `WaveStateSize` | 32 bits | Saved wave-state bytes; a multiple of 4. |
| 16 | `DebugOffset` | 32 bits | Offset to debugger storage; 64-byte aligned. |
| 20 | `DebugSize` | 32 bits | Combined debugger bytes; a multiple of 64. |
| 24 | `ErrorReason` | 64-bit pointer | Error-reason signal payload address, corresponding to native `err_payload_addr`; the declaration requires 4-byte alignment. |
| 32 | `ErrorEventId` | 32 bits | Native event ID for exception notification; zero when the thunk receives no event. |
| 36 | `Reserved1` | 32 bits | Reserved. |

The save area is page aligned; wave-save storage begins at page-aligned
offset `T`. `ErrorReason` is a payload pointer, not an HSA signal handle or a
copy of the payload. The queue's context storage and this referenced payload
have separate allocations and owners. The thunk initializes debug/error
fields before native creation. Its public declaration requires the area to
remain valid for the queue's lifetime.
[Header contract][header-layout] · [Initialization][header-initialization]

## Topology-sized capacity

Context-save capacity represents the state that may reside on the assigned
hardware. It is independent of one kernel's private-segment size, grid size,
or LDS request. Dispatch [scratch sizing](aql/scratch.md) and
[LDS allocation](pm4/lds.md) have different units and owners. The native
calculation below uses topology; the create-time checks consume its result.
[Sizing implementation][native-sizing] · [Creation checks][native-acquisition]

### Linux calculation

At Linux revision `50d05c7c`, `kfd_queue_ctx_save_restore_size` operates on
decimal `gfx_target_version` values from `80001` onward. Here `90402` means
compiler target `gfx942`, and `90010` means `gfx90a`; these numbers are not
packed physical GC IP versions. Define:

| Symbol | Native quantity |
| --- | --- |
| `X` | `NUM_XCC(dev->gpu->xcc_mask)` for the topology node. |
| `C` | `simd_count / simd_per_cu / X`, the per-XCC CU count used by this calculation. |
| `N` | Wave capacity selected by the table below. |
| `V`, `G`, `L`, `H` | Per-CU VGPR, SGPR, LDS and hardware-register save bytes. |
| `P` | Host `PAGE_SIZE`. `AMDGPU_GPU_PAGE_SIZE` separately equals 4096 bytes. |

[Native quantities][native-sizing] · [GPU page size][gpu-page]

| Decimal target predicate | Wave capacity `N` | Control-stack bytes per wave `B` |
| --- | --- | --- |
| `80001 <= gfxv < 100100` | `min(C*40, (array_count / simd_arrays_per_engine)*512)` | 8 |
| `100100 <= gfxv < 120500` | `C*32` | 12 |
| `120500 <= gfxv <= 120501` | `C*64` | 12 |

The cited function has no nonzero wave-count case above `120501`; numeric
ordering supplies no sizing contract for later targets.
[Wave-count predicates][native-wave-count]

The following table reproduces the native storage budgets, rather than
inferring register-file capacities from a marketing family name. Each row
names the exact targets receiving that override; the default covers other
targets admitted by the sizing function.

| Compiler targets / decimal values | `V` | `G` | `L` | `H` |
| --- | --- | --- | --- | --- |
| Default | `0x40000` | `0x4000` | `0x10000` | `0x1000` |
| `gfx908`, `gfx90a`, `gfx942` / `90008`, `90010`, `90402` | `0x80000` | `0x4000` | `0x10000` | `0x1000` |
| `gfx950` / `90500` | `0x80000` | `0x4000` | `lds_size_in_kb*1024` | `0x1000` |
| `gfx1100`, `gfx1101`, `gfx1151`, `gfx1200`, `gfx1201` / `110000`, `110001`, `110501`, `120000`, `120001` | `0x60000` | `0x4000` | `0x10000` | `0x1000` |
| `gfx1250`, `gfx1251` / `120500`, `120501` | `0x80000` | `0x8000` | `lds_size_in_kb*1024` | `0x8000` |

[Per-CU native budgets][native-cu-bytes]

With `align_up(value, alignment)` measured in bytes, the calculation is:

```text
W = align_up(C * (V + G + L + H), 4096)
T = align_up(40 + N*B + 8, 4096)
if decimal target major == 10: T = min(T, 0x7000)
D = align_up(N * 32, 64)
S = align_up(T + W, P)
total mapped extent = align_up(X * (S + D), P)
```

`W` is wave/workgroup storage capacity, while the extra eight bytes in `T`
are part of the native control-stack sizing formula. The GFX10 `0x7000`
control-stack cap carries an explicit source caveat: it is sufficient for
the AQL path's SPI-event bound but insufficient for theoretical PM4 cases.
Allocating a larger `T` is not a native remedy because creation requires
the topology's exact value. That source does not establish unrestricted PM4
preemption at every theoretical occupancy.
[Size construction and cap][native-sizing] · [Exact-size check][native-acquisition]

KFD exports `cwsr_size` and `ctl_stack_size` in node properties. Compute
creation requires `ctl_stack_size == topology.ctl_stack_size` and
`ctx_save_restore_size >= topology.cwsr_size`; it then calculates the full
mapped extent using its own debugger size and node XCC count. A legal larger
`S` also becomes the per-XCC stride. A queue CU mask or a small dispatch
does not reduce these create-time checks.
[Topology properties][topology-sizes] · [Native acquisition][native-acquisition]

### Thunk calculation and older kernels

ROCr revision `f9ba16bb` calculates local sizes and then substitutes each
nonzero `HsaNodeProperties.CwsrSize` and `CtlStackSize` independently. The
local calculation remains for kernels without those properties. Debugger
capacity still comes from the thunk's local wave calculation, rather than
either substituted size. [Selection of native sizes][thunk-sizing]

The fallback and native formulas are not identical:

| Quantity | Thunk rule at the cited revision |
| --- | --- |
| Target representation | `(Major << 16) \| (Minor << 8) \| Stepping`, for example `gfx1151 = 0x0B0501`. |
| Wave capacity from `GFX_VERSION_NAVI10` onward | `C * NumSIMDPerCU * MaxWavesPerSIMD`. |
| VGPR bytes | Ordered ranges; the range from `GFX_VERSION_PLUM_BONITO` through `GFX_VERSION_GFX1201` uses `0x60000`. |
| LDS bytes | `LDSSizeInKB*1024` for every target in this calculation. |
| Hardware-register bytes | `0x1000` below `GFX_VERSION_GFX1250`; for exactly `gfx1250`, `NumSIMDPerCU*MaxWavesPerSIMD*512`. |
| Latest explicit SGPR/VGPR case | `GFX_VERSION_GFX1250`; these helpers provide no nonzero result above that target. |

[Packed target definitions][thunk-targets] · [Local byte and wave budgets][thunk-budgets]
· [LDS and control-entry units][thunk-units]

For example, `gfx1150` falls into the kernel's default VGPR budget but the
thunk's broader `0x60000` range. This is a source-specific difference in
backing estimates, not evidence that one dispatch owns that many registers.
The authoritative creation check uses native topology. Native `gfx1251`
arithmetic alone also does not establish that this pinned thunk can construct
a `gfx1251` queue: the local helpers execute before size substitution.
[Native predicates][native-cu-bytes] · [Thunk predicates and order][thunk-budgets]
[Substitution caller][thunk-sizing]

## Mapping and native queue state

The full allocation is established before queue creation. KFD first acquires
a BO mapping for the complete page-rounded extent. Context storage has a
separate SVM path requiring registered, mapped, accessible
`GPU_ALWAYS_MAPPED` ranges. The [queue-storage contract](architectures.md#kfd-queue-storage)
explains that construction and the thunk's SVM versus nonpaged selection.
A GPU-accessible pointer alone does not identify which native backing exists.
[Acquisition][native-acquisition] · [Thunk allocation][thunk-allocation]

KFD owns the memory queue descriptor (MQD). In the V9, V10, V11, V12 and
V12.1 initializers, enabled context save sets `QSWITCH_MODE` and supplies:

| MQD field | Initial value |
| --- | --- |
| `cp_hqd_ctx_save_base_addr_lo`, `cp_hqd_ctx_save_base_addr_hi` | Low/high halves of the byte address of this XCC's context region. |
| `cp_hqd_ctx_save_size` | `S` bytes. |
| `cp_hqd_cntl_stack_size` | `T` bytes. |
| `cp_hqd_cntl_stack_offset` | `T`, the initial control-stack frontier. |
| `cp_hqd_wg_state_offset` | `T`, the initial wave-state frontier. |

The V9 initializer additionally tests the supplied base address before
installing these fields. The two frontiers start together: saved control
entries occupy descending addresses below `T`, and wave/workgroup data
occupies ascending addresses above `T`. The GPU save protocol updates these
frontiers; the caller's allocation is capacity, not a serialized list of
currently running waves. [V9 initializer][mqd9-init] · [V10 initializer][mqd10-init]
· [V11 initializer][mqd11-init] · [V12 initializer][mqd12-init] ·
[V12.1 initializer][mqd121-init] · [Frontier interpretation][mqd12-snapshot]

The [compute queue-context chapter](queue-context.md) describes the surrounding
MQD image, its native allocation and per-XCC representation. MQD slice stride
and user context-region stride have separate owners and sizes.

## Inspecting saved state

`hsaKmtGetQueueInfoCtx` invokes `AMDKFD_IOC_GET_QUEUE_WAVE_STATE`. KFD
requires a compute queue that is inactive, CWSR enabled on the device, and
a matching `get_wave_state` callback. The process mutex protects the native
query against queue destruction. The call observes saved state; it neither
suspends an active queue nor waits for a dispatch to complete.
[Thunk query][thunk-query] · [Ioctl serialization][query-ioctl] ·
[Native preconditions][query-preconditions]

The native MQD readers calculate:

```text
saved control bytes = cp_hqd_cntl_stack_size - cp_hqd_cntl_stack_offset
saved wave bytes    = cp_hqd_wg_state_offset - cp_hqd_cntl_stack_size
```

| Native implementation | Control-stack observation |
| --- | --- |
| GFX8/VI | Already in user context storage; the callback returns used sizes without copying the stack or rewriting the four-word snapshot header. |
| GFX9 | Lives one GPU page after the native MQD; the callback copies it into user context storage and writes the four-word snapshot header. The copied range is bounded by the supplied control-stack capacity. |
| GFX10, GFX11, GFX12 and GC12.1.0 | Already in user context storage; the callbacks write the four-word snapshot header without copying the stack. |

[VI reader][mqd8-snapshot] · [V9 reader][mqd9-snapshot] ·
[V10 reader][mqd10-snapshot] · [V11 reader][mqd11-snapshot] ·
[V12 reader][mqd12-snapshot] · [V12.1 reader][mqd121-snapshot]

The public header comment describes `WaveStateOffset` as the lowest address
of saved wave data. The cited V9 through V12.1 snapshot writers instead
store `cp_hqd_wg_state_offset`, whose subtraction above makes it the
**end** offset of that data. The thunk's `HsaQueueInfo.UserContextSaveArea`
independently returns `A+T` as the wave-data base, and `SaveAreaSizeInBytes`
contains the returned used size. The comment and writer therefore cannot
both describe the field as a base. The distinction matters when decoding
the header directly. [Header comment][header-layout] ·
[Native writer][mqd12-snapshot] · [Returned base and size][thunk-query]

The multi-XCC V9 and GC12.1.0 wrappers visit every XCC header, but return
the ordinary used-size outputs for **XCC 0 only**. Other XCCs' used sizes
come from their headers at stride `S`. The thunk's `SaveAreaAllocSize` is
also per XCC; it is neither the combined allocation nor a sum of used bytes.
[Multi-XCC V9 traversal][mqd9-query-xcc] ·
[GC12.1.0 traversal][mqd121-query-xcc] · [Thunk result][thunk-query]

These are borrowed views into queue storage. Resuming the queue permits
later saves to change its state, while removal ends the storage lifetime.
An inactive-queue snapshot is not an application checkpoint that owns copies
of executable code, arguments, external buffers or private scratch.
[Area lifetime][header-layout] · [Native transition][queue-update]

## Save, resume and final ownership

The KFD update path first removes the queue from hardware, then changes its
MQD and remaps queues that remain active. CP hardware scheduling uses its
unmap/remap path; MES uses native remove/add. In the non-HWS path, the driver
explicitly requests `KFD_PREEMPT_TYPE_WAVEFRONT_SAVE` when CWSR is enabled
and `KFD_PREEMPT_TYPE_WAVEFRONT_DRAIN` otherwise. The selected direct loader
still determines the hardware request: GFX9/GFX10 explicitly select SAVE_WAVES,
while GFX7/GFX8/GFX11 map that SAVE request to their default DRAIN_PIPE case.
The [direct-consumer comparison](queue-context.md#reconfiguration-checkpoint-and-final-ownership)
keeps those branches separate from HWS/MES save and resume. [Native update][queue-update]

[Affinity and priority updates](scheduling.md#updating-a-live-queue) use this
same transition. A successful configuration change does not imply that saved
work completed or that its application-owned storage can be reused.

For CP hardware scheduling, even the scheduler's status fence is not the
whole unmap-success test. The cited MEC protocol can report the status fence
after abandoning a preemption request. KFD also checks the HIQ MQD's recorded
nonresponding queue and applies its native recovery path. A client-visible
successful transition has the native driver's full preconditions; observing
one internal fence cannot replace them. [Unmap and status checks][native-unmap]

| Resource | Final user and reuse boundary |
| --- | --- |
| Context-save backing and its debugger area | Native queue execution, save/restore and observation retain access through the queue's lifetime and successful native removal. An empty ring does not release this storage. |
| Error-signal payload named by `ErrorReason` | The queue's exception-reporting path; it remains live while that path can use the pointer. |
| Private scratch | Saved or running waves plus the queue/firmware scratch-ownership protocol. [Scratch retirement](aql/scratch.md#asynchronous-reclaim-while-the-queue-remains-usable) supplies the additional cutoff/remap rules. |
| Code, arguments and application payloads | Every unfinished producer/consumer that can resume or otherwise access them, including work on other queues. |

A complete owner flow is:

1. Obtain native topology, establish the full mapped area, initialize its
   per-XCC headers, and create the queue while all referenced storage is live.
2. Publish work using the queue's [PM4](pm4/publication.md) or
   [AQL](aql/publication.md) protocol. Native scheduling may save and resume
   the work without returning its application-owned storage.
3. Stop further publication when retiring the queue. Join all results the
   application requires using their actual completion dependencies. Queue
   inactivation or destruction alone does not establish successful results.
4. Complete native removal, then release queue-owned context storage. Release
   application resources only after all their independent users have ended.

[Thunk construction][thunk-allocation] · [Native update][queue-update] ·
[Completion and lifetime](aql/publication.md#completion-and-queue-lifetime)

The ordinary KFD destroy path drops VM-mapping queue counts, calls native
queue removal, and then releases its BO/SVM backing references. Those are
distinct reference classes. The thunk's `hsaKmtDestroyQueueCtx` frees its
context allocation only after a successful destroy ioctl. The failure path
has a different contract: KFD can discard queue bookkeeping while returning
`-ETIME` or `-EIO`, while the thunk leaves its storage allocated on an ioctl
error. Failure thus establishes neither ordinary dispatch completion nor a
resumable queue. [Native destruction][queue-destroy] ·
[Reference classes][queue-release] · [Thunk destruction][thunk-destroy] ·
[Allocation release][thunk-free]

Return to [GPU programming](README.md), [architecture and transport](architectures.md),
or the [primary source map](../sources.md).

[queue-classification]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L286-L310
[handler-selection]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device.c#L512-L575
[gc1250]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device.c#L464-L475
[create-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/linux/kfd_ioctl.h#L61-L93
[native-header]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/linux/kfd_ioctl.h#L1103-L1116
[header-layout]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/include/hsakmt/hsakmttypes.h#L707-L765
[header-packing]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/include/hsakmt/hsakmttypes.h#L87-L89
[header-initialization]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/queues.c#L569-L585
[thunk-allocation]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/queues.c#L614-L689
[native-sizing]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_queue.c#L481-L521
[native-cu-bytes]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_queue.c#L411-L462
[native-wave-count]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_queue.c#L464-L479
[gpu-page]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_gart.h#L35-L38
[topology-sizes]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_topology.c#L493-L496
[native-acquisition]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_queue.c#L276-L348
[thunk-sizing]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/queues.c#L370-L409
[thunk-targets]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/libhsakmt.h#L154-L198
[thunk-budgets]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/queues.c#L108-L179
[thunk-units]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/queues.c#L45-L55
[mqd9-init]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L217-L232
[mqd10-init]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v10.c#L130-L141
[mqd11-init]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v11.c#L170-L181
[mqd12-init]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12.c#L145-L156
[mqd121-init]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12_1.c#L206-L217
[mqd9-xcc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L735-L763
[mqd9-xcc-selection]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L1048-L1064
[mqd121-xcc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12_1.c#L451-L485
[thunk-query]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/queues.c#L980-L1015
[query-ioctl]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L573-L589
[query-preconditions]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L2985-L3013
[mqd8-snapshot]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_vi.c#L265-L286
[mqd9-snapshot]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L357-L396
[mqd10-snapshot]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v10.c#L248-L284
[mqd11-snapshot]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v11.c#L285-L320
[mqd12-snapshot]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12.c#L254-L289
[mqd121-snapshot]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12_1.c#L329-L364
[mqd9-query-xcc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L956-L994
[mqd121-query-xcc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12_1.c#L586-L624
[queue-update]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L1088-L1176
[native-unmap]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L2650-L2726
[queue-destroy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L518-L586
[queue-release]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_queue.c#L350-L406
[thunk-destroy]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/queues.c#L928-L950
[thunk-free]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/queues.c#L553-L567
