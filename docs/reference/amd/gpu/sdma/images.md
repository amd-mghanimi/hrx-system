# SDMA image transfers

SDMA can copy between a pitched linear region and a tiled image, or between
two tiled images. The command describes the native surface layout as well as
the copied rectangle: a byte address alone does not identify the swizzle,
mip, plane or compression state. A dependent copy can use a linear temporary
when the two tiled descriptions do not meet the direct-copy restrictions.
[Mesa builders][m-tiled] · [Tiled-to-tiled builder][m-t2t] ·
[PAL copy selection][p-common-image]

## Applicability and operation selection

The selected ordinary forms have COPY opcode 1. `COPY_TILED_SUBWIN`
(`SDMA_SUBOP_COPY_TILED_SUB_WIND = 5`) carries one tiled and one linear
operand. `COPY_T2T_SUBWIN` (`SDMA_SUBOP_COPY_T2T_SUB_WIND = 6`, PAL structure
`SDMA_PKT_COPY_T2T`) carries two tiled operands. RADV and PAL use the
separate [rectangular copy](rectangular-copy.md) representation for their
ordinary linear/linear image path. RadeonSI's modern PRIME helper instead
uses [linear COPY](copy.md), including source row padding in its byte
extent; its GFX7/8 path uses the rectangular helper. These are raw
transfers of format elements without format conversion.
[Mesa packet values][m-packet-macros] · [PAL selection][p-common-image] ·
[RADV selection][m-r-image] · [RadeonSI modern helper][m-si-modern] ·
[RadeonSI legacy helper][m-si-legacy] ·
[Surface units][p-surface]

| Native caller | Actual selection |
| --- | --- |
| RADV buffer/image and image/image commands | `RADV_QUEUE_TRANSFER` selects the SDMA route. Other queue families select graphics/compute copies. An unsupported image on the transfer queue uses its compute gang follower. |
| RADV transfer-queue exposure | Known SDMA IP and available SDMA queues, GFX at least 9, enabled compute queue, enabled experimental/application transfer-queue option, and no disable option. |
| RadeonSI PRIME blit | The caller requires a linear destination, level/offset zero, matching destination copy dimensions, depth one and copy-compatible formats. It can select linear copying or detiling. |
| RadeonSI generation selection | GFX7/8 use the legacy surface transformation; GFX9/10/10.3/11/11.5/11.7/12 use the modern helper. Other switch cases return false. |
| PAL DMA command buffer | `QueueTypeDma` selects the DMA implementation. The Gfx9 device path owns GFX10.1/10.3/11.0/11.5; the separately selected Gfx12 device owns GFX12. Image linearity selects linear/linear, linear/tiled, tiled/linear or T2T. |

[RADV API branch][m-r-buffer-image-api] · [RADV image/image][m-r-image-api] ·
[Transfer-queue exposure][m-r-queue] · [Compute-queue exposure][m-r-compute-queue] ·
[RadeonSI caller][m-si-blit] · [Generation switch][m-si-select] ·
[PAL device selection][p-device-select] · [Older DMA selection][p-p10-select] ·
[GFX12 DMA selection][p-p12-select]

RADV's image predicate has ordered branches: an emulated format returns
`false` for a destination image and `true` for a source image immediately.
Only the remaining path checks sparse-residency support and rejects sample
counts greater than one. Its source capability table selects sparse support
at SDMA IP 4 or later and compression at IP 5 or later except NAVI10 and
GFX1013. Those predicates describe this runtime's choices; a packet header
alone supplies neither image admission nor a queue contract.
[Image predicate][m-r-support] · [Capabilities][m-capabilities]

## Native surface inputs and units

An image transfer consumes the allocation owner's surface description:

| Input | Contribution to the command |
| --- | --- |
| Bound allocation and plane | Device byte VA plus native plane/surface offset; tiled address construction also incorporates the pipe/bank XOR or tile swizzle. |
| Native tiling | Swizzle mode and generation-specific dimension, mip and legacy bank/pipe fields. Addrlib-derived layout supplies these values. |
| Format element | Bytes per element, usually a texel or a block for a block-compressed format. Coordinates, extents and pitches use those elements. |
| Subresource | Plane, mip and array/depth identity. Linear images incorporate the selected mip's byte offset; tiled images retain the base surface and encode the mip separately. |
| Compression state | Older paths use image-layout/DCC/HTILE state and a metadata address. Newer paths consume allocation compression policy and separate read/write modes. |
| Copy region | Source/destination offsets, row and slice pitches, and the nonempty width/height/depth. The image's full extent and copied extent are different operands. |

[Mesa construction record][m-surf-description] · [RADV surface producer][m-r-surf] ·
[PAL surface producer][p-surface] · [Older PAL address owner][p-p10-base] ·
[GFX12 address owner][p-p12-base] · [Addrlib surface construction][m-surf]

RADV uses `(binding VA + surface offset) | (tile_swizzle << 8)` for a tiled
surface; its linear path adds the selected mip offset instead. Its plane
extent is converted to format elements, with image depth or array-layer
count as appropriate. PAL similarly uses its plane/mip-zero base for tiled
operands, but selected subresource offsets for linear images. PAL's ordinary
array slice becomes Z; its planar YUV array case keeps the slice in the
base address and uses Z zero. A manual `width * height` offset cannot replace
these native layout results.
[RADV producer][m-r-surf] · [PAL address/extent helpers][p-p10-input] ·
[GFX12 helpers][p-p12-input] · [PAL Z selection][p-image-z]

For block-compressed texture formats, an element is a format block. This is
separate from DCC, HTILE or allocation compression. A 12-byte pixel also has
a special linear representation: three 4-byte elements, with X and copied
width scaled by three. PAL asserts against the corresponding tiled
non-power-of-two case. These transformations preserve the bytes rather than
converting the pixel format.
[RADV element conversion][m-vk-elements] · [RADV X scale][m-r-scale] ·
[PAL element selection][p-surface]

### Legacy tile units

The Iceland/Tonga generated subwindow fields use `pitch_in_tile` at DWORD
4 bits 27:16 and `slice_pitch` at DWORD 5 bits 21:0, instead of modern
width/height/depth fields. RadeonSI's legacy caller supplies:

```text
pitch_in_tile = tiled_pitch_in_elements / 8 - 1
slice_pitch  = tiled_slice_pitch_in_elements / 64 - 1
```

Its construction record sets `extent` to those values plus one so the common
emitter's subtraction produces the required tile units. The shared member
name `extent` therefore does not imply identical geometry across generations.
The caller requires tiled VA alignment 256 bytes, linear VA alignment four
bytes, tile pitch divisibility by eight and tile slice pitch divisibility
by 64; it also applies chip, tile-mode and range checks. Its tile-pitch
maximum is stricter than the generated 12-bit field.
[Iceland subwindow][l-iceland-subwin] · [Tonga subwindow][l-tonga-subwin] ·
[Complete legacy caller][m-si-legacy]

## Packet representation

A DWORD is 32 bits. The base linear/tiled body occupies 14 DWORDs; the base
T2T body occupies 15. Metadata changes the emitted length. The following
tables describe PAL's complete GFX10 and GFX12 subwindow layouts; later
tables identify the older Linux/Mesa views and concrete disagreements.
[GFX10 subwindow][p10-subwin-layout] · [GFX10 T2T][p10-t2t-layout] ·
[GFX12 subwindow][p12-subwin-layout] · [GFX12 T2T][p12-t2t-layout]

### Header and fixed operands

| DWORD | Native field | GFX10 bits | GFX12 bits | Value or role |
| ---: | --- | --- | --- | --- |
| 0 | `HEADER_UNION.op` | 7:0 | 7:0 | COPY = 1. |
| 0 | `HEADER_UNION.sub_op` | 15:8 | 15:8 | TILED_SUBWIN = 5; T2T = 6. |
| 0 | `HEADER_UNION.tmz` | 18 | 18 | Protected-domain field; composition conditions appear below. |
| 0 | `HEADER_UNION.dcc` | 19 | 19 | Metadata mode selected by the builder. |
| 0 | `HEADER_UNION.gfx103Plus.cpv` | 28 | Unnamed | Cache-policy-valid overlay in the older declaration. |
| 0 | `HEADER_UNION.detile` | 31 | 31 | SUBWIN: one reads tiled source; zero writes tiled destination. |
| 0 | `HEADER_UNION.dcc_dir` | 31 | Unnamed | T2T older metadata direction; one selects source metadata. |
| 1–2 | `TILED_ADDR_LO_UNION.tiled_addr_31_0`, `TILED_ADDR_HI_UNION.tiled_addr_63_32` | Full DWORDs | Full DWORDs | SUBWIN tiled byte VA, low half then high half. |
| 7–8 | `LINEAR_ADDR_LO_UNION.linear_addr_31_0`, `LINEAR_ADDR_HI_UNION.linear_addr_63_32` | Full DWORDs | Full DWORDs | SUBWIN linear byte VA, low half then high half. |
| 1–2 | `SRC_ADDR_LO_UNION.src_addr_31_0`, `SRC_ADDR_HI_UNION.src_addr_63_32` | Full DWORDs | Full DWORDs | T2T source byte VA. |
| 7–8 | `DST_ADDR_LO_UNION.dst_addr_31_0`, `DST_ADDR_HI_UNION.dst_addr_63_32` | Full DWORDs | Full DWORDs | T2T destination byte VA. |

The PAL builders zero-initialize their packets. Header bits 17:16 and other
unnamed positions remain zero except for named generation overlays. The
64-bit address storage is not a statement of usable GPU virtual-address
width. Mesa's newer T2T header differs at bit 31 as described below.
[PAL GFX10 builders][p-p10-t2t] · [PAL GFX12 builders][p-p12-t2t] ·
[Mesa T2T emission][m-t2t]

### Geometry and surface information

The names below omit only the containing `DW_n_UNION`. In T2T, each source
field at DWORDs 3–6 has a destination counterpart at DWORDs 9–12 with the
`dst_` prefix. Coordinates are direct values. Modern surface dimensions,
row/slice pitches and copy dimensions are encoded minus one.

| SUBWIN DWORD / T2T DWORD | Native field names | GFX10 bits | GFX12 bits |
| --- | --- | --- | --- |
| 3 / 3, 9 | `tiled_x` / `src_x`, `dst_x` | 13:0 | 15:0 |
| 3 / 3, 9 | `tiled_y` / `src_y`, `dst_y` | 29:16 | 31:16 |
| 4 / 4, 10 | `tiled_z` / `src_z`, `dst_z` | 12:0 | 13:0 |
| 4 / 4, 10 | `width` / `src_width`, `dst_width` | 29:16 | 31:16 |
| 5 / 5, 11 | `height` / `src_height`, `dst_height` | 13:0 | 15:0 |
| 5 / 5, 11 | `depth` / `src_depth`, `dst_depth` | 28:16 | 29:16 |
| 6 / 6, 12 | `element_size` / `src_element_size`, `dst_element_size` | 2:0 | 2:0 |
| 6 / 6, 12 | `swizzle_mode` / `src_swizzle_mode`, `dst_swizzle_mode` | 7:3 | 7:3 |
| 6 / 6, 12 | `dimension` / `src_dimension`, `dst_dimension` | 10:9 | 10:9 |
| 6 / 6, 12 | `mip_max` / `src_mip_max`, `dst_mip_max` | 19:16 | 20:16 |
| 6 / 6, 12 | `mip_id` / `src_mip_id`, `dst_mip_id` | 23:20 | 28:24 |
| 9 / — | `linear_x`, `linear_y` | 13:0, 29:16 | 15:0, 31:16 |
| 10 / — | `linear_z`, `linear_pitch` | 12:0, 29:16 | 13:0, 31:16 |
| 11 / — | `linear_slice_pitch` | 27:0 | 31:0 |
| 12 / 13 | `rect_x`, `rect_y` | 13:0, 29:16 | 15:0, 31:16 |
| 13 / 14 | `rect_z` | 12:0 | 13:0 |

[GFX10 fields][p10-subwin-layout] · [GFX10 T2T fields][p10-t2t-layout] ·
[GFX12 fields][p12-subwin-layout] · [GFX12 T2T fields][p12-t2t-layout]

`element_size` is `log2(bytes_per_element)`. PAL converts byte row/slice
pitches to elements before subtracting one. Its row helper checks exact
divisibility and, when height exceeds one, alignment to
`max(1, 4 / bytes_per_element)` elements. The format/layout producer supplies
swizzle encodings; PAL GFX12 writes dimensions 0/1/2 for 1D/2D/3D. PAL's
older helper remaps some 1D/3D rotated or Z-swizzle surfaces to dimension 2D.
Field capacity and these producer assertions are separate from the image
allocator's admitted sizes. None of these minus-one builders supplies an
empty-transfer encoding.
[PAL pitch/dimension helpers][p-p10-transforms] ·
[GFX12 helpers][p-p12-input] · [Slice pitch][p-p10-pitch] ·
[Older swizzle translation][p-addr2] · [GFX12 swizzle translation][p-addr3]

### Policy and metadata fields

SUBWIN's final rectangle word is DWORD 13; T2T's is DWORD 14. The older
fields at bits 17:16 and 25:24 are respectively `linear_sw` / `tile_sw`,
or `dst_sw` / `src_sw`. PAL leaves them zero. Its `gfx103Plus` overlay adds
`linear_cache_policy` / `dst_cache_policy` at 20:18 and
`tile_cache_policy` / `src_cache_policy` at 28:26. GFX12 instead places
`linear_mall_policy` / `dst_mall_policy` at 21:20 and
`tile_mall_policy` / `src_mall_policy` at 29:28; the older swap positions
are unnamed. These fields keep their operand identity when detile changes
which operand is the reader.
[GFX10 SUBWIN][p10-subwin-layout] · [GFX10 T2T][p10-t2t-layout] ·
[GFX12 SUBWIN][p12-subwin-layout] · [GFX12 T2T][p12-t2t-layout]

| `META_CONFIG_UNION` field | GFX10 bits | GFX12 bits |
| --- | --- | --- |
| `data_format` | 6:0 | 5:0 |
| `color_transform_disable` | 7 | Unnamed |
| `alpha_is_on_msb` | 8 | Unnamed |
| `number_type` | 11:9 | 11:9 |
| `surface_type` | 13:12 | Unnamed |
| `gfx103Plus.meta_llc` | 14 | Unnamed |
| `read_compression_mode` | Unnamed | 17:16 |
| `write_compression_mode` | Unnamed | 19:18 |
| `max_comp_block_size` | 25:24 | 25:24 |
| `max_uncomp_block_size` | 27:26 | 26 |
| `write_compress_enable` | 28 | Unnamed |
| `meta_tmz` | 29 | Unnamed |
| `pipe_aligned` | 31 | Unnamed |

GFX10 SUBWIN has `META_ADDR_LO_UNION.meta_addr_31_0` and
`META_ADDR_HI_UNION.meta_addr_63_32` at DWORDs 14–15, then config at 16;
T2T shifts all three one DWORD later. GFX12 has no metadata-address pair:
its optional config is DWORD 14 for SUBWIN or 15 for T2T. Unlisted metadata
bits are unnamed in the respective declaration.
[GFX10 layouts][p-p10-layouts] · [GFX12 layouts][p-p12-layouts]

### Emitted lengths and older source views

| Actual builder | SUBWIN DWORDs | T2T DWORDs | Metadata selection |
| --- | ---: | ---: | --- |
| Mesa before SDMA IP 7 | 14 or 17 | 15 or 18 | Three extra words only when compressed metadata is selected. T2T requires IP at least 4. |
| Mesa SDMA IP 7 or later | 14 or 15 | 15 or 16 | One extra config word when selected. |
| PAL GFX10 DMA implementation | 17 | 18 | Always emits the full declared body, including zero metadata words when DCC is zero. |
| PAL GFX12 DMA implementation | 14 or 15 | 15 or 16 | One extra word if either allocation may be compressed. |

[Mesa SUBWIN][m-tiled] · [Mesa T2T][m-t2t] ·
[PAL older SUBWIN][p-p10-transforms] · [Older T2T][p-p10-t2t] ·
[PAL newer SUBWIN][p-p12-transforms] · [Newer T2T][p-p12-t2t]

| Source view | Geometry and tiled-information distinction |
| --- | --- |
| Linux Iceland/Tonga | X/Y and linear pitch are 14-bit; Z/rectangle depth are 11-bit. Tiled pitch/slice use the legacy fields above. DWORD 6: `element_size[2:0]`, `array_mode[6:3]`, `mit_mode[10:8]`, `tilesplit_size[13:11]`, `bank_w[16:15]`, `bank_h[19:18]`, `num_bank[22:21]`, `mat_aspt[25:24]`, `pipe_config[30:26]`. |
| Linux Vega10 | 14-bit X/Y/width/height/pitch and 11-bit Z/depth. Info has `element_size[2:0]`, `swizzle_mode[7:3]`, `dimension[10:9]`, `epitch[31:16]`. SUBWIN header has `mip_max[23:20]`, `mip_id[27:24]`; T2T names the maximum but no mip ID. |
| Linux Navi10 | 14-bit X/Y/pitch and 13-bit Z/depth, info mip fields at 19:16 and 23:20; metadata-address pair and config. Its config does not name the later `meta_llc` or `pipe_aligned` fields. |
| Linux SDMA6.0 and SDMA7.1 | The selected forms have identical named layouts at the cited Linux revision: the older geometry/mip/metadata-address body, with CPV, cache-policy, `meta_llc` and `pipe_aligned` fields. |

[Iceland][l-iceland-subwin] · [Tonga][l-tonga-subwin] ·
[Vega10 SUBWIN][l-vega-subwin] · [Vega10 T2T][l-vega-t2t] ·
[Navi10][l-navi-subwin] · [SDMA6.0][l-sdma6-subwin] ·
[SDMA7.1][l-sdma71-subwin]

Mesa's information helper follows SDMA IP: below 4 it writes the legacy
bank/pipe fields; at 4 it writes dimension/epitch and puts mip values in the
header; at 5/6 it writes dimension and mip fields in the info word. At IP 7
or later it moves mip ID to bit 24 and omits dimension from the expression.
For exact SDMA2.0, its SUBWIN rectangle fields receive direct width/height/
depth; later versions receive minus-one values. The older T2T branch asserts
no selected mip and no compression before IP 5.
[Header helper][m-header] · [Info helper][m-info] ·
[Complete SUBWIN builder][m-tiled] · [Complete T2T builder][m-t2t]

Three disagreements remain distinct. Linux's SDMA7.1 generated view retains
the older metadata-address body while Mesa IP7+ and PAL GFX12 emit the
shorter body. PAL GFX12 both declares and writes dimension where Mesa's
IP7+ expression omits it. Mesa also writes T2T header bit 31 for a
source-compressed/destination-uncompressed pair at IP7+, while PAL's GFX12
T2T declaration leaves that position unnamed and its builder leaves it zero.
RADV can select that source-only compressed direct path when swizzle and
geometry match. These sources establish their respective encodings; they do
not establish identical firmware interpretation or make the three views
interchangeable.
[Linux SDMA7.1][l-sdma71-subwin] · [Mesa info][m-info] ·
[Mesa T2T][m-t2t] · [RADV direct-path predicate][m-r-scan-selector] ·
[RADV compression producer][m-r-surf] · [PAL GFX12 T2T][p-p12-t2t] ·
[PAL GFX12 declaration][p12-t2t-layout]

## Compression and layout admission

### Metadata-address paths

Mesa's pre-IP7 tiled SUBWIN selects compression from the tiled operand.
Its T2T builder permits at most one compressed operand and selects that
operand's metadata. The older config supplies hardware format, number type,
alpha placement, surface type, block sizes, pipe alignment and write
compression. Detiling reads metadata; tiling enables compressed writes for
color, while HTILE follows its separate surface-type selection. RADV derives
the active DCC/HTILE state from the current image layout and compression
capability, and derives the metadata VA from the bound image's native offset.
[Older config builder][m-meta5] · [RADV surface state][m-r-surf] ·
[DCC layout predicate][m-r-dcc] · [HTILE predicate][m-r-htile]

PAL's GFX10 implementation contains similar conditional machinery but has a
different admission boundary at the cited revision. Its `SetupMetaData`
requires the workaround `waSdmaPreventCompressedSurfUse` to be off and the
selected color layout to differ from `ColorDecompressed`, or the selected
depth/stencil layout to equal `DepthStencilCompressed`. However,
`InitLayoutStateMasks` excludes `LayoutDmaEngine` from both active compressed
maps. The public copy API requires the actual engine bit in the supplied
layouts. Consequently a valid ordinary DMA layout does not enter those
compressed metadata branches. `DepthStencilDecomprWithHiZ`, which can admit
DMA under `hiZNeverInvalid`, is a different state and does not satisfy the
HTILE branch.
[Conditional metadata builder][p-p10-meta] · [Complete layout producer][p-p10-layout] ·
[State conversion][p-p10-state] · [Required engine bit][p-api-copy]

The older PAL T2T metadata selector prefers source metadata when both
images have it and `sdmaPreferCompressedSource` is set; otherwise it can
select the destination. That setting and the presence of a metadata
allocation do not override layout admission. The separate
[inline DCC-state update](write.md#pal-metadata-and-separately-embedded-updates)
uses the same non-decompressed color-layout condition. The packet's
representation, these conditional builders and a valid selected caller are
three distinct facts.
[Selection][p-p10-t2t] · [State updater][p-p10-meta] ·
[Setting defaults][p-p10-settings] · [Navi10 workaround][p-p10-wa]

### Allocation compression and independent read/write modes

PAL GFX12's `IsImageCompressed` consumes the bound allocation's
`MaybeCompressed()`, defined as compressed or virtual memory. It describes
potential compression, not a readback of the image's current contents.
Either operand can cause the extra metadata word to be emitted; the direct
T2T path can supply both. The config has independent read/write fields:

| Field | Encodings in the selected PAL definition |
| --- | --- |
| `read_compression_mode` | 0 bypass; 1 reserved; 2 decompressed read; 3 reserved. |
| `write_compression_mode` | 0 bypass; 1 enable compression; 2 disable write compression; 3 reserved. |

[Allocation predicate][p-maybe] · [Image predicate and mode policy][p-p12-meta] ·
[Mode definitions][p12-compression-enums] · [Selected T2T builder][p-p12-t2t]

Image mode selection uses `sdmaImageCompressionMode` and the image's
create/view policy. Buffer mode uses `sdmaBufferCompressionMode`; its default
for potentially compressed storage enables decompressed reads and disables
write compression. A read-bypass request becomes a decompressed read when
`enableCompressionReadBypass` is false. Mode zero is a compression bypass,
not a cache-policy encoding.
[Complete mode selection][p-p12-meta] · [Image view policy][p-p12-view]

The helper writes read mode only for a potentially compressed source. A
potentially compressed destination supplies write mode, format, number type
and block sizes. An image uses its creation/plane controls; a buffer uses
the supplied memory-image format or the image subresource format. The generic
helper's undefined-format case uses `X32_Uint`. Source-only compression
leaves these destination fields zero.
[Metadata construction][p-p12-meta] · [Memory/image format selection][p-p12-transforms]

Mesa's IP7+ helper similarly has separate modes, selecting read mode 2 and
write mode 1 for the corresponding compressed operands. RADV assigns each
GFX12 image's compression flag from
`binding->bo && binding->bo->gfx12_allow_dcc`, after both the linear and
tiled branches. Its direct T2T policy still sends
two-compressed-operand copies through temporary storage, unlike PAL's.
The temporary surface descriptions inherit the respective image compression
flags; this assignment alone does not establish the temporary allocation's
PTE compression attributes.
[Mesa modes][m-meta7] · [RADV operand construction][m-r-surf] ·
[T2T selection][m-r-scan-selector] · [Temporary descriptions][m-r-scan]

RadeonSI's GFX12 PRIME detile is another distinct policy: it does not set
the source's compressed boolean and its source comment attributes read
decompression to PTE.D. That comment and RADV's different flag selection do
not establish an interchangeable compression recipe.
[RadeonSI modern copy][m-si-modern] · [Actual PRIME caller][m-si-blit]

### Cache policy and protected domains

PAL's older copy path writes cache policy only under `supportsMall`.
Its policy helper returns the KMD read/write L2 policy ORed with LLC bit 2
only when the selected Navi2x bypass request applies; otherwise it returns
zero. CPV separately depends on a non-default setting and valid KMD policy.
PAL GFX12 uses two-bit MALL
policy fields, also under `supportsMall`. These controls do not replace
producer/consumer dependencies, payload acquisition or storage retirement.
[Older policy selection][p-p10-policy] · [GFX12 selection][p-p12-policy] ·
[Cache maintenance](cache.md)

A TMZ field does not by itself describe a complete protected transfer.
Mesa's tiled SUBWIN gets TMZ from the tiled description; RADV asserts that
a secure tiled operand also has a secure linear operand. Its T2T builder
has no TMZ parameter and emits header extra zero, with `tmz=false` for the
older metadata helper. RADV's inspected temporary allocator has no
ENCRYPTED flag, and temporary linear descriptions leave `is_secure` false.
Although RADV can expose protected transfer queues, these paths do not
establish a complete protected T2T/staging protocol. That requires the
native protected-allocation and packet-domain contract, beyond the ordinary
unprotected sequences described here.
[SUBWIN wrapper][m-r-emit] · [T2T builder][m-t2t] ·
[Temporary allocation][m-r-temp] · [T2T temporary descriptions][m-r-scan] ·
[Memory/image descriptions][m-r-unaligned] ·
[Protected queue properties][m-r-transfer-properties]

## Direct copying and temporary staging

### Direct-path predicates

The common Mesa SUBWIN helper asserts a nonzero linear row pitch of at most
2^14 elements before IP7, or 2^16 afterward, aligned to
`max(1, 4 / bytes_per_element)` elements. When either Z offset is nonzero
or copy depth differs from one, it also asserts a nonzero slice pitch of
at most 2^28 elements aligned to four elements. That slice limit remains
stricter than the newer full-DWORD field. RADV's pre-IP5 row-alignment
predicate is stricter still: four elements rather than four bytes.
[Common assertions][m-pitch] · [RADV alignment][m-r-align] ·
[RADV memory/image selector][m-r-unaligned-selector]

For tiled/tiled copies, Mesa and PAL use the following alignment tables.
The applicable triple must divide the copy extent and both image offsets.

| Bytes per element | 2D/planar X, Y, Z | 3D X, Y, Z |
| ---: | --- | --- |
| 1 | 16, 16, 1 | 8, 4, 8 |
| 2 | 16, 8, 1 | 4, 4, 8 |
| 4 | 8, 8, 1 | 4, 4, 4 |
| 8 | 8, 4, 1 | 4, 2, 4 |
| 16 | 4, 4, 1 | 2, 2, 4 |

[RADV tables][m-r-align] · [RADV selector][m-r-scan-selector] ·
[PAL older selector][p-p10-t2t] ·
[PAL newer selector][p-p12-t2t]

RADV chooses the 3D table from the source image's resource type, with an
additional DISPLAY or STANDARD micro-mode restriction before SDMA IP7.
It asserts equal element sizes but has no matching-image-type test in
this selector. PAL's older selector has the corresponding displayable/
standard restriction, while its GFX12 selector uses the 3D table for every
3D image. PAL's T2T helpers assert matching image types and element sizes.

RADV selects staging for pre-IP5 mipmapped images, two compressed operands,
incompatible micro modes before IP7 or unequal swizzle modes afterward,
unaligned geometry, or pre-IP6 color/standalone-S8 pairing. PAL's older
selector accepts compatible micro-swizzles; GFX12 requires exact swizzle
equality. PAL also has `forceT2tScanlineCopies`, but its newer selector has
no two-compressed-operand exclusion. These are runtime selection policies,
not an inferred universal limitation on the packet.
[RADV selector][m-r-scan-selector] · [PAL common selection][p-common-image] ·
[PAL older selector][p-p10-t2t] · [PAL GFX12 selector][p-p12-t2t]

### Chunk sequence and reuse

RADV allocates a command-buffer-owned 512 KiB temporary BO lazily, aligned
to 4096 bytes in VRAM with NO_CPU_ACCESS and NO_INTERPROCESS_SHARING. Each
use registers it in the command stream's BO list. Its row chunking chooses
a power-of-two number of rows fitting that allocation; at least one full
aligned row must fit. It does not split an arbitrarily wide row into
horizontal fragments.
[Allocation][m-r-temp] · [Capacity][m-r-temp-size] · [Chunk sizing][m-r-chunk]

For each tiled/tiled chunk, RADV records:

```text
COPY_TILED_SUBWIN detile=1: source image -> linear temporary
NOP: drain the write before the temporary is read
COPY_TILED_SUBWIN detile=0: linear temporary -> destination image
NOP: drain the read before the temporary is overwritten
```

The second drain closes a different hazard from the first. Buffer/image
staging similarly interleaves useful-byte linear row copies with image
subwindow copies and drains on both edges. Temporary row padding does not
authorize reading or writing that padding in the application's buffer.
[T2T staging][m-r-scan] · [Memory/image staging][m-r-unaligned] ·
[NOP emission][m-ac] · [Drain semantics](ordering.md#pending-transfer-drains)

PAL uses one embedded-data allocation sized by the command allocator's
embedded limit. Chunk width/height/depth fit that capacity and the required
row alignment. Its two release/acquire barriers are BottomOfPipe-to-TopOfPipe;
the selected DMA constructors configure the all-image hazard mask, making
each dependency emit one NOP. Arrays advance their slice identity; a chunk
spanning depth slices requires two 3D images.
[Complete staging loop][p-stage-t2t] · [Barrier implementation][p-dma-barrier] ·
[Older constructor][p10-constructor] · [GFX12 constructor][p12-constructor]

PAL's memory/image fallback selector tests byte row pitch modulo four and,
for a tiled image, byte depth pitch modulo four. Its fallback constructs
aligned temporary/image envelopes plus byte copies of the requested
interval. If a write envelope includes neighboring pixels, it first reads
that envelope, overlays the requested bytes, and writes it back. This
read/modify/write composition needs the same ownership of neighboring
pixels as any other overlapping read/modify/write operation.
[Older selector][p-p10-input] · [GFX12 selector][p-p12-input] ·
[Complete unaligned path][p-stage-unaligned]

PAL also widens certain full image-to-image subresource copies to matching
padded extents, under explicit zero-offset, full-extent and subresource
identity checks. Its PRT `CmdCopyMemoryToTiledImage` API is a different
unit contract: the wrapper converts tile coordinates through PRT tile
dimensions and calls the ordinary memory/image path. The public API
includes whole-tile padding and excludes packed mip tails; its name does
not imply selection of `COPY_TILED` subopcode 1.
[Padded-copy predicate][p-common-image] · [PRT wrappers][p-prt] ·
[PRT API][p-api-prt]

## Publication, completion and storage lifetime

A complete unprotected transfer has the following actors and ownership edges:

1. The allocation/image owner supplies valid device mappings, native layout
   and compression state. The producer makes source payload and metadata
   ready; image usage and engine transitions admit the copy path.
2. The command recorder captures CPU region/surface values into packet
   DWORDs. Referenced pixel storage, metadata and temporary memory remain
   device operands. Recording does not snapshot their bytes.
3. The native submission owner publishes commands and establishes external
   waits. Direct or staged transfers execute with the required internal
   dependencies. A compute follower, if selected, has its own joins.
4. Destination metadata fixups finish before a subsequent consumer uses
   the result. Completion observation and the consumer's cache acquisition
   establish distinct edges.
5. Each allocation is reused only after its final user. Command/temporary
   retirement does not retire a later consumer of the destination.

[PAL usage/stage/cache contract][p-api-copy] ·
[RADV copy flow][m-r-memory-image] · [RADV image/image flow][m-r-transfer-image] ·
[Submission publication](publication.md) · [Memory edges](../../interop/README.md#what-belongs-to-an-edge)

RADV's separate HiZ destination requires a post-copy fixup: the selected
mip/layer range is expanded to [0, 1]; the transfer-queue path uses SDMA fill.
The pixel copy alone does not finish this metadata update. Unsupported
transfer-queue images can instead invoke a compute gang. Dirty leader/
follower semaphores and cache flags govern transitions; the native gang
postamble joins the follower before the leader's final completion. An
SDMA-side semaphore wait uses an SDMA poll.
[HiZ fixup][m-r-hiz-fix] · [Transfer HiZ clear][m-r-hiz] ·
[Recorded gang waits][m-r-gang-wait] · [Queue-aware wait emitter][m-r-cs-wait-write] ·
[Native gang join][m-r-gang-native] · [Selected submission][m-r-submit]

| Storage | Final-use and ownership boundary |
| --- | --- |
| CPU region arrays and local surface descriptions | Captured while recording. Raw address-based copy entrypoints do not give the recorder an owning buffer object to retain. |
| Source payload and metadata | Remain valid and unchanged through their last copy or shader reader. A completed detile can finish image reads while the temporary still has readers. |
| Temporary data | The second staging drain closes its last internal reader. Host destruction/recycling also follows the command buffer's native retirement contract. |
| Destination payload and metadata | Copy/fixup completion precedes its next consumer; that consumer has a separate final-use boundary. |
| RADV command and temporary backing | Temporary memory survives reset and is destroyed with the command buffer. Reset/destruction do not wait for pending native work. BO registration supplies handles to submission; it does not retain API image/buffer objects. |
| PAL embedded and command chunks | Automatic reuse with busy tracking follows root-chunk generation or submit-count/done-count ownership. Without busy tracking the client owns completion before reuse; allocator-reset and retained-chunk modes keep their own lifetime contract. |

[Address-based copy boundary][m-r-address-image-api] · [RADV reset][m-r-reset] ·
[RADV destruction][m-r-destroy] · [Native BO registration][m-r-ws-bos] ·
[PAL embedded allocation][p-embedded] · [Root association][p-end] ·
[PAL allocator flags][p-allocator-flags] · [Idle observation][p-idle] ·
[Chunk reuse][p-reuse]

PAL's DMA postamble increments a busy tracker only when its address is
nonzero; the submit owner records the corresponding submit count. That
tracks command-allocator storage, not every application's payload. The
submission API separately owns memory residency references and completion
fences. Submission success alone is not device completion. RadeonSI likewise
retains BO/fence ownership through its winsys; even a non-asynchronous
`cs_flush` can wait only for the CPU submission worker, leaving GPU
completion to the fence/BO owner.
[PAL tracker][p-tracker] · [Submit count][p-submit] ·
[Older postamble][p-post10] · [GFX12 postamble][p-post12] ·
[Native residency and fence contract][p-queue-contract] ·
[Submit versus WaitIdle][p-queue-wait] ·
[RadeonSI flush][m-si-ws-flush] · [BO completion][m-si-bo-wait]

## Other declared tiled forms

`COPY_TILED` subopcode 1 is a different representation from
`COPY_TILED_SUBWIN`. PAL's generated forms occupy 13 DWORDs with tiled
address at 1–2, geometry at 3–4, information at 5, coordinates/policy at
6–7, linear address at 8–9, pitches at 10–11 and count at 12. Its older
count overlays and newer mip placement differ from the subwindow fields.
The inspected ordinary PAL image paths select the subwindow forms; their
extent and count rules do not establish this whole-image form's count unit,
zero meaning or admitted transfer envelope.
[Older TILED declaration][p10-tiled-layout] · [Newer TILED declaration][p12-tiled-layout] ·
[Ordinary image dispatcher][p-common-image]

`COPY_L2T_BROADCAST` is likewise a distinct declared image broadcast, separate
from [linear broadcast](fanout.md). Linux Navi10, SDMA6.0 and SDMA7.1 headers
also declare `COPY_TILED_BC`, `COPY_T2T_BC` and `COPY_TILED_SUBWIN_BC` forms.
These declarations do not supply a selected ordinary caller or make every
block-compressed texture use a BC-named packet. Their missing execution
contract is the form's actual producer, operand units, admission conditions
and completion owner; the subwindow recipes above cannot fill that gap.
[Older broadcast declaration][p-p10-broadcast] ·
[Newer broadcast declaration][p-p12-broadcast] ·
[Navi10 TILED_BC][l-navi-tiled-bc] · [T2T_BC][l-navi-t2t-bc] ·
[SUBWIN_BC][l-navi-subwin-bc] · [SDMA6.0 TILED_BC][l-sdma6-tiled-bc] ·
[T2T_BC][l-sdma6-t2t-bc] · [SUBWIN_BC][l-sdma6-subwin-bc] ·
[SDMA7.1 TILED_BC][l-sdma71-tiled-bc] · [T2T_BC][l-sdma71-t2t-bc] ·
[SUBWIN_BC][l-sdma71-subwin-bc]

[l-iceland-subwin]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/iceland_sdma_pkt_open.h#L1048-L1252
[l-navi-subwin]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L2090-L2357
[l-navi-subwin-bc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L2358-L2568
[l-navi-t2t-bc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L1824-L2089
[l-navi-tiled-bc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L1157-L1329
[l-sdma6-subwin]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L2449-L2746
[l-sdma6-subwin-bc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L2747-L2957
[l-sdma6-t2t-bc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L2183-L2448
[l-sdma6-tiled-bc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L1455-L1634
[l-sdma71-subwin]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L2449-L2746
[l-sdma71-subwin-bc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L2747-L2957
[l-sdma71-t2t-bc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L2183-L2448
[l-sdma71-tiled-bc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L1455-L1634
[l-tonga-subwin]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/tonga_sdma_pkt_open.h#L1048-L1252
[l-vega-subwin]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L1252-L1450
[l-vega-t2t]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L1034-L1251

[m-ac]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L16-23
[m-capabilities]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L1146-1150
[m-header]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L190-204
[m-info]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L222-267
[m-meta5]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L296-319
[m-meta7]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L269-294
[m-packet-macros]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/sid.h#L325-382
[m-pitch]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L129-144
[m-r-address-image-api]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_copy.c#L405-460
[m-r-align]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L25-50
[m-r-buffer-image-api]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_copy.c#L331-403
[m-r-chunk]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L52-100
[m-r-compute-queue]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L146-157
[m-r-cs-wait-write]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cs.h#L172-215
[m-r-dcc]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_image.c#L1631-1666
[m-r-destroy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1310-1377
[m-r-emit]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L210-298
[m-r-gang-native]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1346-1493
[m-r-gang-wait]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L2015-2057
[m-r-hiz]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_clear_hiz.c#L162-215
[m-r-hiz-fix]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_copy.c#L84-104
[m-r-htile]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_image.c#L1541-1599
[m-r-image]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L385-402
[m-r-image-api]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_copy.c#L843-917
[m-r-memory-image]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_copy.c#L106-155
[m-r-queue]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L109-126
[m-r-reset]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1437-1504
[m-r-scale]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.h#L22-44
[m-r-scan]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L474-524
[m-r-scan-selector]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L404-472
[m-r-submit]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1618-1828
[m-r-support]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L526-541
[m-r-surf]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L126-208
[m-r-temp]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_copy.c#L55-75
[m-r-temp-size]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_constants.h#L142-145
[m-r-transfer-image]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_copy.c#L632-687
[m-r-transfer-properties]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L3040-3051
[m-r-unaligned]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L316-383
[m-r-unaligned-selector]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L300-314
[m-r-ws-bos]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L616-660
[m-si-blit]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/gfx/si_blit.c#L1044-1091
[m-si-bo-wait]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_bo.c#L74-146
[m-si-legacy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_sdma_copy_image.c#L162-386
[m-si-modern]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_sdma_copy_image.c#L35-160
[m-si-select]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_sdma_copy_image.c#L388-455
[m-si-ws-flush]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp#L2293-2399
[m-surf]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_surface.c#L3873-3985
[m-surf-description]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.h#L22-62
[m-t2t]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L385-461
[m-tiled]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L321-383
[m-vk-elements]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/vulkan/runtime/vk_image.c#L329-360
[p-addr2]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/addrMgr/addrMgr2/addrMgr2.cpp#L1783-L1879
[p-addr3]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/addrMgr/addrMgr3/addrMgr3.cpp#L1236-L1247
[p-allocator-flags]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdAllocator.h#L44-L70
[p-api-copy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L3195-L3322
[p-api-prt]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L3324-L3392
[p-common-image]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L842-L991
[p-device-select]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/device.cpp#L621-L660
[p-dma-barrier]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L388-L445
[p-embedded]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdBuffer.cpp#L441-L604
[p-end]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdBuffer.cpp#L333-L389
[p-idle]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStreamAllocation.cpp#L457-L480
[p-image-z]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L1871-L1894
[p-maybe]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/gpuMemory.h#L309-L312
[p-p10-base]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Image.cpp#L3026-L3055
[p-p10-broadcast]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L545-L744
[p-p10-input]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L1682-L1805
[p-p10-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Image.cpp#L1117-L1432
[p-p10-layouts]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L1414-L2053
[p-p10-meta]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L1274-L1412
[p-p10-pitch]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.h#L195-L213
[p-p10-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L355-L450
[p-p10-select]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Device.cpp#L2913-L2951
[p-p10-settings]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/settings_gfx9.json#L331-L343
[p-p10-state]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Image.h#L98-L203
[p-p10-t2t]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L674-L882
[p-p10-transforms]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L1418-L1678
[p-p10-wa]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9SettingsLoader.cpp#L477-L497
[p-p12-base]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Image.cpp#L618-L665
[p-p12-broadcast]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L533-L705
[p-p12-input]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L1364-L1536
[p-p12-layouts]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L1466-L1977
[p-p12-meta]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L1027-L1180
[p-p12-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L346-L385
[p-p12-select]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Device.cpp#L1713-L1758
[p-p12-t2t]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L567-L714
[p-p12-transforms]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L1186-L1411
[p-p12-view]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Device.cpp#L2510-L2554
[p-post10]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L178-L211
[p-post12]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L186-L213
[p-prt]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L1120-L1193
[p-queue-contract]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palQueue.h#L223-L284
[p-queue-wait]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palQueue.h#L449-L472
[p-reuse]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdAllocator.cpp#L703-L779
[p-stage-t2t]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L619-L839
[p-stage-unaligned]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L1499-L1839
[p-submit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/queue.cpp#L620-L748
[p-surface]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L1334-L1385
[p-tracker]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStream.cpp#L365-L458
[p10-constructor]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L51-L65
[p10-subwin-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L1830-L2053
[p10-t2t-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L1414-L1655
[p10-tiled-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L1657-L1828
[p12-compression-enums]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L49-L65
[p12-constructor]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L68-L83
[p12-subwin-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L1803-L1977
[p12-t2t-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L1466-L1658
[p12-tiled-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L1660-L1801
