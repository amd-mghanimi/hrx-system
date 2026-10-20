# Cooperative execution and grid synchronization

A cooperative grid barrier joins workgroups within one dispatch. The shader
implements the arrival and visibility protocol; native queue admission and
resource accounting provide the scheduling conditions under which every
participant can reach that barrier. A workgroup barrier alone joins only one
workgroup. AQL barrier packets join dispatch dependencies outside the shader.
These are separate operations with separate participants.
[Shader grid barrier][ockl-grid] · [Workgroup publication](shader-memory.md) ·
[AQL dependencies](aql/barriers.md)

## Applicability and actors

This chapter follows Linux HIP/CLR → ROCr → KFD and the OCKL device-library
implementation. It distinguishes **GWS queue admission** from **hardware
Global Wave Sync instructions**: KFD can mark a queue cooperative on devices
where OCKL implements the grid barrier using memory atomics instead.
[Native GWS ownership][kfd-attach] · [Shader selection][ockl-select]

| Actor | Information and responsibility |
| --- | --- |
| HIP/CLR | Grid geometry, kernel register/LDS requirements, occupancy estimate, stream dependencies and hidden synchronization arguments. |
| ROCr | Agent capability, shared cooperative queue references, native queue construction and destruction. |
| KFD | Native GC identity, firmware admission, process/device GWS ownership and scheduling requests. |
| Packet processor and scheduler | Queue placement and execution under the native scheduling protocol. |
| Participating shaders | Workgroup-local joins, grid arrivals, barrier epochs and memory fences. |

The source revisions in the [source map](../sources.md) describe these
interfaces independently; they are not a claim that every cited component
shipped together. Windows queue admission has a different native owner.
A Linux GWS ioctl or MES field does not establish a Windows contract.

## Shared cooperative queue ownership

`HSA_QUEUE_TYPE_COOPERATIVE` requires multiproducer publication. Its public
contract permits several successful queue-creation calls to return the same
queue, requires the caller to inspect the returned size, and requires a
matching destruction for each acquired reference. Independent handles or
creation calls therefore need not represent independent scheduling resources.
[HSA queue contract][hsa-cooperative]

ROCr's `GpuAgent::QueueCreate` takes an early cooperative branch. Under a
lock, it obtains the agent's cached GWS queue, increments its reference count
and returns it. The lazy creator requires nonzero `NumGws`, creates an
internal AQL queue and calls `AqlQueue::EnableGWS(1)`. That method requests
native GWS allocation before setting the queue type to COOPERATIVE.
`HSA_AMD_AGENT_INFO_COOPERATIVE_QUEUES` reports `NumGws != 0`; the subsequent
native allocation remains fallible. [Shared creation][rocr-create] ·
[Lazy owner][rocr-owner] · [Native enable][rocr-enable] ·
[Capability query][rocr-capability]

`AqlQueue::Destroy` releases a cooperative reference through
`GpuAgent::GWSRelease`. The last reference resets the cached queue owner.
Agent teardown also destroys that queue while its signal pool and asynchronous
handlers still exist. Queue-reference release and workload completion are
different events: a borrowed kernarg record, payload or completion signal
retains its own final-use boundary. [Reference release][rocr-destroy] ·
[Owner reset][rocr-owner] · [Agent teardown][rocr-teardown] ·
[AQL lifetime](aql/README.md#publication-and-ownership)

CLR creates its internal device queue as cooperative when `IS_HIP` and
`enableCoopGroups_` are both true. `VirtualGPU::create` passes that property
to `Device::acquireQueue`, which selects `HSA_QUEUE_TYPE_COOPERATIVE`.
Its dynamic queue-release path excludes cooperative queues. Thus the
`xferQueue()` used by cooperative launch is backed by this ownership path;
its name alone does not describe a separate copy engine.
[Internal caller][clr-internal] · [Queue acquisition][clr-acquire] ·
[Type selection][clr-type] · [Transfer owner][clr-xfer] ·
[Dynamic release predicate][clr-release]

## Native GWS admission and representation

`AMDKFD_IOC_ALLOC_QUEUE_GWS` takes `kfd_ioctl_alloc_queue_gws_args`.
The structure is 16 bytes, aligned to 4 bytes; offsets below are bytes.
The UAPI describes a contiguous resource allocation, while the cited KFD
implementation attaches the device's GWS resource using a nonzero count and
detaches it using zero. It returns `first_gws = 0` after calling the queue
manager. [UAPI representation][kfd-uapi] · [Ioctl implementation][kfd-ioctl]

| Field | Offset / width | Meaning |
| --- | --- | --- |
| `queue_id` | 0 / 32 bits | Process-owned native queue identifier. |
| `num_gws` | 4 / 32 bits | UAPI resource count; nonzero selects attachment in this implementation. |
| `first_gws` | 8 / 32 bits | Returned first resource index. |
| `pad` | 12 / 32 bits | ABI padding; the thunk zero-initializes the structure. |

The thunk writes `firstGWS` only after a successful ioctl. Its native error
translation includes busy → OUT_OF_RESOURCES and unavailable device support
→ NOT_SUPPORTED. The ioctl rejects missing device GWS storage, non-HWS
scheduling, and debugger states lacking GWS support or requiring the cited
CWSR workaround. [Thunk][thunk-gws] · [Native admission][kfd-ioctl]

### Device and firmware predicates

`kfd_gws_init` first excludes `KFD_SCHED_POLICY_NO_HWS`. Without the
`hws_gws_support` override, it then requires `KFD_IS_SOC15` and **any** of
the following source predicates. These rows overlap; they are not a
normalized architecture-family table. [Initialization][kfd-init]

| Native `KFD_GC_VERSION` predicate | Additional condition |
| --- | --- |
| `== 9.0.1` | MEC2 firmware ≥ `0x81b3`. |
| `<= 9.4.0` | MEC2 firmware ≥ `0x1b3`. |
| `== 9.4.1` | MEC2 firmware ≥ `0x30`. |
| `== 9.4.2` | MEC2 firmware ≥ `0x28`. |
| `== 9.4.3`, `== 9.4.4`, or `== 9.5.0` | No additional firmware threshold in this branch. |
| `>= 10.3.0` and `< 11.0.0` | MEC2 firmware ≥ `0x6b`. |
| `>= 11.0.0` and `< 12.0.0` | Masked MES scheduler revision ≥ 68. |
| `>= 12.0.0` | No additional firmware threshold in this branch. |

For native GC ≥12.0.0, this initializer sets `gws_size` to 64 before
allocating the device resource. That value belongs to native admission;
it is not a claim that every shader on those devices executes GWS instructions.
The compiler's numeric ISA identity and native GC identity have different
namespaces. [Initialization][kfd-init] · [Architecture identities](architectures.md)

### Hardware resource versus cooperative marker

`pqm_set_gws` allows one attached queue per process/device. Attaching another
while `qpd.num_gws` is nonzero returns busy. Its two backing paths are:

| Native condition | Queue-owned state |
| --- | --- |
| GC differs from 9.4.3, 9.4.4 and 9.5.0, and MES is disabled | A process resource wrapper retains the device GWS BO. |
| GC is one of those three versions, or MES is enabled | A non-null marker records cooperative admission; no process GWS BO wrapper is created by this branch. |

The queue manager records the device's whole `gws_size`, then updates the
queue. The hardware-resource path adds the process eviction fence to the GWS
BO because AMDGPU and KFD share that resource. It is not a permanently
exclusive allocation that software can treat as ordinary addressable payload
memory. [Queue attachment][kfd-attach] · [BO ownership][gws-bo]

### What reaches the scheduler

KFD's queue update derives `is_gws` from the attached resource/marker and
updates mapped-GWS accounting. The V9 CP scheduler writer sets
`MAP_QUEUES.gws_control_queue` from `is_gws`; its process mapping supplies
`num_gws` when a GWS queue is mapped. The MES path supplies
`mes_add_queue_input.exclusively_scheduled = is_gws`.
[Queue state][kfd-update] · [CP queue mapping][cp-map-queue] ·
[CP process mapping][cp-map-process] · [MES input][mes-input]

Attachment also supplies `UPDATE_FLAG_IS_GWS` to the MQD update. In the V9
MQD manager, a supplied update record and native GC ≥9.4.2 cause
`COMPUTE_RESOURCE_LIMITS.FORCE_SIMD_DIST` to follow that flag: set when the
flag is present, cleared when absent. This predicate belongs to the V9
manager; it is not a rule for all later native GC identities. The V12 and
V12.1 MQD builders do not consume this flag. The MQD register setting and
the queue's MES scheduling field are separate emitted properties.
[Attachment update][kfd-attach] · [V9 MQD update][mqd9-gws] ·
[V12 MQD builder][mqd12] · [V12.1 MQD builder][mqd121]

AMD's April 2024 MES specification, `MES_SCH_API_ADD_QUEUE`, p. 25,
identifies `exclusively_scheduled` as the cooperative-launch flag. Its
separate `is_long_running` flag describes a queue with a long-running
compute job. GPUOpen introduces this document as an RDNA3 scheduling
overview; it supplies neither a V12 scheduling definition nor a duration
guarantee from either flag. [MES specification][mes-manual] ·
[Published scope][mes-scope]

That MES input is not serialized identically by every cited writer:

| Writer | `MESAPI__ADD_QUEUE.exclusively_scheduled` |
| --- | --- |
| `mes_v11_0_add_hw_queue` | Copies the input field into the packet. |
| `mes_v12_0_add_hw_queue` | Zero-initializes the packet and leaves this field zero. |
| `mes_v12_1_add_hw_queue` | Zero-initializes the packet and leaves this field zero. |

The V12 API layout contains the field. Its presence in the layout and KFD's
input assignment do not establish that the V12 writers transmit it. These
sources establish the differing packet contents; they do not explain whether
a particular firmware derives equivalent scheduling through another field
or implements a different policy. Consequently, the V11 path cannot supply
an unqualified exclusivity guarantee for V12 firmware.
[V11 writer][mes11] · [V12 writer][mes12] · [V12.1 writer][mes121] ·
[V12 API layout][mes12-layout]

The common KFD caller zero-initializes `mes_add_queue_input` and leaves
`gws_base` and `gws_size` zero. The V12/V12.1 writers copy those zero values;
the device's GWS admission count does not implicitly reach these fields.
They therefore do not distinguish a cooperatively attached queue in this
path. [Common KFD input][mes-input] · [V12 writer][mes12] ·
[V12.1 writer][mes121]

Attachment is a native queue transition, outside the shader barrier's hot
path. The cited queue manager changes GWS ownership/accounting before its
fallible queue update; the operation does not promise rollback to the prior
state on every error. Queue destruction removes the queue through the native
scheduler, then releases its resource wrapper or marker accounting.
[Attachment transition][kfd-attach] · [Queue destruction][kfd-destroy] ·
[Resource cleanup][kfd-cleanup]

### MES coordination across XCCs

MES V12.1 also has a partition-level mode named `mes_coop_mode`. In AMD's
cited native driver, unified-MES initialization calls
`mes_v12_1_setup_coop_mode` before publishing hardware resources. For more
than one XCC, SPX, DPX and QPX modes enable coordination and select a master
XCC for each partition; CPX disables it. A single-XCC device returns without
enabling the mode. The selection consumes partition topology, independently
of a queue's `is_gws` property. [Initialization][mes121-init] ·
[Partition selection][mes121-coop-mode]

For this mode, the driver sets `SET_HW_RSRC_1.mes_coop_mode` and publishes
`coop_sch_shared_mc_addr` as a command buffer shared by the master and
subordinate scheduler instances. With coordination enabled, it publishes
these fields for the scheduler pipe; the KIQ pipe additionally requires
masked KIQ firmware version ≥`0x74`. Queue addition and removal
route through the selected master MES. These are scheduler-group resources
and routing rules; they supply no per-queue cooperative-grid marker in the
cited writers. [Resource layout][mes121-coop-layout] ·
[Resource publication][mes121-coop-resources] ·
[Queue addition and removal][mes121-coop-routing]

## Occupancy and participating workgroups

HIP's cooperative launch checks device support and rejects a grid exceeding
its computed `max_blocks_per_grid` with `hipErrorCooperativeLaunchTooLarge`.
The occupancy helper uses the actual kernel's wave width, VGPR and SGPR
allocation, trap-handler SGPR reserve, static plus dynamic LDS, and CU/WGP
execution mode. LDS is rounded to the ISA allocation granule. Counts expressed
per CU and per WGP are converted before combining them.
[Launch admission][hip-admission] · [Occupancy calculation][hip-occupancy]

This is runtime admission based on a resource model. It neither defines a
hardware latency bound nor turns an arbitrary spin barrier into a progress
guarantee. A barrier participant that cannot be scheduled cannot contribute
its arrival. All participating workgroups must reach corresponding barrier
phases, and the scheduling contract must permit their progress while earlier
arrivals wait. [Grid algorithm][ockl-atomic] ·
[Affinity versus progress](scheduling.md#construction-execution-and-final-use)

### Cooperative CU count policy

CLR queries `HSA_AMD_AGENT_INFO_COOPERATIVE_COMPUTE_UNIT_COUNT` in HIP mode
and divides it by two when its device settings use WGP units. ROCr normally
returns the ordinary compute-unit count. Two additional policies apply only
when `HSA_COOP_CU_COUNT=1`:

- A nonempty agent `HSA_CU_MASK` makes the cooperative count zero.
- ISA 9.0.10 uses `floor(ordinary_count / 8) * 8 - 8`.

The flag is false unless explicitly set to `1` in this revision. Thus the
special count is not an unconditional property of that ISA, nor a general
formula for deriving occupancy from an affinity mask.
[CLR count units][clr-count] · [ROCr count policy][rocr-count] ·
[Flag parsing][rocr-count-flag]

## Hidden grid synchronization record

CLR's `MGSyncInfo` and OCKL's `mg_info` describe the same 64-bit ABI record.
Its size is 48 bytes and natural alignment is 8 bytes. `MGSyncData` /
`mg_sync` contains two 32-bit words, `w0` and `w1`, and occupies 8 bytes.
[Host representation][clr-layout] · [Device representation][ockl-layout] ·
[Rank and size consumers][ockl-ranks]

| Field | Byte offset / size | Meaning |
| --- | --- | --- |
| `mgs` | 0 / 8 | Device-visible pointer to the shared multi-grid arrival state; null for the single-grid allocation. |
| `grid_id` | 8 / 4 | This grid's index in the multi-grid group. |
| `num_grids` | 12 / 4 | Number of participating grids. |
| `prev_sum` | 16 / 8 | Prefix work-item count used for the multi-grid thread rank. |
| `all_sum` | 24 / 8 | Total work-item count used for the multi-grid size. |
| `sgs` | 32 / 8 | This grid's arrival state. |
| `num_wg` | 40 / 4 | Number of workgroups contributing to the local grid barrier. |
| Tail padding | 44 / 4 | Natural structure padding. |

The 64-bit `prev_sum` and `all_sum` fields do not imply 64-bit query results:
the cited `__ockl_multi_grid_thread_rank` and `__ockl_multi_grid_size` helpers
return 32-bit `uint`. [Rank and size consumers][ockl-ranks]

For a single cooperative grid, CLR allocates this record from its kernarg
storage with 64-byte alignment, clears `sgs`, sets `mgs` to null, fills the
geometry fields and writes the pointer at the kernel metadata's hidden
argument offset. The 64-byte allocation alignment is a caller choice,
distinct from the record's natural alignment. OCKL obtains the pointer from
implicit-argument `size_t` slot 6 for `__oclc_ABI_version < 500`, or slot 11
otherwise: byte offsets 48 and 88 in the 64-bit ABI.
[Hidden-argument caller][clr-hidden] · [Device argument selection][ockl-layout]

The record, its pointer and any referenced multi-grid state remain live until
their last participating shader finishes. AQL ring consumption does not
release them. The single-grid validity helper checks only whether the hidden
pointer is nonzero; it does not validate resource admission or scheduler
progress. [Validity helper][ockl-valid] · [Argument lifetime](aql/dispatch.md)

## Shader arrival, visibility and release

OCKL's full-grid barrier uses a hardware GWS barrier except when its numeric
ISA is exactly `9402`, exactly `9500`, or at least `11000`. Those cases use
the resident atomic barrier. CLR independently disables its GWS initialization
kernel for ISA major >10 or major 9 with minor ≥4. The predicates have
different extents and remain separate source policies.
[Shader selection][ockl-select] · [Initialization selection][clr-gws-select]

On the GWS path, CLR's initialization kernel executes
`ds_gws_init(workgroup_count - 1, 0)`. One leader per workgroup then executes
`ds_gws_barrier(workgroup_count - 1, 0)` at each full-grid barrier. Resource
index 0 is this caller's choice; the instruction wrapper's first operand is
a count minus one. [Initialization dispatch][clr-gws-init] ·
[Initialization shader][clr-gws-shader] · [Grid barrier][ockl-grid]

### Resident atomic barrier

The single-grid algorithm uses `sgs.w0`, with a 16-bit arrival count in bits
15:0 and a 16-bit phase in bits 31:16. It assumes at most 65,535 workgroups.
Each workgroup leader atomically adds 1 at device scope. The last arrival adds
`0x10000 - num_wg`, clearing the arrival count and advancing the phase.
Waiters retain the old phase and reload until it changes, using `s_sleep(1)`
between unsuccessful observations. `w1` remains part of the record but is
not used by this algorithm. [Arrival and wait][ockl-atomic]

Those atomics are relaxed. Payload visibility comes from the surrounding
fences and workgroup joins:

| Phase | Participating work-items | Operation |
| --- | --- | --- |
| Join local producers | All work-items | Workgroup release fence, then `s_barrier`. |
| Publish the group | Work-item `(0,0,0)` in each group | Workgroup acquire fence, then agent release fence. |
| Join grid | One leader per workgroup | GWS barrier or the device-scope atomic arrival/wait. |
| Acquire the grid | Each workgroup leader | Agent acquire fence, then workgroup release fence. |
| Release local consumers | All work-items | `s_barrier`, then workgroup acquire fence. |

[Full-grid fence composition][ockl-grid] · [Leader election][ockl-leaders]

The hardware GWS operation is therefore not used as a substitute for the
shader memory model. The same outer fence composition surrounds both grid
algorithms. Cache instructions implementing those fences depend on ISA,
memory space and coherence properties described in
[shader memory publication](shader-memory.md).

### Split arrival and wait

`__ockl_grid_bar_arrive` and `__ockl_grid_bar_wait` use the atomic algorithm
regardless of `AVOID_GWS()`. Arrival performs the local producer join and
agent release; the elected leader returns the old phase token. Wait consumes
that leader's token, acquires the agent scope and releases local consumers
through the workgroup barrier. Other work-items return zero from arrival,
and their tokens are not consumed by the leader-only wait path.
[Split-phase implementation][ockl-split]

Separating these operations permits work between arrival and wait when that
work does not depend on completion of the current phase. Participation,
token ownership and memory dependencies still apply. The representation
does not authorize reinitializing or recycling the record while participants
from that dispatch remain live.

### Multiple GPU grids

OCKL composes two local grid barriers around a barrier joined by one leader
per grid. The grid leader promotes visibility from agent to system scope
before arriving and acquires system visibility before releasing back to its
agent. Its shared `mgs->w0` uses an 8-bit arrival count and 24-bit phase;
the algorithm assumes at most 255 grids and uses
`memory_scope_all_svm_devices` atomics. [Multi-grid composition][ockl-multi] ·
[Shared arrival algorithm][ockl-atomic-multi]

CLR requests fine-grained SVM storage with atomics for the multi-device
records. Each device record contains an 8-byte shared-state slot followed by
its 48-byte grid record, a 56-byte stride. The hidden-argument caller points
`mgs` at the selected first device's shared-state slot. These backing and
access conditions are part of the protocol: system fence scope alone cannot
make inaccessible memory reachable or supply unsupported cross-device
atomics. [Storage request][clr-multi-storage] · [Record stride][clr-stride] ·
[Hidden pointers][clr-hidden] ·
[Directed peer memory](recipes/host-device.md#peer-gpu-handoff)

## Complete cooperative launch flow

HIP/CLR supplies a concrete composition of queue ownership, admission and
shader execution:

1. Query support, account for the compiled kernel's actual resource use,
   and admit a grid that fits the runtime's cooperative occupancy model.
2. Obtain the shared cooperative queue. Allocate and initialize the hidden
   grid record and publish its arguments under the dispatch memory contract.
3. Emit a fence on the original stream's queue and add its last signal as
   an external dependency of the cooperative queue. CLR calls
   `releaseGpuMemoryFence(kSkipCpuWait)`, which emits the needed queue barrier
   without executing that helper's CPU wait.
4. When its settings select GWS initialization, submit that initialization
   before the cooperative kernel. The kernel executes the selected grid
   barrier protocol while its workgroups remain participants.
5. Emit the cooperative queue's outgoing fence and transfer its last signal
   back to the original queue as an external dependency. Subsequent work
   inherits this dependency; the host need not relay each grid barrier.
6. Observe actual final workload completion before reusing arguments, barrier
   state and payloads. Release the matching cooperative queue reference when
   its owner is finished. Final native queue removal and resource cleanup
   follow the shared owner, rather than every individual launch.

[Launch admission][hip-admission] · [Submission sequence][clr-submit] ·
[Fence helper][clr-fence] · [Hidden state][clr-hidden] ·
[Queue release][clr-destroy] · [Shared final owner][rocr-owner]

For persistent programs, this separates three costs and contracts: native
cooperative admission, per-dispatch dependency and argument publication,
and each in-shader barrier phase. Repeating the shader barrier does not
repeat the GWS allocation ioctl. Conversely, an occupancy check, a high
queue priority or a disjoint CU mask does not establish simultaneous progress
of unrelated persistent queues. Such a design needs the scheduling and
memory contracts of every participating queue and device.

[ockl-select]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/amd/device-libs/ockl/src/cg.cl#L11-L14
[ockl-layout]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/amd/device-libs/ockl/src/cg.cl#L16-L41
[ockl-ranks]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/amd/device-libs/ockl/src/cg.cl#L168-L192
[ockl-leaders]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/amd/device-libs/ockl/src/cg.cl#L43-L55
[ockl-grid]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/amd/device-libs/ockl/src/cg.cl#L143-L166
[ockl-atomic]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/amd/device-libs/ockl/src/cg.cl#L57-L79
[ockl-atomic-multi]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/amd/device-libs/ockl/src/cg.cl#L81-L94
[ockl-valid]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/amd/device-libs/ockl/src/cg.cl#L108-L112
[ockl-split]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/amd/device-libs/ockl/src/cg.cl#L114-L140
[ockl-multi]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/amd/device-libs/ockl/src/cg.cl#L206-L248
[hsa-cooperative]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L2272-L2297
[rocr-create]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2742-L2756
[rocr-owner]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1072-L1094
[rocr-enable]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L2082-L2088
[rocr-capability]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2563-L2565
[rocr-destroy]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L413-L422
[rocr-teardown]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1100-L1135
[rocr-count]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2592-L2610
[rocr-count-flag]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/util/flag.h#L278-L282
[thunk-gws]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/queues.c#L1068-L1096
[clr-internal]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocdevice.cpp#L1882-L1911
[clr-acquire]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L2524-L2530
[clr-type]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocdevice.cpp#L3471-L3477
[clr-xfer]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocdevice.cpp#L3218-L3237
[clr-release]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L2736-L2761
[clr-count]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocdevice.cpp#L1119-L1140
[clr-layout]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/device.hpp#L1721-L1735
[clr-stride]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/device.hpp#L1777-L1779
[clr-hidden]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L4831-L4860
[clr-gws-select]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocsettings.cpp#L182-L186
[clr-gws-init]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocblit.cpp#L3822-L3847
[clr-gws-shader]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/blitcl.cpp#L278-L287
[clr-multi-storage]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocdevice.cpp#L555-L564
[clr-submit]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L5171-L5227
[clr-fence]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L2347-L2365
[clr-destroy]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocdevice.cpp#L3678-L3716
[hip-admission]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/hipamd/src/hip_module.cpp#L369-L386
[hip-occupancy]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/hipamd/src/hip_platform.cpp#L23-L140
[kfd-attach]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L104-L166
[kfd-uapi]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/linux/kfd_ioctl.h#L492-L504
[kfd-ioctl]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L1486-L1529
[kfd-init]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device.c#L577-L611
[gws-bo]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c#L3145-L3206
[kfd-update]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L1130-L1160
[cp-map-queue]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_packet_manager_v9.c#L227-L247
[cp-map-process]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_packet_manager_v9.c#L32-L64
[mes-input]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L207-L281
[mqd9-gws]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L334-L341
[mqd12]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12.c#L183-L245
[mqd121]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12_1.c#L239-L322
[mes11]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/mes_v11_0.c#L321-L379
[mes12]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/mes_v12_0.c#L306-L361
[mes121]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/mes_v12_1.c#L289-L348
[mes12-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/mes_v12_api_def.h#L357-L388
[mes121-init]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdgpu/mes_v12_1.c#L1821-L1833
[mes121-coop-mode]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdgpu/mes_v12_1.c#L1872-L1909
[mes121-coop-layout]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/include/mes_v12_api_def.h#L314-L336
[mes121-coop-resources]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdgpu/mes_v12_1.c#L655-L682
[mes121-coop-routing]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdgpu/mes_v12_1.c#L289-L373
[mes-manual]: https://gpuopen.com/download/documentation/micro_engine_scheduler.pdf#page=25
[mes-scope]: https://gpuopen.com/amd-gpu-architecture-programming-documentation/
[kfd-destroy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L557-L586
[kfd-cleanup]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L188-L217
