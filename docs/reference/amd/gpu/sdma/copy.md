# SDMA linear copy

`COPY_LINEAR` transfers an exact byte range between source and destination
addresses in the executing engine's address space. The packet has no result
or completion operand; its surrounding stream and native submission supply
the dependency, visibility and retirement protocol. ROCr's ordinary builder
uses seven DWORDs and accepts byte-aligned ranges, including one-to-three-byte
tails. The source and destination must not overlap under the HSA async-copy
contract. [Builder][rocr-copy] [Layout][rocr-layout]
[Overlap contract](ordering.md#overlap-and-ownership)

Pitched rows and slices use the separate
[rectangular-copy packet](rectangular-copy.md). Its element counts, geometry,
and per-generation scope fields have their own encoding and caller limits.

One source range can also feed several destinations through
[broadcast, multicast, or ordinary-copy fan-out](fanout.md). Those forms have
separate destination-count, alignment, routing, and completion contracts.

[Indirect source and destination copies](indirect-copy.md) read payload
addresses from device-visible slots at execution time while keeping the byte
length fixed. Their slot scopes and lack of a post-dereference offset give
them different publication and chunking requirements.

[Buffer exchange](swap.md) replaces both operands' contents while preserving
their addresses. It has its own generation predicates, address alignment and
both-operand ownership contract.

## Callers and applicability

The packet builder runs after selection of a copy engine, native mappings and
an owning command stream. An API with “DMA” or “copy” in its name can instead
use shaders or a host copy; the selected caller determines the operation.

| Caller | Ordinary linear-copy path |
| --- | --- |
| Linux TTM | Memory moves select a native buffer-functions backend, map source/destination resource windows and submit a kernel-owned job. These addresses and mappings belong to TTM's native address space. [Caller][linux-ttm-copy] |
| PAL DMA command buffer | `CopyMemoryRegion` repeatedly calls the selected GFX10/GFX12 builder, advancing both addresses by the returned byte length. Allocation-based and raw-address calls supply different compression information. [Loop][pal-loop] [Callers][pal-callers] |
| ROCr async copy | A selected SDMA blit can use the ordinary wrapper. Classic grouped bodies and the odd final destination of paired broadcast also call `BuildCopyCommand`; the fused GFX12.5 paths use different packets. The synchronous three-argument `GpuAgent::DmaCopy` selects the kernel-backed `BlitDevToDev` object. [Wrappers][rocr-wrappers] [Grouped body][rocr-bodies] [Broadcast tail][rocr-broadcast] [Local route][rocr-local] [Blit selection][rocr-init] |
| RADV | Transfer-queue buffer/address copies, staged updates and image staging reach `radv_sdma_copy_memory`. Internal shader upload separately selects SDMA with `RADV_PERFTEST_DMA_SHADERS` and GFX7+, independently of public transfer-queue exposure. [Buffer selector][mesa-copy-route] [Updates][mesa-updates] [Shader selector][mesa-shader-select] |
| RadeonSI | The explicit GFX9/GFX10/GFX10.3/GFX11/GFX11.5/GFX11.7/GFX12 linear-to-linear image cases call `ac_emit_sdma_copy_linear`; its full level-0 DRI_PRIME blit caller copies pitched rows. The corresponding GFX7/8 image path uses a subwindow packet, while general buffer copies use compute or CP DMA. [Generation selector][mesa-si-entry] [Linear path][mesa-si-linear] [Blit caller][mesa-si-blit] [Older path][mesa-cik-linear] [Buffer path][mesa-si-buffer] |

ROCr's ordinary SDMA object selection applies its SDMA override and BASE-profile
gate; by default it excludes major 8 and exact ISA 10.1.3. Directed engine
choice, xGMI routing and on-engine copy policy are described in
[engine selection](engine-selection.md). The DTIF fast-copy branch of the
ordinary wrappers performs host `memcpy` instead of publishing this packet;
the async branch requires no pending dependency signals for that shortcut.
[Initialization][rocr-init] [Wrappers][rocr-wrappers]

## Representation

The common seven-DWORD body below describes one source and one destination.
Header and parameter controls vary by source layout, as do the count's width
and direct versus minus-one interpretation. An additional metadata DWORD is
present in some GFX12 forms. Legacy SI uses a different five-DWORD packet.
These distinctions determine how much command storage the processor consumes,
independently of the payload length.

| DWORD | Common linear-copy body |
| --- | --- |
| 0 | `HEADER`: opcode 7:0 = 1, suboperation 15:8 = 0, plus source-specific controls below. |
| 1 | `COUNT`: byte count or positive byte count minus one, according to the selected native form. Unused upper bits are zero in the ordinary builders. |
| 2 | `PARAMETER`: source/destination policy and swap controls, according to the selected layout. |
| 3, 4 | `SRC_ADDR_LO`, `SRC_ADDR_HI`: source byte address, low then high 32 bits. |
| 5, 6 | `DST_ADDR_LO`, `DST_ADDR_HI`: destination byte address, low then high 32 bits. |

[Linux layouts][linux-tonga-layout] [PAL GFX10 layout][pal10-layout]
[PAL GFX12 layout][pal12-layout] [ROCr layout][rocr-layout]

### Header and parameter variants

The following table lists every additional named field in each ordinary-copy
layout. Fields omitted by a macro header are not assigned a meaning by that
header. PAL and ROCr zero-initialize their structures; their unnamed/reserved
fields remain zero. The Linux ordinary emitters likewise write zero for
controls they do not explicitly select.

| Source layout | Additional DWORD 0 fields | DWORD 2 fields |
| --- | --- | --- |
| Linux Iceland/Tonga | `broadcast` 27. | `dst_sw` 17:16, `dst_ha` 22, `src_sw` 25:24, `src_ha` 30. |
| Linux Vega | `encrypt` 16, `tmz` 18, `broadcast` 27. | `dst_sw` 17:16, `src_sw` 25:24. |
| Linux Navi | Vega header fields plus `backwards` 25. | `dst_sw` 17:16, `src_sw` 25:24. |
| Linux SDMA6 | `encrypt` 16, `tmz` 18, `cpv` 19, `backwards` 25, `broadcast` 27. | `dst_sw` 17:16, `dst_cache_policy` 20:18, `src_sw` 25:24, `src_cache_policy` 28:26. |
| Linux SDMA7.1 | SDMA6 fields except `cpv`; `npd` 28 is defined instead. | Same named fields and positions as the SDMA6 header. |
| PAL GFX10 | `encrypt` 16, `tmz` 18, `backwards` 25, `broadcast` 27; the `gfx103Plus` view adds `cpv` 19. | `dst_sw` 17:16, `src_sw` 25:24; the `gfx103Plus` view adds destination cache policy 20:18 and source cache policy 28:26. |
| PAL GFX12 | `tmz` 18, `dcc` 19, `backwards` 25. | `dst_mall_policy` 21:20, `src_mall_policy` 29:28. |
| ROCr ordinary copy | `npd` 28. | `dst_scope` 19:18, `dst_temporal_hint` 22:20, `src_scope` 27:26, `src_temporal_hint` 30:28. |

[Iceland][linux-iceland-layout] [Tonga][linux-tonga-layout]
[Vega][linux-vega-layout] [Navi][linux-navi-layout]
[SDMA6][linux6-layout] [SDMA7.1][linux71-layout]
[PAL GFX10][pal10-layout] [PAL GFX12][pal12-layout] [ROCr][rocr-layout]

These are source-specific interpretations of overlapping bit positions.
Linux SDMA7.1's three-bit `cache_policy` definitions do not establish ROCr's
scope/temporal-hint interpretation. PAL GFX12's two-bit MALL policies do not
define the full three-bit ROCr temporal-hint fields. Similarly, Linux SDMA7.0
reuses the SDMA6 header's `cpv` name for bit 19 where PAL GFX12 calls it `dcc`.
The actual emitted packet and its consumer determine the applicable form.

The ordinary callers leave endian-swap controls, `encrypt`, `backwards` and
`broadcast` zero. A named `backwards` field alone supplies no overlapping-copy
contract. The `broadcast` control selects a distinct
[fan-out representation](fanout.md), not a second destination hidden inside
the seven-DWORD body. The Iceland/Tonga `_ha` fields have no selected meaning
in these ordinary zero-parameter emitters.

### ROCr emitted controls

The builder writes `L - 1` through its selected 22-bit or 30-bit count view.
The scoped template sets NPD at bit 28 and both scopes to SYS (3). The
unscoped template leaves those controls zero; both leave temporal hints zero.
ROCr names its scope values CU (0), SE (1), DEV (2), and SYS (3), but this
builder selects only SYS when scopes are enabled. NPD means “no prior
dependency” in the template description; its interaction with the surrounding
copy stream is explained in [ordering](ordering.md#scope-and-prior-dependency).
[Scope definitions and layout][rocr-scope-layout] [Builder][rocr-copy]

The factory selects independent `useGCR` and `scopeFields` template controls.
Major-9 ISAs use V4 (`false, false`); major 10 uses V5 (`true, false`) except
on DXG, which uses V4. Majors 11/12 use V4 on DXG, otherwise V6 (`false, true`)
when the ISA minor is at least 5 and V5 otherwise. These are the inspected
factory predicates, not a conversion from compiler target to SDMA IP. DXG's
driver-owned GCR wrapping and the discrepancy with broader scoped-template
comments are described in [cache transport ownership](cache.md).
[Factory][rocr-select] [Template aliases][rocr-templates]

### PAL policy and compression selection

PAL's GFX10 builder sets TMZ from its copy flag. With `supportsMall`, it
populates separate read/write cache policies and CPV; otherwise those fields
remain zero. `GetCachePolicy` returns a nonzero MALL-bypass policy only for
Navi2x with the corresponding read/write bypass setting: bit 2 requests LLC
no-allocation and bits 1:0 carry the KMD-provided L2 policy. CPV is selected
when the setting differs from its default and `sdmaL2PolicyValid` is true.
Thus the outer MALL-support gate, ASIC predicate, setting and native policy
validity all participate. [Policy helpers][pal10-policy] [Builder][pal10]

PAL's GFX12 builder instead gets its two-bit source/destination MALL policies
from `sdmaSrcMallPolicy` and `sdmaDstMallPolicy` when MALL is present, or zero
otherwise. The named values are regular temporal (0), non-temporal (1),
high-priority temporal (2), and last-use (3). The helper's discussion of wider
temporal hints does not enlarge the two-bit packet fields. Cache-placement
hints do not establish a release or acquire. [MALL helper][pal12-policy]

The GFX12 metadata form sets `dcc` and appends DWORD 7:

| DWORD 7 bits | `META_CONFIG` field |
| --- | --- |
| 5:0 | `data_format`: native color-format encoding. |
| 11:9 | `number_type`: native surface-number encoding. |
| 17:16 | `read_compression_mode` |
| 19:18 | `write_compression_mode` |
| 25:24 | `max_comp_block_size` |
| 26 | `max_uncomp_block_size` |

Other bits are unnamed and zero in PAL's initialized packet. Linux's SDMA6
DCC macros use the same positions for the SDMA7.0 emission. This field
agreement does not make metadata presence or policy selection identical.
[PAL layout][pal12-layout] [Linux macros][linux-dcc-layout]

PAL's compressed-block setting names 64 B (0), 128 B (1), and 256 B (2),
with 256 B as the default; that setting does not define encoding 3.
Its uncompressed-block enum names 128 B (0) and 256 B (1), defaults to 256 B,
and requires the maximum uncompressed block to be at least the compressed
block. [Compressed sizes][pal12-compressed-size]
[Uncompressed sizes][pal12-uncompressed-size]

`CmdCopyMemory` derives compression flags from each allocation's
`MaybeCompressed()` result, which includes virtual allocations as well as
compression-enabled ones. `CmdCopyMemoryByGpuVa` has no allocation objects;
under `PAL_BUILD_GFX12` it supplies both compression flags. The GFX12 builder
uses `SetupMetaData` when either flag is present and omits DWORD 7 otherwise.
The source flag populates only the read mode. The destination flag populates
format, number type, write mode and block sizes; without that flag, those
fields remain zero even when the source requires metadata. Buffer copies
supply no image descriptors. For a flagged destination, the format defaults
to `X32_Uint`, with the device's default compressed-block size and fixed
default uncompressed-block size. With the default buffer compression setting,
a flagged source selects read decompression and a flagged destination selects
disabled compressed writes; an unflagged source leaves the read mode zero.
The read-mode encoding is bypass (0) or decompression (2); the write-mode
encoding is bypass (0), compression enabled (1), or disabled (2). Other values
are reserved in these enums. An explicit read-bypass mode also depends on
`enableCompressionReadBypass`; when disabled, the helper still selects read
decompression. Metadata presence alone therefore does not mean compressed
writes are enabled. [Copy callers][pal-callers] [Allocation predicate][pal-compressed]
[Metadata selection][pal12-meta] [Mode selection][pal12-compression]
[Mode encodings][pal12-mode-values] [Packet builder][pal12]

Linux SDMA7.0 always emits bit 19 and DWORD 7, including a zero metadata word
for uncompressed moves. Its TTM caller supplies DCC flags from BO creation,
VRAM placement and destination tiling metadata. The native emitter maps read
decompression to mode 2 and selects write mode 1 or 2 from the write-disable
flag. PAL's uncompressed seven-DWORD form cannot replace that kernel-owned
eight-DWORD sequence by assuming the optional-word rule is universal.
[TTM flags][linux-ttm-copy] [Native emitter][linux7]

### Legacy SI byte copy

The AMDGPU SI emitter uses `DMA_PACKET_COPY` (opcode 3) in header bits 31:28,
sets `b` at bit 26, clears `t` and `s` at bits 23 and 22, and puts the direct
byte count in bits 19:0. It then emits destination low, source low,
destination high, and source high. Each high word is masked to its low eight
bits, giving the emitted address representation 40 bits. There is no separate
count word, parameter word or metadata word. The source's `0xffff8`-byte cap
is distinct from the header's representable `0xfffff`; the retained Radeon
SI consumer uses the latter cap for the same byte-copy form.
[SI header][linux-si-header] [AMDGPU emitter and cap][linux-si-copy]
[Radeon caller][radeon-si-copy]

CIK instead uses the seven-DWORD order above and a direct byte count. Its
`0x1fffff` software cap does not by itself define a 21-bit native count field;
the cited CIK header supplies the opcode/header packing, while the emitter
writes the count word directly. [CIK header][linux-cik-header]
[CIK emitter and cap][linux-cik-copy]

The retained Mesa R600 driver also has older five-DWORD DMA copies. Both
emit destination low, source low, destination high-eight, source high-eight
after the header, but their header and count units differ from SI:

| Mesa source family | Header, count and selected byte extent |
| --- | --- |
| R600/R700 `r600_dma_copy_buffer` | Opcode 3 at 31:28, `t=0` at 23 and `s=0` at 22; direct DWORD count at 15:0, capped at `0xffff`. Low address words clear bits 1:0. The caller requires DWORD-aligned offsets and length; the builder emits resource-relative offsets with per-packet BO relocation references and advances by four times the count. |
| Evergreen/Cayman `evergreen_dma_copy_buffer` | Opcode 3 at 31:28, subcommand at 27:20, direct count at 19:0 capped at `0xfffff`. The builder adds each BO's GPU base. Aligned addresses and length select subcommand 0 with a DWORD count; otherwise subcommand `0x40` uses a byte count. Each address advances by the selected byte extent. |

[R600 header][mesa-r600-header] [R600 builder][mesa-r600-copy]
[R600 caller][mesa-r600-caller] [Evergreen header][mesa-eg-header]
[Evergreen builder][mesa-eg-copy] [Evergreen caller][mesa-eg-caller]

Those callers register source READ and destination WRITE references and use
their winsys submission/retirement protocol. Their copy callbacks depend on
an available DMA queue; unsupported source geometry follows another resource
copy path. The generation-specific
[pending-transfer ordering](ordering.md#pending-transfer-drains) remains
separate from these byte/DWORD count rules.

## Count representation and native IP

For a positive length `L`, an N-bit `L - 1` field represents `1..2^N` bytes.
Thus a 30-bit count can represent 1 GiB, even though its largest field value
is `0x3fffffff`. A zero field means one byte. Zero-length work omits the copy.
Mesa uses direct `L` before SDMA4.0 and `L - 1` from SDMA4.0, so old
direct-count limits cannot be reused without identifying the encoded quantity.
[Mesa emitter][mesa-copy], [ROCr fields][rocr-layout]

Linux selects backends using native SDMA IP and revision, independently of
compiler GFX names. [Native selection][linux-discovery]

| Linux native selection | Count written; per-packet byte cap | Emitted DWORDs and ordinary controls |
| --- | --- | --- |
| SI backend | Direct header count; `0xffff8`. | Five; byte-copy header above, `copy_flags` unused. [Source][linux-si-copy] |
| CIK backend | Direct `L`; `0x1fffff`. | Seven; zero parameter word, `copy_flags` unused. [Source][linux-cik-copy] |
| v2.4 backend | Direct `L`; `0x1fffff`. | Seven; zero parameter word, `copy_flags` unused. Iceland defines a 22-bit count. [Source][linux24] |
| v3.0/v3.1 backend | Direct `L`; `0x3fffe0`. | Seven; zero parameter word, `copy_flags` unused. Tonga defines a 22-bit count; the cap comment identifies an unspecified hardware limitation. [Source][linux3] |
| 4.0.0/4.0.1, 4.1.0–4.1.2, 4.2.0/4.2.2, 4.4.0 → v4.0 | `L - 1`; `1 << 22` below native 4.4.0, `1 << 30` at 4.4.0. | Seven; TMZ from the copy flag, zero parameters. [Source][linux-v4] |
| 4.4.2/4.4.4/4.4.5 → v4.4.2 | `L - 1`; `1 << 30`. | Seven; TMZ from the copy flag, zero parameters. [Source][linux442] |
| 5.0.0/5.0.1/5.0.2/5.0.5 → v5.0 | `L - 1`; `1 << 22`. | Seven; TMZ from the copy flag, zero parameters. [Source][linux5] |
| 5.2.0–5.2.7 → v5.2 | `L - 1`; `1 << 30`. | Seven; TMZ from the copy flag, zero parameters. [Source][linux52] |
| 6.0.0–6.0.3, 6.1.0–6.1.4, 6.4.0 → v6.0 | `L - 1`; `1 << 30`. | Seven; TMZ from the copy flag, zero parameters. SDMA6 defines a 30-bit count. [Source][linux6] |
| 7.0.0/7.0.1 → v7.0 | `L - 1`; `1 << 30`. | Eight; TMZ from the copy flag, CPV bit 19 set, zero parameters, metadata word above. Uses the SDMA6 count definition. [Source][linux7] |
| 7.1.0 → v7.1 | `L - 1`; `1 << 30`. | Seven emitted, eight budgeted; TMZ from the copy flag, zero parameters and NPD. Its own header defines a 30-bit count. [Source][linux71] |

Native-IP rows list the actual discovery cases; they are not open-ended major
version ranges. The legacy SI/CIK/VI paths select their backends by ASIC
family. [Native discovery][linux-discovery] [SI selection][linux-si-select]
[CIK selection][linux-cik-select] [VI selection][linux-vi-select]

The v4.0, v4.4.2 and v5.2 emitters write the count word directly. Their
1 GiB branches do not use the inherited Vega/Navi 22-bit count macros. Those
macros therefore cannot be treated as the complete count contract of these
backends. ROCr's gfx94x path independently selects its larger count view.
[Vega definition][linux-vega-count] [Navi definition][linux-navi-layout]
[ROCr selection][rocr-select]

## Runtime caps and policy margins

| Consumer and selection | Maximum bytes in one ordinary copy |
| --- | --- |
| Linux's `1 << 30` backends above; PAL's 30-bit builders | `0x40000000` (1 GiB). |
| ROCr's larger ISA-selected override | `0x3fffffff` (1 GiB minus one byte); largest emitted count is `0x3ffffffe`. |
| Mesa helper at native SDMA5.2 or later | `0x3fffff00` (1 GiB minus 256 bytes). |

ROCr selects the larger override for ISA major 9 with minor at least 4 or
exactly 9.0.10, major 10 with minor at least 3, and majors 11/12. Other
selected major-9/10 cases use `0x3fffff`. Disabling the override uses the
older `0x3fffe0` cap and 22-bit union. The ordinary builder emits successive
exact `min(remaining, cap)` ranges without DWORD rounding. The inspected code
gives no rationale for the override's one-byte headroom.
[Selector][rocr-select], [builder][rocr-copy], [fallback cap][rocr-layout]

Mesa uses `0x3fff00` below SDMA5.2. Its comment attributes that older cap to
an undocumented restriction near the top of the 22-bit count range, without an
affected-IP list, firmware threshold or mechanism. Relative to the older
direct-count maximum the margin is 255 bytes; relative to the modern
representable byte length it is 256. This describes a maximum-length policy,
not corruption of the last 255 bytes of arbitrary copies. The source does not
establish why later generations retain the margin. [Cap
definitions][mesa-caps], [helper selection][mesa-copy]

Mesa's 5.2 threshold is not a universal hardware width transition. Linux's v4
backend already selects the 1 GiB cap at native SDMA4.4.0. PAL selects 22
versus 30 bits using its GFX10.3-or-later predicate; that consumer predicate
does not classify CDNA by compiler-major number. A runtime policy, an encoded
limit and a native-IP contract remain separate facts. [Linux v4
selection][linux-v4], [PAL builder][pal10]

## Chunking and alignment

PAL's caller keeps the remaining length and both addresses in `gpusize`. Its
builder first caps the chunk, then rounds down to a DWORD multiple when both
addresses are DWORD-aligned and at least four bytes remain. The caller
advances by exactly the returned byte length; a final one-to-three-byte tail
uses a byte copy. Aligned 13-byte ranges therefore become 12 bytes plus one
byte, with no padding or overrun. The source explains that aligned addresses
and length let firmware use a faster DWORD mode. [Caller][pal-loop],
[builder][pal10]

Mesa's common helper also attempts an aligned-prefix/tail split, but its
pinned implementation applies a 32-bit alignment mask to the 64-bit remaining
size before capping it. A remaining extent of `2^32` bytes produces zero byte
progress. That expression is not a general wide-length chunking contract;
PAL's cap-first, wide-address loop above preserves the full extent.
[Helper][mesa-copy]

RADV's Vulkan caller has a separate input bound. It advertises
`maxBufferSize = 0xFFFFFFFC`; Vulkan 1.3 / `VK_KHR_maintenance4` requires
buffer creation to respect that maximum. Each buffer-copy region must fit
its source and destination buffers, and a raw `VkDeviceAddressRangeKHR` must
fit its originating buffer. For buffers subject to that contract, both
`vkCmdCopyBuffer2` and `vkCmdCopyMemoryKHR` therefore remain below the
helper's 4 GiB discrepancy. Allocation padding or adjacent allocations do
not enlarge the buffer's valid range. These are API validity obligations;
the copy caller forwards the supplied 64-bit region extent rather than
clamping it. [Maximum][mesa-buffer-maximum] [Property][mesa-buffer-property]
[Buffer creation][vulkan-buffer-size] [Buffer regions][vulkan-copy-regions]
[Address ranges][vulkan-address-range] [Address-copy contract][vulkan-address-copy]
[Copy caller][mesa-copy-route]

RadeonSI's linear-image caller passes `src_pitch * copy_height * bpp` bytes,
including row padding. The ordinary packet copies that entire contiguous
extent; it does not know the image's logical width. Rectangles that preserve
pitch gaps use the separate [subwindow representation](rectangular-copy.md).
This image caller has its own allocation and pitch contract; RADV's buffer
maximum does not bound it.
[Pitched copy extent][mesa-si-linear]

Linux's TTM caller separately rounds the maximum chunk down to 256 bytes where
needed for memory-channel utilization and DWORD mode, assuming aligned
page-copy addresses. The 1 GiB cap already meets that alignment. This is a
performance policy, not corroboration of Mesa's undocumented margin. [TTM
chunking][linux-loop]

The outer TTM memory-move loop further limits each submission to 256 MiB and
the source/destination resource extents, then maps its two transfer windows.
Its comment attributes that submission cap to avoiding timeouts. Thus even a
backend advertising a 1 GiB packet cap does not imply that this caller submits
1 GiB moves as single packets. Allocation segmentation, submission size and
packet size have different owners. [Memory-move caller][linux-ttm-copy]

## Programming sequence and lifetime

ROCr's ordinary async wrapper builds packets in a temporary stack object or
vector. `SubmitCommand` copies those command bytes into reserved ring storage;
its software dependency-wait path instead captures them in a callback-owned
allocation before returning. Neither captures the payload bytes addressed by
COPY. The caller's source and destination mappings remain live until their
device users finish. [Wrappers][rocr-wrappers] [Command capture][rocr-capture]
[Ring copy][rocr-stream]

The ordinary stream satisfies its dependency signals, performs the selected
HDP/cache acquire, executes the copies, applies its selected cache release and
updates the output signal. Event notification and ring padding may follow the
signal update. A grouped classic body has different owners: a prologue supplies
the dependencies/acquire, each engine body copies its entries, and an epilogue
joins the groups and completes the public signal. An odd broadcast tail
retains the shared source through all preceding destinations, not just the
final ordinary packet. [Ordinary stream][rocr-stream]
[Completion tail][rocr-completion] [Grouped flow](fanout.md#batch-composition-and-descriptor-ownership)
[Broadcast tail][rocr-broadcast]

The HSA async-copy contract additionally requires the caller to establish a
system-scope release before copying and a system-scope acquire before using
the result. Selected packet scopes or a ready signal are not substitutes for
those API obligations. [Public copy contract][rocr-api]
[Cache ownership](cache.md) [Completion observers](fence.md)

RADV's transfer queue supplies a concrete staged-upload flow. `CmdUpdateBuffer` and
`CmdUpdateMemoryKHR` capture the caller's host bytes in an upload BO and record
COPY from that BO. The original host pointer is no longer the SDMA source
after capture; the upload BO remains the source until device completion.
Normal buffer/address copies do not perform that capture.
[Update selection][mesa-updates] [Host-data capture][mesa-upload]
[Copy selection][mesa-copy-route]

RADV's shader uploader similarly borrows a staging BO, but tracks it with a
timeline. Before reusing a submission slot, it waits for the slot's previous
sequence, then resets the command stream or resizes the staging BO. It records
the copy, fills the staging bytes and submits with a new upload sequence.
Shader-consuming submissions wait for the required sequence. Shader destruction
also waits for its upload sequence before freeing the destination allocation;
later shader executions retain their own lifetime obligations. This connects
source reuse, upload completion and destination use to distinct owners.
[Slot reuse][mesa-shader-get] [Upload][mesa-shader-upload]
[Submission][mesa-shader-submit] [Consumption wait][mesa-shader-consume]
[Shader destruction][mesa-shader-destroy]

Linux TTM instead attaches reservation dependencies and any required mapping
flush to a job, then returns a scheduler-finished fence chained to native
completion. IB suballocation release carries that fence. TTM's accelerated
move cleanup also carries it when retaining or releasing the old placement;
constructing the COPY words does not end the old storage's use.
[Job preparation and copy submission][linux-job-copy]
[Job completion and IB release][linux-job-owner]
[Native-to-scheduler completion](fence.md#linux-ring-completion-and-user-fences)
[Placement cleanup][linux-move-retirement]

PAL and RADV's explicit transfer-barrier and internal staging paths use
a [pending-transfer drain](ordering.md#pending-transfer-drains) to order
dependent copies and temporary-buffer reuse. Ordinary copy packet construction
does not supply that dependency, and an in-stream drain is not a host
retirement observation.
[Native command ownership](command-buffers.md)

Source storage remains unchanged through its final read. Destination storage
remains live through every downstream reader, and the completion cell through
its final writer and waiter. Primary-ring consumption permits ring-byte reuse;
it does not replace the completion of referenced transfers. [Native copy
stream](atomics.md#copy-completion-through-add64)
[Queue frontiers](publication.md)

[linux-discovery]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c#L2785-L2846
[linux442]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L2282-L2329
[linux-vega-count]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L116-L121
[linux-v4]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_0.c#L2572-L2637
[linux6]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L1821-L1868
[linux7]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_0.c#L1753-L1817
[linux71]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c#L1716-L1763
[linux-loop]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L2493-L2564
[mesa-caps]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/sid.h#L356-L360
[mesa-copy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L92-L127
[rocr-select]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L852-L904
[rocr-copy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2140-L2184
[rocr-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L84-L150
[pal10]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L453-L524
[pal12]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L388-L454
[pal-loop]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L447-L480
[linux-iceland-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/iceland_sdma_pkt_open.h#L71-L146
[linux-tonga-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/tonga_sdma_pkt_open.h#L71-L146
[linux-vega-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L87-L162
[linux-navi-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L111-L192
[linux6-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L108-L207
[linux71-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L108-L207
[linux-dcc-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L95-L100
[linux-si-header]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sid.h#L557-L582
[linux-si-copy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/si_dma.c#L792-L837
[linux-cik-header]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cikd.h#L495-L507
[linux-cik-copy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cik_sdma.c#L1301-L1346
[linux24]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v2_4.c#L1189-L1235
[linux3]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v3_0.c#L1570-L1639
[linux5]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c#L2006-L2053
[linux52]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v5_2.c#L2013-L2060
[linux-ttm-copy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L295-L381
[radeon-si-copy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/radeon/si_dma.c#L230-L282
[pal10-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L746-L846
[pal12-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L707-L802
[pal10-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L354-L450
[pal12-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L345-L385
[pal-callers]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L484-L531
[pal-compressed]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/gpuMemory.h#L309-L312
[pal12-meta]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L1122-L1181
[pal12-compression]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L1039-L1114
[pal12-mode-values]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L49-L65
[rocr-scope-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L78-L150
[rocr-templates]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_blit_sdma.h#L579-L590
[linux-si-select]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/si.c#L2687-L2741
[linux-cik-select]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cik.c#L2186-L2268
[linux-vi-select]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vi.c#L2048-L2167
[linux-job-copy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L2464-L2565
[linux-job-owner]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_job.c#L296-L382
[linux-move-retirement]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/ttm/ttm_bo_util.c#L601-L725
[pal12-compressed-size]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/settings_gfx12.json#L1451-L1483
[pal12-uncompressed-size]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Chip.h#L387-L395
[rocr-wrappers]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1576-L1635
[rocr-bodies]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1348-L1574
[rocr-broadcast]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1637-L1695
[rocr-local]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1168-L1170
[rocr-init]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L938-L1066
[mesa-copy-route]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_buffer.c#L370-L447
[mesa-updates]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_buffer.c#L468-L534
[mesa-shader-select]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_device.c#L1616-L1621
[mesa-si-linear]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_sdma_copy_image.c#L35-L160
[mesa-si-blit]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/gfx/si_blit.c#L1044-L1082
[mesa-cik-linear]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_sdma_copy_image.c#L162-L239
[mesa-si-buffer]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_buffer.c#L795-L806
[mesa-r600-header]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/r600/r600d.h#L3806-L3814
[mesa-r600-copy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/r600/r600_hw_context.c#L569-L605
[mesa-r600-caller]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/r600/r600_state.c#L2980-L3069
[mesa-eg-header]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/r600/evergreend.h#L2792-L2801
[mesa-eg-copy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/r600/evergreen_hw_context.c#L13-L60
[mesa-eg-caller]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/r600/evergreen_state.c#L4326-L4423
[rocr-capture]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L308-L362
[rocr-stream]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L396-L664
[rocr-completion]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L614-L664
[rocr-api]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2090-L2188
[mesa-upload]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1512-L1615
[mesa-shader-get]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_shader.c#L2867-L2902
[mesa-shader-upload]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_shader.c#L2957-L2980
[mesa-shader-submit]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_shader.c#L2904-L2955
[mesa-shader-consume]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1605-L1670
[mesa-shader-destroy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_pipeline_cache.c#L40-L64
[mesa-si-entry]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_sdma_copy_image.c#L388-L455
[mesa-buffer-maximum]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_constants.h#L83-L86
[mesa-buffer-property]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L2060-L2066
[vulkan-buffer-size]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/resources.adoc#L381-L386
[vulkan-copy-regions]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/commonvalidity/copy_buffer_common.adoc#L7-L36
[vulkan-address-range]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/commonvalidity/device_address_range_common.adoc#L7-L12
[vulkan-address-copy]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/copies.adoc#L91-L136
