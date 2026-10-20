# Scratch storage and reclamation

Scratch is the memory backing each workitem's private variables, spills and
call frames. An AQL dispatch supplies its private-byte requirement; the AMD
command processor (CP) combines that requirement with queue-owned backing and
the compiled kernel's scratch ABI. A dispatch can finish while its scratch
allocation remains assigned to the queue for later dispatches. Reclamation
therefore transfers ownership through a firmware protocol, rather than merely
observing one kernel's completion. [Private address space][llvm-private] ·
[Dispatch requirement][packet] · [Allocation ownership][allocation-policy]

## Applicability and participants

The queue extension and protocols below follow ROCr at
`f9ba16bbe70e365b2f59b268e847bef19ad9db6e`, with its Linux KFD transport.
The compiler owns private-object layout and initial-register requirements;
ROCr owns backing allocations, descriptors and signal handlers; CP firmware
selects scratch for dispatches and reports reclamation events. The native
driver supplies queue mapping and removal. This AQL protocol is distinct from
programming scratch registers in a [raw PM4 dispatch](../pm4/dispatch.md).
[Compiler initialization][llvm-initialization] · [Queue owner][queue-init] ·
[Native queue adapter][driver-queue]

ROCr selects asynchronous reclaim with the following **ISA-version and CP
microcode predicates**. These are runtime admission rules, not substitutes for
the negotiated queue capabilities or a list of every firmware implementation.
`HSA_ENABLE_SCRATCH_ASYNC_RECLAIM=0` disables this selection for all rows.
[Selection predicate][async-support] · [Configuration][flags]

| ISA version reported to ROCr | Additional selection condition |
| --- | --- |
| Major 9, minor 4, including gfx942 | `EngineId.ui32.uCode >= 177`. |
| Major 9, minor 5, including gfx950 | `EngineId.ui32.uCode >= 24`. |
| Major 12, minor at least 5 | No additional microcode comparison in this predicate. |
| Other versions, including GFX11, GFX11.5 and GFX12.0 | Asynchronous reclaim is not selected by this predicate. Ordinary insufficient-scratch and single-use handling are separate paths. |

The `AMD_QUEUE_CAPS_CP_ASYNC_RECLAIM` bit is written by firmware at queue
connection. ROCr predicts support from the predicate above before that first
connection. `AMD_QUEUE_CAPS_SW_ASYNC_RECLAIM` tells firmware that the extended
queue storage exists and may be accessed. Both sides of the protocol matter;
setting the software bit cannot add a firmware capability. [Capability
definitions][queue-abi] · [Connection protocol][async-protocol]

## Queue representation

For the 64-bit HSA machine model, `amd_queue_t` occupies 256 bytes and is
64-byte aligned. `amd_queue_v2_t` preserves that prefix, uses previously
reserved fields, and appends 128 pairs of per-XCC indices, making its size
2,304 bytes with the same alignment. The header's V2 comment saying that the
original structure is 64 bytes conflicts with its declarations: 64 is the
alignment, not the prefix extent. [HSA queue prefix][hsa-queue] · [AMD queue
declarations][queue-abi]

The scratch-related fields below are offsets from the start of
`amd_queue_v2_t`; widths are in bits. The dispatch indices count packets, not
bytes or DWORDs. [Queue fields][queue-abi]

| Byte offset | Width | Field and meaning |
| --- | --- | --- |
| 40 | 32 | `caps`: bit 0 is `AMD_QUEUE_CAPS_CP_ASYNC_RECLAIM`; bit 1 is `AMD_QUEUE_CAPS_SW_ASYNC_RECLAIM`. |
| 56 | 64 | `write_dispatch_id`, the producer reservation index. |
| 128 | 64 | `read_dispatch_id`, the queue read index; its scratch-reclaim use requires the protocol below. |
| 140 | 32 | `compute_tmpring_size`, containing the target's `COMPUTE_TMPRING_SIZE` encoding. |
| 144 | 4 × 32 | `scratch_resource_descriptor[4]`, the main scratch buffer resource descriptor. |
| 160 | 64 | `scratch_backing_memory_location`, the queue scratch address or legacy process-relative offset. |
| 168 | 64 | `scratch_backing_memory_byte_size`, declared in V2; the cited descriptor builder does not populate it. |
| 176 | 32 | `scratch_wave64_lane_byte_size`, normalized bytes per lane for a 64-lane wave. |
| 180 | 32 | `queue_properties`: bit 4, `AMD_QUEUE_PROPERTIES_USE_SCRATCH_ONCE`, selects the single-use return handshake. |
| 184 | 64 | `scratch_max_use_index`, the last dispatch index permitted to use main scratch. |
| 192 | 64 | `queue_inactive_signal`, an HSA signal handle carrying firmware requests and acknowledgments. |
| 200 | 64 | `alt_scratch_max_use_index`, the alternate allocation's cutoff. |
| 208 | 4 × 32 | `alt_scratch_resource_descriptor[4]`, declared in V2; the cited descriptor builder does not populate it. |
| 224 | 64 | `alt_scratch_backing_memory_location`, the alternate backing address. |
| 232, 236, 240 | 32 each | `alt_scratch_dispatch_limit_x`, `alt_scratch_dispatch_limit_y`, `alt_scratch_dispatch_limit_z`, alternate allocation grid limits in workitems. |
| 244 | 32 | `alt_scratch_wave64_lane_byte_size`, the alternate normalized per-lane size. |
| 248 | 32 | `alt_compute_tmpring_size`, the alternate ring-size encoding. |
| 256 + 16 × XCC | 64 | `scratch_last_used_index[XCC].main`, last main-scratch dispatch reported by this XCC. |
| 264 + 16 × XCC | 64 | `scratch_last_used_index[XCC].alt`, the corresponding alternate-scratch dispatch. |

The two cutoff fields start at `UINT64_MAX`. The runtime reduces them during
reclamation and restores the selected allocation's cutoff after satisfying a
new request. Firmware's per-XCC records are separate from these runtime-owned
cutoffs. [Initialization][queue-init] · [Main allocation publication][main-install]

The descriptor builder writes the backing locations, normalized sizes and
alternate grid limits, but a declared field alone does not establish that it
carries a live value. In particular, the main byte-size field and alternate
resource-descriptor array above are not populated by `InitScratchSRD` and its
helpers. The builder selects different main resource-descriptor layouts for
GFX10, GFX11 and GFX12. [Descriptor construction][srd-builders] · [Layout
selection and final fields][srd-init]

## Wave size, physical slots and per-XCC backing

For an insufficient-scratch request, let `P` be packet private bytes per
workitem and `L` the requested wave width, 32 or 64 lanes. ROCr rounds `P` to
`A / L` bytes, where `A` is the per-wave alignment: 1,024 bytes before ISA
major 11 and 256 bytes for major 11 or later. Thus `B = align_up(P, A / L) × L`
is the aligned backing per wave. This alignment belongs to scratch layout;
the allocation is independently rounded to native pages. [Alignment
selection][queue-alignment] · [Request sizing][request-sizing] · [Page
rounding][allocation-policy]

`scratch_wave64_lane_byte_size` stores `B / 64`, even for a wave32 allocation.
For example, a GFX11 wave32 request of 17 bytes per workitem rounds to 24 bytes
per workitem, uses 768 bytes per wave, and writes 12 into that queue field.
Its `WAVESIZE` value is three 256-byte units. Reading the field as unconditionally
equal to packet private bytes would mis-size wave32 backing. [Normalization][srd-init]

The following encodings are selected by ROCr's main descriptor builder.
`S` denotes allocated main bytes, `X` is `NumXcc`, `E` is `NumShaderBanks`,
and `C` is `NumFComputeCores / NumSIMDPerCU`. Each `WAVES` field is bits 11:0.
The builder writes zero when no main scratch is allocated.
The remaining high bits of each register layout are reserved and zeroed by
the builder.
[Register layouts][tmpring-layout] · [Legacy builder][tmpring-legacy] ·
[GFX11/GFX12 builders][tmpring-rdna]

| Builder selection | `COMPUTE_TMPRING_SIZE.WAVESIZE` | `WAVES` calculation before its physical-slot clamp |
| --- | --- | --- |
| Major 10 and the default branch, including GFX9 | Bits 24:12, in 1,024-byte units for these targets | `floor((S / X) / B)`, per XCC. The builder asserts divisibility by `E / X`. |
| Major 11 | Bits 26:12, in 256-byte units | `floor(floor(S / B) / E)`, per shader engine. |
| Major 12 | Bits 29:12, in 256-byte units | The same per-engine calculation as GFX11, with the wider `WAVESIZE` field. |

The legacy builder clamps to `C × MaxSlotsScratchCU`; the GFX11/GFX12 builders
clamp to `(C / X) × MaxSlotsScratchCU`. Their count units therefore cannot be
interchanged by copying a register word between targets. Resource-descriptor
DWORD 2 receives `uint32_t(S / X)`, the per-XCC size, rather than the total
allocation. `scratch_backing_memory_location` is the allocation address for
ISA major greater than 8; older targets use its offset from the process's
scratch pool. [Per-XCC descriptor][srd-size] · [Backing address selection][allocation-address]

Retained main scratch covers **every physical scratch slot** because firmware
may reuse it for later dispatches. ROCr's growth path calculates
`device_slots = align_up(C, E) × MaxSlotsScratchCU`, accounting for asymmetric
CU harvest. Its requested retained size is `B × device_slots`, even if the
current dispatch has very few workgroups. [Retained ownership][allocation-policy]
· [Physical slot calculation][request-sizing]

### Dispatch-sized backing

Single-use scratch instead uses a dispatch occupancy bound. That bound is not
simply the grid's wave count: ROCr accounts for distribution across shader
engines and asymmetric CU harvest. With `W` workitems per workgroup, `G` the
product of `ceil(grid_axis / workgroup_axis)`, and `E` and `C` as above, the
source computes:

```text
waves_per_group = ceil(W / L)
asymmetric_cus = C - align_down(C, E)
rounds = floor(G / C)
symmetric_groups = G - rounds × asymmetric_cus
groups_per_engine = ceil(symmetric_groups / E)
                    + (asymmetric_cus != 0 ? rounds : 0)
```

For ISA major at least 10, when `groups_per_engine < 16` and
`W × groups_per_engine < 256`, it raises that count to
`min(ceil(256 / W), 16)`. It then computes
`dispatch_slots = min(groups_per_engine × E × waves_per_group, device_slots)`
and requests `B × dispatch_slots` bytes before page rounding. These are the
runtime's allocation formulas, not a promise of measured occupancy or a
different launch geometry. [Dispatch bound][request-sizing]

ROCr additionally checks an allocator limit using
`align_up(bytes_per_workitem × WaveFrontSize, 1024)`, where `WaveFrontSize`
comes from agent properties. The limit is 8,387,584 bytes below ISA major 12
and 67,106,816 bytes from major 12. This check has different inputs and units
from the wave32 normalization and target register field width; the numeric
constants, rather than nearby size comments, establish this runtime limit.
[Limit constants][wave-limits] · [Selection][wave-limit-selection] ·
[Allocator check][allocation-policy]

## Allocation policy and ordinary growth

An ordinary retained-versus-single-use decision compares the full physical
slot request against `use_once_limit`. At this revision,
`HSA_SCRATCH_SINGLE_LIMIT` defaults to **140 MiB**. Without asynchronous reclaim,
ROCr also selects single-use when currently bound pool bytes plus the new
request would exceed one eighth of the scratch pool. The latter calculation
subtracts free cache blocks. These are memory-pressure policies, not scratch
hardware limits.
[Default constant][default-limits] · [Classification][allocation-policy]

When asynchronous reclaim is selected, the default threshold is **3 GiB times
`NumXcc`**. A nonzero explicit `HSA_SCRATCH_SINGLE_LIMIT_ASYNC` replaces that
total directly; zero selects the computed default. `MaxScratchDevice` supplies
a separate runtime aperture bound of 4 GiB per XCC below ISA major 12 and
8 GiB per XCC from major 12. The initial async threshold and the API setter
have different validation paths: the setter rejects values beyond this bound.
[Environment parsing][flags] · [Async threshold initialization][threshold-init]
· [Aperture bound][async-support] · [Threshold update][threshold-update]

`hsa_amd_agent_set_async_scratch_limit` applies a new byte threshold to the
agent's queues. Increasing it does not immediately allocate more scratch;
decreasing it reclaims allocations exceeding the new limit and can block
until their current dispatches complete. Its API entry rejects an agent for
which asynchronous reclaim is not enabled. A zero API threshold is an actual
new limit, unlike the zero environment value used during initialization.
[API contract][threshold-api] · [Entry conditions][threshold-entry] ·
[Applying the limit][check-limits]

`HSA_NO_SCRATCH_RECLAIM` or ISA major below 8 forces retained classification in
`AcquireQueueMainScratch` and disables that allocator's occupancy-reduction
path. This selection is separate from `AsyncScratchReclaimEnabled`; the flag
is not itself a global prohibition in every reclamation function.
[Allocator selection][allocation-policy] · [Async selection][async-support]

The ordinary firmware-request path is:

1. CP reports insufficient scratch through `queue_inactive_signal`. ROCr
   recognizes mask `0x401`; bit `0x400` selects wave32, otherwise the handler
   uses wave64. It finds the dispatch that needs scratch by scanning from the
   read index because hardware end-of-pipe handling can leave that index
   behind the requesting packet. [Event handling][scratch-events] ·
   [Packet lookup][request-packet]
2. Under its scratch lock, the handler derives the sizes above and releases
   its previous main allocation before acquiring a replacement. The allocator
   tries reusable cache blocks, allocation and residency, cache trimming, and
   a reserved block. Live single-use allocations can cause a deferred retry
   notified when space returns. [Replacement][main-install] · [Allocation
   attempts][allocation-attempts]
3. If permitted, memory pressure can reduce the wave occupancy bound while
   preserving the allocator's workgroup and per-XCC shader-engine constraints.
   That result is single-use. Cooperative queues, disabled reclaim, and
   `HSA_NO_SCRATCH_THREAD_LIMITER` prevent this occupancy-reduction path.
   [Reduction conditions and loop][occupancy-reduction]
4. The handler installs descriptors and size fields, sets the single-use bit
   when selected, restores `scratch_max_use_index = UINT64_MAX`, and performs
   `hsa_signal_store_screlease(queue_inactive_signal, 0)` to restart processing.
   If neither allocation exists after recovery, the event handler reports
   `HSA_STATUS_ERROR_OUT_OF_RESOURCES`. [Publication][main-install] ·
   [Terminal allocation failure][scratch-events]

The queue's normal execution reuses sufficiently sized retained backing
without this allocation callback. Thus fixed backing consumes capacity, while
growth and single-use return introduce host-handler work. The source explains
those additional operations; it does not supply a universal latency for them.
[Allocation rationale][allocation-policy] · [Firmware dispatch model][async-protocol]

## Single-use return and cache ownership

With `AMD_QUEUE_PROPERTIES_USE_SCRATCH_ONCE` set, firmware surrenders main
scratch after one dispatch. ROCr recognizes the exact inactive-signal value
`512` (`0x200`) as that return. It releases the allocation, zeroes the main
scratch state, rebuilds the descriptor, stores zero to the inactive signal
with relaxed ordering, then release-stores `queue_properties` with the
single-use bit cleared. That final property update resumes queue processing.
It is a different acknowledgment from the release store to the inactive
signal used for an allocation request. [Single-use handler][scratch-events]

For ISA major 8 with CP microcode below 729, the single-use allocation path
also strengthens the dispatch packet's release fence to SYSTEM to flush
scratch stores on the affected firmware. This exact predicate belongs to
scratch reclamation; it does not imply that every newer dispatch can omit its
payload publication fence. [Firmware workaround][main-install]

Returning an allocation to ROCr's scratch cache does not necessarily release
physical backing. A free cache block can satisfy a later request. A block
marked for trimming is deallocated when its owner returns it; trimming active
blocks marks that intent rather than freeing storage underneath firmware.
The native deallocator makes base-profile storage nonresident, returns its
pool extent, and notifies waiters. [Cache return][scratch-cache] · [Cache
trimming][scratch-trim] · [Native release][scratch-release]

## Asynchronous reclaim while the queue remains usable

The public source describes CP's side as protocol pseudocode and implements
the runtime side in `AsyncReclaimMainScratch`. Its essential invariant is
**prevent future use of an allocation, then wait out its recorded last use**.
A queue disconnect publishes firmware's per-XCC last-use records; reconnect
causes firmware to reread the queue scratch fields. Editing the memory while
the queue remains connected does not update CP's cached copy under this
protocol. [Connection and disconnect model][async-protocol]

For main scratch, ROCr performs this sequence under the same scratch lock
that serializes growth:

1. Suspend the queue through a native update with queue percentage zero.
2. Compute the maximum `scratch_last_used_index[XCC].main` over the agent's
   `NumXcc` records and store it in `scratch_max_use_index`.
3. Resume through a native update with percentage 100. Firmware's new cutoff
   prevents later packets from using the old main allocation. A later packet
   needing scratch can request another allocation.
4. Wait while `scratch_max_use_index >= read_dispatch_id`. The runtime's
   protocol associates this condition with outstanding use of that scratch.
5. Release main backing and clear its size and descriptor state. A pending
   growth handler can then acquire the lock and install new backing.

[Reclaim implementation][async-main] · [Suspension and resumption][suspend-resume]
· [Clearing main state][free-main]

This read-index check has the **additional firmware scratch-use contract**
above. The ordinary [AQL publication contract](publication.md) only makes the
read index a ring-slot reuse boundary; it does not independently establish
kernel completion, payload visibility, or permission to free scratch on an
arbitrary queue. A cutoff without the disconnect/reconnect exchange omits the
mechanism that makes firmware observe it. [Protocol and runtime check][async-main]
· [Firmware cached state][async-protocol]

The Linux transport passes the percentage update through
`hsaKmtUpdateQueue` to `AMDKFD_IOC_UPDATE_QUEUE`. KFD unmaps/removes the queue
before changing its descriptor and subsequently remaps active queues; its
active predicate includes a nonzero percentage. Queue unmapping can preserve
unfinished waves through context save, so suspension alone is not dispatch
completion. The later resumption and last-use wait retain a purpose even
after a successful unmap. [User-mode transport][update-ioctl] · [KFD update
ordering][kfd-update] · [Active predicate][kfd-active]

The [context-save ownership model](../context-save.md#save-resume-and-final-ownership)
traces the separate save-area backing and the resources that suspended waves
still need. Saving registers and LDS leaves private scratch under its own
firmware and dispatch lifetime.

Successful native transitions are premises of that sequence. The cited
runtime's suspend/resume helpers assert native success but do not propagate
failure. Their return alone therefore cannot prove that a failed native
operation disconnected the queue or installed a new cutoff.
[Runtime error handling][suspend-resume] · [Fallible native adapter][driver-queue]

The reclaim caller can be an ordinary memory allocation. On
`HSAKMT_STATUS_NO_MEMORY`, the KFD allocation path invokes the owning agent's
`Trim` and retries; GPU trimming asks all its AQL queues to reclaim scratch
before trimming free cache blocks. Consequently, although the protocol is
named asynchronous reclaim, the host caller can wait for a running
scratch-using dispatch. It does not permit moving or freeing the private state
of an unfinished persistent kernel. [Allocation caller][allocation-trim] ·
[Agent trim][agent-trim] · [Queue traversal][threshold-update] ·
[Last-use wait][async-main]

### Alternate scratch

V2 has an independent alternate location, size, cutoff and per-XCC use index.
Its allocation branch chooses dispatch-sized backing only when
`dispatch_size < use_alt_limit` and `dispatch_slots < device_slots`, retaining
the requesting grid's X/Y/Z limits. Its successful allocation and reclamation
use the corresponding alternate fields. [Alternate allocation][alt-install]
· [Alternate reclaim][async-alt]

At the cited revision, however, configuration unconditionally sets
`enable_scratch_alt_ = false`, even after reading `HSA_ENABLE_SCRATCH_ALT`.
The source attributes this to debugger support. `use_alt_limit` is therefore
zero in that configuration; the candidate branch is not selected. Its ABI
presence does not establish an enabled dual-allocation deployment. The source
also builds alternate `COMPUTE_TMPRING_SIZE` only in the default target branch,
so the main GFX11/GFX12 layouts do not by themselves specify alternate setup
for those targets. [Explicit disable][flags] · [Limit selection][queue-alignment]
· [Builder selection][srd-init]

## Final queue teardown

Queue destruction first unregisters the queue from the agent's collection and
synchronously terminates its scratch/error callbacks. It then inactivates the
native queue before returning main and alternate backing, destroying signals,
and freeing the ring. This sequence separates firmware access, host-handler
access, and cached-allocation ownership. A completion signal for the most
recent dispatch does not join all three actors. [Queue destruction][queue-destroy]

`Inactivate` invokes native queue destruction and an acquire fence, but at
this revision it only asserts the native return code and returns success.
As with suspend/resume, successful native removal is an explicit premise of
the storage-release sequence, not something that this wrapper's return value
independently establishes after a driver failure. [Inactivation][suspend-resume]
· [Native destruction adapter][driver-queue]

Return to [AQL](README.md), [kernel dispatch](dispatch.md), or
[barriers and signals](barriers.md).

[llvm-private]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L6068-L6106
[llvm-initialization]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L7407-L7424
[packet]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L3030-L3050
[hsa-queue]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L2327-L2382
[queue-abi]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/amd_hsa_queue.h#L49-L153
[async-support]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_gpu_agent.h#L485-L524
[queue-alignment]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L200-L224
[queue-init]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L298-L325
[srd-builders]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1764-L1892
[srd-size]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1805-L1814
[srd-init]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L2029-L2077
[tmpring-layout]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/registers.h#L103-L152
[tmpring-legacy]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1894-L1924
[tmpring-rdna]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1958-L2025
[request-packet]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1053-L1081
[request-sizing]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1083-L1155
[main-install]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1199-L1266
[alt-install]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1157-L1197
[scratch-events]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1288-L1319
[allocation-policy]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2886-L2947
[allocation-attempts]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2963-L3015
[allocation-address]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3064-L3066
[occupancy-reduction]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3012-L3061
[wave-limits]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L93-L96
[wave-limit-selection]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L262
[default-limits]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/util/flag.h#L70-L73
[flags]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/util/flag.h#L166-L205
[threshold-init]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L638-L650
[threshold-update]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3169-L3198
[threshold-api]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L4864-L4892
[threshold-entry]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ext_amd.cpp#L2073-L2089
[check-limits]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L762-L775
[scratch-cache]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/scratch_cache.h#L132-L183
[scratch-trim]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/scratch_cache.h#L195-L211
[scratch-release]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3152-L3166
[async-protocol]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L795-L853
[async-main]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L877-L926
[async-alt]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L944-L987
[suspend-resume]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L722-L748
[free-main]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L780-L793
[driver-queue]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/driver/kfd/amd_kfd_driver.cpp#L479-L497
[update-ioctl]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/queues.c#L895-L925
[kfd-update]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L1088-L1176
[kfd-active]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_priv.h#L557-L561
[allocation-trim]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/driver/kfd/amd_kfd_driver.cpp#L372-L379
[agent-trim]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3603-L3608
[queue-destroy]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L356-L410
