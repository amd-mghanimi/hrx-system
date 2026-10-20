# SDMA ordering and completion

Packet order alone does not establish completion of dependent transfers.
ROCr's ordinary copy path explicitly allows serial COPY/WRITE operations to
overlap and selects FENCE, rather than WRITE, for completion publication. PAL
and RADV insert an explicit drain at transfer dependency boundaries. These
source contracts distinguish execution ordering from the separate [cache
visibility](cache.md) and storage lifetime obligations. [ROCr completion
selection][rocr-completion]

## NOP representation and framing

The classic `SDMA_PKT_NOP` header has the same fields in Linux's Iceland,
Tonga, Vega10, Navi10, SDMA6.0 and SDMA7.1 definitions and PAL's GFX10/GFX12
structures. PAL's `BuildNops` defines the count as body DWORDs, excluding the
header. Linux's CIK `SDMA_NOP_COUNT` independently supplies the same 14-bit
mask and shift. [Iceland][nop-iceland], [Tonga][nop-tonga],
[Vega10][nop-vega10], [Navi10][nop-navi10], [SDMA6.0][nop-sdma6],
[SDMA7.1][nop-sdma71], [PAL GFX10][pal-nop-fields],
[PAL GFX12][pal12-nop-fields], [PAL builder][pal-nop], [CIK macro][nop-cik]

| Word / bits | Native field | Meaning in the counted form |
| --- | --- | --- |
| DWORD 0, 7:0 | `op` / `SDMA_OPCODE_NOP` / `SDMA_OP_NOP` | Opcode 0. |
| DWORD 0, 15:8 | `sub_op` | Zero in the cited builders. |
| DWORD 0, 29:16 | `count` / `SDMA_NOP_COUNT` | Number of following body DWORDs; mask `0x3fff`. |
| DWORD 0, 31:30 | Unnamed in the PAL structures | Zero in the cited builders. |
| DWORD 1, 31:0 | `DATA0_UNION.data0` / `SDMA_PKT_NOP_DATA0_data0`, when a body is present | First body DWORD, followed by any remaining counted body. |

A total extent of `D` DWORDs encodes `(D - 1) << 16`. The representable
positive extent is 1 through 16,384 DWORDs, at most 64 KiB including the
header. A one-DWORD packet encodes as zero and has no body. These are field
bounds; the firmware predicate and the caller's available command space still
govern an actual packet. The body count is neither a byte count nor a delay.

Mesa's generic `SDMA_PACKET` packing macro retains 16 upper bits and its
diagnostic parser reads the whole upper halfword as a NOP count. Its actual
SDMA NOP producers below emit header-only packets. Those generic helpers do
not establish a 16-bit native count in place of the 14-bit packet definitions.
[Packing macro][mesa-packing], [diagnostic parser][mesa-parser]

PAL's C structure includes `DATA0` and therefore occupies two DWORDs, but its
real preamble and barrier callers request `WriteNops(..., 1)`. The builder
writes into a larger `CmdStream` reservation, and `CommitCommands` retains the
requested logical extent while reclaiming the unused reservation. Structure
size, reserved host storage and submitted packet size are distinct quantities.
[Preamble][pal-preamble], [reservation][pal-reserve], [commit][pal-commit]

The legacy SI DMA encoding differs: Linux selects `DMA_PACKET_NOP` opcode
`0xf` in bits 31:28 with the other macro arguments zero, producing
`0xf0000000`. Its IB padder emits repeated one-DWORD headers. The generic
legacy `DMA_PACKET` macro's low count argument does not by itself define a
variable-length NOP body. [SI macro][nop-si], [selected ring header][nop-si-ring],
[SI IB padding][nop-si-pad]

### Firmware and padding owners

Linux's counted, or burst, NOP selection tests the SDMA instance's
`burst_nop` flag. Its firmware initialization enables that flag for
`ucode_feature_version >= 20`; the firmware build's `ucode_version` is a
different field. The CIK inserter already uses this predicate: its first word
contains `SDMA_NOP_COUNT(D - 1)` when enabled, and all other words remain zero.
Without it, the same extent consists of `D` separate zero-valued NOP headers.
Both forms occupy all `D` DWORDs. [Firmware selector][nop-firmware],
[CIK firmware selector][nop-cik-firmware], [CIK inserter][nop-cik-insert]

PAL's builder comment describes variable-length NOPs as starting with OSS4.
That attribution and Linux's firmware-selected CIK path retain their separate
owners; a matching count field or a compiler target name cannot replace the
native firmware/interface condition.

Wrap padding, minimum submission size and IB/ring alignment are separate
framing requirements. ROCr uses repeated zero words at ring wrap but counted
NOPs for its selected submission-size padding. Linux's kernel-owned SDMA
IB-padding helpers append up to seven NOP DWORDs to reach eight-DWORD
alignment. AMDGPU's userspace DMA query separately reports four-byte IB-size
alignment, while final native ring commit uses the ring's own alignment and
NOP callback. [IB-padding helper][nop-cik-pad], [userspace query][nop-user-align]
The [queue publication](publication.md#wrap-and-padding) and
[command-buffer](command-buffers.md#native-generation-and-transport) chapters
give those owners and units. Padding proves which command bytes a producer
publishes; a dependency drain additionally needs an execution contract from
the consuming path.

Mesa's winsys padding policies all emit repeated header-only SDMA NOPs, but
their alignment and empty-IB behavior differ:

| Producer | SDMA IB framing |
| --- | --- |
| RADV AMDGPU | Align to 16 DWORDs; an empty IB becomes 16 NOP DWORDs. Its SDMA path does not use the counted PM4 padding helper. |
| Gallium AMDGPU | Align to 16 DWORDs; an empty IB stays empty. The selected header is `0xf0000000` for `gfx_level <= GFX6`, zero otherwise. |
| Gallium Radeon | Align to eight DWORDs; an empty IB stays empty. It uses the same legacy-versus-zero header choice. |

[RADV emitter][mesa-pad-emitter], [RADV finalizer][mesa-pad],
[AMDGPU alignment policy][mesa-align], [Gallium AMDGPU][mesa-gallium-pad],
[Gallium Radeon][mesa-radeon-pad]

### Command annotations and empty submissions

PAL's `CmdNop` copies a caller's annotation into the NOP body. For a payload
of `P` DWORDs, it submits `P + 2` DWORDs: header, zero `DATA0`, then the copied
payload. The header count is `P + 1`. The public API describes these bytes as
debug data skipped by the processor; their storage belongs to the command
stream, rather than a separately borrowed GPU payload. Its named
`MaxPayloadSize` is 254 DWORDs and its ordinary command reservation is
256 DWORDs, distinct from the header's representable ceiling.
[Annotation contract][pal-annotation-api], [payload limit][pal-annotation-limit],
[GFX10 recorder][pal-annotation], [GFX12 recorder][pal12-annotation],
[reservation size][pal-reserve-size]

PAL also inserts a one-DWORD preamble so an otherwise empty DMA command
buffer has something to submit. Its device-specific dummy-stream constructors
build a NOP with the native stream's size alignment when an OS queue operation
needs a submission without client commands. These are actual framing callers;
they supply no separate memory completion result.
[Preamble][pal-preamble], [shared GFX9-family dummy stream][pal-dummy],
[GFX12 dummy stream][pal12-dummy]

## Pending-transfer drains

The ordinary Mesa `ac_emit_sdma_nop` emits one zero DWORD and documents that
it waits for pending copies. RADV's transfer-queue barrier reaches that exact
helper through `radv_sdma_emit_nop`. Its comment treats automatic GFX9+ RAW
tracking as insufficient, without identifying a repaired revision or firmware
threshold. This is a conservative consumer policy, not an all-device erratum
or evidence that a particular omitted drain fails. [Mesa emitter][mesa-nop],
[RADV wrapper][radv-nop], [barrier caller][radv-barrier]

RADV uses that same one-DWORD helper for temporary-buffer dependencies and
selected timestamp observations:

| Caller | Transfer dependency |
| --- | --- |
| Buffer-to-image copy with incompatible pitch | Drain buffer-to-temporary row copies before the temporary-to-image transfer; drain that last read before reusing the temporary for another row group or slice. |
| Image-to-buffer copy with incompatible pitch | Drain image-to-temporary before the temporary-to-buffer row copies; drain those reads before overwriting the temporary. |
| Tiled-to-tiled scanline fallback | Drain source-to-temporary before temporary-to-destination, then drain the temporary's final read before the next iteration. |
| Transfer timestamp with `flush_before_timestamp_write` enabled | Insert a drain before the timestamp packets under this optional runtime policy. |

[Pitch selector][radv-temp-selector], [buffer/image caller][radv-temp-buffer-caller],
[image caller][radv-temp-image-caller], [buffer/image loops][radv-temp-buffer-image],
[tiled scanline loop][radv-temp-tiled],
[timestamp caller][radv-timestamp-drain]

The temporary BO is allocated lazily and added to each command stream that
uses it. The first drain establishes the temporary's data dependency; the
second establishes its last-read boundary for an in-stream overwrite. Neither
drain independently tells the host that the command buffer or BO can be
destroyed. That remains a [submission retirement](command-buffers.md#publication-completion-and-final-use)
obligation. [Temporary owner][radv-temp-owner]

PAL independently supplies an all-image-types hazard mask in its GFX10 and
GFX12 DMA constructors. The shared `CmdReleaseThenAcquire` path then emits one
NOP when the combined source-stage mask is nonzero. That mask includes global
and memory barriers, so the path applies to buffer dependencies even without
an image barrier. Image transitions can require an additional drain after
their own transfer work. The older generic comment claiming strict in-order
execution does not describe these operative conditions.
[Constructors][pal-constructor], [GFX12 constructor][pal12-constructor],
[barrier implementation][pal-barrier]

The source composition for a dependent copy is:

```text
COPY A -> B
NOP                       complete pending transfers before consuming B
COPY B -> C
FENCE completion          publish final completion to its owned word
```

Each copy has nonoverlapping source and destination ranges. The dependency is
between the first packet's destination and the second packet's source. The NOP
carries no address, value, cache-operation mask or signal. It does not by
itself notify the host, acquire another engine's payload, or select the memory
mapping and cache policy needed by this composition.

## Completion operations have different results

| Operation | Source contract and observation boundary |
| --- | --- |
| COPY/WRITE | Ordinary transfer work can overlap. A trailing WRITE is not a substitute for a completion fence. |
| One-DWORD NOP in the cited PAL/Mesa drain paths | Joins pending transfer work at an in-stream dependency boundary; it produces no memory result for a host waiter. |
| FENCE32 | Orders the preceding transfers and writes a known DWORD. The memory-type/scope fields and host observation follow the selected [fence contract](fence.md). A single DWORD store is not an atomic 64-bit signal update. |
| GET_GLOBAL timestamp | PAL specifies completion of preceding commands before recording the clock. The timestamp write still has its own visibility and final-use boundary; a later completion marker can establish that the timestamp destination is ready to read. |
| Memory poll | Waits for the selected operand predicate. Signal ownership and payload visibility remain the separate [poll](poll.md) and cache contracts. |

[PAL timestamp ordering][pal-timestamp] supplies a second way to separate
transfers when a clock sample is also needed. The recorded interval includes
that ordering point. It differs from timing an unseparated stream in which
transfers can overlap.

## Overlap and ownership

Overlap within one copy and dependency between distinct copies are separate
contracts. ROCr's public asynchronous-copy API explicitly excludes overlapping
source/destination ranges: submission can succeed while destination contents
remain undefined. Neither an inserted drain nor choosing a forward/reverse
chunk order establishes `memmove` behavior for overlapping ranges. [ROCr API
precondition][rocr-overlap]

The producer owns the input bytes through their final device read. Each
intermediate allocation remains live until its last dependent consumer has
completed, and completion storage remains live through its final writer and
waiter. The ring's acquired consumed frontier only retires command bytes; it
does not establish completion of referenced transfers. A fence placed before a
later consumer cannot authorize release of that consumer's input. ROCr's [ring
publication and reuse][rocr-ring] and its [completion
sequence](atomics.md#memory-and-lifetime) have separate owners for these
boundaries.

## Architecture and transport differences

PAL's GFX11.5 device selection reaches the DMA constructor and barrier path
above. Its pending-transfer drain therefore has an actual generation-specific
caller, rather than only a matching NOP bit pattern. [PAL
factory][pal-factory] [GFX11.5 selection][pal-asic]

RADV's supported-device predicate excludes compute-only GFX9 devices. Its
transfer-queue exposure additionally requires a known SDMA IP, an available
SDMA queue, experimental or application-profile opt-in, an enabled compute
queue, GFX9 or later, and transfer queues not disabled. Its drain callers
consequently do not describe CDNA SDMA4.4.2 or every retained legacy NOP
encoding in the source. Linux's use of NOP for ring padding establishes that representation,
not by itself the pending-transfer dependency semantics used by PAL and RADV.
[RADV device selection][radv-admission], [transfer-queue selection][radv-transfer-admission]

Mesa's older r600 driver has a separate native DMA dependency policy.
`r600_need_dma_space` checks whether the current DMA IB already references
the next destination for read/write or its source for write, and invokes
`r600_dma_emit_wait_idle` on that hazard. The latter emits `0xf0000000` for
its `gfx_level >= EVERGREEN` branch; its older branch emits nothing. Native
DMA queue availability and `DBG_NO_ASYNC_DMA` separately gate creation of
that stream. This is a distinct legacy caller contract, not a reason to
transfer the modern counted-NOP or RADV admission rules to older engines.
[Legacy dependency owner][mesa-legacy-drain], [queue selection][mesa-legacy-queue]

## Scope and prior dependency

ROCr names COPY_LINEAR header bit 28 NPD, “no prior dependency.” The scoped
copy builder sets NPD and SYS source/destination scopes. Linux's OSS7.1 header
agrees on the NPD position, but its ordinary copy emitter leaves that bit
zero. The ROCr factory selects the scoped V6 template for non-DXG ISA major
11/12 with minor at least 5, while its DXG path selects the unscoped V4
template. [Template meaning][npd-name] [Copy builder][npd-builder] [Linux
field][npd-layout] [Linux copy][npd-zero] [Factory][npd-transport]

These choices identify the emitted policies; the cited definitions do not
explain the complete prior-dependency domain that NPD bypasses. NPD is not a
prefetch-control bit, a completion marker, or a substitute for an explicit
producer/consumer dependency.

[rocr-completion]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L469-L479
[mesa-nop]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L15-L22
[radv-nop]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L210-L215
[radv-barrier]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L16383-L16397
[pal-constructor]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L50-L66
[pal12-constructor]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L68-L85
[pal-barrier]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L387-L447
[pal-nop]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L158-L173
[pal-nop-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L2522-L2545
[pal12-nop-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L2681-L2704
[nop-iceland]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/iceland_sdma_pkt_open.h#L2155-L2170
[nop-tonga]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/tonga_sdma_pkt_open.h#L2228-L2243
[nop-vega10]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L2950-L2972
[nop-navi10]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L4501-L4523
[nop-sdma6]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L5233-L5255
[nop-sdma71]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L5234-L5256
[nop-cik]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cikd.h#L494-L500
[pal-preamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L140-L155
[pal-reserve]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStream.cpp#L163-L201
[pal-commit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStream.cpp#L252-L282
[nop-si]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sid.h#L557-L582
[nop-si-ring]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/si_dma.c#L734-L756
[nop-si-pad]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/si_dma.c#L410-L419
[nop-firmware]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_sdma.c#L151-L187
[nop-cik-firmware]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cik_sdma.c#L520-L551
[nop-cik-insert]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cik_sdma.c#L199-L210
[nop-cik-pad]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cik_sdma.c#L798-L813
[nop-user-align]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_kms.c#L479-L494
[pal-annotation-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L4630-L4638
[pal-annotation-limit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L2197-L2201
[pal-annotation]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L214-L230
[pal12-annotation]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L216-L232
[pal-reserve-size]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/device.h#L1016-L1024
[pal-dummy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Device.cpp#L655-L700
[pal12-dummy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Device.cpp#L1069-L1120
[radv-temp-buffer-image]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L316-L382
[radv-temp-tiled]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L474-L531
[radv-timestamp-drain]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L2781-L2793
[radv-temp-owner]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_copy.c#L55-L75
[radv-temp-selector]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L300-L314
[radv-temp-buffer-caller]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_copy.c#L106-L155
[radv-temp-image-caller]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_copy.c#L632-L687
[radv-transfer-admission]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L108-L126
[mesa-packing]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/sid.h#L324-L330
[mesa-parser]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_parse_ib.c#L668-L685
[mesa-pad-emitter]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L305-L368
[mesa-pad]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L450-L510
[mesa-align]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L464-L528
[mesa-gallium-pad]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp#L2235-L2254
[mesa-radeon-pad]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/radeon/drm/radeon_drm_cs.c#L574-L592
[mesa-legacy-drain]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/r600/r600_pipe_common.c#L203-L285
[mesa-legacy-queue]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/r600/r600_pipe_common.c#L625-L636
[pal-timestamp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L107-L137
[rocr-overlap]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2090-L2116
[pal-factory]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Device.cpp#L2943-L2947
[pal-asic]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Device.cpp#L5496-L5506
[radv-admission]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L2501-L2512
[npd-name]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_blit_sdma.h#L584-L590
[npd-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L131-L135
[npd-zero]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c#L1716-L1731
[npd-transport]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L852-L884
[npd-builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2140-L2184
[rocr-ring]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1954-L2094
