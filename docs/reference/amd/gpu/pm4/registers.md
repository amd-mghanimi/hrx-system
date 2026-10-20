# Shader register transport

Shader-register packets install the live state consumed by a compute dispatch.
An inline SET carries values in command memory; a LOAD fetches values from a
separate GPU allocation. These paths have different input-storage lifetimes.
Neither path supplies the compiler ABI, joins an earlier payload producer, or
retires a previous program merely by changing its address registers.
[Compiled dispatch](dispatch.md) owns the register contents and launch ABI;
this chapter owns their transport.

## Applicability and selected paths

The layouts below follow PAL's GFX10/GFX11 and GFX12 definitions. Opcode
numbers also have a generation boundary: Linux's legacy SI header assigns
`LOAD_SH_REG` to `0x61`; its CIK header and the modern definitions below use
`0x5f`. [SI opcode][l-si-load] [CIK opcode][l-cik-load]

Compute shader state exists on both graphics and compute queues. Selecting
compute shader state in a packet is distinct from selecting MEC/ACE as its
executor. PAL's pipeline binder makes this distinction explicit: its GFX11
packed and unpacked pair paths require `IsAce == false`; ACE takes sequential
SET packets. The existence of a MEC packet definition or a device-wide feature
flag does not replace that caller predicate.
[Pipeline selection][p-pipeline]

| Mechanism | Opcode | Representation and actual use |
| --- | --- | --- |
| `SET_SH_REG` | `0x76` | Consecutive inline values. PAL uses this for its ordinary ACE program and dynamic-resource binding. |
| `SET_SH_REG_INDEX` | `0x9b` | Consecutive values with an indexed update mode. Native CU-mask composition and GFX12 graphics-queue interleave shadowing have separate owners. |
| `SET_SH_REG_PAIRS` | `0xba` | Inline offset/value pairs. PAL has a separately gated GFX11 F32 graphics path and GFX12 compute callers. |
| `SET_SH_REG_PAIRS_PACKED` | `0xbb` | Two 16-bit offsets and two values per group, plus a register-write count. PAL's GFX11 pipeline path is graphics-queue-only. |
| `SET_SH_REG_PAIRS_PACKED_N` | `0xbd` | The packed representation with a smaller firmware-selected write-count domain. |
| `LOAD_SH_REG` | `0x5f` | A memory base and one or more register ranges. PAL uses it in its software-managed graphics-queue shadow restore, including the compute register bank. |
| `LOAD_SH_REG_INDEX` | `0x63` | Memory addressing mode, data format and count. RADV loads indirect grid inputs; PAL restores ABI state and loads execute-indirect user data. |

[Opcode definitions][m-opcodes] [Dynamic and sequential binding][p-binding]
[GFX12 binding][p12-pipeline] [Shadow restore][p-shadow]
[RADV grid inputs][m-grid] [PAL ABI restoration][p-abi]

### Engine and firmware predicates

PAL's GFX10/GFX11 backend recognizes enhanced indexed loads unconditionally
for its GFX11 predicate. Otherwise it requires both GFX10.3-core-or-later and
`cpUcodeVersion >= 39`. RADV independently selects memory-loaded grid SGPRs
for `gfx_level >= GFX10_3`; below that it passes a pointer for the shader to
read. The RADV comment records a compute-queue restriction despite the opcode's
earlier presence. These are selected consumer policies, not a guarantee derived
from the opcode number. [Enhanced load predicate][p-enhanced]
[Firmware input][p-load-firmware] [Threshold][p-load-version]
[RADV selection][m-grid-selection]

PAL classifies GFX11 as RS64 when `pfpUcodeVersion >= 300`, and F32 below
that threshold. Its pair predicates are more specific:

| Selected path | Complete source predicate |
| --- | --- |
| Packed pairs | GFX11 RS64 classification, PFP firmware at least 1448, and MEC firmware at least 463. The pipeline caller additionally excludes ACE. |
| F32 unpacked pairs | GFX11 F32 classification, then PFP firmware at least 43 for Phoenix1, 9 for Phoenix2, or 41 for Strix1 and the compiled-in Strix Halo branch. The pipeline caller excludes ACE. |
| Expanded `PACKED_N` range | PFP firmware at least 1463 expands the builder's maximum from 8 to 14 register writes. This is additional to selecting the packed path. |

[RS64 and packed predicate][p-pair-predicate]
[Firmware constants][p-pair-versions] [RS64 classification][p-rs64-version]
[F32 predicate][p-f32]
[Count limits][p-packed-limits] [Builder selection][p-packed]

Mesa describes packed SH packets as requiring register shadowing. At the cited
revision its normal feature initialization enables kernel-queue shadowing only
below GFX11, while the GFX11 dedicated-VRAM branch derives packed-SH support
from that flag. That combination leaves the normal packed-SH selector clear;
the packed emitter remains conditional source evidence. GFX12 instead enables
unpacked pairs. Mesa's comment says packed forms are absent on GFX12, whereas
PAL's generated GFX12 MEC header still contains their structures. The actual
GFX12 callers cited here use unpacked pairs; the conflicting presence statements
do not establish another executable protocol.
[Mesa selection][m-pair-selection] [Mesa format contract][m-pair-contract]
[GFX12 generated packed forms][p12-packed-layout]

## Inline consecutive writes

Register addresses and packet offsets have different units. PAL uses DWORD
register addresses and subtracts `PERSISTENT_SPACE_START`, `0x2c00`.
Mesa's byte-addressed register names subtract `SI_SH_REG_OFFSET`, `0xb000`,
then divide by four. For example, PAL's `COMPUTE_USER_DATA_0` at `0x2e40`
and Mesa's byte address `0xb900` both yield packet offset `0x240`.
This is a register-space offset, not a GPU virtual address.
[PAL builder][p-set] [Native register interval][l-set]
[Mesa construction][m-set] [Byte-addressed interval][m-register-space]

For `n >= 1` consecutive values, SET uses `n + 2` DWORDs and type-3 header count
`n`. A generated two-DWORD structure describes the fixed prefix; its size is
not the complete command extent. The values follow it in increasing register
order. PAL's command-stream writer copies the caller's CPU values into command
storage during recording. That CPU array is not later borrowed by the GPU.
[Construction][p-set] [Recording copy][p-set-copy]

| Word and bits | Meaning |
| --- | --- |
| 0, 31:30 / 29:16 / 15:8 | Type 3 / packet DWORD count minus two / opcode. |
| 0, low byte | Engine-specific header controls. PAL's shared SET builder supplies `ShaderCompute`; the generated MEC header labels the low byte reserved. |
| 1, 15:0 | Starting DWORD offset in SH register space. |
| 1, 22:16 | Reserved in the MEC SET and SET_INDEX layouts. |
| 1, 27:23 | MEC `vmid_shift`; zero in the ordinary SET builder. |
| 1, 31:28 | MEC update `index`; zero in the ordinary SET builder. |
| 2 through `n + 1` | Consecutive 32-bit values. |

[MEC header][p-mec-header] [MEC SET and INDEX][p-mec-set]
[Header builder][p-header] [Ordinary SET builder][p-set]

The graphics header assigns bit 0 to predication, bit 1 to shader type
(`ShaderCompute` is 1), bit 2 to `RESET_FILTER_CAM`, and reserves bits 7:3.
PAL's ordinary compute SET clears predication and CAM reset. Mesa's cited
sequential SET macro emits the low header bits through its generic `PKT3`
helper without setting the compute bit. These are actual caller conventions;
the generated MEC reserved-byte view alone does not reconcile them.
[Graphics header][p-me-header] [PAL construction][p-set]
[Mesa construction][m-set] [Mesa header bits][m-header]

The graphics ME/PFP ordinary SET layout reserves the upper half of word 1.
Its SET_INDEX form instead reserves bits 27:16 and uses bits 31:28 for the
index. These engine-specific views cannot be collapsed into an assertion that
every non-offset bit is reserved. GFX12 retains the cited MEC SET/INDEX field
positions. [Graphics SET][p-me-set] [PFP INDEX][p-pfp-index]
[GFX12 MEC SET/INDEX][p12-mec-set]

### Indexed updates retain native policy

The MEC enums define index `0` as default, `1` as `insert_vmid`, and, for
SET_INDEX, `3` as `apply_kmd_cu_and_mask`. A VMID-insertion definition does not
make the process VM a shader-binding input. The [compute-affinity](../scheduling.md)
chapter describes the real CU-mask owner and its composition with the kernel
mask; writing a raw mask is not an equivalent update.
[MEC modes][p-mec-set]

GFX12 PFP SET_INDEX adds index `2`,
`compute_dispatch_interleave_shadow`. RadeonSI uses that indexed update for
`COMPUTE_DISPATCH_INTERLEAVE` on a graphics queue. Its compute-queue path uses
the buffered plain-register write instead. The same register's value therefore
does not fully specify the required transport. [GFX12 PFP modes][p12-pfp-set]
[Actual interleave caller][m-interleave]

## Inline register pairs

Unpacked pairs use one header followed by `n >= 1` groups of two DWORDs:
`(offset, value)`. Each offset occupies bits 15:0; bits 31:16 are reserved.
The extent is `1 + 2n` DWORDs and header count is `2n - 1`. There is no separate
register-count word. [GFX11 PFP pairs][p-pairs-layout]
[GFX12 MEC pairs][p12-pairs-layout] [PAL pair builder][p-pairs]

Packed pairs instead carry an even register-write count `r`:

| Word | Meaning |
| --- | --- |
| 0 | Header, with count `3r/2`; total extent is `2 + 3r/2` DWORDs. |
| 1 | `reg_writes_count` in bits 15:0; bits 31:16 reserved. This counts register writes, not three-DWORD groups. |
| `2 + 3i` | First offset in bits 15:0; second offset in bits 31:16. |
| `3 + 3i` | Value for the first offset. |
| `4 + 3i` | Value for the second offset. |

Both packed opcodes have this representation. `PACKED_N` is not a different
way to compute the payload length. PAL's packed builders set `RESET_FILTER_CAM`.
Mesa's format comment also calls for CAM reset and distinct adjacent offsets;
its actual pair finalizer and RadeonSI emitter set CAM reset for graphics queues
and clear it for compute queues. That queue predicate also applies to Mesa's
unpacked pairs. [Packed layouts][p-packed-layout] [PAL builder][p-packed]
[Mesa format contract][m-pair-contract] [Mesa finalizer][m-cam]
[RadeonSI packed emission][m-si-packed] [Unpacked emission][m-si-pairs]

PAL handles an odd write count by repeating the first offset and value in the
last slot, and uses ordinary SET for a single register. Its mutable builder
updates the caller's final pair before copying the payload. Its immutable
pipeline path prepares that padding beforehand. Repetition is an actual
register write, not unused padding bytes; a harmless repeated value and the
applicable adjacency rule are part of the construction.
[Mutable construction][p-packed] [Pipeline preparation][p-pair-preparation]
[Immutable recording][p-const-pairs]

The compact form is not always the smallest one. For consecutive registers,
ordinary SET needs `n + 2` DWORDs; unpacked pairs need `2n + 1`, and packed
pairs need `2 + 3*ceil(n/2)` after padding. Mesa's finalizer converts a wholly
consecutive packed sequence back to ordinary SET. These are command-byte
counts, not dispatch-latency measurements. The header's count capacity, the
register-bank span, firmware limits and each recorder's reservation size are
independent bounds. [Mesa finalization][m-finalize]

### Header selection belongs to the actual caller

PAL's GFX12 pipeline calls `BuildSetShPairs<ShaderCompute>`, but the inspected
wrapper calls its header helper without forwarding the template arguments.
That helper defaults to `ShaderGraphics` and no CAM reset. Its compute
user-data path calls `BuildSetShPairsHeader<ShaderCompute>` directly instead.
The resulting low header bits therefore differ in these two source paths.
The generated MEC header reserves that byte; these source facts do not prove
that either convention generalizes to PFP or that all defined combinations
are interchangeable. [GFX12 wrapper][p12-pairs-builder]
[Header defaults][p12-pair-defaults] [User-data caller][p12-user-data]

## Memory-backed register loads

A LOAD command carries a memory reference rather than a snapshot of the values.
The backing remains valid through every command or shader that references it.
Each input version becomes visible before its load and stays unchanged through
its final reader; an ordered update can supply the next version in the same
storage. Loading a pointer into a user register extends neither the pointed-to
allocation's lifetime nor its visibility automatically.
[Indirect input ownership](indirect.md#publication-and-last-use)

### Ranged LOAD_SH_REG

The packet has a three-DWORD prefix followed by one or more two-DWORD range
descriptors. For `k >= 1` ranges its extent is `3 + 2k` DWORDs and header count is
`1 + 2k`. The generated five-DWORD structure includes only the first range.
[Layout][p-load-layout] [Single and multiple range builders][p-load]

| Word and bits | Meaning |
| --- | --- |
| 0 | Type-3 header, opcode `0x5f`. |
| 1, 31:2 | Base-address low portion; bits 1:0 reserved for DWORD alignment. PAL writes the aligned low byte-address DWORD. |
| 2 | Base-address high DWORD. PAL's builder requires its upper 16 bits zero, a 48-bit address restriction stricter than the storage word. |
| `3 + 2i`, 15:0 | Register-range offset; bits 31:16 reserved. |
| `4 + 2i`, 13:0 | Range DWORD count; bits 31:14 reserved. |

PAL's actual caller is a universal-queue context with mid-command-buffer
preemption enabled and software-managed register shadowing. It allocates a
full SH-register-space shadow region and supplies separate graphics and compute
range lists against that same base. This is persistent context storage, not a
tightly packed per-dispatch argument array. The preamble first joins its
previous submission and performs the required stage/cache work; context setup
owns the load/shadow controls. Firmware-managed shadowing follows another path.
[Shadow allocation][p-shadow-allocation] [Restore owner][p-shadow]

The backing belongs to the queue context. PAL's queue destruction calls
`WaitIdle` before destroying its contexts; the universal-context destructor
then frees the shadow allocation. This retirement boundary spans all of that
context's submissions. [Queue destruction][p-queue-destroy]
[Shadow destruction][p-shadow-destroy]

### Indexed LOAD_SH_REG_INDEX

The ordinary direct-address, offset-and-size form is five DWORDs, with header
count `3`. The enhanced MEC layout and GFX12 preserve the following fields:

| Word and bits | Meaning |
| --- | --- |
| 0 | Type-3 header, opcode `0x63`. |
| 1, 1:0 | Addressing index: direct address `0`, indirect address `2` in the cited MEC enums. |
| 1, 31:2 | Memory-address low bits. The address is DWORD-aligned; index bits occupy its otherwise-zero low bits. |
| 2 | Memory-address high DWORD. PAL's builders require the upper 16 address bits zero. |
| 3, 15:0 | Destination register offset for offset-and-size format. |
| 3, 30:16 | Reserved. |
| 3, 31 | Data format: offset-and-size `0`, offset-and-data `1`. |
| 4, 13:0 | `num_dwords`; bits 31:14 reserved. |

[Enhanced MEC definition][p-load-index-mec] [GFX12 MEC definition][p12-load-index-mec]
[GFX12 construction][p12-load-index]

Offset-and-size loads contiguous values into contiguous registers. RADV's
indirect-grid caller requests three DWORDs; PAL's ABI restore requests one.
The source address and register offset are independent: these callers do not
allocate a complete SH shadow image. [Grid caller][m-grid] [ABI restore][p-abi]

PFP additionally defines index `1`, an offset mode. The earlier GFX10.1 PFP
view has a one-bit index; the enhanced view has two. In offset mode word 2
holds the 32-bit address offset instead of address-high. PAL's execute-indirect
user-data builder emits this mode for its PFP command path, splitting the load
when either the virtual argument layout or physical SGPR mapping ceases to be
contiguous. That caller is distinct from a direct-address ACE load.
[PFP modes and representation][p-load-index-pfp]
[Execute-indirect user-data loads][p-ei-load]

In that flow, PAL's non-V2 `EXECUTE_INDIRECT` command carries the argument
buffer's GPU address and byte stride alongside the generated IB2 reference.
The offset-mode loads live in that IB2; they are not standalone packets with
self-contained source addresses. The C++ caller exposes the enclosing argument
owner, while the CP's internal base switching is outside those definitions.
[Argument and IB2 construction][p-ei-inputs]
[Non-V2 command selection][p-ei-selection] [Enclosing packet][p-ei-packet]

The offset-and-data enum describes memory-carried register offsets and values.
PAL's builder comment calls its count a pair count, while the field is named
`num_dwords` and the implementation stores the count unchanged. The inspected
ordinary callers use offset-and-size, so they do not resolve the units for
offset-and-data. Likewise, an indirect-address enum alone does not establish
the pointer-fetch and final-storage contract of a selected caller. The direct
and offset flows above retain their actual formats instead of extending their
counts or lifetimes to these defined modes.
[Builder and format distinction][p-load-index]

## Publication, binding history and final use

Inline values become part of command storage at recording time. Memory-backed
values remain a separate input until the CP has fetched them. Both eventually
produce live register state consumed by a dispatch, but rewriting a packet or
its source array is not a way to change an already captured dispatch input.
Code and payload referenced by those registers retain their own last users.

PAL's GFX10/GFX11 ABI switch and echo helper supply a concrete producer-to-load
flow:

1. The queue preamble owns the global internal-table address in
   `COMPUTE_USER_DATA_0`. The command recorder does not know that address.
2. On the first PAL-to-HSA ABI switch in a command buffer, PAL allocates a
   scratch DWORD and dispatches its echo shader to save the register value.
3. The helper waits for that shader to become idle. Its graphics-queue path
   additionally orders the PFP behind the ME before the later load.
4. Returning to the PAL ABI loads the saved DWORD with direct-address,
   offset-and-size `LOAD_SH_REG_INDEX`. Ending the command buffer in the HSA
   ABI also emits the restore before the ordinary postamble.
5. Command-owned scratch and command bytes remain retained through the load
   and any later submitted use; ordinary postamble/submission retirement owns
   their reuse.

[ABI switch][p-abi] [Echo producer and join][p-echo]
[Final restore and postamble][p-restore-tail]
[Command allocation and retirement](command-buffers.md#native-submission-retirement)

That example is an attributed runtime flow, not a complete cache recipe for
arbitrary memory on another generation. In particular, GFX12's CP and shader
routes differ from GFX10/GFX11. A GPU-produced register-input block needs the
actual producer join and [cache transition](cache.md#cache-clients-and-dependencies)
before LOAD; a register load itself does not write back shader caches.

The complete lifetime distinction is:

| Object | Required final user |
| --- | --- |
| CPU array copied into inline SET/PAIRS | The recording copy completes its use; the copied command payload has a longer lifetime. |
| Memory block referenced by LOAD | Every register fetch, plus any shader or later command that also reads that block. |
| Persistent shadow backing | The native context's restore/save users, not just one dispatch. |
| Live program/resource/user registers | The launch and native state-transition rules, including ABI and runtime-owned state. |
| Executable and pointed-to arguments/payload | Every dispatched instruction or memory access that can reach them. |
| Command bytes | Every submitted command reference under the transport's retirement protocol. |

[Recording copy][p-set-copy] [Native shadow owner][p-shadow]
[Program and input ownership](dispatch.md#applicability-and-executable-ownership)
[Submission retirement](command-buffers.md#native-submission-retirement)

## Adjacent register mechanisms

`SET_SH_REG_OFFSET`, opcode `0x77`, is a separate four-DWORD graphics
ME/PFP representation. Word 1 has a register offset in bits 15:0, reserved
bits 29:16 and an index in bits 31:30. Its enums name `normal_operation=0`,
`data_indirect_2dw_256b=1` and `data_indirect_1dw=2`. The remaining operand
views differ by engine:

| Definition | Word 2 | Word 3 |
| --- | --- | --- |
| PFP, both cited generations | `ib_offset` / `data_offset` aliases. | A dummy-word alias, or reserved bits 15:0 and `driver_data` in bits 31:16. |
| ME, GFX10/GFX11 definition | `calculated_lo`. | `calculated_hi` in bits 15:0 and `driver_data` in bits 31:16. |
| ME, GFX12 definition | `calculated_lo`. | The split high/driver-data view plus a full-DWORD `calculated_hi` alias. |

[PFP layout][p-offset] [GFX12 PFP layout][p12-offset]
[ME layout][p-me-offset] [GFX12 ME layout][p12-me-offset]

These definitions do not supply an ordinary compute caller, source-base owner
or complete units/lifetime recipe. This packet is distinct from
`LOAD_SH_REG_INDEX`.

`COPY_DATA` can write a register value fetched from memory; RadeonSI's indirect
grid input path uses three confirmed copies. Its register destination and
confirmation semantics belong to [control-value copies](copy.md), not the
SET/LOAD count formats. [RadeonSI grid copies][m-si-grid]
[Confirmed copy construction][m-si-copy]

Configuration/context writes, context RMW, queue controls, profiling and trap
state retain their own native owners. The [dispatch binding
surface](dispatch.md#register-binding-and-launch) identifies the ordinary
program fields and their separation from native context.

[p-pipeline]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineChunkCs.cpp#L684-L751
[m-opcodes]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/sid.h#L208-L269
[p-binding]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineChunkCs.cpp#L773-L832
[p12-pipeline]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PipelineChunkCs.cpp#L862-L912
[p-shadow]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9QueueContexts.cpp#L925-L1008
[m-grid]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L15047-L15104
[p-abi]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L108-L147
[p-enhanced]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L448-L464
[p-load-firmware]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L369-L401
[p-load-version]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Chip.h#L625
[m-grid-selection]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L2796-L2797
[p-pair-predicate]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Device.h#L556-L575
[p-pair-versions]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Chip.h#L724-L726
[p-rs64-version]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/device.h#L525-L527
[p-f32]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Device.cpp#L7801-L7839
[p-packed-limits]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L212-L224
[p-packed]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L3696-L3850
[m-pair-selection]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L1174-L1188
[m-pair-contract]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/sid.h#L241-L269
[p12-packed-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L2384-L2483
[p-set]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4010-L4053
[l-set]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/soc15d.h#L435-L440
[m-set]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf.h#L298-L309
[m-register-space]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/sid.h#L18-L19
[p-set-copy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdStream.cpp#L608-L662
[p-mec-header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L43-L58
[p-mec-set]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L2165-L2237
[p-header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L284-L302
[p-me-header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_me_pm4_packets.h#L43-L57
[m-header]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/sid.h#L271-L287
[p-me-set]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_me_pm4_packets.h#L2842-L2896
[p-pfp-index]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L3507-L3540
[p12-mec-set]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L2283-L2350
[p12-pfp-set]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L4147-L4201
[m-interleave]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/gfx/si_compute.c#L731-L738
[p-pairs-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L4977-L5005
[p12-pairs-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L2353-L2381
[p-pairs]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L3934-L4004
[p-packed-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L5008-L5103
[p-pair-preparation]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineChunkCs.cpp#L115-L144
[p-const-pairs]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdStream.cpp#L807-L833
[m-finalize]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_pm4.c#L131-L194
[p12-pairs-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L340-L398
[p12-pair-defaults]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.h#L258-L267
[p12-user-data]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L1152-L1196
[p-load-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L2827-L2880
[p-load]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L3074-L3135
[p-shadow-allocation]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9QueueContexts.cpp#L814-L893
[p-queue-destroy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/queue.cpp#L398-L418
[p-shadow-destroy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9QueueContexts.cpp#L684-L702
[p-load-index-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L2668-L2745
[p12-load-index-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L1767-L1842
[p12-load-index]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L614-L661
[p-load-index-pfp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L2884-L2965
[p-ei-load]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9UniversalCmdBuffer.cpp#L9485-L9563
[p-load-index]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L3139-L3210
[p-echo]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/rpm/gfx9/gfx9RsrcProcMgr.cpp#L4170-L4215
[p-restore-tail]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1230-L1297
[p-offset]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L3541-L3593
[p12-offset]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L4204-L4257
[m-si-grid]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/gfx/si_compute.c#L434-L451
[m-si-copy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_cp_dma.c#L328-L345
[l-si-load]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sid.h#L519-L524
[l-cik-load]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cikd.h#L458-L463
[p-me-offset]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_me_pm4_packets.h#L2898-L2949
[p12-me-offset]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_me_pm4_packets.h#L2736-L2788
[p-ei-inputs]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9UniversalCmdBuffer.cpp#L10397-L10489
[p-ei-selection]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9UniversalCmdBuffer.cpp#L10499-L10520
[p-ei-packet]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L1306-L1368
[m-cam]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_pm4.c#L248-L281
[m-si-packed]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_state_draw.cpp#L1343-L1367
[m-si-pairs]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_state_draw.cpp#L1380-L1405
