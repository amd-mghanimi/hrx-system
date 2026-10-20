# PM4 control-value copies

`COPY_DATA` moves one 32- or 64-bit value through a command processor. Its
source can be memory, an immediate value, or a selected processor result such
as a clock, counter or atomic return. The destination route and confirmation
control determine how that write participates in the command stream. Shader
completion, payload visibility and storage retirement belong to the surrounding
protocol. Bulk copies use [DMA_DATA](dma.md).

## Applicability

PAL supplies distinct MEC, ME and PFP definitions in both its `gfx9` and `gfx12`
families. The `gfx9` directory includes GFX10/GFX11 definitions; its name is
not a device predicate. MEC is the compute-command form. ME/PFP fields and
callers describe graphics-capable command engines and are identified separately
below. Linux's GC12.1 header changes the routing fields again.
[Earlier MEC][p-mec] [ME][p-me] [PFP][p-pfp]
[GFX12 MEC][p12-mec] [ME][p12-me] [PFP][p12-pfp]
[GC12.1 definitions][l121-fields]

A selector declaration establishes an encoding, not every source/destination
combination, register-access permission or native queue interface. PAL's
generic builders implement fewer selector cases than their headers declare.
Linux register-read callers run through driver-owned rings; they do not
establish a user-ring register-access contract.
[Earlier builder][p-builder] [GFX12 builder][p12-builder]
[GC12.1 driver caller][l121-read]

## Representation

The packet contains six little-endian DWORDs. Word 0 uses the
[type-3 header](memory-commands.md#representation), opcode `0x40` and count
`4`. Word indices below are zero-based; PAL's `ordinal1` is word 0.

| Word and field | Representation |
| --- | --- |
| 1, `src_sel` 3:0 | Source selector. |
| 1, `dst_sel` 11:8 | Destination selector. |
| 1, `src_cache_policy` / `src_temporal` 14:13 | Source policy in the selected layout. |
| 1, `count_sel` 16 | `0`: 32 bits; `1`: 64 bits. This is a width selector, not a length. |
| 1, `wr_confirm` 20 | `0`: do not wait for confirmation; `1`: wait for confirmation. |
| 1, `dst_cache_policy` / `dst_temporal` 26:25 | Destination policy in the selected layout. |
| 1, MEC `pq_exe_status` 29 | `0`: default; `1`: phase update. The ordinary PAL builder selects default. |
| 1, ME/PFP `engine_sel` 31:30 | ME enum: micro engine `0`. Earlier PFP enum: prefetch parser `1`; GFX12 PFP names both `0` and `1`. This is a different view from MEC's bit 29. |
| 2–3, memory source | Low/high parts of a byte address. In word 2, `src_32b_addr_lo` occupies 31:2 or `src_64b_addr_lo` occupies 31:3; the low two/three bits are reserved. Word 3 is `src_memtc_addr_hi`, bits 63:32 of the address. |
| 2–3, immediate source | `imm_data` in word 2; `src_imm_data` in word 3 supplies the high half for a 64-bit value. |
| 4–5, memory destination | In word 4, `dst_32b_addr_lo` occupies 31:2 or `dst_64b_addr_lo` occupies 31:3; the low two/three bits are reserved. Word 5 is `dst_addr_hi`, bits 63:32 of the byte address. |

[Earlier layouts][p-mec] [Graphics engine views][p-me] [PFP view][p-pfp]
[GFX12 layouts][p12-mec] [GFX12 graphics][p12-me] [GFX12 PFP][p12-pfp]

Non-memory operand views and added controls are source-specific:

| Layout | Fields differing from the common memory form |
| --- | --- |
| Earlier PAL MEC/ME/PFP | Register operand `src_reg_offset` is word 2 bits 17:0; `dst_reg_offset` is word 4 bits 17:0. Remaining bits in those views are reserved. |
| Earlier PAL MEC/ME | GDS operand `src_gds_addr_lo` is word 2 bits 15:0; `dst_gds_addr_lo` is word 4 bits 15:0. PFP has no GDS operand view. |
| PAL GFX12 MEC/ME/PFP | `src_reg_offset_lo` / `dst_reg_offset_lo` occupy full words 2/4; `src_reg_offset_hi` / `dst_reg_offset_hi` occupy bits 5:0 of words 3/5, giving a 38-bit representation. Word 3 retains full-width memory/immediate views; word 5 retains its memory-address view. GDS views are absent. |
| PAL GFX12 MEC/ME/PFP | Word 1 `mode` bit 21 names PF/VF disabled `0` and enabled `1`; `aid_id` occupies 24:23. The ordinary builder leaves both zero. |

[Earlier operand views][p-mec] [GFX12 operand views][p12-mec]
[GFX12 initialization][p12-builder]

The common and generation-specific tables together describe each PAL view;
its remaining control bits are reserved. Memory-address operands have a full
64-bit representation, which does not establish the process VM's usable
address range.

Earlier policy values are `LRU=0`, `STREAM=1`, `NOA=2`, `BYPASS=3`.
GFX12 uses `RT=0`, `NT=1`, `HT=2`, `LU=3` at the same bit positions.
PAL's ordinary builders choose the zero policy for both operands. These fields
are neither shader joins nor acquire/release scope declarations. A value called
`BYPASS` in the earlier layout does not retain that meaning when interpreted
as a GFX12 temporal field. [Earlier policies][p-mec]
[GFX12 policies][p12-mec] [Earlier builder][p-builder]
[GFX12 builder][p12-builder]

### Source and destination selectors

These tables enumerate PAL's declared selector sets. A dash means that the
specified enum does not contain that selector; it does not describe every
possible firmware revision.

| Source selector | Earlier MEC / ME | Earlier PFP | GFX12 MEC / ME | GFX12 PFP |
| --- | --- | --- | --- | --- |
| `0`, `mem_mapped_register` | Yes | Yes | Yes | Yes |
| `1`, `tc_l2_obsolete` | Yes | Yes | Yes | Yes |
| `2`, `tc_l2` | Yes | Yes | Yes | Yes |
| `3`, `gds` | Yes | — | — | — |
| `4`, `perfcounters` | Yes | — | Yes | — |
| `5`, `immediate_data` | Yes | Yes | Yes | Yes |
| `6`, `atomic_return_data` | Yes | — | Yes | — |
| `7` / `8`, `gds_atomic_return_data0` / `gds_atomic_return_data1` | Yes | — | — | — |
| `9`, `gpu_clock_count` | Yes | — | Yes | — |
| `10`, `system_clock_count` | Yes | — | Yes | — |
| `12`, `exec_ind_arg_buf` | ME only, `__GFX11` spelling | Yes | — | Yes |

| Destination selector | Earlier MEC / ME | Earlier PFP | GFX12 MEC / ME | GFX12 PFP |
| --- | --- | --- | --- | --- |
| `0`, `mem_mapped_register` | Yes | Yes | Yes | Yes |
| `1`, `memory_sync_across_grbm` | ME only | — | ME only | — |
| `2`, `tc_l2` | Yes | Yes | Yes | Yes |
| `3`, `gds` | Yes | — | — | — |
| `4`, `perfcounters` | Yes | — | Yes | — |
| `5`, `tc_l2_obsolete` | Yes | Yes | Yes | Yes |
| `6`, `mem_mapped_reg_dc` | MEC only | — | MEC only | — |
| `7`, `exec_ind_spill_table` | ME only, `__GFX11` spelling | Yes | — | Yes |

[Earlier MEC selectors][p-mec] [ME selectors][p-me] [PFP selectors][p-pfp]
[GFX12 MEC selectors][p12-mec] [ME selectors][p12-me]
[PFP selectors][p12-pfp]

Mesa's GFX11 JSON tables differ from these PAL enums: ME and MEC declare
source and destination `11` as `ext32perfcntr`, which the PAL tables above do
not name. Mesa's ME table also reserves destination `7` and does not list
source `12`, while PAL's ME enum gives those its `__GFX11` indirect-execution
names. The tables are source-attributed alternatives, not a combined selector
set; their declarations alone do not resolve firmware applicability.
[Mesa GFX11 ME][m11-me] [Mesa GFX11 MEC][m11-mec]
[PAL ME][p-me] [PAL MEC][p-mec]

Mesa names source `1` `COPY_DATA_SRC_MEM` and destination `5`
`COPY_DATA_DST_MEM`. Its actual control-value callers use these encodings;
PAL's `obsolete` spelling alone is not evidence that they are unused. The
TC/L2 route is selector `2` in both directions. Mesa's shared emitter takes
two 64-bit operands and caller flags for width, confirmation and PFP selection;
it does not join producers or establish mapping lifetime. Its PFP case rejects
register operands because they would execute out of order with ME register
setting. [Mesa definitions][m-fields] [Shared emitter][m-emitter]

The earlier PAL builder handles memory source selectors `1`/`2`, immediate,
register, performance-counter and GPU-clock sources. GFX12's memory-source
case handles only selector `2`. Memory destinations are `2`/`5` in the earlier
builder and only `2` in GFX12. Both handle register/performance destinations
and `memory_sync_across_grbm`, with that sync form restricted to ME on a
graphics-capable engine; the earlier builder additionally handles GDS
destinations. Header declarations for atomic-return, system-clock or special
indirect-execution sources do not imply that these generic builders implement
them. [Earlier cases][p-builder] [GFX12 cases][p12-builder]

### GC12.1 routing and source disagreements

Linux's `gfx_v12_1_pkt.h` retains the source/destination, temporal, width,
confirmation and MEC execution-status positions above, but defines these
additional word-1 fields:

| Field | Bits | Declared values |
| --- | --- | --- |
| `SRC_SCOPE` | 5:4 | CU `0`, SE `1`, DEVICE `2`, SYSTEM `3`. |
| `MODE` | 7:6 | LOCAL_XCD `0`, REMOTE_OR_LOCAL_AID `1`, REMOTE_XCD `2`, REMOTE_MID `3`. |
| `SRC_DST_REMOTE_MODE` | 17 | Source is remote `0`; destination is remote `1`. |
| `MID_DIE_ID` | 19:18 | Two-bit identifier. |
| `XCD_DIE_ID` | 24:21 | Four-bit identifier. |
| `DST_SCOPE` | 28:27 | CU `0`, SE `1`, DEVICE `2`, SYSTEM `3`. |

[GC12.1 fields and values][l121-fields]

This table replaces PAL GFX12's mode/AID interpretation: bit 21 is part of
`XCD_DIE_ID`, and `MODE` is now a two-bit field at 7:6.

GC12.1's register high-part mask is 14 bits, giving a 46-bit representation
with its low word. Linux's separate `nvd.h` instead declares an eight-bit high
part, while PAL's GFX12 structures use six bits. These are distinct source
layouts; the wider macro is not proof that all GFX12 devices accept the wider
register operand. [Linux combined definitions][l-fields]
[GC12.1 definitions][l121-fields] [PAL GFX12][p12-mec]

The inspected GC12.1 register-read emitter normalizes its register offset and
sets only source `0`, destination `5` and confirmation `1`. Its new routing
and scope fields are zero, and the source high word is zero. That caller
therefore supplies no sequence for remote-die copies or SYSTEM-scope payload
publication. A field name does not supply the missing mapping, producer or
native-access contract. [GC12.1 native caller][l121-read]

## Execution and ordering

Width, confirmation and sampling stage answer separate questions. Count bit
16 selects the transferred result width; it does not promise an indivisible
update to concurrent observers. Confirmation waits for the selected write
confirmation, not prior shaders or CP DMA. The stage-aware PAL callers perform
those joins or select `RELEASE_MEM` before they can fulfill a later-stage
write. [Earlier stage selection][p-stages]
[GFX12 stage selection][p12-stages]

| Selected caller | Emitted operation and surrounding owner |
| --- | --- |
| PAL compute immediate at a CP stage | Source `5`, destination `5` in the earlier implementation or `2` in GFX12, selected 32/64-bit width, confirmation `1`. After stage-mask optimization, CS/bottom-of-pipe requests use RELEASE_MEM instead. Pending CP DMA has its own join. |
| PAL compute timestamp at a CP stage | GPU-clock source `9`, 64-bit width. Earlier destination `5` is confirmed; GFX12 destination `2` is unconfirmed. Later-stage timestamps use RELEASE_MEM. |
| PAL sampled performance counters | Counter source `4` to TC/L2 destination `2`, confirmed. A 64-bit counter can be captured by two 32-bit copies; the sample/stop sequence gives the pair its snapshot meaning. |
| ROCr sampled atomic return | Source `6` to TC/L2 destination `2`, 64-bit width, confirmation `1`, immediately after the selected SWAP64. The AQL carrier's SYSTEM release and completion signal precede the CPU acquire/read. |

[Earlier immediate and timestamp][p-stages]
[GFX12 immediate and timestamp][p12-stages]
[PAL counter copy][p-counter] [Paired counter words][p-counter64]
[ROCr paired commands][r-atomic] [ROCr result observer][r-result]
[Counter sample protocol](counters.md#reset-start-sample-and-stop)
[Atomic-return protocol](atomics.md#retrieving-the-previous-value)

The [timing chapter](timing.md#gfx12-timestamp-confirmation) traces GFX12's
timestamp and scratch-confirmation barriers. Those extra operations are
material: the timestamp copy alone does not establish when a host or another
engine may consume the result. The declared `system_clock_count` source has
a different selector from the actual `gpu_clock_count` callers; its enum name
does not establish clock correlation or replace the timestamp protocol.

### Resident control values

PAL's nested compute call is a memory-to-memory use. When both the parent
predicate address and the child's inherited-predicate address exist, it emits
a confirmed 32-bit TC/L2-to-TC/L2 copy before calling the child. The caller
also tracks the child's embedded data, scratch and commands. Copy completion
therefore precedes the child's predicate use, while the child's storage remains
borrowed through that use. Reusable submissions retain the separate
[predicate publication and retirement contract](conditional.md#submission-and-mutable-predicate-storage).
[Earlier nested caller][p-nested] [GFX12 nested caller][p12-nested]

RadeonSI has a different memory-source example: when a compute shader uses
the number-of-workgroups system value and the dispatch is indirect, it copies
three 32-bit values from the indirect buffer into the corresponding user-data
registers. The source route is memory `1`, destination register `0`, with
confirmation. Its wrapper adds source and destination BOs, when present, to
the submission's buffer list with read/write usage. The caller converts the
byte-addressed register name to a DWORD register offset; memory offsets remain
bytes. The [indirect-dispatch protocol](indirect.md) owns the producer-to-CP
visibility dependency. [Actual compute caller][m-compute]
[Operand and buffer-list wrapper][m-wrapper]

RADV's older compute-indirect alignment path copies the three dispatch-count
DWORDs to command-buffer upload storage aligned to 32 bytes, then dispatches
from that storage. It selects three confirmed memory `1` to memory `5`
copies only when the compute queue's indirect address needs that alignment and
`has_async_compute_align32_bug` is set. Mesa sets that flag for `GFX7`.
The 32-byte allocation alignment belongs to the subsequent indirect-dispatch
consumer; it does not change COPY_DATA's four-byte operand alignment. Upload
storage remains borrowed by the dispatch after the copies finish.
[Dispatch caller][m-aligned-dispatch] [Device predicate][m-align-predicate]

### Unconfirmed counter readback

RadeonSI's counter path requests 64-bit PERF-to-memory `5` copies without
confirmation. Its synthetic zero counters use the same width and destination
with an immediate source. The sequence is:

```text
start marker = 1 → measured work
  → RELEASE_MEM(marker = 0) → equality wait
  → sample / selected idle / stop → unconfirmed counter copies
  → submission completion observed by synchronized BO mapping
  → CPU reads accumulated results
```

The release-zero marker precedes the counter copies. It orders sampling after
the measured work; it cannot indicate that the later result stores are ready.
`si_pc_query_get_result` instead uses a synchronized read mapping. A nonblocking
request returns not-ready when that mapping cannot yet be supplied. The winsys
map joins outstanding BO writers, flushing the current command stream when
necessary. [Start, stop and read][m-si-sample]
[Actual suspension order][m-si-suspend] [Result reader][m-si-result]
[Synchronized map][m-map]

Query reset retains the oldest buffer only when it is idle for both reads and
writes; otherwise it drops that query reference and allocates replacement
storage. Thus result width, early stop marker and query reset are not alternate
completion signals. [Reset and allocation][m-reset] [Idle query][m-idle]

RADV's performance-query path takes another approach: selected counter reads
are confirmed 64-bit PERF-to-TC/L2 copies. The end-sampling path then writes
each pass's availability value, and the CPU getter checks all pass flags when
reporting availability. The earlier shader-completion marker precedes sampling;
it is distinct from these later result-availability stores. The
[performance-query chapter](counter-queries.md) describes pass serialization,
profiling ownership and result interpretation.
[Confirmed samples][m-radv-sample] [Sampling and availability][m-radv-stop]
[Preceding execution join][m-radv-end] [CPU availability check][m-radv-result]

### Driver-owned register readback

Linux's KIQ register-read path borrows a driver writeback slot, emits a
confirmed 32-bit register-to-memory copy and appends a native polling fence.
On successful completion it performs a CPU memory barrier, reads the slot and
releases it. The native owner supplies the mapping and observer; confirmation
alone is not the CPU wait. When MES scheduling is ready, the generic accessor
takes a separate MES path instead. [KIQ owner][l-read-owner]
[GFX11 emitter][l11-read] [GFX12 emitter][l12-read]
[GC12.1 emitter][l121-read]

The GFX9 clock caller uses source `9`, destination `5`, 64-bit width and
confirmation, followed by the same polling-fence/readback pattern. Its actual
selection is GC `9.0.1` with `amdgpu_sriov_runtime` true; other cases in that
function read the clock through different paths. This is a native-driver
example of a selected clock source, not a general user-queue submission recipe.
[Clock copy and owner][l-clock] [Selection][l-clock-select]

## Memory and lifetime

An immediate is captured in command words when the stream is built. A memory
source is a borrowed GPU mapping read during execution; a register or atomic
return is a selected processor state, not a host pointer. Destination storage
remains owned through its last writer and final reader. Address fields do not
retain any of those objects. [Builders][p-builder]
[GFX12 builder][p12-builder] [Mesa emitter][m-emitter]

PAL asserts four-byte alignment for a 32-bit memory operand and eight-byte
alignment for a 64-bit operand. Neither assertion establishes the complete readable
source extent. The separate
[source-backing observation](memory-commands.md#copy-source-backing) describes
that deployment-specific distinction without treating result width as fetch
width. [Address checks][p-builder]

A complete memory-copy sequence has these owners:

1. The allocation owner supplies source and destination GPU mappings with the
   required access path, readable extent and alignment.
2. The producer completes and publishes the source to the selected CP route.
   Command publication separately makes the six packet words visible.
3. The CP reads the source and writes the selected result. The stream's
   confirmation and dependency operations establish the execution edge used
   by its next consumer.
4. A later shader, engine or host acquires the result through that mapping's
   visibility protocol. Its final read releases result storage.
5. The submission owner retires command storage through the transport's actual
   final access, independently of result readiness.

[Cross-queue dependencies](handoff.md), [cache control](cache.md),
[native publication](publication.md) and
[command-storage retirement](command-buffers.md) supply those surrounding
contracts. In the ROCr atomic-return flow, the external completion wait proves
result readiness but the helper's mutex alone does not prove retention of its
shared executable IB; the [carrier ownership analysis](atomics.md#retrieving-the-previous-value)
keeps that separate obligation explicit.

[p-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L730-L918
[p-me]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_me_pm4_packets.h#L724-L912
[p-pfp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L919-L1081
[p12-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L665-L844
[p12-me]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_me_pm4_packets.h#L621-L798
[p12-pfp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L836-L1010
[p-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L980-L1174
[p12-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L2238-L2386
[p-stages]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L397-L516
[p12-stages]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L198-L324
[l-fields]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/nvd.h#L251-L317
[l121-fields]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1_pkt.h#L234-L300
[l121-read]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1.c#L3827-L3844
[m-emitter]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L236-L264
[m-fields]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/sid.h#L99-L117
[p-nested]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1637-L1680
[p12-nested]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L632-L667
[m-compute]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/gfx/si_compute.c#L434-L451
[m-wrapper]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_cp_dma.c#L328-L345
[m-si-sample]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_perfcounter.c#L133-L215
[m-si-suspend]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_perfcounter.c#L282-L310
[m-si-result]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_perfcounter.c#L356-L385
[m-map]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_bo.c#L370-L466
[m-reset]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_query.c#L505-L565
[m-idle]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_pipe.h#L2151-L2156
[l-read-owner]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_gfx.c#L1224-L1297
[l11-read]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L6347-L6362
[l12-read]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c#L4692-L4707
[l-clock]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_0.c#L4259-L4331
[l-clock-select]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_0.c#L4333-L4370
[m11-me]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx11.json#L7805-L7892
[m11-mec]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx11.json#L12704-L12791
[p-counter]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdStream.cpp#L1604-L1624
[p-counter64]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L3948-L3964
[r-atomic]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4723-L4773
[r-result]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4781-L4804
[m-aligned-dispatch]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L15085-L15135
[m-align-predicate]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L1028-L1032
[m-radv-sample]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_perfcounter.c#L542-L564
[m-radv-stop]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_perfcounter.c#L610-L661
[m-radv-end]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_perfcounter.c#L728-L757
[m-radv-result]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L2327-L2342
