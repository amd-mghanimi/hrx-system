# Compute affinity and queue priority

Compute affinity selects the execution resources on which a queue's work is
eligible to run. Queue priority supplies scheduling preferences to the native
queue manager and command processor. Neither setting establishes a dependency
between dispatches, acquires another device's writes, or completes a running
program. Those operations retain their [publication](aql/publication.md),
[memory](shader-memory.md) and [completion](aql/barriers.md) protocols.

## Applicability and actors

The Linux path described here is ROCr → libhsakmt → KFD for a compute queue.
KFD translates a logical mask into `COMPUTE_STATIC_THREAD_MGMT_SE*` fields in
the memory queue descriptor (MQD). Its queue manager updates hardware state
through CP hardware scheduling, MES, or the non-HWS load/unload path.
PAL and Mesa provide separate examples of programming compute masks through
PM4 preambles; their input masks have different numbering from the KFD UAPI.
[ROCr driver calls][rocr-driver] · [Native update][kfd-update] ·
[PAL preamble][pal-preamble] · [Mesa preambles][mesa-preambles]

| Layer | Information it owns |
| --- | --- |
| Application or higher runtime | Requested work, dependencies, affinity policy and priority. An application mask need not use the native driver's numbering. |
| ROCr | Agent identity, optional `HSA_CU_MASK`, HSA queue ownership and the logical mask submitted through the native driver. |
| KFD | Native GC revision, harvested CU topology, shader engines (SE), shader arrays (SH/SA), XCC partition membership, MQD format and scheduler state. |
| Packet processor and shader scheduler | Dispatch placement and wave execution subject to configured masks, scheduling state and available resources. |

The priority encodings below belong to Linux KFD compute queues. Windows
WDDM priorities and DRM scheduled-IB context priorities have their own native
interfaces; equal-looking integers do not make them interchangeable. Likewise,
a compiler ISA name and the native `KFD_GC_VERSION` predicate are distinct
identities. [Architecture and transport](architectures.md)

## Logical masks at the runtime boundary

`hsa_amd_queue_cu_set_mask` accepts a `uint32_t` bit vector. Its count is
**bits**, in multiples of 32. A zero count requests the default enabled set,
still constrained by `HSA_CU_MASK`; it is not an empty affinity set. The
public API describes one bit per CU and requires even-indexed adjacent CU
pairs on devices with workgroup processors. The getter's count must cover
`HSA_AMD_AGENT_INFO_COMPUTE_UNIT_COUNT` to request the whole mask.
[Public mask contract][hsa-mask]

ROCr's `AqlQueue::SetCUMasking` constructs a mask, intersects it with the
agent's global mask, clips the extent to the agent's CU count, and submits
the resulting vector through `SetQueueCUMask`. `HSA_CU_MASK` is looked up
using the runtime's agent enumeration index. The constructor normally calls
`SetCUMasking(0, nullptr)`; `HSA_CU_MASK_SKIP_INIT` changes that initialization
path. These are runtime policies, separate from the native MQD layout.
[Mask construction][rocr-mask] · [Agent selection][rocr-global-mask] ·
[Initialization][rocr-mask-init] · [Initialization flag][rocr-mask-flag]

The public API promises `HSA_STATUS_CU_MASK_REDUCED` when a global mask removes
requested CUs. At the cited ROCr revision, the branch that submits a native
mask returns the driver's result directly, before updating `cu_mask_` or
returning that reduced-mask status. `GetCUMasking` copies `cu_mask_`; it does
not read the MQD or hardware. Consequently, a successful native update and a
subsequent cached getter are different observations in this implementation.
The thunk also maintains a software copy after a successful ioctl.
[Setter and getter][rocr-mask] · [Thunk mask ownership][thunk-mask]

### CLR's WGP mask conversion

CLR's `Device::acquireQueue` supplies a concrete caller. When
`enableWgpMode_` is set, each bit in its application/global mask expands to
two adjacent CU bits before ROCr sees the vector: logical bits 0 and 1 become
`0x3` and `0xC`. One input DWORD therefore produces two HSA mask DWORDs.
Its settings enable WGP mode for ISA major ≥10 according to
`GPU_ENABLE_WGP_MODE`, except major 12 with minor ≥5, where they disable it.
This conversion belongs to CLR; applying it again to an already expanded
HSA mask changes the requested placement. [Caller conversion][clr-mask] ·
[Settings predicate][clr-settings]

CLR also has a separate global-mask policy: if a custom/global intersection
is empty, the cited caller replaces it with the global mask. Thus the custom
mask at that boundary is not proof of a strict intersection. The final vector
is borrowed by a synchronous queue-creation call, which applies priority and
affinity before returning the queue to its caller. [Mask policy][clr-mask] ·
[Creation descriptor][clr-create] · [ROCr construction][rocr-create]

### Pairing predicates remain layer-specific

| Source | Predicate and action |
| --- | --- |
| ROCr `SetCUMasking` | ISA major ≥10 requires each adjacent pair to be `00` or `11`, except ISA major 12, minor ≥5. |
| KFD `pqm_update_mqd` | Native GC ≥10.0.0 requires each adjacent pair to be `00` or `11`; the cited predicate has no GC12.1 exception. |
| KFD generic mask mapper | Native GC ≥10.0.0 advances in two-CU groups and writes two enable bits per selected group. |
| KFD V12.1 mask mapper | Uses a different representation: each selected input bit expands to two hardware enable bits. |

[ROCr validation][rocr-mask] · [KFD validation][kfd-mask-admission] ·
[Generic mapping][kfd-map] · [V12.1 mapping][kfd-map121]

These sources do not establish a single pairing rule for every deployment.
ROCr accepting a vector does not imply that the paired native driver accepts
it, and the existence of a V12.1 mapper does not remove the earlier KFD
validation. Resolving a particular deployment requires its actual runtime,
native driver, topology and GC identity rather than substituting an ISA
generation label for those predicates.

## KFD mask representation and placement

`AMDKFD_IOC_SET_CU_MASK` uses the following UAPI structure. Offsets are bytes;
the structure is 16 bytes in the 64-bit Linux ABI.

| Field | Offset / width | Meaning |
| --- | --- | --- |
| `queue_id` | 0 / 32 bits | Process-owned native queue identifier. |
| `num_cu_mask` | 4 / 32 bits | Nonzero bit count, a multiple of 32. |
| `cu_mask_ptr` | 8 / 64 bits | User virtual address of the mask DWORDs. |

KFD copies `num_cu_mask / 8` bytes into temporary kernel storage, bounded to
1024 bits, performs the queue update, and releases that temporary copy before
returning. The bound is an input-copy policy, not a statement that a GPU has
1024 CUs. Unlike ROCr's public setter, this native interface does not use a
zero count to mean reset. The updated MQD owns the persistent representation;
the input vector is not GPU-borrowed storage. [UAPI][kfd-uapi] ·
[Copy and lifetime][kfd-mask-ioctl]

### Harvested SE/SH numbering

The generic `mqd_symmetrically_map_cu_mask` distributes logical bits across
SEs and SHs using active CU counts. Within each SH, enabled positions occupy
the low bits of its 16-bit mask. The driver counts active CUs from the
harvested topology bitmap, while hardware handles their physical placement.
Copying physical fuse-bit positions directly into this logical vector would
skip that translation. [Topology and mapping][kfd-map]

The traversal is CU position outermost, SH next, SE innermost, skipping
positions beyond each SH's active count. For the non-WGP, single-XCC case:

| Example topology | Logical mask bits | MQD destination |
| --- | --- | --- |
| Four SEs, one SH per SE | Bits 0, 1, 2, 3 | CU position 0 in SE0, SE1, SE2, SE3. |
| Four SEs, one SH per SE | Bit 4 | CU position 1 in SE0. |
| Four SEs, two SHs per SE | Bits 0–3, then 4–7 | CU position 0 in SH0 across the four SEs, then SH1 across the four SEs. |
| Four SEs, two SHs per SE | Bit 8 | CU position 1 in SE0/SH0. |

These examples assume every listed SH has those CU positions. Harvesting
changes which positions participate in the traversal. The generic WGP path
uses a step of two and a two-bit enable mask instead of the single-bit step
used by the table. [Traversal and examples][kfd-map]

For multiple XCCs in one KFD node, the mapper runs once per XCC. Input index
starts at `inst`; each participating SE/SH position advances it by
`cu_inc * NUM_XCC(xcc_mask)`, where `cu_inc` is one or two in the generic
mapper. In the non-WGP case with six XCCs, instance `j` therefore consumes
bits `j`, `j + 6`, `j + 12`, and so on. The mask is interleaved across the
node's XCCs rather than being one contiguous bitmap per physical die.
The native topology also clips the mask to `cu_info.number / num_nodes`.
[Generic mapping][kfd-map] · [V9 per-XCC update][kfd-mqd9-xcc]

### Partition virtual IDs and queue logical IDs

Linux's GC9.4.3 partition setup and KFD's AQL queue initialization configure
different XCC identities. Their numbering cannot be interchanged merely
because both range over the partition's XCC count.

| Identity | Source owner and construction |
| --- | --- |
| Native GC instance | Register access uses `GET_INST(GC, i)` to map the driver's instance index. |
| Partition-relative virtual XCC | The non-PSP partition path writes `CP_HYP_XCP_CTL.VIRTUAL_XCC_ID = i % num_xccs_per_xcp`, alongside `NUM_XCC_IN_XCP`. The PSP path delegates partition setup. |
| Queue logical XCC | KFD sets each AQL MQD's `compute_current_logic_xcc_id = (queue_start + xcc) % node_xcc_count` and `compute_tg_chunk_size = 1`. The start advances across queue creations. |

[Partition setup][linux-virtual-xcc] · [Per-XCC queue initialization][kfd-logical-xcc]

KFD obtains the node's XCC membership from its compute partition, then builds
one MQD per member. Its AQL master-XCC branch enables the read-pointer update;
the other MQDs retain their separate execution state. The native PM4 queue
branch has a different distribution setup and a `pm4_target_xcc_in_xcp` field.
These configuration differences explain why a shader-distribution index,
a physical chiplet index and a
[PRED_EXEC participant bit](pm4/conditional.md#virtual-xcc-selection-pred_exec)
are distinct inputs. A cardinality query alone does not provide their mapping.
[Node membership][kfd-xcc-membership] · [MQD construction][kfd-logical-xcc]

### MQD family differences

KFD selects its manager by native GC version after handling the earlier
CIK/VI ASIC cases. The selected implementations retain the following
differences; register capacity is not the number of populated SEs.
[Manager selection][kfd-manager-selection]

| Selected path | Mask programming |
| --- | --- |
| V9, GC ≥9.0.1 below the V10 selection | Generic mapper; SE0–3 always. SE4–7 are additionally written except on GC9.4.3, 9.4.4 and 9.5.0. Per-XCC MQDs retain separate masks. |
| V10, GC ≥10.1.1 below 11.0.0 | Generic mapper; SE0–3. |
| V11, GC ≥11.0.0 below 12.0.0 | Generic mapper; SE0–7. The debugger workaround has a separate mask owner. |
| V12, GC ≥12.0.0 below 12.1.0 | Generic mapper; SE0–7. |
| V12.1, GC ≥12.1.0 | Separate mapper; SE0–1 per XCC. Each selected logical bit writes a pair of hardware bits. |

[V9][kfd-mqd9] · [V10][kfd-mqd10] · [V11][kfd-mqd11] ·
[V12][kfd-mqd12] · [V12.1][kfd-map121]

V12.1 advances logical CU position by one and its hardware bit position by
two. Its ordinary destination is the pair starting at
`2 * cu + 16 * sh`, with a special mapping for `cu == 8 && sh == 0` to
bits 30–31. Input indices advance by the number of XCCs, and each XCC's MQD
is updated independently. This source-specific packing differs from the
generic mapper's two-CU grouping. [V12.1 mapping][kfd-map121] ·
[Per-XCC caller][kfd-mqd121-xcc]

When `is_dbg_wa` owns the queue mask, KFD rejects a user mask update with
`EBUSY`. Affinity and debugger control therefore cannot be treated as
independent writers of the same MQD fields. [Admission][kfd-mask-admission]

## PM4 preambles use per-SE masks

In PAL's cited Gfx9-family compute/universal preamble,
`COMPUTE_STATIC_THREAD_MGMT_SE0` contains `SA0_CU_EN` in bits 0–15 and
`SA1_CU_EN` in bits 16–31. `GetCuMaskPerSe` combines a 16-bit CU mask with
separate SE/SA selection. This is the register representation, not the
flattened HSA vector. [Register fields][pal-mask-fields] ·
[Preamble builder][pal-preamble]

PAL emits `SET_SH_REG_INDEX` with `apply_kmd_cu_and_mask`, whose index value
is 3 in bits 28–31 of packet DWORD 1. The lower 16 bits hold the register
offset. The preamble explains that this ANDs the user-mode mask with the
kernel-mode mask, preserving CUs assigned by the native driver to real-time
compute. A user-mode mask therefore cannot be interpreted as authority to
enable every represented CU. The [PM4 dispatch chapter](pm4/dispatch.md)
describes the surrounding register and launch protocol.
[Packet layout][pal-mask-packet] · [KMD mask composition][pal-preamble]

Mesa's `AMD_CU_MASK` produces `spi_cu_en`, a per-SA mask. Its compute
preambles repeat that mask into SH0 and SH1 of each relevant SE. The parser
clips it to `max_good_cu_per_sa`; additional graphics and late-allocation
constraints apply specifically when `has_graphics` is true. This environment
variable is neither ROCr's per-agent `HSA_CU_MASK` nor its flattened CU
numbering. [Mesa mask policy][mesa-mask] · [Preamble use][mesa-preambles]

The cited Mesa builder routes shader registers through `ac_pm4_set_reg`,
which selects ordinary `SET_SH_REG` or its paired-register forms. That is
a distinct emitted sequence from PAL's explicit KMD-AND index operation;
agreement on register names alone does not establish identical packet or
native-submission behavior. [Mesa packet selection][mesa-registers]

## Priority encodings and scheduler inputs

The HSA API's three priorities, libhsakmt's seven priorities, KFD's 0–15
range and the MQD's three pipe-priority classes have separate encodings.
For the ordinary HSA compute path:

| HSA priority | HSA value | libhsakmt choice / value | KFD `queue_priority` | MQD `cp_hqd_pipe_priority` |
| --- | --- | --- | --- | --- |
| `HSA_AMD_QUEUE_PRIORITY_LOW` | 0 | `HSA_QUEUE_PRIORITY_MINIMUM` / −3 | 0 | LOW / 0 |
| `HSA_AMD_QUEUE_PRIORITY_NORMAL` | 1 | `HSA_QUEUE_PRIORITY_NORMAL` / 0 | 7 | MEDIUM / 1 |
| `HSA_AMD_QUEUE_PRIORITY_HIGH` | 2 | `HSA_QUEUE_PRIORITY_HIGH` / 2 | 11 | HIGH / 2 |

[HSA enumeration][hsa-priority] · [ROCr conversion][rocr-priority-map] ·
[Thunk enumeration][thunk-priority] · [Wire conversion][thunk-priority-map] ·
[Pipe mapping][kfd-priority-map] · [Pipe values][kfd-priority-values]

The complete thunk map, indexed by its input priority plus three, is
`{0, 3, 5, 7, 9, 11, 15}`. KFD maps native values 0–6 to LOW, 7–10 to
MEDIUM and 11–15 to HIGH. ROCr reserves its additional internal maximum
priority for PC sampling; it maps to thunk maximum 3 and native value 15.
An HSA enum value is therefore not a native UAPI value even where two
numbers happen to match. [ROCr conversion][rocr-priority-map] ·
[Thunk update][thunk-update] · [MQD setter][kfd-mqd9]

The HSA comments place LOW below NORMAL/HIGH compute, NORMAL below HIGH,
both LOW and NORMAL below graphics, and HIGH above graphics. Those are
the API's stated relationships, not a bound on preemption latency or a
complete description of every native scheduler's arbitration.
[Public priority contract][hsa-priority]

For KFD's MES add-queue path, the full native priority becomes
`inprocess_gang_priority`, while `gang_global_priority_level` is set to
`AMDGPU_MES_PRIORITY_LEVEL_NORMAL`. Process quantum is 100000 and gang
quantum is 10000 in units of 100 ns: 10 ms and 1 ms respectively. These
are firmware scheduling inputs, not measured switching costs or guaranteed
service intervals. In particular, HSA HIGH does not select MES's real-time
global priority class in this builder. [MES input][kfd-mes] ·
[Quantum constants][kfd-quantum]

## Updating a live queue

Changing affinity or priority invokes native queue management. The caller
does not simply publish another dispatch packet. `AqlQueue::SetPriority`
passes the queue's ring backing, size, priority and queue percentage 100
through the thunk. Native `UPDATE_QUEUE` carries these fields together:

| Field | Byte offset / width in the 24-byte 64-bit Linux UAPI structure |
| --- | --- |
| `ring_base_address` | 0 / 64 bits, user virtual address. |
| `queue_id` | 8 / 32 bits. |
| `ring_size` | 12 / 32 bits, bytes. |
| `queue_percentage` | 16 / 32 bits; KFD interprets bits 0–7 as percentage and 8–15 as `pm4_target_xcc`. |
| `queue_priority` | 20 / 32 bits, native value 0–15. |

[ROCr setter][rocr-priority] · [Thunk update][thunk-update] ·
[UAPI][kfd-uapi] · [Native decoding][kfd-update-ioctl]

Both priority and mask updates reach `update_queue`, which makes the queue
unmapped before changing its MQD:

| Native scheduling path | Update sequence |
| --- | --- |
| CP hardware scheduling without MES | Unmap dynamic queues, update the MQD, then remap the runlist. The unmap scope can exceed the one queue being changed. |
| MES | Remove an active queue, update the MQD, then add it again if active. |
| Non-HWS, active queue | Request wavefront save when CWSR is enabled, otherwise drain; the selected direct callback determines the native dequeue action. Update the MQD, then load it if active. |

[Native transition][kfd-update]

The [direct-consumer comparison](queue-context.md#reconfiguration-checkpoint-and-final-ownership)
distinguishes callbacks that select SAVE_WAVES from those that map the SAVE
request to DRAIN_PIPE.

This mechanism makes the operation a configuration transition with possible
preemption and scheduler work. It supplies no constant-time hot-path bound.
Successful wavefront save can preserve unfinished work, so an affinity update
does not retire that work's code, arguments, scratch or data. The
[context-save ownership rules](context-save.md#save-resume-and-final-ownership)
continue to apply across the transition.

An update failure is not evidence of automatic rollback. ROCr assigns its
cached priority before calling the driver; KFD also updates queue properties
before entering the fallible native transition. Subsequent submission and
cleanup depend on the live queue's native state, rather than an assumption
that a failed setter left every layer unchanged. [ROCr setter][rocr-priority] ·
[KFD property update][kfd-properties] · [Native transition][kfd-update]

## Construction, execution and final use

A creation-time configuration keeps affinity changes outside the workload's
publication path. For an ordinary, non-counted compute queue, CLR's descriptor
caller and ROCr's construction implement the following flow:

1. Resolve the intended agent and native topology. Construct a nonempty
   eligible set using that boundary's mask units and applicable pairing rule.
   Include global-mask policy when determining the effective request.
2. Keep the mask vector live while calling `hsa_amd_queue_create`. Select
   priority explicitly: `HSA_AMD_QUEUE_PRIORITY_NORMAL` is 1, whereas a
   zero-initialized priority field denotes LOW. Set the descriptor version,
   compute engine/type and ring size in bytes.
3. ROCr creates the queue, applies a non-default priority and any explicit
   mask, then publishes the queue pointer. Failure while applying either
   setting destroys that queue before publication. The caller's temporary
   mask storage can be released after the synchronous call.
4. Publish arguments, code and commands through the ordinary
   [AQL protocol](aql/publication.md). Affinity and priority add no new
   payload release/acquire edge and replace no work dependency.
5. Observe the workload's actual completion conditions. Stop further
   publication and complete native queue removal before releasing
   queue-owned state. Code, payloads and signals remain live through
   their last independent user, including any work that can resume.

[Compute parameters][hsa-compute-params] · [CLR caller][clr-create] ·
[ROCr construction][rocr-create] · [Final ownership](context-save.md#save-resume-and-final-ownership)

Counted queues are a different ownership model. Their public API may share
one native queue among several handles of the same priority and explicitly
prohibits `hsa_amd_queue_set_priority` and `hsa_amd_queue_cu_set_mask` on
those handles. A handle alone therefore does not establish independently
mutable native scheduling state. [Counted queue contract][hsa-counted]

[Cooperative queues](cooperative.md#shared-cooperative-queue-ownership) also
have shared ownership: repeated creation may return the same queue, with a
matching reference release required for each acquisition. The ordinary
construction sequence above does not describe a fresh native allocation
for every cooperative request.

For a persistent producer/consumer design, disjoint affinity sets express
eligible placement but do not establish exclusive ownership, simultaneous
residency or fairness. Priority changes preference rather than creating
an execution dependency. A progress argument also needs runnable queues,
available wave/LDS/scratch resources, and a scheduling contract that lets
the producer execute while consumers wait. The
[device-generated SDMA protocol](sdma/device-publication.md) and
[resident interop protocol](../interop/pipelines.md) retain these requirements
alongside their independent command and payload credits.

[rocr-driver]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/driver/kfd/amd_kfd_driver.cpp#L486-L505
[kfd-update]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L1070-L1176
[pal-preamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9QueueContexts.cpp#L55-L135
[mesa-preambles]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/common/ac_cmdbuf.c#L51-153
[hsa-mask]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L1708-L1778
[rocr-mask]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1507-L1587
[rocr-global-mask]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/util/flag.h#L503-L510
[rocr-mask-init]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L334-L353
[rocr-mask-flag]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/util/flag.h#L272-L276
[rocr-create]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ext_amd.cpp#L2567-L2616
[clr-mask]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocdevice.cpp#L3479-L3518
[clr-settings]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocsettings.cpp#L163-L170
[clr-create]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocdevice.cpp#L3520-L3563
[kfd-mask-admission]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L648-L687
[kfd-map]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager.c#L100-L208
[kfd-map121]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12_1.c#L49-L138
[kfd-uapi]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/linux/kfd_ioctl.h#L100-L113
[kfd-mask-ioctl]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L525-L570
[kfd-mqd9-xcc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L796-L815
[kfd-manager-selection]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L3296-L3328
[kfd-mqd9]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L70-L117
[kfd-mqd10]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v10.c#L45-L74
[kfd-mqd11]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v11.c#L44-L104
[kfd-mqd12]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12.c#L44-L82
[kfd-mqd121-xcc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12_1.c#L507-L522
[pal-mask-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_registers.h#L5075-L5090
[pal-mask-packet]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L2199-L2232
[mesa-mask]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/common/ac_gpu_info.c#L174-222
[mesa-registers]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/common/ac_pm4.c#L365-398
[hsa-priority]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L3685-L3720
[rocr-priority-map]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/driver/kfd/amd_kfd_driver.cpp#L71-L88
[rocr-priority]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L751-L760
[kfd-properties]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L634-L645
[kfd-update-ioctl]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L467-L522
[kfd-priority-map]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager.c#L29-L47
[kfd-priority-values]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_priv.h#L657-L661
[kfd-mes]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L207-L250
[kfd-quantum]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.h#L35-L38
[hsa-compute-params]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L3820-L3848
[hsa-counted]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L5090-L5120
[thunk-mask]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/queues.c#L950-L1008
[thunk-priority]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/include/hsakmt/hsakmttypes.h#L667-L678
[thunk-priority-map]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/queues.c#L695-L698
[thunk-update]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/queues.c#L895-L927
[linux-virtual-xcc]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdgpu/gfx_v9_4_3.c#L796-L825
[kfd-logical-xcc]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L731-L795
[kfd-xcc-membership]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdkfd/kfd_device.c#L886-L901
