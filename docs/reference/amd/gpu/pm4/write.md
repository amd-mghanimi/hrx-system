# PM4 inline writes

`WRITE_DATA` carries its input DWORDs inside the command stream and writes
them through a selected command-processor route. An ordinary memory write
advances the destination for each DWORD; a fixed-address form feeds each
DWORD to the same destination. The packet's write confirmation, the execution
stage of an earlier producer, consumer cache visibility and final storage
ownership are separate parts of the surrounding protocol.

## Applicability

PAL defines MEC, ME and PFP forms in its `gfx9` and `gfx12` families. The
earlier directory includes later GFX10/GFX11 layouts and is not a physical
target predicate. MEC is the compute-command form; ME/PFP controls belong to
graphics-capable command engines. Linux additionally supplies legacy,
GC9.4-specific and GC12.1 field views.
[Earlier MEC][p-mec] [ME][p-me] [PFP][p-pfp]
[GFX12 MEC][p12-mec] [ME][p12-me] [PFP][p12-pfp]
[Linux combined definitions][l-nvd] [GC9.4 definition][l-soc15]
[GC12.1 definitions][l121-fields]

The declared selector sets are larger than the generic PAL builders' supported
cases. Register and native queue-control forms also have driver-owned access
and scheduling contracts; their presence in a packet header does not grant
access from an arbitrary user queue. The ordinary memory forms still need
native mappings for the entire destination extent and command backing.
[Earlier builder][p-builder] [GFX12 builder][p12-builder]

## Representation

An ordinary packet contains four fixed little-endian DWORDs followed by `N`
payload DWORDs. Word 0 uses the [type-3 header](memory-commands.md#representation)
with opcode `0x37` and count `N + 2`. Word indices below are zero-based; PAL's
`ordinal1` is word 0. The 14-bit header count can represent a positive payload
of at most 16,381 DWORDs. That arithmetic is not a command allocator's
reservation limit or an all-firmware execution guarantee.
[Header construction][p-header] [Payload construction][p-builder]

| Word and field | Representation |
| --- | --- |
| 1, `dst_sel` 11:8 | Destination route from the selected engine's enum. |
| 1, `addr_incr` 16 | `0`: increment the address for successive DWORDs; `1`: write every DWORD to the same destination. |
| 1, `wr_confirm` 20 | `0`: continue without waiting for write confirmation; `1`: wait for write confirmation. |
| 1, earlier PAL `cache_policy` 26:25 | `LRU=0`, `STREAM=1`, `NOA=2`, `BYPASS=3`. |
| 1, PAL GFX12 `temporal` 26:25 | `RT=0`, `NT=1`, `HT=2`, `LU=3`; these values do not inherit the earlier cache-policy meanings. |
| 1, ME/PFP `engine_sel` 31:30 | ME declares micro engine `0`; PFP declares micro engine `0` and prefetch parser `1`. MEC reserves these bits. |
| 2, memory `dst_mem_addr_lo` 31:2 | Low part of the destination byte address; bits 1:0 are reserved, giving four-byte alignment. |
| 3, memory `dst_mem_addr_hi` | Bits 63:32 of the byte address. The full representation does not establish the process VM's usable range. |
| 4 onward | Inline payload, one DWORD per destination write. There is no 32/64-bit count selector as in COPY_DATA. |

[Earlier fields and enums][p-mec] [Graphics engine fields][p-me]
[PFP fields][p-pfp] [GFX12 fields][p12-mec] [GFX12 ME][p12-me]
[GFX12 PFP][p12-pfp]

The operand views and additional controls differ by source layout:

| Layout | Additional fields or operand views |
| --- | --- |
| Earlier PAL MEC/ME/PFP | Register `dst_mmreg_addr` is word 2 bits 17:0; the remaining bits of that view are reserved. |
| Earlier PAL MEC/ME | GDS `dst_gds_addr` is word 2 bits 15:0; PFP has no GDS view. |
| PAL GFX12 MEC/ME/PFP | Word 1 `mode` bit 21 names PF/VF disabled `0` and enabled `1`; `aid_id` occupies 24:23. |
| PAL GFX12 MEC/ME/PFP | Register `dst_mmreg_addr_lo` occupies all of word 2; `dst_mmreg_addr_hi` is word 3 bits 5:0, giving a 38-bit representation. The memory-address view of word 3 remains full-width. GDS views are absent. |

[Earlier operand views][p-mec] [GFX12 operand views][p12-mec]

Together these tables describe the named fields in each PAL view; its other
control bits are reserved. Linux's differing views below are distinct layouts,
not extra fields to merge into one packet. A two-DWORD payload is two inline
words, not an atomic 64-bit store contract.

### Destination selectors

A dash means the named PAL enum lacks that selector. It is not a statement
about all possible native firmware interfaces.

| Selector | Earlier MEC | Earlier ME | Earlier PFP | GFX12 MEC | GFX12 ME | GFX12 PFP |
| --- | --- | --- | --- | --- | --- | --- |
| `0`, `mem_mapped_register` | Yes | Yes | Yes | Yes | Yes | Yes |
| `1`, `memory_sync_across_grbm` | — | Yes | — | — | Yes | — |
| `2`, `tc_l2` | Yes | Yes | Yes | Yes | Yes | Yes |
| `3`, `gds` | Yes | Yes | — | — | — | — |
| `5`, `memory` | Yes | Yes | Yes | Yes | Yes | Yes |
| `6`, `memory_mapped_adc_persistent_state` | Yes | — | — | Yes | — | — |
| `8`, `preemption_meta_memory__GFX10` | — | — | Yes | — | — | — |

[Earlier MEC selectors][p-mec] [ME selectors][p-me] [PFP selectors][p-pfp]
[GFX12 MEC selectors][p12-mec] [ME selectors][p12-me]
[PFP selectors][p12-pfp]

PAL's earlier generic builder handles register, TC/L2, MEMORY and GDS
destinations. GFX12 handles register, TC/L2 and MEMORY. Both leave the named
MEMORY_SYNC_ACROSS_GRBM and ADC-persistent cases unimplemented after asserting
their engine classes. The preemption-metadata selector is not a generic
builder case. The builder's enum presence and its accepted switch cases are
different evidence. [Earlier switch][p-builder] [GFX12 switch][p12-builder]

### Linux routing and layout differences

Linux's SI definitions use the same destination, increment and confirmation
positions, and name ME `0`, PFP `1` and constant engine `2` in
`WRITE_DATA_ENGINE_SEL`. CIK additionally names LRU `0` and stream `1` for
`WRITE_DATA_CACHE_POLICY`. These native definitions are separate from ROCm's
diagnostic CI structure below. [SI fields][l-si] [CIK fields][l-cik]

Linux's `nvd.h` defines `AID_ID` at 23:22 and `TEMPORAL` at 25:24, while
PAL's GFX12 structures use 24:23 and 26:25. Its `MODE` remains at bit 21;
the older `CACHE_POLICY` alias at 26:25 overlaps that header's `TEMPORAL`
view. `PACKET3_WRITE_DATA__DST_MMREG_ADDR_LO` is a full DWORD and
`PACKET3_WRITE_DATA__DST_MMREG_ADDR_HI` has an eight-bit mask, versus PAL's
six-bit high part. A caller emitting zero for those fields cannot resolve
either discrepancy. These source-specific views remain separate.
[Linux fields][l-nvd] [PAL GFX12 fields][p12-mec]

The separate `soc15d.h` declares `PACKET3_WRITE_DATA__RESUME_VF_MI300`
at word 1 bit 19. That definition is not part of PAL's earlier reserved-bit
view or a general memory-write completion operation; it names a native
virtual-function control field. The macro alone supplies no complete resume
protocol. [GC9.4 field definition][l-soc15]

Linux's `gfx_v12_1_pkt.h` retains destination selector, increment and
confirmation positions but declares this different word-1 routing layout:

| Full field spelling | Bits | Declared values |
| --- | --- | --- |
| `PACKET3_WRITE_DATA__SCOPE` | 13:12 | CU `0`, SE `1`, DEVICE `2`, SYSTEM `3`. |
| `PACKET3_WRITE_DATA__MODE` | 15:14 | LOCAL_XCD `0`, REMOTE_OR_LOCAL_AID `1`, REMOTE_XCD `2`, REMOTE_MID `3`. |
| `PACKET3_WRITE_DATA__MID_DIE_ID` | 19:18 | Two-bit identifier. |
| `PACKET3_WRITE_DATA__XCD_DIE_ID` | 24:21 | Four-bit identifier. |
| `PACKET3_WRITE_DATA__TEMPORAL` | 26:25 | RT `0`, NT `1`, HT `2`, LU `3`. |
| `PACKET3_WRITE_DATA__COOP_DISABLE` | 27 | MASTER_AND_SLAVE_COOP `0`, MASTER_ONLY `1`. |

Here `PACKET3_WRITE_DATA__DST_MMREG_ADDR_HI` has a 14-bit mask, giving a
46-bit register representation with the full low DWORD. Word 4's `DATA`
macro supplies a full DWORD. This header declares selectors `1`, `3` and
`4` reserved and describes MEMORY `5` as the same as TC/L2 `2`; the earlier
GDS selector `3` therefore cannot be carried into this view. Those declarations
do not by themselves establish a remote-die memory publication sequence or a
wider register-access permission. [GC12.1 layout][l121-fields]

### Other public field views

Mesa's GFX12 MEC packet table agrees with PAL on `mode`, `aid_id`, `temporal`
and the six-bit high register operand. It therefore preserves the disagreement
with Linux's combined `nvd.h` view rather than resolving it.
[Mesa GFX12 MEC definition][m12-mec]

ROCm's public headers supply three further views. Their source names and
selection predicates matter:

| Source view | Distinction from the layouts above |
| --- | --- |
| DXG/WDDM `PM4_MEC_WRITE_DATA` | Word 1 names `resume_vf` at bit 19, with `cache_policy` at 26:25. Register/GDS/memory operands match the earlier MEC widths. The structure embeds exactly two payload DWORDs; its cache enum has `NOA=2`, `BYPASS=3`. |
| KFD test `PM4WRITE_DATA_CI` | Word 1 additionally names `atc` at 24 and `volatile_setting` at 27, with `engine_sel` at 31:30 naming ME `0`, PFP `1`, constant engine `2`. Its cache enum calls value `2` BYPASS, unlike the newer enum. Address words are stored as raw low/high DWORDs. |
| KFD test `PM4WRITE_DATA_GFX125X` | Word 1 uses scope 13:12, mode 15:14, increment 16, `md_id` 19:18, confirmation 20, `xcd_id` 24:21, temporal 26:25 and cooperative disable 27. These positions agree with the Linux GC12.1 routing view; address words are raw low/high DWORDs. |

[WDDM representation][r-wddm-fields] [WDDM enums][r-wddm-enums]
[CI representation][r-ci-fields] [CI enums][r-ci-enums]
[GFX125X representation][r125-fields]

The KFD helper selects its GFX125X form for exactly `FAMILY_GFX125X`, mapped
from engine Major 12, Minor 5. It emits MEMORY `5`, scope `3`, incrementing
addresses and confirmation. Its other branch selects the CI form, including
ATC when `hsakmt_is_dgpu()` is false and its cache-policy value `2`.
This is the diagnostic helper's selection, not a production runtime's family
admission rule. Neither its broad older branch nor a matching bit position
establishes that the views are interchangeable.
[Selected construction][r-kfd-builder] [Family mapping][r-kfd-family]

## Recording and execution

PAL zero-initializes its fixed packet body. Its earlier builder selects LRU;
GFX12 leaves mode, AID and temporal controls zero. A zero-initialized
`WriteDataInfo` selects incrementing addresses and confirmation, with
predication disabled. Its engine selector is used only on graphics-capable
engines. The selected destination view determines the builder's address
assertions.
[Builder input contract][p-info] [Earlier construction][p-builder]
[GFX12 construction][p12-builder]

The scalar overload records one value in the packet. The array overload
copies `pData` into inline command storage when non-null. Passing null still
writes the fixed body and returns the full size; it leaves payload population
to its caller. The earlier periodic builder copies a host pattern repeatedly
into that same inline payload. Pattern repetition does not itself select
fixed-address destination writes.
[Scalar, array and periodic construction][p-builder]
[GFX12 scalar and array construction][p12-builder]

PAL's default `CmdStreamReserveLimit` is 256 DWORDs. `ReserveCommands` obtains
the stream's configured reservation, while individual writers subtract their
packet headers and other commands before choosing a payload size. For example,
the GFX12 SPM upload subtracts one register-setting packet and the four-DWORD
WRITE_DATA prefix, rounds the remaining capacity down to whole mux lines,
and commits/reserves between chunks. `AllocateCommands` has a separate
exact-size path bounded by command-chunk capacity. None of these allocator
rules changes the 14-bit packet count's representable maximum.
[Default reservation][p-reserve-limit] [Stream initialization][p-reserve-init]
[Reservation method][p-reserve]
[Exact allocation][p-allocate] [SPM chunk selection][p12-spm-chunks]

Consequently, the original host array can stop being an input after recording
has captured it. The command bytes are now the GPU's input and retain their
own [publication and retirement](command-buffers.md#publication-and-memory-ownership)
obligation. A borrowed GPU source address in COPY_DATA or DMA_DATA has a
different lifetime from WRITE_DATA's captured inline words.

Write confirmation describes when the command processor can continue past
this write. It does not automatically wait for an earlier shader dispatch or
asynchronous CP DMA, nor supply the next shader's cache acquire. The
[handoff protocol](handoff.md) and [cache chapter](cache.md) identify those
separate producer and consumer operations.

## Selected memory callers

PAL's compute event writer optimizes the requested stage mask, joins pending
CP DMA when required and selects the operation for that stage. CS or
bottom-of-pipe uses `RELEASE_MEM`; other stages use a confirmed DWORD
`WRITE_DATA` to MEMORY selector `5`. An event's stage therefore comes from
the caller's sequence, not the fact that its value is written by the CP.
[Earlier compute event][p-event] [GFX12 compute event][p12-event]

Other PAL compute uses include initialization of command-allocator scratch
fences through `BuildWriteToZero`, Boolean predicate normalization and
bus-addressable memory markers. Each records inline values but retains
the destination for its actual reader. In particular, predicate state can be
read by later conditional commands and nested work; recording its initial
value does not complete that lifetime.
[Scratch allocation and initialization][p-scratch]
[Compute zero writer][p-zero] [Predicate writes][p-predicate]
[Bus marker][p-bus-marker] [Predicate reuse](conditional.md)

An update API's name does not determine its packet. PAL's
`GfxCmdBuffer::CmdUpdateMemory` delegates to the resource processor, which
copies host input into command-owned embedded GPU storage and emits
asynchronous `DMA_DATA` transfers. GFX12 delegates the transfer to
`CopyMemoryCp` and records CP-write/cache history. The embedded source remains
borrowed by DMA even though the original host input has been captured. That
is a different input lifetime from WRITE_DATA's inline payload.
[Public delegation][p-update-entry] [Earlier update implementation][p-update]
[GFX12 update implementation][p12-update] [Compute DMA emission][p12-copy]
[CP DMA ownership](dma.md) [Recorded cache history](cache.md#recorded-execution-and-cache-history)

Mesa's common `ac_emit_cp_write_data_head` always sets confirmation, emits
the caller's engine/destination/address and leaves increment and policy
controls zero. Its array wrapper appends a copy of the host words; its
immediate wrapper selects MEMORY `5`. These helper defaults do not describe
every direct WRITE_DATA encoder in Mesa. [Shared emitters][m-emitter]

### Small buffer updates

RADV's `radv_update_memory` requires four-byte-aligned destination and length,
returns for zero length, and selects inline writes for sizes below 1024 bytes
on a non-transfer queue. `radv_update_memory_cp` emits pending cache work,
then copies the input words into WRITE_DATA. Its MEC predicate is compute
queue family and `gfx_level >= GFX7`: that branch uses MEMORY `5`, while
the other branch uses MEMORY_SYNC_ACROSS_GRBM `1` with the micro engine.
The 1024-byte cutoff is RADV's policy, well below the packet's representable
payload maximum. [Update selection][m-update] [MEC predicate][m-mec]
[Policy threshold][m-update-threshold]

Larger updates and transfer queues instead copy host input into the command
buffer's upload allocation and enter the copy machinery. RADV retains older
upload allocations when that storage grows. Both routes capture the original
CPU input, but the upload route also has a separate GPU source allocation
that survives the copy's last read. [Upload allocation and capture][m-upload]

### Resident descriptor replacement

RadeonSI's bindless descriptor update is a complete shader-consumer example:

1. The CPU owns an updated descriptor shadow. The descriptor buffer remains
   resident and can still be referenced by earlier shaders.
2. `si_upload_bindless_descriptors` joins prior pixel/compute shader users
   with `SI_BARRIER_SYNC_PS | SI_BARRIER_SYNC_CS` before overwriting that buffer.
3. Each selected descriptor is copied into a confirmed, incrementing TC/L2
   WRITE_DATA payload. `si_cp_write_data` adds the destination to the command
   stream's write-use buffer list.
4. The caller records scalar-cache invalidation for following consumers. It
   additionally records L2 invalidation when
   `cp_sdma_ge_use_system_memory_scope` is true; modifying a shared cache does
   not invalidate every shader cache that can hold the old descriptor.
5. Command storage and the descriptor allocation retain their separate final
   users. A later shader can continue using the descriptor after the CP write
   itself has finished.

[Descriptor update and consumer flags][m-descriptor]
[Buffer-list and encoder wrapper][m-wrapper]
[Cache client relationships](cache.md#cache-clients-and-dependencies)
[Submission storage](command-buffers.md#publication-and-memory-ownership)

This sequence explains why confirmation cannot replace the pre-write shader
join or the post-write cache acquire. It also distinguishes updating a resident
allocation from recording a descriptor in fresh storage.

### Stage markers and progress values

RadeonSI allocates a four-byte fine-fence cell from cached system memory and
initializes it to zero. A top-of-pipe fence uses PFP WRITE_DATA; a bottom-of-pipe
fence uses `BOTTOM_OF_PIPE_TS` release carrying the 32-bit marker instead of
a clock sample. The host's unsynchronized mapping
checks whether that cell is nonzero. The selected stage gives that observation
its meaning: the early PFP marker is not completion of later shaders or the
entire submission. [Fine-fence storage, writes and observer][m-fine-fence]

Linux's GC12.1 KIQ fence emitter records one confirmed MEMORY write of
`lower_32_bits(seq)`, despite taking a 64-bit C argument. An optional second
register write requests an interrupt. The result word and notification tail
are distinct operations; observing the first does not remove the latter's
command-storage lifetime. These are driver-owned KIQ operations, not a
substitute for a shader-completion release on a compute queue.
[KIQ completion and notification][l121-fence]
[Native command retirement](command-buffers.md#native-submission-retirement)

ROCm's DXG/WDDM `BuildWriteData64Command` records two incrementing DWORDs,
MEMORY `5`, confirmation and its earlier-layout BYPASS policy `3`. Its name
describes payload width, not atomicity. The translated AQL queue uses it for
an absolute read-pointer value in its dispatch, barrier and vendor-packet
paths when platform atomics are unavailable; the atomic-capable branch uses
an atomic increment. Before reusing translated command frames, `Process`
waits a separate native submission fence as needed. In the non-atomic path it
also waits that fence before CPU-decrementing a pending completion signal.
The read cursor is not itself the frame-retirement fence.
[Two-word construction][r-wddm-write] [Dispatch caller][r-wddm-dispatch]
[Barrier caller][r-wddm-barrier] [Vendor-packet caller][r-wddm-vendor]
[Frame reuse and completion-signal observer][r-wddm-observer]
[Windows retirement](../wddm.md#hardware-queue-progress-and-command-retirement)

### Control-cell reuse and later command readers

RADV's gang-submission protocol allocates two DWORD semaphores in a
GL2-bypass buffer. The leader writes the start value; the ACE preamble waits
for it and then clears it. At the other end, the follower emits an EOP
completion value, and the leader postamble waits for that value before
clearing it. The complete join makes command-buffer resubmission safe.
WRITE_DATA performs initialization/reset on the PM4 paths; the follower's
shader-completion producer is the separate EOP operation. An SDMA leader
uses its own packet forms. [Gang storage and pre/postambles][m-gang]

ROCr's PC-sampling path for `!LargeBarEnabled()` provides another reuse
example. After swapping the active sample buffer, it waits for the old
buffer's written count, emits sample DMA copies, and resets that count with
a confirmed TC/L2 DWORD write of zero. For ISA Major 12, Minor 0 or 5, it
also emits GL2 writeback before DMA. The normal host path requests a
SYSTEM release on `ExecutePM4` and waits for its signal to reach zero before
advancing the host write offset. The reset write itself is neither the
sample copy nor the host's completion observation. On GFX9 and newer,
the supplied signal selects asynchronous submission, so the shared
command-buffer owner must also span submission and acquired completion.
[Path selection][r-pcs-selection] [Copy, reset and host observer][r-pcs]
[Generation-specific carrier and completion][r-carrier]
[DMA completion policies](dma.md#mec-completion-discrepancy-and-ordinary-caller)
[Carrier and command ownership](../aql/transfers.md#carrier-representation-and-publication)

The destination can even become future command input. On RADV's compute
device-generated-command path, WRITE_DATA installs four trailer DWORDs into
the generated IB. The recorded inline payload is patched on the CPU to
refer to the new continuation IB before submission. The current stream
chains to the generated IB, whose trailer later returns execution to that
continuation; GFX8 additionally emits L2 writeback. The GFX path uses IB2
instead of this trailer write. The destination trailer remains live through
its later CP fetch, independently of the WRITE_DATA source packet's lifetime.
[Trailer construction and chaining][m-dgc] [Generated-work caller][m-dgc-caller]
[Continuation ownership](command-buffers.md)

## Fixed-address data ports

`addr_incr=1` is a real streaming mode, not a pattern-repeat count. Mesa's SPM
mux configuration selects a register data port, uses confirmation and writes
a line of inline words with address increment disabled. The selected port
interprets successive writes; an ordinary memory destination is not implicitly
a FIFO merely because it receives repeated writes to one address.
[SPM data-port upload][m-spm]

Linux's GC12.1 register-write emitter selects no-increment and no confirmation
for KIQ, while its other ring types select confirmation. This native
queue-control write is distinct from the confirmed memory fence above;
the field alone describes neither user-queue access nor a completed register
operation. [Native ring selection][l121-wreg]

The older GFX6 and GFX7 native register emitters also omit confirmation.
GFX6 selects PFP; GFX7 selects PFP for a graphics ring and ME otherwise.
Their emitted fields describe these kernel-owned operations, independently
of the confirming memory-update helpers. [GFX6 emitter][l6-wreg]
[GFX7 emitter][l7-wreg]

Graphics streamout illustrates a different exception to common helper defaults.
For GFX9+, Mesa directly emits an unconfirmed register WRITE_DATA, then a
streamout flush event and a register wait for the update-done condition.
The complete event/wait sequence owns readiness; the preceding write alone
does not. This graphics-engine sequence does not replace a compute shader
join. [Streamout sequence][m-streamout]

## Memory and lifetime

| Storage or state | Final relevant user |
| --- | --- |
| Original CPU payload | Recording copies it into the packet; it is no longer a GPU input through that host pointer. A header-only construction still needs its payload filled before publication. |
| Command packet and inline words | Every submitted command consumer, including the containing ring/IB's native retirement and any later command tail. |
| Memory destination | The CP write and every subsequent consumer of its data, control value or predicate. Write confirmation does not retire those later readers. |
| Shader-visible descriptors or payload | Earlier readers must finish before replacement; later readers need the appropriate cache visibility and keep the allocation live through their own completion. |
| Reusable event/control cell | Its writer and all dependent waits/reads. Rearming the cell belongs after the final reference to the previous value. |

A complete memory-update flow captures the input, publishes the command
backing through its native transport, joins any users of the old destination,
performs the selected write, establishes consumer visibility and completes the
last dependent reader before reuse. Command backing is retired through its
own submission contract. PAL's retained-buffer reset requires no queued or
executing use and no remaining nested reference; its compute postamble drains
CP DMA and shader users before updating command-storage tracking.
[Retained reset contract][p-reset] [Compute postamble][p-postamble]
[Producer/consumer composition](handoff.md#owners-and-a-complete-sequence)

[p-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L2571-L2670
[p-me]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_me_pm4_packets.h#L3462-L3567
[p-pfp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L4073-L4170
[p12-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L2897-L2996
[p12-me]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_me_pm4_packets.h#L3228-L3334
[p12-pfp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L4761-L4867
[p-header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L274-L301
[p-info]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.h#L180-L193
[p-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4600-L4760
[p12-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L2079-L2203
[l-nvd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/nvd.h#L105-L157
[l-soc15]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/soc15d.h#L128-L170
[l121-fields]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1_pkt.h#L101-L150
[p-event]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1327-L1384
[p12-event]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L984-L1042
[p-scratch]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdBuffer.cpp#L445-L498
[p-zero]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1161-L1174
[p-predicate]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1391-L1441
[p-bus-marker]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L367-L381
[p-update-entry]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdBuffer.cpp#L572-L584
[p-update]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/rpm/gfx9/gfx9RsrcProcMgr.cpp#L442-L503
[p12-update]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/rpm/gfx12/gfx12RsrcProcMgr.cpp#L284-L330
[m-emitter]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L57-L86
[m-descriptor]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_descriptors.c#L1814-L1869
[m-wrapper]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_cp_dma.c#L310-L326
[m-fine-fence]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_fence.c#L201-L240
[l121-fence]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1.c#L3805-L3825
[r-wddm-write]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/dxg/wddm/cmd_util.cpp#L60-L93
[r-wddm-dispatch]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/dxg/wddm/queue.cpp#L744-L787
[m-spm]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_spm.c#L1918-L1971
[m-streamout]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf.c#L1013-L1051
[p-reset]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L2244-L2306
[p-postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1230-L1267
[m12-mec]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx12.json#L15937-L16116
[r-wddm-fields]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/include/impl/pm4_cmds.h#L212-L249
[r-wddm-enums]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/include/impl/pm4_cmds.h#L564-L587
[r-ci-fields]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/include/pm4_pkt_struct_ci.h#L28-L60
[r-ci-enums]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/include/pm4_pkt_struct_common.h#L349-L354
[r125-fields]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/include/pm4_pkt_struct_gfx125x.h#L171-L200
[r-kfd-builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/PM4Packet.cpp#L33-L90
[r-kfd-family]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/KFDTestUtil.cpp#L194-L242
[m-update]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_buffer.c#L449-L490
[m-mec]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1275-L1281
[m-update-threshold]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_constants.h#L57-L57
[m-upload]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1512-L1615
[r-wddm-barrier]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/dxg/wddm/queue.cpp#L790-L877
[r-wddm-vendor]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/dxg/wddm/queue.cpp#L879-L981
[r-wddm-observer]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/dxg/wddm/queue.cpp#L1043-L1084
[m-gang]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1346-L1493
[r-pcs-selection]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3758-L3794
[r-pcs]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4676-L4953
[m-dgc]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L759-L834
[m-dgc-caller]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L14843-L14873
[p12-copy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L1609-L1646
[l121-wreg]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1.c#L3846-L3867
[r-carrier]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1609-L1759
[l-si]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sid.h#L377-L392
[l-cik]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cikd.h#L265-L284
[l6-wreg]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v6_0.c#L2441-L2452
[l7-wreg]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v7_0.c#L3180-L3191
[p-reserve-limit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/device.h#L1022-L1022
[p-reserve]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStream.cpp#L164-L204
[p-allocate]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStream.cpp#L304-L319
[p-reserve-init]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStream.cpp#L44-L76
[p12-spm-chunks]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PerfExperiment.cpp#L2511-L2566
