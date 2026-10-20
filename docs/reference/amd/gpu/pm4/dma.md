# Command-processor DMA

DMA_DATA performs command-processor copies, fills and prefetches. It runs
through PM4, not an [SDMA queue](../sdma/README.md), and has different
completion controls from [COPY_DATA](copy.md). The caller owns both transfer
ranges and command storage, publishes prior producers before DMA reads, and
joins DMA writes before applying consumer visibility operations. Each range
and command allocation retains its own completed-use boundary.

## Packet families

The packet shape and count width have different generation boundaries in
RADV and RadeonSI:

| Selected source generation | Packet | Size and type-3 count | Final-word byte count |
| --- | --- | --- | --- |
| Before GFX7 | `CP_DMA`, opcode `0x41`. | Six DWORDs; count `4`. | Bits 20:0. |
| GFX7/GFX8 | `DMA_DATA`, opcode `0x50`. | Seven DWORDs; count `5`. | Bits 20:0, with the legacy tail controls. |
| GFX9+ | `DMA_DATA`, opcode `0x50`. | Seven DWORDs; count `5`. | Bits 25:0, with the revised tail controls. |

The selected field width is not the driver's chunk size or a physical-address
limit. PAL's cited `gfx9` backend describes its GFX10/GFX11 packet family;
its GFX12 backend has separate generated definitions. ME/PFP and MEC layouts
also differ even when their DWORD counts agree.
[RADV selection][m-dma] [RadeonSI selection][m-si-dma]
[PAL MEC layout][p11-mec] [GFX12 MEC layout][p12-mec]

## DMA_DATA representation and controls

The cited PAL GFX10/GFX11 and GFX12 forms have this common seven-DWORD shape.
Word numbers start at zero, including the header:

| Word and bits | Native field and representation |
| --- | --- |
| 0, 31:30 / 29:16 / 15:8 | Type `3`, count `5`, opcode `0x50`; the unpredicated zero-low-byte header is `0xc0055000`. |
| 0, 7:0 | MEC reserves these bits. ME/PFP name `predicate` at 0, `shaderType` at 1 and `resetFilterCam` at 2, reserving 7:3. PAL supplies caller predication, graphics shader type and no CAM reset. |
| 1 | Per-engine controls in the next table. |
| 2, 31:0 | `src_addr_lo_or_data`: source low DWORD, or the complete immediate 32-bit pattern. |
| 3, 31:0 | `src_addr_hi`: source high DWORD; the builder clears it for immediate data. |
| 4–5, 31:0 each | `dst_addr_lo`, `dst_addr_hi`: destination low/high DWORDs. |
| 6, 25:0 | `byte_count`: direct byte count. |
| 6, 26 / 27 | `sas` / `das`: source/destination address-space selectors. |
| 6, 28 / 29 | `saic` / `daic`: `0` increments the address, `1` does not increment. |
| 6, 30 | `raw_wait`. |
| 6, 31 | `dis_wc`. |

[GFX10/GFX11 MEC][p11-mec] [ME][p11-me] [PFP][p11-pfp]
[GFX12 MEC][p12-mec] [ME][p12-me] [PFP][p12-pfp]
[Earlier PFP header][p11-header] [GFX12 PFP header][p12-header]
[PAL header and operands][p11-builder] [GFX12 builder][p12-builder]

The complete word-1 partition is:

| Bits | ME/PFP | MEC |
| --- | --- | --- |
| 0 | `engine_sel`: ME enum names micro-engine `0`; PFP also names prefetch parser `1`. | Reserved. |
| 1 / 2 | `src_indirect` / `dst_indirect`. | Reserved. |
| 12:3 | Reserved. | Reserved, together with bits 2:0. |
| 14:13 | `src_cache_policy` on GFX10/GFX11; `src_temporal` on GFX12. | Same positions and source-specific names. |
| 19:15 | Reserved. | Reserved. |
| 21:20 | `dst_sel`. | `dst_sel`. |
| 24:22 | Reserved. | Reserved. |
| 26:25 | `dst_cache_policy` on GFX10/GFX11; `dst_temporal` on GFX12. | Same positions and source-specific names. |
| 28:27 | Reserved. | Reserved. |
| 30:29 | `src_sel`. | `src_sel`. |
| 31 | `cp_sync`. | Reserved. |

The earlier cache-policy enums name LRU `0`, STREAM `1`, NOA `2`, BYPASS `3`;
GFX12 names RT `0`, NT `1`, HT `2`, LU `3`. The ordinary PAL builders
zero-initialize these fields instead of accepting arbitrary policy inputs.
Equal positions do not make the two policy vocabularies interchangeable.
[Earlier controls and enums][p11-dma-enums] [GFX12 controls and enums][p12-dma-enums]
[Earlier builder][p11-builder] [GFX12 builder][p12-builder]

Linux's GC12.1 packet header additionally defines source/destination scope
shifts at 15/27. Its physical `CP_DMA_ME_CONTROL` and `CP_DMA_PFP_CONTROL`
masks give those fields two bits each. The earlier PAL layouts reserve those
positions. Physical control registers also contain VMID, TMZ and memory-log
fields; their layout is not a replacement for the PM4 control word.
[GC12.1 packet macros][l121-dma] [GC12.1 register masks][l121-dma-control]

There is a concrete disagreement in that Linux header: both
`PACKET3_DMA_DATA_CMD_RAW_WAIT` and `PACKET3_DMA_DATA_CMD_DIS_WC` shift by
30. The same revision's physical ME/PFP command masks place RAW_WAIT at 30
and DIS_WC at 31, agreeing with PAL and ROCr's packet definitions. The
duplicate macro value does not establish a new encoding for write confirmation.
[Packet macros][l121-dma] [Command masks][l121-dma-command]
[ROCr packet macros][r-dma-fields]

The principal ordering controls express different obligations:

| Control | Role in the ordinary callers |
| --- | --- |
| RAW_WAIT | Orders this operation after preceding DMA operations, including copy-after-copy read dependencies. It does not alone publish this copy to a later shader or host. |
| CP_SYNC | A PFP/ME control that stalls the selected processor for DMA completion. The corresponding bit is reserved in the cited MEC layout. |
| DIS_WC | Disables write confirmation. PAL keeps it clear for asynchronous data copies whose later barrier must guarantee arrival at the selected destination. |

[PAL transfer contract][p11-dma-info] [RADV emitter][m-dma]

## Selectors and address modes

| Selector | GFX10/GFX11 PAL values | GFX12 PAL values |
| --- | --- | --- |
| `src_sel`, word 1 bits 30:29 | `0` address using SAS; `1` GDS; `2` immediate data; `3` address using L2. | `0`, `2`, `3` have the corresponding names; `1` is not declared. |
| `dst_sel`, word 1 bits 21:20 | `0` address using DAS; `1` GDS; `2` nowhere; `3` address using L2. | `0`, `2`, `3` have the corresponding names; `1` is not declared. |
| `sas` / `das`, word 6 bits 26/27 | ME/PFP name memory `0` and register `1`; MEC names only memory `0`. | Same engine distinction. |

These are source-defined selector namespaces. The GDS names in earlier PAL
and Linux comments are not a GFX12 application contract. Ordinary memory copies
use incrementing addresses; immediate fill repeats a DWORD; destination-nowhere
prefetch has no ordinary destination payload. Non-increment and register modes
need their own access and dependency contract.
[Earlier selector enums][p11-dma-enums] [GFX12 selector enums][p12-dma-enums]
[ME/PFP address spaces][p11-me-enums] [GFX12 ME/PFP address spaces][p12-pfp-enums]

PAL's earlier universal-command-buffer `EXECUTE_INDIRECT` construction has
a separate offset form. Its spill-table update uses
`BuildDmaData<true, true>` for source and destination offsets; its incrementing
constant update uses `<false, true>` for an absolute source and destination
offset. Both select PFP and synchronization. The builder writes each indirect
operand as a 32-bit byte offset with a zero high word; the enclosing command
carries argument and spill-table bases and strides.
[Builder][p11-builder] [Spill-table copy][p-ei-dma]
[Constant copy][p-ei-constant] [Enclosing packet][p-ei-packet]

The argument and generated-command owners are described with
[register transport](registers.md#indexed-load_sh_reg_index). This is the
non-V2 execute-indirect path on a universal queue, including compute commands
recorded there. The generated MEC packet reserves the indirect bits; the
source flow does not establish a standalone indirect-pointer operation on
that engine. GFX12's separate builder has an indirect template, but the cited
call inventory supplies no ordinary caller for its enabled form.
[Actual command selection][p-ei-selection] [GFX12 builder][p12-builder]

The public PAL register-copy API specifies a source register offset in bytes
and a DWORD-aligned destination. Its four compute/universal implementations
select SAS=register and synchronization, but leave `DmaDataInfo.numBytes` zero;
the aggregate has no default count and the builders copy it directly into
`byte_count`. Those callsites demonstrate selected fields without supplying
evidence for a nonzero register transfer or MEC register-mode admission.
[Public register-copy contract][p-register-contract]
[Earlier compute caller][p11-register-copy] [Earlier graphics caller][p11-register-copy-gfx]
[GFX12 compute caller][p12-register-copy] [GFX12 graphics caller][p12-register-copy-gfx]
[Earlier input aggregate][p11-dma-info] [GFX12 input aggregate][p12-dma-info]

## Legacy CP_DMA and count revisions

Mesa's six-DWORD `CP_DMA` form has unpredicated header `0xc0044100`:

| Word and bits | Generic Mesa packet representation |
| --- | --- |
| 0 | Type 3, count `4`, opcode `0x41`. |
| 1, 31:0 | Source address low DWORD or immediate pattern. |
| 2, 15:0 | Source address high portion. |
| 2, 21:20 / 27 / 30:29 / 31 | `DST_SEL` / `ENGINE` / `SRC_SEL` / `CP_SYNC`. Other positions are unnamed in this packet object. |
| 3, 31:0 | Destination address low DWORD. |
| 4, 15:0 | Destination address high portion; upper positions unnamed. |
| 5, 20:0 | Direct `BYTE_COUNT`. |
| 5, 21 | `DISABLE_WR_CONFIRM`. |
| 5, 23:22 / 25:24 | `SRC_SWAP` / `DST_SWAP`: none `0`, byte swap in 16/32/64-bit units `1`/`2`/`3`. |
| 5, 26 / 27 | `SAS` / `DAS`: memory `0`, register `1`. |
| 5, 28 / 29 / 30 | `SAIC` / `DAIC` / `RAW_WAIT`; bit 31 unnamed. |

The legacy selector enum names source address `0`, GDS `1`, data `2`, and
destination address `0`, GDS `1`; its GDS entries also request SAS/DAS=`1`.
The separate CIK enum adds L2 address selector `3`. Engine `0` is ME and `1`
is PFP. The GFX7/GFX8 seven-DWORD emitter retains the 21-bit count and legacy
tail in word 6; from GFX9, count bits 25:0 replace those swap fields and
write-confirm disable moves to bit 31. The ordinary Mesa emitter leaves the
swap fields clear.
[Generic packet fields][m-packet-dma] [Selector and swap enums][m-packet-dma-enums]
[RADV emitter][m-dma] [RadeonSI emitter][m-si-dma]

Linux's early CIK/VI `DMA_DATA` headers name the following word-1 controls.
Their macros give shifts and enumerated values, without masking arguments
to a declared width:

| Word-1 shift | CIK/VI packet macro and named values |
| --- | --- |
| 0 | `ENGINE`: ME `0`, PFP `1`. |
| 13 / 25 | `SRC_CACHE_POLICY` / `DST_CACHE_POLICY`: LRU `0`, Stream `1`, Bypass `2`. |
| 15 / 27 | `SRC_VOLATILE` / `DST_VOLATILE`. |
| 20 | `DST_SEL`: address using DAS `0`, GDS `1`, address using L2 `3`. |
| 29 | `SRC_SEL`: address using SAS `0`, GDS `1`, data `2`, address using L2 `3`. |
| 31 | `CP_SYNC`. |

Other positions are unnamed in these packet blocks. Their word 6 keeps the
legacy 21-bit count, write-confirm disable at 21 and swap controls at 22/24.
In particular, the modern PAL NOA=`2`/BYPASS=`3` enum is not the CIK/VI
header's policy vocabulary.
[CIK packet macros][l-cik-dma] [VI packet macros][l-vi-dma]

Mesa's physical-register views differ again: GFX7 names two-bit cache
policies, ATC at 12/24 and volatile at 15/27. GFX8/GFX8.1 name memory types
at 11:10/23:22, ATC at 12/24 and one-bit cache policy at 13/25, without
volatile names. These register views are not interchangeable packet overlays.
The ordinary Mesa emitters select routing and synchronization while leaving
the optional control bits zero; they do not settle those policy differences.
[GFX7 control register][m-gfx7-control] [GFX8 control register][m-gfx8-control]
[GFX8.1 control register][m-gfx81-control]
[RADV values][m-dma] [RadeonSI values][m-si-dma]

These layouts also expose an address-width distinction. Mesa's generic packet
objects and live legacy emitter retain 16 high address bits, while Linux's
SI packet comment and Mesa's GFX6 physical DMA address-register types name
eight. Modern PAL packets carry complete high DWORDs; the inspected physical
DMA address registers expose narrower fields: Linux GC12.1 names 25 high bits
for ME and 16 for PFP, while its packet comment still labels destination-high
as eight bits. Packet storage capacity, register views, and the native VM's
usable address range are separate facts.
[Linux SI packet][l-si-dma] [Mesa GFX6 register views][m-gfx6-dma]
[Modern packet][p12-mec] [GC12.1 packet][l121-dma]
[GC12.1 address registers][l121-dma-address]

## DMA_DATA_FILL_MULTI

PAL also defines a distinct seven-DWORD packet, opcode `0x9a`, type-3 count
`5`. It is not the immediate-pattern `DMA_DATA` form:

| Word and bits | Native field |
| --- | --- |
| 0 | Type-3 header; unpredicated zero-low-byte value `0xc0059a00`. |
| 1, 0 / 31 | ME/PFP `engine_sel` / `cp_sync`; both reserved in MEC. Both ME and PFP enums name engine `0` (ME) and `1` (PFP). |
| 1, 10 | `memlog_clear`. |
| 1, 21:20 | `dst_sel`; declared value `3`, destination using L2. |
| 1, 26:25 | GFX10/GFX11 `dst_cache_policy`, or GFX12 `dst_temporal`, using the corresponding four policy values above. |
| 1, 30:29 | `src_sel`; declared value `2`, data. |
| 1, remaining positions | Reserved: 9:1, 19:11, 24:22 and 28:27, plus 0 and 31 on MEC. |
| 2 / 3, 31:0 each | `byte_stride` / `dma_count`. |
| 4–5, 31:0 each | Destination address low/high DWORDs. |
| 6, 25:0 / 31:26 | `byte_count` / reserved. |

[Earlier MEC][p11-fill-mec] [ME][p11-fill-me] [PFP][p11-fill-pfp]
[GFX12 MEC][p12-fill-mec] [ME][p12-fill-me] [PFP][p12-fill-pfp]
[Opcode definitions][p12-dma-opcodes]

The representation has no separate arbitrary-pattern DWORD. These generated
definitions do not supply a complete memory-log operation, a bias rule for
`dma_count`, or an ordinary caller's publication and retirement sequence.
The ordinary fill below has a real immediate-value operand and query-reset
callers; its pattern semantics do not transfer to this packet by name.

## Immediate DWORD fill

The ordinary incrementing fill repeats one 32-bit value across the destination.
Its operands have the same representation in the cited GFX10/GFX11 and GFX12
MEC layouts:

| Word and field | Fill operand |
| --- | --- |
| 1, `src_sel` bits 30:29 | `data = 2`; word 2 supplies the pattern instead of a source address. |
| 1, `dst_sel` bits 21:20 | `dst_addr_using_l2 = 3` selects the TC_L2 route, with the generation-specific cache meaning described below. |
| 2, `src_addr_lo_or_data` | The complete 32-bit pattern, including its high bit. |
| 3, `src_addr_hi` | Ignored for immediate data; PAL clears it, and RADV emits zero from its 32-bit value argument. |
| 4–5, `dst_addr_lo` / `dst_addr_hi` | The destination byte address. RADV requires DWORD alignment. |
| 6, `byte_count` bits 25:0 | Direct byte count, not a count of pattern DWORDs. RADV requires a multiple of four bytes. |

[GFX10/GFX11 selectors][p11-dma-selectors]
[GFX12 selectors][p12-dma-selectors] [MEC operands][p11-mec]
[GFX12 operands][p12-mec] [PAL builder][p11-builder]
[GFX12 builder][p12-builder] [RADV emitter][m-dma]
[RADV fill loop][m-dma-callers]

PAL's pipeline-query reset is a concrete fill caller. When the command buffer
can issue pipeline-statistics queries, it first joins earlier query writes with
`WriteWaitEop`; otherwise the caller supplies that ordering through semaphores.
It then fills the result range with `PipelineStatsResetMemValue32` and the
separate timestamp range with zero. Both fills request `sync=true` through the
shared PFP-layout builder. That request retains the MEC completion discrepancy
described next; the immediate-data operands do not resolve it.
[GFX10/GFX11 query reset][p11-query-reset]
[GFX12 query reset][p12-query-reset]

## MEC completion discrepancy and ordinary caller

Mesa's emitter states that MEC DMA is synchronous and sets CP_SYNC only for
graphics queues. PAL's compute copy instead uses `sync=false`, tracks active
CP blits, and describes them as asynchronous in its postamble. PAL drains them
with `BuildWaitDmaData`, which emits a zero-byte operation with `sync=true`
through a shared PFP-layout builder. The MEC layout reserves that bit. The
GFX12 PAL implementation retains the distinction.
[Mesa emission][m-dma] [PAL copy][p11-copy] [PAL postamble][p11-postamble]
[PAL builder][p11-builder] [PFP layout][p11-pfp] [MEC layout][p11-mec]
[PAL wait][p11-dma-wait] [GFX12 copy][p12-copy]
[GFX12 postamble][p12-postamble] [GFX12 wait][p12-dma-wait]
[GFX12 MEC layout][p12-mec]

Busy bookkeeping and comments do not, by themselves, prove asynchronous
hardware behavior. The contradictory descriptions do not authorize setting a
reserved MEC control. RADV supplies a complete ordinary compute dependency
without doing so:

1. Its buffer-copy planner chooses CP DMA for an eligible small, non-sparse
   copy on a compute queue.
2. The copy loop flushes pending producer caches, uses the appropriate L2
   source/destination route, and clears its logical final CP_DMA_SYNC flag.
   The stream remains marked DMA-busy.
3. A COPY-stage barrier calls the drain: DMA_DATA with zero source address,
   destination address and count. Exact GFX12 retains source/destination
   selector `3` for its MALL route; the earlier MEC forms have a zero body.
   Although requested through the logical sync flag, the MEC emitter leaves
   CP_SYNC clear.
4. The following compute dispatch emits pending consumer cache operations
   before reading the copied payload.

[Copy planner][m-meta] [Queue mapping][m-queue] [Copy and drain][m-dma-callers]
[COPY-stage handling][m-copy-stage] [Barrier call][m-barrier]
[Consumer preparation][m-consumer]

This is an in-command-buffer dependency before any kernel submission trailer.
The fill loop has a different policy: it retains final logical synchronization.
Command-buffer end also drains pending DMA. Those actual callers are more
specific evidence than the preparation helper's last-packet flag alone.
[Copy/fill distinction][m-dma-callers] [Command-buffer end][m-end]

PFP_SYNC_ME is a graphics-front-end edge, not a compute-ring operation.
PAL's command-storage postamble separately joins shader users and relies on
a KMD EOP for later cache publication. Neither service is implicit in a raw
MEC user ring. [Linux PFP distinction][l12-fence] [PAL retirement][p11-postamble]

## Combined release wait and firmware identity

PAL can combine a CP-DMA wait with RELEASE_MEM. Its defaults use the PFP
firmware **image version**, separately from the ME **feature version** used
for [CS_PARTIAL_FLUSH](memory-commands.md#compute-completion-and-firmware).
Linux exposes `ver` and `feature` separately, and PAL stores the former in
`pfpUcodeVersion`. [PAL query][p-firmware] [Linux query fields][l-firmware]

| PAL generation | Default combined-wait eligibility |
| --- | --- |
| GFX10.1/10.3 | Disabled by the generation predicate. |
| GFX11.0 | PFP image version at least 2150. |
| GFX11.5 | Excluded by the exact GFX11.0 predicate, regardless of the numeric version. |
| GFX12 | Separate implementation, PFP image version at least 2330. |

[GFX11 predicate][p11-settings] [GFX11 threshold][p11-threshold]
[GFX12 predicate][p12-settings] [GFX12 threshold][p12-threshold]

Both settings begin true; PAL modifies defaults and reads overrides afterward.
The builder consumes the resulting setting. This is firmware-informed runtime
policy, not an unoverrideable capability query.
[GFX11 default][p11-setting-default] [GFX12 default][p12-setting-default]
[Settings lifecycle][p-settings-life]

If a requested combined wait is unavailable, the compute caller emits an
explicit DMA wait before its EOP fence and equality wait. Cache work not
represented by the release follows in ACQUIRE_MEM. Split barriers can defer
DMA completion and cache work together: flushing before a writer finishes
allows it to dirty the destination again.
[GFX10/11 completion][p11-wait-eop] [GFX12 completion][p12-wait-eop]
[Split-barrier planning][p11-barrier]

## Counts, alignment and algorithm selection

The GFX9+ 26-bit byte count is direct, not length minus one. Its representation
capacity, driver chunk size and algorithm threshold are different quantities.

| Boundary | Source value or policy |
| --- | --- |
| GFX9+ field capacity | `(1 << 26) - 1` bytes; field width alone is not an execution guarantee for every firmware. |
| Earlier field capacity | `(1 << 21) - 1` bytes; Mesa's ordinary chunk policy rounds this down to 2,097,120 bytes. |
| Largest DWORD-aligned GFX9+ field value | `0x03fffffc` bytes; applying fill alignment does not select a driver chunk policy. |
| PAL compute chunks | `1 << 25` bytes, a power-of-two split policy. |
| Mesa GFX9/GFX10 chunks | 67,108,832 bytes: the field mask rounded down to 32-byte alignment. |
| Mesa GFX11+ chunks | 32,736 bytes: 32,767 rounded down to 32-byte alignment. The reason for this smaller cap is not supplied by the cited code. |
| PAL shader threshold | A region exceeding the client setting chooses a shader; its default is 64 KiB. |
| CLR PAL threshold and region splitting | Overrides the PAL setting to `0xffffffff`; its selected linear CP-copy path splits regions at `(1 << 26) - 8` bytes before PAL's own packet splitting. |
| RADV shader threshold | Normally prefers a shader from 4096 bytes. On GFX10+ dedicated-VRAM devices, non-device-local copies may use CP DMA through 65,536 bytes. Alignment and sparse-resource requirements also affect selection. |

[Field layout][p11-mec] [PAL older limit][p11-dma-limit]
[PAL GFX12 limit][p12-dma-limit] [PAL copies][p11-copy]
[GFX12 copies][p12-copy] [Mesa cap][m-dma]
[PAL algorithm][p-copy-select] [Default threshold][p-copy-default]
[CLR override][r-clr-settings] [CLR region loop][r-clr-copy]
[RADV algorithm][m-meta] [RADV thresholds][m-thresholds]

Algorithm policy belongs to the actual frontend. RadeonSI's general
clear/copy selection can choose a compute shader before CP DMA and excludes
the CP fallback for its system-scope CP/SDMA route. That does not eliminate
all GFX12 CP-DMA callers: its dedicated shader-upload path can still choose
CP DMA on a device with that engine, dedicated VRAM, and no full CPU-visible
VRAM aperture when `NO_DMA_SHADERS` is clear and `bo_offset < 0` selects a
new allocation. PAL's ordinary `CmdFillMemory` implementation also uses a
shader; its query-reset callers above are the concrete DMA-immediate fills.
[RadeonSI ordinary choice][m-si-selection] [Shader-upload choice][m-si-upload-select]
[Upload construction][m-si-upload]
[PAL ordinary fill][p-fill-selection]

### Readback counts and record boundaries

ROCr's PC-sampling readback distinguishes a sample count, an aggregate byte
extent, and each DMA packet's byte count. Its `e531543` implementation caps a
chunk at `(1 << 26) - 1` and advances both addresses and remaining bytes by
exactly the encoded count. A 64 MiB contiguous span therefore becomes
67,108,863 bytes followed by one byte. A packet boundary can split a 64-byte
sample; the host consumes records only after the complete readback, rather
than interpreting each DMA packet as a record batch. Circular-buffer wrap
splits the destination into two spans, each independently packetized.
[Chunk bound][r-sample-chunk-fixed] [Packet loop][r-sample-copy-fixed]
[Host publication][r-sample-publish-fixed] [Record consumer][r-sample-consumer-fixed]
[Record layouts][r-sample-records]

The earlier `8d57824` loop instead caps at `1 << 26` and masks that value to
26 bits while advancing by the unmasked chunk. At exactly 64 MiB it encodes
zero. The corrected bound avoids that representation mismatch; neither source
establishes a special maximum-length meaning for zero.
[Earlier loop][r-sample-copy] [Earlier bound][r-sample-chunk]
[Count macro][r-dma-fields]

Reachability depends on the caller's allocation policy. The cited
rocprofiler-sdk selects a total session buffer of 64 MiB when
`gfx_target_version / 100` is `904`, `905` or `1205`, and 4 MiB otherwise.
With the default device-buffer cap, ROCr divides that request among XCCs and
halves each share for one trap buffer: at most 32 MiB or 2 MiB respectively,
before any additional division. That SDK path does not reach the old 64 MiB
chunk boundary. The public ROCr constructor accepts larger requests in
multiples of two records; for example, 128 MiB per XCC produces a 64 MiB
trap buffer under the default 256 MiB cap, subject to successful native
allocation and sampling admission.
[SDK caller][r-sample-sdk-caller] [SDK sizes][r-sample-sdk-sizes]
[Constructor][r-sample-constructor] [Allocation arithmetic][r-sample-bounds-fixed]
[Default cap][r-sample-limit]

The aggregate extent has a separate source-width issue: that newer readback
assigns `sample_count * sample_size` to a 32-bit `to_copy` before splitting
the spans. A 4 GiB product narrows to zero even though each packet chunk is
representable. The configurable device-buffer cap has no matching upper-bound
check in its parser. This is an aggregate-count mismatch under larger custom
buffer configurations, not a DMA packet limit or a property of the SDK's small
buffers. A wider aggregate would also need a command-capacity bound: each
DMA adds seven DWORDs to a fixed 4 KiB construction array, and the queue's
staged IB accepts fewer than 4 KiB of commands.
[Aggregate width][r-sample-total-fixed] [Assignment and host clamp][r-sample-clamp-fixed]
[Configurable cap][r-sample-cap-parser] [Construction storage][r-sample-storage-fixed]
[IB capacity][r-sample-ib-size-fixed] [IB copy][r-sample-ib-fixed]

### Copy alignment and prefetch ranges

RADV can choose CP DMA for an unaligned copy; 32-byte alignment is a performance
preference. Fill requires a DWORD-aligned destination and length and repeats a
32-bit value. The older source/counter-alignment repair is restricted to
families through Carrizo or Stoney; it is not the GFX10/11/12 rule.
[Actual transfer loops][m-dma-callers] [Operation selection][m-meta]

Prefetch has a different access envelope. RADV caps a GFX11+ request at
32,736 bytes, then rounds its address and end outward to 32-byte boundaries;
an unaligned capped request can therefore describe a 32,768-byte interval.
RadeonSI's helper caps its GFX7+ request at 32,736 bytes and requires the
resulting size and address to be 32-byte aligned. From GFX9 these helpers use
destination-nowhere; earlier L2 prefetch uses a same-address transfer.
They disable write confirmation. PAL's cited prefetch also uses
destination-nowhere and disabled confirmation. The backing must cover the
actual read interval; these policies do not permit an ordinary copy to exceed
its source or destination range.
[RADV prefetch][m-prefetch] [RadeonSI prefetch][m-si-prefetch]
[PAL prefetch][p12-prefetch]

Prefetch bookkeeping is also caller-specific. RADV's helper emits directly
without setting `dma_is_busy`, whereas its command-buffer end calls a drain
that returns immediately when that flag is clear. RadeonSI's end-of-IB
path emits its drain whenever GFX7+ and `has_cp_dma` are true. Thus RADV's
ordinary-copy busy tracking is not evidence for an unconditional prefetch
drain. Source, shader and command ownership still belongs to the complete
submission path, including the separately described MEC completion question.
[RADV prefetch][m-prefetch] [RADV drain][m-dma-callers] [RADV end][m-end]
[RadeonSI end][m-si-end]

[Translation and shader warmup](translation.md) separates this payload
prefetch from `PRIME_UTCL2`. It also follows RADV's upload dependency and
post-dispatch prefetch ordering: issuing a warmup request does not publish
new code or retire the shader allocation.

## Cache routes and architecture differences

GFX10/GFX11 ordinary CP copies generally route through L2. On GFX12, the same
TC_L2 selector names route through MALL and bypass shader GL2. PAL's copy marks
shader L2 stale and its planner distinguishes both handoff directions:

| Handoff | GFX12 PAL cache action |
| --- | --- |
| Shader GL2 writer → CP reader | Write back GL2 so the bypass path sees the payload. |
| CP writer → shader GL2 reader | Invalidate GL2 and write it back to preserve other valid dirty data. |

[PAL GFX12 copy][p12-copy] [Cache planner][p12-cache]
[Client classes][p12-cache-actors] [Mesa route][m-dma]

The [cache-client planner](cache.md#gfx12-cp-and-shader-handoffs) accounts for
missing split-barrier information and retained dirty data from earlier writers.
Its deferred CP-DMA token path keeps cache work after the DMA join. Mesa's
INV_L2 likewise lowers to writeback plus invalidation. Transfer completion
or a route name alone does not establish CPU visibility or fresh shader
scalar/vector caches. [Mesa cache actions][m-cache]

Mesa selects `cp_sdma_ge_use_system_memory_scope` for exact GFX12. The cited
RADV revision rejects later generations, so this predicate is not a GFX12.1
recipe. Mesa also excludes CP DMA on compute-only GFX940+ devices. A compute
queue on a graphics-capable RDNA device is a different condition from a
compute-only CDNA device. [Common device properties][m-info]
[RADV generation boundary][m-admission]

## Input capture and final use

An ordinary memory-copy packet captures addresses and count, not source
payload. The source mapping and published version remain valid through their
final DMA reader. The destination remains valid through DMA writes and every
later dependent reader. A shader consuming only the copied destination does
not itself retain the original source. Command storage has its own final
fetch/execution owner. [Ordinary PAL copy][p11-copy]
[Command-storage retirement](command-buffers.md#publication-and-memory-ownership)

PAL's CPU-input update path crosses a different boundary: it copies the CPU
bytes into embedded GPU storage before recording DMA commands. The captured
CPU input and the embedded GPU source therefore have different last users;
the latter follows the command-storage allocation. This capture fact does
not turn an ordinary address-bearing copy into an inline payload operation.
[Earlier update capture][p11-update] [GFX12 update capture][p12-update]

CLR's PAL linear-copy path shows the surrounding ownership flow. Its CP
choice depends on SDMA being disabled or a small linear buffer-to-buffer copy.
It joins prior compute work before the copy and checks both resources' events,
waiting when the previous event belongs to another engine. That event check
also follows view owners; it is not an unconditional same-engine wait.
The caller adds both memory references to the command owner and emits the copy
regions. A copy-to-kernel barrier follows before it stamps the resource events
for both source and destination. The opposite branch selects an SDMA engine
and has its own overlap/fence path. These runtime-owned events and barriers
are not implicit services of a raw PM4 packet.
[CLR selection and resources][r-clr-copy] [CLR barrier definitions][r-clr-barriers]
[Resource event wait][r-clr-wait]

ROCr's sampling readback has another composition: wait for the selected
trap-buffer writers, perform the target-specific GL2 writeback, emit one or
more DMA packets to the circular host buffer, and reset the device's written
counter. Intermediate chunks disable confirmation; the final emitted chunk
keeps it enabled. The caller then acquires its AQL-carried completion before
publishing host-buffer progress. Target buffers, result bytes and the shared
executable IB retain their distinct owners, including the
[carrier's completed-use requirement](../aql/transfers.md).
[Producer wait and cache step][r-sample-producer] [Copy and final observer][r-sample-copy]
The newer readback holds an outer `pcs_pm4_mutex_` across both the atomic
exchange and copy submit-and-wait pairs. That caller-level owner extends
beyond the helper's submission mutex and protects the shared executable IB
through both normal completions. [Complete readback owner][r-sample-owner-fixed]

Shader completion, DMA completion, payload visibility and command retirement
remain separate edges. PAL's MEC control discrepancy, a logical busy flag,
or an intermediate write-confirm choice cannot substitute for a missing edge.

[p11-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L1315-L1387
[p11-dma-selectors]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L1262-L1287
[p12-dma-selectors]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L1112-L1135
[p11-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L2225-L2320
[p12-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L2392-L2477
[p11-query-reset]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineStatsQueryPool.cpp#L463-L521
[p12-query-reset]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PipelineStatsQueryPool.cpp#L491-L544
[p11-dma-info]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.h#L149-L178
[m-dma]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cp_dma.c#L20-L127
[p11-copy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1738-L1770
[p11-postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1228-L1263
[p11-pfp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L1520-L1595
[p11-dma-wait]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4339-L4357
[p12-copy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L1608-L1640
[p12-postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L947-L979
[p12-dma-wait]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L2481-L2499
[p12-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L1163-L1235
[m-meta]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_buffer.c#L233-L391
[m-queue]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L2095-L2104
[m-dma-callers]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cp_dma.c#L202-L391
[m-copy-stage]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1708-L1720
[m-barrier]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L16384-L16399
[m-consumer]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L15321-L15384
[m-end]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L8887-L8902
[l12-fence]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c#L4565-L4627
[p-firmware]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuDevice.cpp#L933-L985
[l-firmware]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_kms.c#L237-L244
[p11-settings]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9SettingsLoader.cpp#L855-L867
[p11-threshold]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Chip.h#L729-L730
[p12-settings]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12SettingsLoader.cpp#L165-L191
[p12-threshold]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12SettingsLoader.cpp#L45-L49
[p11-setting-default]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/settings_gfx9.json#L117-L126
[p12-setting-default]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/settings_gfx12.json#L343-L352
[p-settings-life]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxDevice.cpp#L128-L172
[p11-wait-eop]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1780-L1847
[p12-wait-eop]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L1721-L1785
[p11-barrier]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L1330-L1395
[p11-dma-limit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.h#L297-L299
[p12-dma-limit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.h#L218-L220
[p-copy-select]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/rpm/rsrcProcMgr.cpp#L5524-L5563
[p-copy-default]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/device.cpp#L539-L545
[m-thresholds]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_constants.h#L45-L55
[m-prefetch]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cp_dma.c#L132-L188
[p12-prefetch]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L2851-L2870
[p12-cache]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L375-L432
[p12-cache-actors]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L52-L75
[m-cache]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cs.c#L74-L112
[m-info]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L1152-L1218
[m-admission]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L2501-L2513
[p11-dma-enums]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L1254-L1313
[p11-fill-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L1390-L1477
[p11-me]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_me_pm4_packets.h#L1224-L1298
[p11-me-enums]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_me_pm4_packets.h#L1154-L1221
[p11-fill-me]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_me_pm4_packets.h#L1301-L1396
[p11-fill-pfp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L1598-L1693
[p12-dma-enums]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L1104-L1161
[p12-fill-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L1238-L1325
[p12-me]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_me_pm4_packets.h#L1239-L1313
[p12-fill-me]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_me_pm4_packets.h#L1316-L1411
[p12-pfp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L1402-L1476
[p12-pfp-enums]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L1333-L1399
[p12-fill-pfp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L1479-L1574
[m-si-dma]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_cp_dma.c#L13-L139
[l121-dma]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1_pkt.h#L389-L438
[l121-dma-control]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_1_0_sh_mask.h#L14447-L14483
[l121-dma-command]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_1_0_sh_mask.h#L14504-L14545
[l121-dma-address]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_1_0_sh_mask.h#L14484-L14530
[r-dma-fields]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_gpu_pm4.h#L126-L136
[p-register-contract]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L3449-L3463
[p11-register-copy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1075-L1094
[p11-register-copy-gfx]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9UniversalCmdBuffer.cpp#L9354-L9373
[p12-register-copy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L584-L601
[p12-register-copy-gfx]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12UniversalCmdBuffer.cpp#L7042-L7059
[p12-dma-info]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.h#L120-L149
[p-ei-dma]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9UniversalCmdBuffer.cpp#L9559-L9587
[p-ei-constant]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9UniversalCmdBuffer.cpp#L9640-L9661
[p-ei-packet]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L1306-L1368
[p-ei-selection]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9UniversalCmdBuffer.cpp#L10435-L10520
[m-packet-dma]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/registers/pkt3.json#L185-L253
[m-packet-dma-enums]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/registers/pkt3.json#L4-L62
[l-si-dma]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sid.h#L417-L464
[m-gfx6-dma]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/registers/gfx6.json#L8106-L8115
[p12-dma-opcodes]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_pm4_it_opcodes.h#L163-L163
[r-clr-settings]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/pal/paldevice.cpp#L912-L922
[r-clr-copy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/pal/palresource.cpp#L1521-L1698
[m-si-selection]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/nir/ac_nir_meta_cs_clear_copy_buffer.c#L357-L453
[m-si-upload]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/gfx/si_shader_binary.c#L122-L221
[p-fill-selection]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/rpm/rsrcProcMgr.cpp#L3171-L3332
[r-sample-copy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4871-L4953
[r-sample-limit]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/util/flag.h#L74-L74
[r-sample-chunk]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L102-L102
[m-si-prefetch]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_state_draw.cpp#L642-L720
[m-si-end]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_gfx_cs.c#L150-L161
[p11-update]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/rpm/gfx9/gfx9RsrcProcMgr.cpp#L443-L503
[p12-update]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/rpm/gfx12/gfx12RsrcProcMgr.cpp#L285-L330
[r-clr-barriers]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/pal/palvirtual.hpp#L488-L530
[r-sample-producer]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4840-L4869
[p11-header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L43-L58
[p12-header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L43-L58
[l-cik-dma]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cikd.h#L395-L457
[l-vi-dma]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vid.h#L271-L333
[m-gfx7-control]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/registers/gfx7.json#L9619-L9630
[m-gfx8-control]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/registers/gfx8.json#L9885-L9896
[m-gfx81-control]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/registers/gfx81.json#L9995-L10006
[m-si-upload-select]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/gfx/si_shader_binary.c#L313-L337
[r-clr-wait]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/pal/palresource.cpp#L1711-L1726
[r-sample-chunk-fixed]: https://github.com/ROCm/rocm-systems/blob/e53154361af6ec17954c64c020e7c66701669ef4/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L102-L102
[r-sample-copy-fixed]: https://github.com/ROCm/rocm-systems/blob/e53154361af6ec17954c64c020e7c66701669ef4/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4934-L4973
[r-sample-publish-fixed]: https://github.com/ROCm/rocm-systems/blob/e53154361af6ec17954c64c020e7c66701669ef4/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4996-L5015
[r-sample-consumer-fixed]: https://github.com/ROCm/rocm-systems/blob/e53154361af6ec17954c64c020e7c66701669ef4/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L5092-L5166
[r-sample-records]: https://github.com/ROCm/rocm-systems/blob/e53154361af6ec17954c64c020e7c66701669ef4/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ven_amd_pc_sampling.h#L59-L106
[r-sample-sdk-caller]: https://github.com/ROCm/rocm-systems/blob/e53154361af6ec17954c64c020e7c66701669ef4/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/pc_sampling/hsa_adapter.cpp#L309-L337
[r-sample-sdk-sizes]: https://github.com/ROCm/rocm-systems/blob/e53154361af6ec17954c64c020e7c66701669ef4/projects/rocprofiler-sdk/source/lib/rocprofiler-sdk/pc_sampling/utils.cpp#L73-L104
[r-sample-constructor]: https://github.com/ROCm/rocm-systems/blob/e53154361af6ec17954c64c020e7c66701669ef4/projects/rocr-runtime/runtime/hsa-runtime/pcs/pcs_runtime.cpp#L126-L156
[r-sample-bounds-fixed]: https://github.com/ROCm/rocm-systems/blob/e53154361af6ec17954c64c020e7c66701669ef4/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4122-L4143
[r-sample-total-fixed]: https://github.com/ROCm/rocm-systems/blob/e53154361af6ec17954c64c020e7c66701669ef4/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4731-L4733
[r-sample-clamp-fixed]: https://github.com/ROCm/rocm-systems/blob/e53154361af6ec17954c64c020e7c66701669ef4/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4848-L4880
[r-sample-cap-parser]: https://github.com/ROCm/rocm-systems/blob/e53154361af6ec17954c64c020e7c66701669ef4/projects/rocr-runtime/runtime/hsa-runtime/core/util/flag.h#L300-L306
[r-sample-storage-fixed]: https://github.com/ROCm/rocm-systems/blob/e53154361af6ec17954c64c020e7c66701669ef4/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3901-L3926
[r-sample-ib-size-fixed]: https://github.com/ROCm/rocm-systems/blob/e53154361af6ec17954c64c020e7c66701669ef4/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L98-L98
[r-sample-ib-fixed]: https://github.com/ROCm/rocm-systems/blob/e53154361af6ec17954c64c020e7c66701669ef4/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1618-L1640
[r-sample-owner-fixed]: https://github.com/ROCm/rocm-systems/blob/e53154361af6ec17954c64c020e7c66701669ef4/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4716-L5016
