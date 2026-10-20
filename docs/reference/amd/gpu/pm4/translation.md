# Translation priming and invalidation

`PRIME_UTCL2` requests address-translation warmup for a GPU virtual range.
`INVALIDATE_TLBS` participates in native translation invalidation after mapping
changes. Neither operation creates a mapping or establishes payload visibility.
The mapping owner, data producer, command processor and final reader have
separate responsibilities. PAL's priming API, Linux's VM update paths and
RADV's shader prefetch illustrate those boundaries.
[Priming contract][p-api-range] [Native update retirement][l-tlb-fence]

## Applicability and operation choice

| Mechanism | Selected source and observable role |
| --- | --- |
| Translation priming | PAL's `gfx9` and GFX12 compute backends select `PRIME_UTCL2` for translation-only ranges. Their shader-prefetch policy defaults ACE to this method. This is runtime selection, not a firmware support matrix. |
| Payload prefetch | PAL selects `DMA_DATA` to destination-nowhere for its data-prefetch branch. RADV also uses CP DMA for compute shader prefetch. Reading code/data into a cache is a different operation from priming its translation. |
| PASID invalidation | Linux's GMC owner selects KIQ, MES or a VMID-by-VMID path according to native IP, firmware and scheduling state. A declared `INVALIDATE_TLBS` opcode does not identify which path actually runs. |
| Mapping readiness and retirement | The native VM owner publishes page-table updates, orders the selected invalidation and retains translation storage. Workload completion and payload cache maintenance remain separate dependencies. |

[PAL range builders][p9-cache-builder] [GFX12 builder][p12-cache-builder]
[ACE defaults][p9-methods] [GFX12 defaults][p12-methods]
[RADV payload prefetch][m-prefetch]

The `gfx9` directory name in PAL covers merged later-generation definitions.
Its CE, PFP and MEC layouts are distinct. PAL and Mesa also disagree on some
fields within similarly named views; the tables preserve the source that owns
each meaning. The [architecture map](../architectures.md) distinguishes those
source predicates from compiler targets and native IP revisions.

## PRIME_UTCL2 fields

PAL's merged PFP/MEC and GFX12 PFP/MEC forms have five DWORDs. Word numbers
start at zero, including the header:

| Word and bits | PAL PFP view | PAL MEC view |
| --- | --- | --- |
| 0, 31:30 / 29:16 / 15:8 | Type `3`, count `3`, opcode `0x5d`. | Same framing. |
| 0, 7:0 | `predicate` at 0, `shaderType` at 1, `resetFilterCam` at 2; 7:3 reserved. The builder leaves the low byte zero, producing `0xc0035d00`. | All eight bits reserved. |
| 1, 2:0 | `cache_perm`. The builders describe a bitmask: Read at bit 0, Write at 1, Execute at 2. | Same numeric field; no generated permission enum. |
| 1, 3 | `prime_mode`: `0` = `dont_wait_for_xack`, `1` = `wait_for_xack`. | Same enum values. |
| 1, 29:4 | Reserved. | Reserved, together with 31:30. |
| 1, 31:30 | `engine_sel`: `1` = `prefetch_parser`. | Reserved; no engine selector. |
| 2 / 3, 31:0 each | `addr_lo` / `addr_hi`, unshifted address halves. | Same fields. |
| 4, 13:0 | `requested_pages`, direct page count. | Same field. |
| 4, 31:14 | Reserved. | Reserved. |

[Merged PFP][p9-pfp-prime] [Merged MEC][p9-mec-prime]
[GFX12 PFP][p12-pfp-prime] [GFX12 MEC][p12-mec-prime]
[Merged opcodes][p9-opcodes] [GFX12 opcodes][p12-opcodes]
[Builder and units][p9-prime-builder] [GFX12 builder][p12-prime-builder]
[Header construction][p9-header-builder] [Header defaults][p9-header-defaults]
[PFP header fields][p9-pfp-header] [MEC header fields][p9-mec-header]
[GFX12 PFP header][p12-pfp-header] [GFX12 MEC header][p12-mec-header]

The builders assert 4 KiB address alignment and describe `requested_pages`
in 4 KiB units. The raw 14-bit field can represent values through `0x3fff`;
that upper value describes 64 MiB minus 4 KiB. It is not count-minus-one.
The generated field does not specify a zero-count execution rule. There is
no destination payload or acknowledgment address in the packet.
[Alignment and count assignment][p9-prime-builder]

Mesa's GFX11 and GFX12 packet schemas have the same address/count fields but
reserve bit 3: they do not name `prime_mode`. Their PFP view reserves 29:3 and
names `engine_sel` at 31:30; their MEC view reserves 31:3. Thus `wait_for_xack`
cannot be inferred from these Mesa schemas. PAL's examined callers all select
mode zero; the enum declaration alone does not establish a usable completion
protocol or its scope.
[Mesa GFX11 PFP][m11-pfp-prime] [MEC][m11-mec-prime]
[Mesa GFX12 PFP][m12-pfp-prime] [MEC][m12-mec-prime]
[Actual range selection][p9-cache-builder] [GFX12 selection][p12-cache-builder]

PAL's older CE header has additional, overlapping views. Its `.gfx10` control
word reserves 2:0, names `prime_mode` at 3 and `engine_sel` at 31:30 with
constant-engine value `2`; 29:4 are reserved. The `.gfx101` view instead names
numeric `cache_perm` at 2:0, reserving 31:3. The `.gfx10Vrs` view uses those
three bits for a permission **enum**: Read `0`, Write `1`, Execute `2`.
The address halves and `.gfx10.requested_pages` retain the five-DWORD shape.
These are alternative overlays, not fields to combine. In particular, the CE
permission enum is not the PFP builder's permission bitmask.
[CE overlays][p9-ce-prime]

## Actual priming callers and boundaries

PAL's `BuildPrimeGpuCaches` receives a virtual address, byte size, `usageMask`
and `addrTranslationOnly`. It selects data prefetch only when neither
`CoherCpu` nor `CoherMemory` is present **and** `addrTranslationOnly` is false.
Otherwise it selects translation priming. For a nonempty, nonwrapping range,
after the optional byte clamp its arithmetic is:

```text
first_page = align_down(virtual_address, 4096)
last_page  = align_down(virtual_address + size - 1, 4096)
page_count = 1 + (last_page - first_page) / 4096
```

Each builder emits one PRIME packet. It has no split loop, zero-size guard or
end-overflow guard. Assignment into the 14-bit field truncates a larger count;
the subsequent reserved-bit assertion does not validate the original count.
The optional clamp defaults to zero, meaning disabled. The representable
packet extent and the public API's `gpusize` byte extent therefore have
different domains.
[Range arithmetic][p9-cache-builder] [GFX12 arithmetic][p12-cache-builder]
[Count assignment][p9-prime-builder] [Settings][p-core-settings]

Two source disagreements affect the selected compute path:

| Selected output | What the sources establish |
| --- | --- |
| `cache_perm=4` in the older backend; `cache_perm=2` in GFX12 | The older builder explicitly calls `4` Execute. Both builder comments define bit 1 as Write, so GFX12's `2` differs under that documented convention. Neither range builder derives the permission from `usageMask`; the source does not explain the change. |
| `engine_sel=prefetch_parser`, including `EngineTypeCompute` callers | Both use the PFP-shaped builder and set word 1 bits 31:30 to `1`. Their MEC layouts reserve those bits. Static assertions compare packet size and mode enum values, not the full control word. The source does not establish whether particular firmware ignores or interprets those bits. |

[Older builder][p9-prime-builder] [GFX12 builder][p12-prime-builder]
[Older emitted values][p9-cache-builder] [GFX12 emitted values][p12-cache-builder]
[Compute callers][p9-compute-prime] [GFX12 compute caller][p12-compute-prime]

The explicit `CmdPrimeGpuCaches` implementations also differ in command
reservation. The older backend reserves and commits each range separately;
GFX12 reserves once and emits the entire range array before committing. Its
default reservation is 256 DWORDs, while the public API names no range-count
bound. That implementation is not evidence of an arbitrary-size array
contract. The concrete shader caller below supplies one uploader-owned range.
[Range-array API][p-api-prime] [Per-range reservation][p9-compute-prime]
[Single reservation][p12-compute-prime] [Reservation contract][p-reserve]
[Default size][p-reserve-limit]

### Shader binding and ownership

PAL's compute pipeline path obtains its prefetch address and length from the
code uploader when `pipelinePrefetchEnable` is set. The older compute backend
emits the prefetch during dirty-pipeline dispatch validation; GFX12 emits it
while recording `CmdBindPipeline`. Both supply the `prefetchShaders` build
flag. That flag and a nonzero uploader address enable selection by actual
command-stream engine. The selected ACE or graphics method may disable
prefetch, choose data prefetch, or set `addrTranslationOnly` for PRIME.
The range carries `CoherShaderRead` and the uploaded section extent.
[Older range creation][p9-cs-init] [Older binding][p9-cs-prefetch]
[GFX12 range creation][p12-cs-init-pal] [GFX12 binding][p12-cs-prefetch]
[Older PAL-ABI dispatch][p9-dispatch-pal] [HSA-ABI dispatch][p9-dispatch-hsa]
[GFX12 bind caller][p12-compute-bind]

XGL forwards its captured `prefetchShaders` setting when
`enableAceShaderPrefetch || queueType != Compute`. The settings default true,
with application policy able to change ACE prefetch. This supplies a concrete
shader-prefetch client without establishing a client contract for arbitrary
explicit range arrays.
[XGL capture][xgl-prefetch-capture] [Begin selection][xgl-prefetch-begin]
[Defaults][xgl-prefetch-defaults]

The code uploader allocates always-resident GPU memory and records its GPU
address plus suballocation offset. Its instruction-fetch safety padding is
separate from the explicitly primed section extent. The CPU path copies ELF
sections into mapped storage, and `End` unmaps it. The DMA path records copies
and submits them at `End`. The compute pipeline captures the returned upload
fence token; binding records pending upload/paging fences and the queue waits
for uploads before native submission. PRIME does not replace this ready-code
protocol.
[Uploader allocation][p-upload-begin] [Mapped copies][p-upload-cpu]
[DMA copies][p-upload-dma] [Upload completion][p-upload-end]
[Older pipeline token][p9-upload-token] [GFX12 pipeline token][p12-upload-token]
[Binding dependencies][p-bind-upload-owner] [Queue upload wait][p-upload-wait]
[Native submission][p-native-submit]

The host range descriptors are read while recording and their values become
command bytes. They are not a copy of the addressed code or data. The selected
mapping and backing remain live through both priming and subsequent shader
accesses. Pipeline destruction frees the allocation in the examined destructor;
it supplies no automatic wait for the final shader. Command storage has its
own [retirement protocol](command-buffers.md#cpu-rebuild-after-completed-use).
Neither reservation commit nor a translation-mode field is a completion
observation for these independent users.
[Descriptor consumption][p9-compute-prime] [Pipeline destruction][p-pipeline-destroy]

For data prefetch, PAL's descriptor contract requires the requested usage to be
covered by the preceding barrier's destination mask and counts the prefetch as
a read for later memory dependencies. Translation-only mode explicitly has no
effect on those barrier dependencies. A complete operation therefore retains
the real producer join, [payload release/acquire](cache.md), shader completion
and mapping lifetime even when warmup is requested.
[Per-range dependency contract][p-api-range]

### RADV's overlapping payload prefetch

RADV binds the shader BO to the command stream and records its upload sequence.
Submission adds a shader-upload timeline wait when the queue has not covered
that sequence. Changing the compute pipeline marks compute prefetch pending.
For `gfx_level >= GFX7`, `radv_compute_dispatch` emits dispatch packets first,
then invokes the pending prefetch so it can overlap shader execution. This is
an actual payload-prefetch caller, not a `PRIME_UTCL2` completion example.
[Shader references][m-shader-bind] [Upload wait][m-upload-wait]
[Compute binding][m-compute-bind] [Dispatch ordering][m-after-dispatch]

The helper uses the shader address and `code_size`, then emits `DMA_DATA`.
GFX11+ caps the requested bytes at 32,736 before rounding the covered interval
outward to 32-byte boundaries. From GFX9 it uses destination-nowhere; older
forms use a same-address transfer. It disables write confirmation and sets no
explicit `CP_SYNC` in this prefetch packet. The
[DMA chapter](dma.md#copy-alignment-and-prefetch-ranges) describes the actual
fetch extent and caller-specific drain bookkeeping. A warmup operation neither
retires the executable nor supplies the upload wait that precedes its use.
[Shader extent][m-shader-prefetch] [Packet construction][m-prefetch]

## INVALIDATE_TLBS fields

The PAL merged and GFX12 forms and Mesa's GFX11/GFX12 schemas describe a
two-DWORD packet. Its type-3 header has type `3`, count `0` and opcode `0x98`;
Linux's unpredicated header is `0xc0009800`. The generated body views are:

| Word 1 bits | MEC | PFP / ME |
| --- | --- | --- |
| 2:0 | `invalidate_sel`: `0` = `invalidate`, `1` = `use_pasid`. | `invalidate_sel`; only `0` is named. |
| 4:3 | `mmhub_invalidate_sel`: `0` = `do_not_invalidate_mmhub`, `1` = `use_mmhub_flush_type`, `2` = `use_gfx_flush_type`. | Same enum. |
| 20:5 | `pasid`, 16 bits. | Reserved, together with 24:21. |
| 24:21 | Reserved. | Reserved. |
| 27:25 | `mmhub_flush_type`, three bits. | Same field. |
| 28 | Reserved. | Reserved. |
| 31:29 | `gfx_flush_type`, three bits. | Same field. |

The older PAL ME definition labels this specifically as its `.gfx10` overlay;
the layout is not a general ME GFX9 admission rule. The numeric flush fields
do not themselves define the cache levels or completion guarantees of each
value. Header low-byte differences follow the
[PM4 framing contract](memory-commands.md#representation).
[Merged MEC][p9-mec-invalidate] [PFP][p9-pfp-invalidate] [ME][p9-me-invalidate]
[GFX12 MEC][p12-mec-invalidate] [PFP][p12-pfp-invalidate] [ME][p12-me-invalidate]
[Mesa GFX11][m11-mec-invalidate] [Mesa GFX12][m12-mec-invalidate]

Linux's `soc15d.h`, `nvd.h` and GC12.1 header expose narrower construction
macros: `DST_SEL(x)` shifts by 0, `ALL_HUB(x)` by 4, `PASID(x)` by 5 and
`FLUSH_TYPE(x)` by 29. Its KIQ callbacks pass destination selector `1`, a
16-bit PASID, Boolean `all_hub` and the requested flush type. They do not emit
a separate MMHUB flush-type value. Relative to the generated MEC view,
`all_hub=true` selects MMHUB enum `2`, which uses the GFX flush type. These
macros are source-specific expressions without masks; their names are not
an alternative complete field partition.
[Linux macros][l-macros] [GC12.1 macros][l121-macros]
[GFX9 emitter][l9-emit] [GC9.4.3 emitter][l943-emit]
[GFX10 emitter][l10-emit] [GFX11 emitter][l11-emit]
[GFX12 emitter][l12-emit] [GC12.1 emitter][l121-emit]

The inspected PAL implementation defines these invalidation layouts but has
no `INVALIDATE_TLBS` builder/caller in its `src` or `inc` trees. Linux's actual
transport selection below supplies a native owner. A raw application queue
does not acquire KIQ, VMID, PASID or page-table ownership from the packet
definition.

## Native invalidation routes

`amdgpu_gmc_flush_gpu_tlb_pasid` selects its KIQ packet path only when
`flush_pasid_uses_kiq` and that instance's KIQ scheduler readiness are both
true. Otherwise it invokes the selected GMC PASID callback. The inspected
GMC9 initialization sets the KIQ policy; GMC10 and GMC11 set it outside
`amdgpu_emu_mode`. GMC12.0 and GMC12.1 do not set it. Their complete KIQ
emitters are consequently insufficient
evidence that this ordinary PASID path uses them.
[Common selection][l-pasid] [GMC9 policy][l9-policy]
[GMC10 policy][l10-policy] [GMC11 policy][l11-policy]
[GMC12 selection][l12-pasid] [GC12.1 selection][l121-pasid]

| Selected native path | Request, observation and applicability |
| --- | --- |
| KIQ PASID path | Reserves the invalidation packet(s) plus a polling fence, serializes ring construction, emits the invalidation, emits the fence, commits, then polls that fence. Ring reservation, command submission and observed fence progress are distinct steps. |
| GMC12.0 unified MES | Requires `enable_uni_mes`, the MES scheduler ring ready, and masked scheduler version at least `0x84`. Sends a GFXHUB request and, for `all_hub`, one MMHUB request. |
| GMC12.1 unified MES | Requires `enable_uni_mes`, MES ring 0 ready and masked scheduler version at least `0x6f`. The master XCC sends the GFXHUB request; slave-XCC requests return because the source assigns their invalidation to the master. `all_hub` adds each present MMHUB0/MMHUB1. |
| PASID fallback | Scans VMIDs 1 through 15 and flushes the matching valid PASID mappings. `all_hub` selects the native hub mask; otherwise it selects the generation's GFXHUB instance. The resulting VMID operation is not the same packet stream as KIQ PASID invalidation. |
| VMID firmware register path | Under the generation's ready-KIQ/MES and SR-IOV predicates, the GMC helper constructs an invalidation request for engine 17 and waits for the matching VMID acknowledgment bit through the native firmware register service. |
| VMID direct register path | The GMC helper owns request/acknowledgment registers, serialization and applicable MMHUB semaphore. GMC12.0 and GMC12.1's direct fallback passes flush type `0`, even when the caller supplied another value. |

[KIQ composition and polling][l-pasid] [Polling fence owner][l-poll-fence]
[GMC12 PASID paths][l12-pasid] [GC12.1 PASID paths][l121-pasid]
[GMC12 VMID paths][l12-vmid] [GC12.1 VMID paths][l121-vmid]
[Firmware register owner][l-register-service]
[Direct GMC12 request/ack][l12-direct] [Direct GC12.1 request/ack][l121-direct]

Scheduled workload submission has another VMID owner. Its
`emit_flush_gpu_tlb` writes the page-directory address low/high registers,
uses the ring's allocated `vm_inv_eng`, requests type `0` and waits for
`1 << vmid` in the matching acknowledgment register. The standalone helper's
engine `17` is not substituted for that ring allocation. Applicable MMHUB
semaphore acquisition/release surrounds this request.
[Scheduled GMC12 sequence][l12-scheduled] [Scheduled GC12.1 sequence][l121-scheduled]
[Invalidation-engine allocation][l-engine-owner]

MES invalidation uses its own API envelope, not PM4 opcode `0x98`: the
builder supplies PASID, flush type and converted hub identity, then the MES
submission helper attaches its completion address/value and waits for it.
GC12.1 also supplies the XCC route. An API-status fence belongs to that
native request; it is not an application shader-completion signal.
[MES ABI][l-mes-abi] [MES12 builder][l-mes12-build]
[MES12 completion][l-mes12-complete] [MES12.1 builder][l-mes121-build]
[MES12.1 completion][l-mes121-complete]

### Flush types and generation policy

KFD names values `0`, `1` and `2` as `TLB_FLUSH_LEGACY`,
`TLB_FLUSH_LIGHTWEIGHT` and `TLB_FLUSH_HEAVYWEIGHT`. Those names retain the
selected native request constructor; they do not define a payload-cache
release to another device. The common GMC path can prepend extra type-2
and type-0 requests under its explicit workaround flags.
[KFD values][l-flush-types] [Common ordering][l-pasid]

The inspected GMC9 setup enables extra type 2 specifically for GC9.4.0
with XGMI physical nodes. Its comment identifies cached PTEs in both TC and
TLB and a concurrent-access race requiring another TLB flush afterward.
This is not a blanket CDNA or XGMI rule. The GFXHUB12.1 request constructor
adds `INVALIDATE_L2_PDE3`; MMHUB4.2 does too. MMHUB4.1 and MMHUB4.2 force
their request's `FLUSH_TYPE` to zero rather than preserving the argument.
These register-level choices remain distinct from the three-bit packet field.
[GC9.4.0 predicate][l9-policy] [GFXHUB12.1 request][l121-hub]
[MMHUB4.1 request][l-mm41] [MMHUB4.2 request][l-mm42]

## Page-table publication and final storage use

The [SDMA page-table update chapter](../sdma/page-tables.md) describes entry
generation, ordinary COPY/WRITE selection, packet counts and the lifetime of
entry data retained inside native IB allocations.

A scheduled Linux compute job illustrates the full ownership flow:

1. The VM update backend writes PTEs/PDEs. The CPU backend executes `mb()`
   and flushes HDP when it acquires the reset-domain read lock; the reset
   branch returns success without issuing a new flush. The SDMA backend
   returns an update-job fence;
   when an output fence is requested and the update is not immediate, it sets
   `DRM_SCHED_FENCE_DONT_PIPELINE` on that fence. Neither action is a
   completion fence for the future shader.
   [CPU publication][l-cpu-commit] [SDMA publication][l-sdma-commit]
2. Command submission collects per-BO and PDE update dependencies. The VM
   owner records translation sequence progress separately from execution
   fences. The native job therefore receives the mapping state needed by
   its embedded GPU addresses.
   [Submission dependencies][l-cs-vm] [Translation sequence][l-vm-update]
3. Before the workload IB, `amdgpu_vm_flush` emits the required VMID flush,
   PASID mapping and other selected context operations, followed by
   `hw_vm_fence`. The submission wrapper then emits the workload and its
   separate completion tail. A VM-ready fence does not join that later
   workload.
   [VM context sequence][l-vm-flush] [IB ordering][l-ib]
4. For updates that require a flush and supply a non-null update fence,
   `!unlocked && need_tlb_fence` additionally selects a PASID TLB fence
   attached to the root reservation as `BOOKKEEP`. Its worker
   waits for the update dependency, requests type `2`, `all_hub=true`,
   instance `0`, then signals. Child page-table BOs share that root
   reservation; TTM deletion waits through `BOOKKEEP`. This connects the
   translation reader to page-table-storage retirement.
   [Update selection][l-update-range] [Attachment predicate][l-vm-tlb]
   [Worker and error result][l-tlb-fence]
   [Shared reservation][l-pt-owner] [Final deletion][l-ttm-delete]
5. Payload, executable and command allocations remain owned through their
   actual final workload users. Completing a page-table update or its
   invalidation does not establish that a shader, DMA engine or another
   queue has stopped using the mapped bytes.
   [Scheduled job resource lifetime][l-job-free]
   [Cross-queue completion](handoff.md)

`need_tlb_fence` is selected by user-queue VM initialization and compute-VM
conversion in this source. It is not an unconditional attachment on every
mapping update, and the unlocked update path has its own lifetime context.
[VM initialization][l-vm-init] [Compute conversion][l-vm-compute]

### KFD map and unmap

ROCr's KFD driver delegates residency to `hsaKmtMapMemoryToGPU` or
`hsaKmtMapMemoryToGPUNodes`; it does not replace that mapping operation with
PRIME. KFD's map ioctl waits for memory-update completion and then calls its
heavyweight TLB-flush wrapper. The unmap path updates the VM, conditionally
waits and flushes, then drops the DMA mapping. Its explicit
post-unmap flush predicate is GC at least 9.4.2, or GC9.4.1 with SDMA firmware
at least 18, or GC9.4.0. That source predicate is not an inferred compiler
target comparison.
[ROCr residency owner][r-residency] [KFD map][l-kfd-map]
[KFD unmap][l-kfd-unmap] [Flush wrapper and predicate][l-kfd-wrapper]

### Error propagation and completion claims

The normal ordered paths above have important error-path distinctions in the
cited Linux revision:

| Owner | Reported outcome |
| --- | --- |
| Common KIQ PASID path | Returns allocation/fence-emission errors and can report polling timeout. Reset serialization has a skip path; reset-aware control flow is part of the contract. |
| GMC PASID fallback | Uses `void` callbacks. The common function returns zero after invoking them; GMC12.0/12.1 callbacks discard MES invalidation return values. |
| Allocated TLB-fence worker | Stores a returned flush error on the fence before signaling it. Signaled and successful are separate observations. |
| TLB-fence allocation failure | Waits synchronously, calls the flush, discards its return and supplies a stub fence. That stub does not carry a failed flush status. |
| KFD flush wrapper | Returns `void` and discards `amdgpu_vm_flush_compute_tlb`'s status. Successful map/unmap ioctl return is consequently not an independent proof that every invalidation path succeeded. |
| KFD BO unmap helper | Checks queue references, but ignores returns from BO unmap, clearing freed mappings and adding its synchronization fence. Its normal sequencing cannot be promoted into complete failure rollback evidence. |

[Common path and reset behavior][l-pasid] [GMC12 callback][l12-pasid]
[GC12.1 callback][l121-pasid] [TLB-fence worker and allocation branch][l-tlb-fence]
[KFD wrapper][l-kfd-wrapper] [KFD BO helper][l-kfd-bo-unmap]

These are precise source contracts, not evidence of a hardware failure on a
particular deployment. A native completion claim needs the selected route's
acknowledgment and error handling as well as the update dependency. Payload
visibility and the last submitted user still determine when the application
can reuse the mapped allocation.

[p-api-range]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L2178-L2195
[l-tlb-fence]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_tlb_fence.c#L51-L111
[p9-cache-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4811-L4867
[p12-cache-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L2835-L2890
[p9-methods]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/settings_gfx9.json#L2019-L2088
[p12-methods]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/settings_gfx12.json#L489-L553
[m-prefetch]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cp_dma.c#L132-188
[p9-pfp-prime]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L3258-L3321
[p9-mec-prime]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L1830-L1886
[p12-pfp-prime]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L3468-L3531
[p12-mec-prime]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L1930-L1986
[p9-opcodes]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_pm4_it_opcodes.h#L86-L134
[p12-opcodes]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_pm4_it_opcodes.h#L98-L162
[p9-prime-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L2819-L2857
[p12-prime-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L2794-L2833
[p9-header-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L274-L302
[p9-header-defaults]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.h#L805-L810
[p9-pfp-header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L44-L57
[p9-mec-header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L44-L54
[p12-pfp-header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L44-L57
[p12-mec-header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L44-L54
[m11-pfp-prime]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx11.json#L4749-4813
[m11-mec-prime]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx11.json#L14325-14378
[m12-pfp-prime]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx12.json#L4968-5032
[m12-mec-prime]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx12.json#L14555-14608
[p9-ce-prime]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_ce_pm4_packets.h#L803-L884
[p-core-settings]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/settings_core.json#L1535-L1558
[p9-compute-prime]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1619-L1634
[p12-compute-prime]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L865-L884
[p-api-prime]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L4488-L4500
[p-reserve]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStream.cpp#L162-L281
[p-reserve-limit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/device.h#L1020-L1023
[p9-cs-init]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineChunkCs.cpp#L87-L114
[p9-cs-prefetch]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineChunkCs.cpp#L681-L750
[p12-cs-init-pal]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PipelineChunkCs.cpp#L787-L825
[p12-cs-prefetch]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PipelineChunkCs.cpp#L861-L912
[p9-dispatch-pal]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L661-L745
[p9-dispatch-hsa]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L747-L925
[p12-compute-bind]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L104-L166
[xgl-prefetch-capture]: https://github.com/GPUOpen-Drivers/xgl/blob/e9782eb33ce5e5e4ed2e339542a28c1b933624b4/icd/api/vk_cmdbuffer.cpp#L613-L649
[xgl-prefetch-begin]: https://github.com/GPUOpen-Drivers/xgl/blob/e9782eb33ce5e5e4ed2e339542a28c1b933624b4/icd/api/vk_cmdbuffer.cpp#L1312-L1360
[xgl-prefetch-defaults]: https://github.com/GPUOpen-Drivers/xgl/blob/e9782eb33ce5e5e4ed2e339542a28c1b933624b4/icd/settings/settings_xgl.json#L7495-L7518
[p-upload-begin]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/codeObjectUploader.cpp#L257-L339
[p-upload-cpu]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/codeObjectUploader.cpp#L420-L482
[p-upload-dma]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/codeObjectUploader.cpp#L341-L418
[p-upload-end]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/codeObjectUploader.cpp#L648-L683
[p9-upload-token]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputePipeline.cpp#L229-L271
[p12-upload-token]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputePipeline.cpp#L57-L162
[p-bind-upload-owner]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdBuffer.cpp#L1405-L1458
[p-upload-wait]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/queue.cpp#L588-L616
[p-native-submit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuQueue.cpp#L826-L968
[p-pipeline-destroy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/pipeline.cpp#L68-L89
[m-shader-bind]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L9398-9478
[m-upload-wait]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1605-1671
[m-compute-bind]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L9506-9517
[m-after-dispatch]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L15361-15384
[m-shader-prefetch]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L3068-3078
[p9-mec-invalidate]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L1704-L1747
[p9-pfp-invalidate]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L2595-L2636
[p9-me-invalidate]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_me_pm4_packets.h#L1898-L1939
[p12-mec-invalidate]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L1721-L1764
[p12-pfp-invalidate]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L2680-L2721
[p12-me-invalidate]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_me_pm4_packets.h#L1797-L1838
[m11-mec-invalidate]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx11.json#L14067-14133
[m12-mec-invalidate]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx12.json#L14297-14363
[l-macros]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/soc15d.h#L466-L470
[l121-macros]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1_pkt.h#L549-L553
[l9-emit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_0.c#L1031-L1041
[l943-emit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_4_3.c#L278-L288
[l10-emit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c#L8764-L8774
[l11-emit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L6119-L6129
[l12-emit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c#L4606-L4616
[l121-emit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1.c#L3780-L3790
[l-pasid]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.c#L776-L866
[l9-policy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L2163-L2180
[l10-policy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v10_0.c#L958-L964
[l11-policy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v11_0.c#L934-L940
[l12-pasid]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v12_0.c#L345-L386
[l121-pasid]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v12_1.c#L407-L460
[l-poll-fence]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_fence.c#L159-L181
[l12-vmid]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v12_0.c#L305-L332
[l121-vmid]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v12_1.c#L361-L394
[l-register-service]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.c#L868-L920
[l12-direct]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v12_0.c#L213-L293
[l121-direct]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v12_1.c#L284-L349
[l12-scheduled]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v12_0.c#L388-L434
[l121-scheduled]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v12_1.c#L462-L499
[l-engine-owner]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.c#L642-L708
[l-mes-abi]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/mes_v12_api_def.h#L930-L957
[l-mes12-build]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/mes_v12_0.c#L1183-L1208
[l-mes12-complete]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/mes_v12_0.c#L152-L272
[l-mes121-build]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/mes_v12_1.c#L943-L973
[l-mes121-complete]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/mes_v12_1.c#L155-L272
[l-flush-types]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd.h#L45-L49
[l121-hub]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfxhub_v12_1.c#L667-L693
[l-mm41]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/mmhub_v4_1_0.c#L67-L86
[l-mm42]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/mmhub_v4_2_0.c#L691-L711
[l-cpu-commit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_cpu.c#L119-L136
[l-sdma-commit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_sdma.c#L106-L146
[l-cs-vm]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_cs.c#L1105-L1203
[l-vm-update]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c#L1053-L1104
[l-vm-flush]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c#L774-L925
[l-ib]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c#L124-L357
[l-update-range]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c#L1130-L1262
[l-vm-tlb]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c#L1072-L1104
[l-pt-owner]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_pt.c#L441-L480
[l-ttm-delete]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/ttm/ttm_bo.c#L192-L320
[l-job-free]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_job.c#L296-L312
[l-vm-init]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c#L2619-L2635
[l-vm-compute]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c#L2708-L2749
[r-residency]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/driver/kfd/amd_kfd_driver.cpp#L1015-L1034
[l-kfd-map]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L1283-L1391
[l-kfd-unmap]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L1393-L1484
[l-kfd-wrapper]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_priv.h#L1591-L1605
[l-kfd-bo-unmap]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c#L1269-L1293
