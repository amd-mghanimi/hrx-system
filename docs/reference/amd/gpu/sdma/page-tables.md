# Page-table updates

SDMA writes page-table entries on behalf of the native virtual-memory owner.
Linux uses three constructions: inline data for small updates, a generated
address progression for contiguous mappings, and a copy of CPU-built entries
for scattered pages. Successful completion makes the table writes available to
the native translation protocol; translation invalidation and completion of later
workloads have separate owners. A packet encoding does not confer ownership of
the page tables or their address space.
[Update selection][update] [Native translation protocol](../pm4/translation.md)

## Applicability and native selection

The Linux `amdgpu_vm_pte_funcs` interface supplies `copy_pte`, `write_pte`,
and `set_pte_pde` callbacks. Each selected SDMA backend installs its table and
the corresponding native schedulers. `copy_pte` emits ordinary linear COPY;
it does not select the dedicated `PTEPDE_COPY` suboperation. Here `n` is the
number of 64-bit entries supplied to a callback.
[Callback interface][interface] [Scheduler installation][schedulers]

The VM owner selects the update backend before these callbacks run.
`amdgpu_vm_init` uses `AMDGPU_VM_USE_CPU_FOR_GFX`; compute-VM conversion uses
`AMDGPU_VM_USE_CPU_FOR_COMPUTE` and waits for previous SDMA work before
switching to CPU updates. The presence of an SDMA packet family therefore
does not mean every native VM uses it.
[VM initialization][vm-select] [Compute conversion][vm-compute]

| Native writer | Source-selected ASIC or SDMA IP | COPY byte count / WRITE DWORD count / generated-entry count |
| --- | --- | --- |
| `si_dma` | Verde, Tahiti, Pitcairn, Oland, Hainan; IP block 1.0.0 | `8*n` / `2*n` / `2*n` in the header. GEN splits locally. |
| `cik_sdma` | Bonaire, Hawaii, Kaveri, Kabini, Mullins; 2.0.0 | `8*n` / `2*n` / `n`. |
| `sdma_v2_4` | Topaz; 2.4.0 | Same direct counts. |
| `sdma_v3_0` | Fiji, Tonga, Carrizo, Stoney; 3.0.0 | Same direct counts. |
| Same 3.0 writer | Polaris10/11/12, VegaM; 3.1.0 | Same direct counts; the 3.1 IP block uses the 3.0 functions. |
| `sdma_v4_0` | 4.0.0/1, 4.1.0/1/2, 4.2.0/2, 4.4.0 | `8*n-1` / `2*n-1` / `n-1`. |
| `sdma_v4_4_2` | 4.4.2/4/5 | Same minus-one counts. |
| `sdma_v5_0` | 5.0.0/1/2/5 | Same minus-one counts. |
| `sdma_v5_2` | 5.2.0/1/2/3/4/5/6/7 | Same minus-one counts. |
| `sdma_v6_0` | 6.0.0/1/2/3, 6.1.0/1/2/3/4, 6.4.0 | Same minus-one counts. |
| `sdma_v7_0` | 7.0.0/1 | Same minus-one counts; ordinary COPY appends an eighth zero DWORD. |
| `sdma_v7_1` | 7.1.0 | Same minus-one counts; GEN selects explicit header memory controls. |

[SI selector][si-select] [CIK selector][cik-select] [VI selector][vi-select]
[3.1 function alias][v3-alias] [IP discovery][discovery]
[SI writers][si-writers] [CIK writers][cik-writers]
[2.4 writers][v24-writers] [3.0 writers][v3-writers]
[4.0 writers][v4-writers] [4.4.2 writers][v442-writers]
[5.0 writers][v5-writers] [5.2 writers][v52-writers]
[6.0 writers][v6-writers] [7.0 writers][v7-writers] [7.1 writers][v71-writers]

These are native implementation selectors, not a correspondence inferred from
compiler GFX names. A paging queue is also a separate selection: the v4 backend
enables it for IP4.0.0 with firmware at least 430 or IP4.2.0 with firmware at
least 123, with an additional exclusion for an IP4.0.0 SR-IOV VF. Other cases
in that helper are false; the v4.4.2 helper always returns false. Scheduler
installation uses each instance's page queue when enabled, otherwise its normal
ring. These predicates describe routing, not a firmware-version requirement
for the generated-entry opcode.
[v4 page-queue predicate][v4-page] [VF selection][v4-init]
[v4.4.2 predicate][v442-page] [Scheduler installation][schedulers]

## Entry construction and input ownership

The native VM owner supplies the table BO, entry offset, target addresses,
count, byte increment and flags. Its table walker determines the leaf level,
fragment and entry range; the GMC owner adjusts the addresses and flags for
the selected entry format. Physically contiguous DMA pages can use the same
progression path as VRAM. A scattered physical-page run uses explicit entries.
[Physical-run selection][range-update] [Table walk][table-walk]
[Entry flags][entry-flags]

PDE installation also uses this interface: it writes one parent slot with
increment zero and therefore selects inline WRITE. Table clearing derives
its count from the table BO's byte size divided by eight, using the native
initial-entry flags. Its count is not the fragment count of a mapped range.
[PDE installation][pde-update] [Table initialization][table-clear]

| Construction | Captured input | Storage read by SDMA |
| --- | --- | --- |
| Contiguous update with `n < 3` | CPU starts with `addr \| flags`, appends low/high value pairs and advances by `incr`. | Values are inline in the WRITE packet. |
| Contiguous update with `n >= 3` | GEN carries address seed, flags and increment separately. | The packet body contains the generation inputs. |
| Scattered update | CPU resolves each DMA page address and selected flags into a 64-bit entry. | Ordinary COPY reads an entry array retained at the end of the same IB allocation. |

[WRITE/GEN choice][set-ptes] [COPY source][copy-ptes]
[Scattered-entry production][update]

The CPU-side DMA-address array is consumed while constructing entries. The
resulting entry array has a different lifetime: SDMA reads it after submission.
Its destination is the native table's GPU base plus the supplied byte offset;
each entry advances that destination by eight bytes. Neither pointer is an
application-selected translation-table handle.
[COPY address construction][copy-ptes] [Entry stride and flags][update]

The CPU update backend shares the table/flag owners but uses another write
path. It waits for table moves, resolves each address and calls
`amdgpu_gmc_set_pte_pde`, which writes `(addr & pte_addr_mask) | flags`.
Its commit executes `mb()`, then attempts to acquire the reset-domain read lock.
It issues the HDP flush only when that lock is acquired; otherwise it returns
success with a source comment attributing the flush to reset. The entry-value
expression describes the CPU helper; it is not a specification of GEN's
arbitrary-mask arithmetic.
[CPU update][cpu-update] [CPU entry construction][cpu-entry]
[CPU commit][cpu-commit]

## Generated-entry packet

Linux calls the operation `DMA_PTE_PDE_PACKET` on SI,
`SDMA_OPCODE_GENERATE_PTE_PDE` on CIK, and `SDMA_OP_GEN_PTEPDE` or
`SDMA_OP_PTEPDE` with `SDMA_SUBOP_PTEPDE_GEN` in later declarations.
The selected emitters establish its body. The generated Iceland, Tonga,
Vega, Navi, SDMA6 and SDMA7.1 headers do not provide a separate GEN body
definition from which to infer additional count or mask semantics.
[SI encoding][si-opcodes] [CIK encoding][cik-opcodes]
[Iceland opcodes][iceland-opcodes] [Tonga opcodes][tonga-opcodes]
[Vega opcodes][vega-opcodes] [SDMA7.1 opcodes][v71-opcodes]

Word numbers are zero-based DWORD offsets. Low/high pairs below each occupy
two full 32-bit words. `pe` is the native destination byte address, `addr` the
address seed, and `incr` the callback's unsigned 32-bit byte increment.

| Word | SI emission | CIK through the cited SDMA7.1 writer |
| --- | --- | --- |
| 0 | `(2<<28) \| (1<<26) \| (1<<21) \| (ndw & 0xfffff)`. | Opcode `12` at 7:0, subopcode `0` at 15:8. Other selected bits are zero except the SDMA7.1 controls below. |
| 1 | `pe[31:0]`. | `pe[31:0]`. |
| 2 | `pe[39:32]`, masked to eight bits. | Full `pe[63:32]` word. |
| 3–4 | `flags`, called the mask in the emitter. | Same operands. |
| 5–6 | `addr` when `AMDGPU_PTE_VALID` is set, otherwise zero. | `addr`, called the value in the emitter. |
| 7 | `incr`. | `incr`. |
| 8 | Zero. | Zero. |
| 9 | Absent. | `n` for CIK/2.4/3.0; `n-1` for the cited 4.0 and later writers. |

[SI generator][si-gen] [CIK generator][cik-gen] [4.0 generator][v4-gen]
[6.0 writers][v6-writers] [7.1 generator][v71-gen]

SI chooses `ndw = min(2*n, 0xffffe)` for each nine-DWORD packet, then advances
the destination by `ndw*4` bytes and the address seed by `(ndw/2)*incr`.
The later writers emit one ten-DWORD packet for the supplied count. Their full
high-address words do not establish a universal 64-bit hardware address range.
The emitted increment is 32 bits followed by zero; these sources do not supply
a signed-increment or arithmetic-overflow contract.
[SI split][si-gen] [Later body][cik-gen]

The native use establishes the intended mapping progression with valid
entry-format inputs. The source comment “mask” does not establish arbitrary
AND/OR/replacement behavior, arithmetic-versus-mask order, atomic 64-bit
publication or a zero-count operation. The dedicated COPY form's 19-bit count
below is a different representation and supplies no GEN limit. Likewise,
`AMDGPU_VM_MAX_UPDATE_SIZE = 0x3ffff` is declared with a hardware-maximum
comment but is not applied by the inspected producer or emitters.
[Native choice][set-ptes] [Declared constant][update-constant]
[Actual producer][table-walk]

### SDMA7.1 memory controls

The active SDMA7.1 generator uses the COPY-named header macros to set
`MTYPE=3` at 17:16 and `SNOOP=1` at 22. `SCOPE`, declared at 25:24, remains
zero. The emitter comments identify 3 as UC and discuss a possible future
RW/system-scope setting; that commented expression is not the emitted packet.
[Selected header][v71-gen] [Header fields][v71-pte-copy]

The matching GFXHUB12.1 cache initializer starts `GCVM_L2_CNTL5` at
`0x00003fe0` and modifies its small-fragment field. Its written value leaves
`WALKER_FETCH_PDE_MTYPE_ENABLE` bit 15 clear. The GART-enable caller invokes
this initializer only outside SR-IOV VF mode. This supplies the native walker
context for that path, rather than a promise about every firmware, reset or
virtualized state.
[Register default][walker-default] [Initializer][walker-init]
[Caller predicate][walker-caller] [Register fields][walker-masks]

## Dedicated COPY, backwards-copy and RMW declarations

Vega, Navi, SDMA6 and SDMA7.1 declare opcode `12` with suboperations
`GEN=0`, `COPY=1`, `RMW=2`, `COPY_BACKWARDS=3`. The native VM callbacks above
select GEN or ordinary COPY/WRITE. The following dedicated forms describe
the generated layouts; those callbacks provide no execution recipe for them.
Bits absent from the macro groups remain unspecified by those groups.
[Vega family][vega-pte] [Navi family][navi-pte]
[SDMA6 family][v6-pte] [SDMA7.1 family][v71-pte]

### PTEPDE_COPY

All four layouts name eight DWORDs:

| Word and bits | Named operand |
| --- | --- |
| 0, 7:0 / 15:8 | `op` / `sub_op`. |
| 1–2, 31:0 each | `src_addr_31_0` / `src_addr_63_32`. |
| 3–4, 31:0 each | `dst_addr_31_0` / `dst_addr_63_32`. |
| 5–6, 31:0 each | `mask_dw0` / `mask_dw1`. |
| 7, 18:0 | `count`. |

| Header family | Additional named fields |
| --- | --- |
| Vega | Word 0 `ptepde_op` at 31. |
| Navi | Word 0 `tmz` at 18 and `ptepde_op` at 31. |
| SDMA6 | Word 0 `tmz` at 18, `cpv` at 28, `ptepde_op` at 31. Word 7 `dst_cache_policy` at 24:22 and `src_cache_policy` at 31:29. |
| SDMA7.1 | Word 0 instead names `mtype` at 17:16, `snoop` at 22 and `scope` at 25:24. Word 7 retains the two cache-policy fields. |

[Vega COPY][vega-copy] [Navi COPY][navi-copy]
[SDMA6 COPY][v6-copy] [SDMA7.1 COPY][v71-pte-copy]

The count field's width is explicit. Count bias, mask transformation and the
operation selected by `ptepde_op` are not defined by these macros or an actual
consumer in the inspected VM path.

### PTEPDE_COPY_BACKWARDS

The four headers share this seven-DWORD named layout:

| Word and bits | Named operand |
| --- | --- |
| 0, 7:0 / 15:8 | `op` / `sub_op`. |
| 0, 29:28 / 30 / 31 | `pte_size` / `direction` / `ptepde_op`. |
| 1–2, 31:0 each | Source address low/high. |
| 3–4, 31:0 each | Destination address low/high. |
| 5, 7:0 / 15:8 | `mask_first_xfer` / `mask_last_xfer`. |
| 6, 16:0 | `COUNT_IN_32B_XFER.count`. |

[Vega backwards COPY][vega-backwards] [Navi backwards COPY][navi-backwards]
[SDMA6 backwards COPY][v6-backwards] [SDMA7.1 backwards COPY][v71-backwards]

The field name identifies 32-byte transfers, but does not define count bias,
direction polarity, `pte_size` values or first/last-mask application. A
generation-specific consumer or operation specification is needed to turn
that layout into an overlap-safe copying sequence.

### PTEPDE_RMW

| Word and bits | Named operand |
| --- | --- |
| 0, 7:0 / 15:8 | `op` / `sub_op`. |
| 1–2, 31:0 each | `addr_31_0` / `addr_63_32`. |
| 3–4, 31:0 each | `mask_31_0` / `mask_63_32`. |
| 5–6, 31:0 each | `value_31_0` / `value_63_32`. |

| Header family | Additional named fields and extent |
| --- | --- |
| Vega | Word 0 `gcc` at 19, `sys` at 20, `snp` at 22, `gpa` at 23. Seven DWORDs. |
| Navi | Adds word 0 `mtype` at 18:16 and `l2_policy` at 25:24. Seven DWORDs. |
| SDMA6 and SDMA7.1 | Also name word 0 `llc_policy` at 26 and `cpv` at 28, plus word 7 `num_of_pte` at 31:0. Eight DWORDs. |

[Vega RMW][vega-rmw] [Navi RMW][navi-rmw]
[SDMA6 RMW][v6-rmw] [SDMA7.1 RMW][v71-rmw]

SDMA7.1's RMW header retains this policy view even though its COPY header
changes. The shared name does not establish memory atomicity, mask arithmetic
or `num_of_pte` bias; none is supplied by a selected RMW caller here.

## IB partition, completion and reuse

The SDMA VM backend allocates a native job and one IB without an application
VM argument. The allocation starts at 256 DWORDs, adds two DWORDs per requested
scattered entry when allocating a replacement job, and is capped at 16,384
DWORDs. Commands grow from the beginning; CPU-generated entry data grows from
the end. Only the command prefix belongs to the submitted IB length. COPY's
source address reaches the retained data beyond that prefix.
[Allocation][allocate] [Native IB allocation][job-allocate]
[Command/data placement][update]

`copy_pte_num_dw` reserves command space rather than defining a packet's wire
length. It is 5 for SI, 7 for CIK through SDMA6, and 8 for SDMA7.0/7.1. The
7.0 writer emits eight DWORDs with `CPV=1` and a final zero word; the 7.1
writer emits seven while reserving eight. The producer also reserves seven
DWORDs for padding before assigning two DWORDs per source entry. The complete
ordinary packet layouts are in [linear copy](copy.md) and [inline writes](write.md).
[Reservation contract][interface] [7.0 writer][v7-writers]
[7.1 writer][v71-writers] [7.1 reservation][v71-callbacks]

A normal successful native flow has the following ownership boundaries:

1. The VM owner reserves the table resources and selects actual address,
   placement and flag inputs. Preparation attaches the supplied synchronization
   dependencies; each update adds dependencies for table moves.
   [Preparation][prepare] [Move dependencies][update]
2. The CPU constructs commands and any scattered-entry data in the owned IB.
   Allocation sets `AMDGPU_IB_FLAG_EMIT_MEM_SYNC`; the native submission path
   owns its cache operations and the surrounding VM/fence sequence.
   [IB allocation][ib-get] [Native submission](command-buffers.md)
3. Full IBs are committed and replaced within the same VM scheduling entity.
   Entity insertion is ordered, and scheduler selection keeps the current
   engine while its preceding job is unfinished. This is a property of this
   native entity, not an ordering rule for arbitrary application queues.
   [Commit][commit] [Entity ordering and engine selection][entity-order]
4. Commit pads the command prefix, submits the job and records its finished
   fence on the root reservation, or in `last_unlocked` for that update mode.
   An output-fence request on a non-immediate update additionally sets
   `DRM_SCHED_FENCE_DONT_PIPELINE`. Job submission retires the IB suballocation
   against its finished fence, retaining both commands and tail data.
   [Commit][commit] [Job submission][job-submit]
   [Fence-backed IB release][job-resources] [Suballocation release][ib-free]
5. The translation owner orders invalidation after the writes and establishes
   mapping readiness for the next user. Its selected TLB-fence path also links
   translation-reader completion to page-table BO retirement. The eventual
   shader or DMA users still determine payload, code and command lifetime.
   [Mapping readiness and final use](../pm4/translation.md#page-table-publication-and-final-storage-use)

### Construction and error-path distinctions

The cited rollover implementation computes remaining capacity before committing
and replacing the IB, then uses that old local value to size the first COPY in
the new allocation. With the seven-DWORD COPY reservation, an old remainder
of 15 DWORDs gives `(15-7-7)/2 = 0` entries. The SDMA6 copy writer encodes
`8*n-1`, yielding the word `0xffffffff` for that call. A smaller old remainder
can underflow the unsigned subtraction. These source paths do not establish a
safe empty-copy operation; a replacement allocation's capacity and a positive
entry chunk are distinct construction invariants.
[Rollover and chunk calculation][update] [COPY encoding][v6-writers]

Errors also have different channels. `amdgpu_vm_pte_update_flags` calls the
fallible backend through a `void` wrapper and discards its result, while the
PDE-update and table-clear paths propagate their backend returns. Dependency
insertion consumes its fence reference on both success and failure, yet the
SDMA update and shared synchronization helper put that reference again on
failure. Normal submission order therefore does not prove complete failure
propagation or rollback. These observations describe the cited implementation;
they are not a report of a fault on a particular installed driver.
[PTE wrapper][entry-flags] [PDE update][pde-update] [Table clear][table-clear]
[Dependency ownership][dependency] [Synchronization helper][sync-push]
[SDMA dependency call][update]

Mapping-ready acknowledgment, error status, translation-storage retirement
and final payload use remain separate even when all four use native fences.
The [translation chapter](../pm4/translation.md#error-propagation-and-completion-claims)
describes the invalidation transports and their additional return-value rules.

[update]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_sdma.c#L219-L298
[interface]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.h#L237-L256
[schedulers]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c#L3226-L3241
[si-select]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/si.c#L2687-L2741
[cik-select]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cik.c#L2186-L2268
[vi-select]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vi.c#L2048-L2167
[v3-alias]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v3_0.c#L1632-L1639
[discovery]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c#L2785-L2846
[si-writers]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/si_dma.c#L321-L406
[cik-writers]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cik_sdma.c#L717-L789
[v24-writers]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v2_4.c#L652-L724
[v3-writers]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v3_0.c#L926-L998
[v4-writers]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_0.c#L1585-L1659
[v442-writers]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L1179-L1253
[v5-writers]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c#L1153-L1227
[v52-writers]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v5_2.c#L1052-L1126
[v6-writers]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L1043-L1117
[v7-writers]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_0.c#L1058-L1135
[v71-writers]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c#L1048-L1141
[v4-page]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_0.c#L1736-L1751
[v4-init]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_0.c#L1761-L1783
[v442-page]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L1330-L1339
[range-update]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c#L1130-L1262
[table-walk]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_pt.c#L788-L939
[entry-flags]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_pt.c#L657-L711
[set-ptes]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_sdma.c#L173-L203
[copy-ptes]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_sdma.c#L158-L171
[cpu-update]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_cpu.c#L34-L143
[cpu-entry]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.c#L163-L178
[cpu-commit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_cpu.c#L119-L136
[si-opcodes]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sid.h#L557-L582
[cik-opcodes]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cikd.h#L495-L555
[iceland-opcodes]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/iceland_sdma_pkt_open.h#L25-L59
[tonga-opcodes]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/tonga_sdma_pkt_open.h#L25-L59
[vega-opcodes]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L25-L79
[v71-opcodes]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L25-L99
[si-gen]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/si_dma.c#L374-L406
[cik-gen]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cik_sdma.c#L774-L789
[v4-gen]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_0.c#L1643-L1659
[v71-gen]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c#L1107-L1141
[update-constant]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.h#L46-L56
[v71-pte-copy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L3437-L3525
[walker-default]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfxhub_v12_1.c#L27-L35
[walker-init]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfxhub_v12_1.c#L287-L354
[walker-caller]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfxhub_v12_1.c#L493-L531
[walker-masks]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_1_0_sh_mask.h#L41199-L41213
[vega-pte]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L1747-L1979
[navi-pte]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L3006-L3256
[v6-pte]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L3437-L3724
[v71-pte]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L3437-L3724
[vega-copy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L1747-L1811
[navi-copy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L3006-L3076
[v6-copy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L3437-L3525
[vega-backwards]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L1820-L1895
[navi-backwards]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L3085-L3160
[v6-backwards]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L3534-L3609
[v71-backwards]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L3534-L3609
[vega-rmw]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L1904-L1979
[navi-rmw]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L3169-L3256
[v6-rmw]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L3618-L3724
[v71-rmw]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L3618-L3724
[allocate]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_sdma.c#L28-L65
[job-allocate]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_job.c#L199-L277
[v71-callbacks]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c#L1242-L1247
[prepare]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_sdma.c#L77-L95
[ib-get]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c#L64-L100
[commit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_sdma.c#L106-L146
[entity-order]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/scheduler/sched_entity.c#L468-L660
[job-submit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_job.c#L372-L382
[job-resources]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_job.c#L296-L312
[ib-free]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c#L97-L100
[pde-update]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_pt.c#L626-L647
[table-clear]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_pt.c#L361-L429
[dependency]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/scheduler/sched_main.c#L671-L704
[sync-push]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_sync.c#L434-L456
[vm-select]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c#L2594-L2687
[vm-compute]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c#L2708-L2749
