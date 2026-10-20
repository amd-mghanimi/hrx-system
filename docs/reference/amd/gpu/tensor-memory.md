# Tensor descriptors and LDS layouts

The Tensor Data Mover (TDM) lets a shader wave describe a multidimensional
copy between global memory and local data share (LDS). One instruction can
traverse a tile, insert gaps in an LDS destination, gather or scatter rows,
and notify an LDS barrier when it completes. The wave supplies addresses and
layout in SGPRs; the payload does not pass through its VGPRs.

This chapter covers the **27 July 2026 CDNA5 ISA guide**, §10.11 and
tables 61–67. The [architecture map](architectures.md#cdna5-and-gfx1250)
connects CDNA5 to `gfx1250`. Triton's pinned Gluon API and lowering provide
concrete descriptor builders and callers. Their layout restrictions and
wave assignments remain compiler choices. The
[asynchronous-transfer chapter](async-memory.md) owns completion counters,
barrier phases and storage retirement; [clusters](clusters.md) owns
multicast membership and completion across workgroups.

## Instruction and descriptor representation

`TENSOR_LOAD_TO_LDS` and `TENSOR_STORE_FROM_LDS` consume the tensor descriptor
`D#` once per wave. They ignore `EXEC`, including `EXEC == 0`. A native
descriptor's `count` field suppresses a transfer independently of lane
activity. Tensor instructions cannot occur inside an instruction clause.
[Instruction rules, §10.11.1][isa-instructions]

| Instruction operand | Descriptor storage |
| --- | --- |
| `VADDR0` | Group 0: four SGPRs, 128 bits. |
| `VADDR1` | Group 1: eight SGPRs, 256 bits. |
| `VADDR2` | Group 2: four SGPRs, 128 bits, or `NULL`. |
| `VADDR3` | Group 3: four SGPRs, 128 bits, or `NULL`. |
| `VADDR4` | Unused, `NULL` (`0x7c`). |

Groups 2 and 3 are either both supplied or both `NULL`; a missing group acts
as zero-filled SGPRs. Two groups describe a dense tile of up to two dimensions.
Four groups carry higher dimensions, iteration state or gather/scatter indices.
The instruction's `SCOPE`, `TH` and `NV` fields select its memory policy;
similarly named group-0 fields belong to context restore.
[Operands and cache-policy fields, table 61][isa-instructions]
[Restore fields, table 62][isa-group0]

The following bit positions are **relative to the named group**, with bit 0
in its first DWORD. A DWORD is four bytes. Normal address and extent fields
are unsigned; source-language coordinates still need their own signed-offset
and bounds handling.

### Address, mode and notification fields

| Group | Bits | Native field | Meaning |
| --- | --- | --- | --- |
| 0 | 1:0 | `count` | `1`: valid tensor. `0`: no copy and no barrier arrival. `2–3`: context restore. |
| 0 | 2 | `is_restore` | Zero for a normal shader-issued descriptor. |
| 0 | 3 | `is_store` | Restore-only opcode override; normal issue selects load/store in the instruction. |
| 0 | 4 | `nv` | Restore-only nonvolatile override; zero for normal issue. |
| 0 | 6:5 | `scope` | Restore-only scope override, ignored when `is_restore == 0`. |
| 0 | 9:7 | `th` | Restore-only temporal-hint override, ignored when `is_restore == 0`. |
| 0 | 10 | `User_null` | Restore state for an originally null descriptor. |
| 0 | 29:11 | Reserved | Zero. |
| 0 | 30 | Gather index size | `0`: 16-bit indices; `1`: 32-bit indices. |
| 0 | 31 | Gather Mode | Enables the row-indexed descriptor form for load or store. |
| 0 | 63:32 | `lds_addr` | Tile start in LDS, in bytes. |
| 0 | 120:64 | `global_addr` | Tile start in global memory, in bytes. |
| 0 | 125:121 | Reserved | Zero. |
| 0 | 127:126 | `type` | `2`, the image descriptor type. |
| 1 | 15:0 | `workgroup_mask` | Cluster workgroups participating in a tensor load; zero outside a cluster. Stores ignore the mask. |
| 1 | 17:16 | `data_size` | `log2(element bytes)`: `0/1/2/3` means `1/2/4/8` bytes. |
| 1 | 18 | `atomic_barrier_enable` | Requests an LDS barrier arrival after completion. |
| 1 | 19 | `iterate_enable` | Selects repeated 2D/3D transfers; ignored in gather mode. |
| 1 | 20 | `pad_enable` | Inserts gaps into a load's LDS destination; ignored by stores. |
| 1 | 21 | `early_timeout` | Requests the cluster load's early combine timeout. |
| 1 | 24:22 | `pad_interval` | Biased power-of-two interval in DWORDs; decoded below. |
| 1 | 31:25 | `pad_amount` | Number of skipped DWORDs minus one. |
| 1 | 47:32 | `atomic_barrier_address` | LDS byte address bits `18:3`; the 64-bit object is eight-byte aligned. |

[Group 0 and group-1 prefix, tables 62–63][isa-group0]
[Group-1 continuation][isa-group1]
[Notification and multicast interpretation][isa-padding]

For a valid shader-issued descriptor with restore-only fields zero, the
first DWORD distinguishes the three forms:

| Descriptor form | Group-0 DWORD 0 |
| --- | --- |
| Dense | `0x00000001` |
| Gather/scatter with 16-bit row indices | `0x80000001` |
| Gather/scatter with 32-bit row indices | `0xc0000001` |

Table 62, Triton's builder, CK's bitfields and Opus's constant constructor
agree on these positions. MLIR's `AMDGPUMakeDmaBaseLowering` at the cited
revision reverses the two bits: it sets bit 30 for gather and bit 31 for
32-bit indices. Its conversion expectation uses the same ordering. The
32-bit form sets both bits in either implementation, concealing the
disagreement; the 16-bit form distinguishes them.
[Field table][isa-group0] [Triton builder][triton-gather-base]
[CK representation][ck-group0] [Opus constructor][opus-group0]
[MLIR lowering][mlir-gather-mode] [Conversion expectation][mlir-gather-check]

### Dense extents and strides

Native dimension 0 is the innermost, contiguous dimension. `tensor_dimN`
is the valid extent remaining from the supplied tile start; `tile_dimN`
is the extent requested by this transfer. The global stride slots are
**element counts**, not byte counts or products of dimensions to be multiplied
again. Each slot already contains the cumulative stride to the next dimension.
[Address calculation, §10.11.2][isa-addressing]

| Group | Bits | Native field | Unit or interpretation |
| --- | --- | --- | --- |
| 1 | 79:48 | `tensor_dim0` | 32-bit element extent. |
| 1 | 111:80 | `tensor_dim1` | 32-bit element extent. |
| 1 | 127:112 | `tile_dim0` | 16-bit element extent; zero makes the tensor a no-op. |
| 1 | 143:128 | `tile_dim1` | 16-bit extent; zero means dimension unused. Gather mode instead carries the valid index count. |
| 1 | 159:144 | `tile_dim2` | 16-bit extent; zero means dimension unused. Ignored in gather mode. |
| 1 | 207:160 | `tensor_dim0_stride` | 48-bit stride to the next row, in elements. |
| 1 | 255:208 | `tensor_dim1_stride` | 48-bit stride to the next plane, in elements. Ignored in gather mode. |
| 2 | 31:0 | `tensor_dim2` | 32-bit element extent. |
| 2 | 63:32 | `tensor_dim3` | 32-bit element extent; iteration repurposes this field. |
| 2 | 111:64 | `tensor_dim2_stride` | 48-bit cumulative element stride; iteration repurposes this field. |
| 2 | 127:112 | `tile_dim3` | 16-bit extent; zero means unused. Iteration repurposes this field. |
| 3 | 47:0 | `tensor_dim3_stride` | 48-bit cumulative element stride. |
| 3 | 79:48 | `tensor_dim4` | 32-bit element extent. |
| 3 | 95:80 | `tile_dim4` | 16-bit extent; zero means unused. |
| 3 | 127:96 | Reserved | Zero. |

[Groups 1–2, tables 63–64][isa-group1]
[Groups 2–3, tables 64–65][isa-group2]
[Triton stride packing][triton-create]

For an active rank `r`, element width `E = 1 << data_size` bytes, and local
tile coordinates `x0..x(r-1)`, the global address is:

```text
global_addr + E * (x0 + x1 * tensor_dim0_stride
                     + x2 * tensor_dim1_stride
                     + x3 * tensor_dim2_stride
                     + x4 * tensor_dim3_stride)
```

Only active dimensions contribute. Without padding, LDS advances through the
tile's elements contiguously. A request past a `tensor_dimN` returns zero
on a load; a store drops that element. These logical tensor bounds do not
replace valid global mappings or the workgroup's LDS allocation.
[Traversal][isa-addressing] [Bounds and native address errors, §10.11.6][isa-bounds]

## Moving a descriptor through a tensor

The native descriptor contains the **current tile address**, not an original
allocation base plus a separate coordinate. Updating that address does not
implicitly shorten its extents. Triton's `updateTensorDescriptor` reflects
this boundary: `add_offsets` advances the address using 64-bit arithmetic
over the decoded strides, `set_bounds` replaces the extents, and
`clamp_bounds` derives smaller extents from the offsets. Gluon uses
outermost-first dimensions, reversing the native descriptor's order.
[Descriptor mutation][triton-update]
[Public update contract][triton-update-api]

For example, a row-major BF16 tensor with shape `[M, N]` and row stride `S`
elements begins a tile at source coordinates `[m, n]` with:

```text
global_addr = base + 2 * (m * S + n)
tensor_dim0 = N - n
tensor_dim1 = M - m
tensor_dim0_stride = S
```

This example assumes `0 <= m <= M` and `0 <= n <= N`; the compiler handles
other coordinates before native issue. Advancing by `K` columns adds `2*K`
bytes. A descriptor whose old bounds remain unchanged still describes the
old number of valid elements from its new address. Interior-loop advancement
and final partial-tile bounds are therefore distinct updates. Gluon's
`async_load`/`async_store` offset convenience performs the address update
with bounds clamping; the lower-level update API also exposes address-only
advancement. [Convenience operations][triton-copy-api]
[Partial-tile caller][triton-clamp-caller]

The base descriptor builder retains tensor shape, stride and padding metadata.
The issue-time filler supplies the LDS destination, wave's tile dimensions,
predicate, multicast mask and barrier address once the operation's layout is
known. A logical descriptor and a fully populated native transfer descriptor
thus have different information at different points in lowering.
[Base builder][triton-create] [Issue-time fields][triton-fill]

## LDS padding and padded stores

Padding inserts **unwritten holes** in a load's LDS stream. This differs
from zero-filled tensor out-of-bounds elements. For enabled padding, table 63
decodes the fields as:

```text
interval_bytes = 4 * 2^(pad_interval + 1)   // 8..1024 bytes
padding_bytes  = 4 * (pad_amount + 1)      // 4..512 bytes
```

The innermost tile's byte extent must be a multiple of four for padding.
Triton converts its element-based layout quantities to DWORDs, then applies
these biased encodings. Native stores ignore the padding controls: they have
no general operation that removes arbitrary holes from an LDS layout.
[Padding behavior, §10.11.2][isa-padding]
[Encoded units][isa-group1] [Tile-width condition][isa-group2]
[Compiler encoding][triton-padding]

The guide's prose also says interval and amount must either both be zero or
both be nonzero, without distinguishing encoded fields from decoded sizes.
That wording conflicts with treating raw zero as “no padding”: table 63
assigns zero a positive size in each field and provides a separate enable
bit. The table and compiler agree on the biased encodings above; the prose
does not establish an additional raw-zero rejection rule.
[Prose][isa-padding] [Field table][isa-group1]

Descriptor APIs also differ in their input units. CK copies
`TDMLdsPaddingConfig` directly into the encoded fields. Opus's
`padding<T, IntervalElements, AmountElements>` instead accepts element
counts and applies the byte scaling and bias: 64 `uint32_t` elements with
a four-element gap encode as interval `5`, amount `3`. Opus requires both
decoded counts to be positive when enabled, then separately rejects the
encoded pair `(0, 0)` while admitting other pairs with one encoded zero.
That additional constraint differs from the manual's ambiguous zero/nonzero
wording; agreement on scaling does not settle all raw-zero cases.
[CK field assignment][ck-padding] [Opus encoding and constraints][opus-padding]
[Element-count caller][opus-padding-caller]

Triton implements a narrower padded-row store using ordinary store bounds.
When the padding interval equals the innermost block extent, it widens
`tile_dim0` by the row's padding amount and clamps `tensor_dim0` to the
original data width. The LDS walk then consumes the padded row, while the
global stores corresponding to padding are dropped as out of bounds. Its
store and scatter verifiers require this single-interval shape. This is a
composition of existing semantics, not a native de-padding mode.
[Dense-store composition][triton-store-padding]
[Scatter composition][triton-gather-base]
[Admitted store/scatter layouts][triton-store-verifier]

## Descriptor iteration

For 2D and 3D tensors, `iterate_enable` repeats the descriptor while changing
its global and LDS starting addresses. The normal stride fields can select
every Nth row; the iteration increments place successive tiles into their
desired global/LDS regions. The descriptor repurposes group 2:

| Group-2 bits | Iteration field | Interpretation |
| --- | --- | --- |
| 63:32 | `lds_addr_increment` | LDS starting-address increment per iteration, in elements. |
| 111:64 | `global_addr_increment` | Global starting-address increment per iteration, in elements. |
| 127:112 | `iterate_count` | Encoded repeat count: `0` means once, `1` means twice; table 64 gives `255` as 256 repeats. |

Iteration therefore replaces the fields otherwise needed for a fourth dense
dimension. Gather mode ignores `iterate_enable` and uses its own index-list
representation. Table 64 allocates a 16-bit slot to `iterate_count` but only
describes values through `255`; the cited specification does not establish
whether the higher bits extend the count or are constrained. Field width
alone is insufficient evidence for 65,536 repeats.
[Iteration, §10.11.3.1][isa-iteration]
[Field overlay][isa-group1] [Count examples][isa-group2]

The cited Gluon descriptor API exposes rank 1–5 copies and software address
advancement, but no iteration-enable parameter. Its builder leaves the
iteration bit clear. Native descriptor iteration and a compiler loop issuing
several descriptors consequently remain separate programming forms.
[API][triton-create-api] [Builder][triton-padding]

CK exposes a `uint16_t` iteration count and copies it directly into the
16-bit slot. This corroborates the representation but supplies no separate
legal-value rule for the upper byte.
[Iteration parameter][ck-iteration-config] [Field assignment][ck-iteration]

## Row gather and scatter

Gather mode describes a 2D tile whose global rows come from an index list.
`tile_dim0` is the width of each row; `tile_dim1` is the number of valid
indices. `tensor_dim1` supplies the row bound. Groups 2 and 3 contain indices
instead of higher-dimensional layout, and `tile_dim2` and the second global
stride are ignored. The same representation selects global destinations
for `TENSOR_STORE_FROM_LDS` scatter. [Gather/scatter, §10.11.3.2][isa-iteration]

| Index width | Group 2 | Group 3 | Rows per native instruction |
| --- | --- | --- | --- |
| 16 bits | Indices 0–7, two per DWORD. | Indices 8–15, two per DWORD. | Up to 16. |
| 32 bits | Indices 0–3, one per DWORD. | Indices 4–7, one per DWORD. | Up to 8. |

Triton, CK and Opus pack indices within each group into consecutive
low-to-high bit ranges. Thus 16-bit indices 12 and 13 occupy group-3 bits
`79:64` and `95:80`. Table 67 instead prints `111:96` and `127:112` for
those two entries inside its `95:64` row, duplicating the following row's
positions. The three implementations corroborate the consecutive packing;
the printed table is internally inconsistent at those entries.
[Tables 66–67][isa-group2] [Table-67 continuation][isa-bounds]
[Triton packing][triton-index-packing]
[CK index array][ck-index-array] [CK group-3 assignment][ck-index-assignment]
[Opus packing][opus-index-packing]

The ISA permits arbitrary and repeated row indices, but guarantees correct
out-of-bounds handling only when indices are nondecreasing. An in-bounds
permutation and a list relying on zero-fill/drop for out-of-range rows are
therefore different contracts. Repeated indices also do not specify which
payload wins when scatter writes conflict; the cited passage defines index
selection, not duplicate-write arbitration. [Index ordering][isa-iteration]

Triton's index layout broadcasts each wave's indices to its lanes, matching
the SGPR descriptor. It removes redundant register copies, predicates off
redundant waves and splits larger lists into native-sized chunks. Each chunk
changes the LDS address and index groups; column position and remaining
bounds come from the base descriptor. The gather verifier checks that the
index and shared-memory layouts agree on the cluster's row distribution.
[Gather layout requirements][triton-gather-verifier]
[Chunk emission][triton-gather-emit]

Arrival counts depend on this expansion. Dense partitioned copying attaches
an explicit barrier only to the final instruction from each participating
wave. The cited gather/scatter emitter instead copies the barrier-enabled
base group into **every chunk**, producing one arrival per non-null signaling
chunk. A high-level copy count is not by itself a barrier participant count.
The public gather/scatter callers cited below use explicit zero counter waits
and supply no barrier operand. [Dense final notification][triton-dense-emit]
[Gather/scatter barrier fields][triton-gather-base]
[Per-chunk issue][triton-gather-emit]

## Compiler layouts and complete caller flow

One native descriptor has one LDS starting address. Triton's partitioned
shared layout maps each wave's transfer to one partition piece, sometimes
expanding a logical copy into several instructions. Its distribution keeps
the transfer's innermost stream aligned with the partition's padded rows.
An explicit active-wave hint additionally requires the logical-piece count
to divide the active-wave count so that each selected wave issues one native
instruction. These are compiler layout rules, not additional descriptor bits.
[Partition distribution][triton-partitions]
[Partitioned issue][triton-dense-emit]
[Hint admission][triton-hint-verifier]

| Programming form | Native or compiler composition |
| --- | --- |
| Dense 1D–5D load/store | Native dimensions; the compiler selects per-wave tiles and ordinary two-/four-group operands. |
| Padded dense load | Native LDS gap insertion. |
| Padded dense store/scatter | Compiler's restricted row-width/bounds composition. |
| Repeated 2D/3D tile | Native iteration overlay; separate from Gluon's address-advancing loop. |
| 2D gather/scatter | Native row indices, with compiler chunking beyond 16 or 8 indices. |
| Partitioned LDS destination | Compiler wave assignment, destination selection and possibly multiple instructions. |
| Explicit fused load | Compiler selects one of 2–4 descriptors by disjoint wave roles, then issues one native instruction site. It does not encode several active descriptors in `count`. |
| Descriptor-based prefetch | Compiler derives addresses and emits global prefetch operations; it neither fills an LDS tile nor establishes tensor-transfer completion. |

[Rank family and caller][triton-rank-caller]
[Fused operation][triton-fused-api] [Fused lowering][triton-fused]
[Prefetch lowering][triton-prefetch]

A concrete source caller, `tdm_gather_kernel`, composes ingress and egress:

1. The caller supplies a valid global input mapping, row indices, output
   mapping, column coordinate and shared layout. The workgroup allocates its
   LDS tile before issuing a transfer.
2. It builds an input descriptor, advances its column address with bounds
   clamping, and issues the row gather. Each wave receives a uniform index
   tuple; the compiler expands longer tuples into native chunks.
3. It executes `tdm.async_wait(0)` before consuming that LDS tile. This waits
   each issuing wave's tensor class; any dependency across different waves'
   storage still needs the workgroup's corresponding synchronization.
4. It builds an output descriptor for the dense output shape and issues a
   tensor store from LDS. A final zero tensor wait retires the transfer's
   reads of LDS and writes to global memory for the issuing wave.
5. Workgroup completion joins all participants. An external CPU or device
   observer additionally follows the surrounding
   [global release/acquire contract](shader-memory.md#global-release-and-acquire-sequences).

[Gather caller][triton-gather-caller]
[Reverse dense-load/scatter caller][triton-scatter-caller]

The compiler supplies the cross-wave part of this flow through its LDS
dependency pass. TDM waits carry `MemWaitOpTrait`, which the common analysis
classifies as completion requiring synchronization before a later dependent
actor. Overlapping LDS reads/writes also contribute dependencies. The AMD
pipeline runs that analysis after shared allocation; local barriers lower
to workgroup-scoped release/acquire fences around an execution barrier.
The native `WaitTensorcnt` lowering itself emits only the counter wait.
[Wait trait][triton-wait-trait] [Completion classification][triton-completion]
[Dependency and barrier insertion][triton-membar]
[Pass placement][triton-pipeline] [Local barrier lowering][triton-local-barrier]
[Native counter wait][triton-native-wait]

In a resident producer/consumer loop, separate ready and empty notifications
allow these same transfers to overlap compute. Address advancement controls
the next input; reader retirement controls when its LDS destination can be
overwritten. The [slot protocol](async-memory.md#reusable-input-and-output-slots)
keeps those obligations distinct, including final drain.

### Target-selected policies

Triton assigns parsed target names with major version 12 and minor version 5
to its `GFX1250` family, enabling TDM and multi-workgroup launch for that
family. `supportsMulticast` additionally excludes the `-strict` selection;
`getMaxMulticastMaskPopcount` returns five for the enabled path. The
[cluster lowering](clusters.md#request-matching-and-masks) chooses
power-of-two subgroups within that bound. LLVM's strict target feature list
still inherits `FeatureMcastLoadInsts`. Instruction availability therefore
does not explain Triton's stricter selection. These compiler sources do not
identify a public erratum explaining the difference. ROCr's separate
revision-zero strict-target selection is described with the
[cluster applicability rules](clusters.md#request-matching-and-masks).
[Triton target predicates][triton-targets]
[Compiler family selection][triton-family]
[LLVM common and strict features][llvm-multicast-features]

LLVM independently applies the
[tensor issue-depth policy](async-memory.md#compiler-selected-tensor-depth).
Neither that policy nor a descriptor's correct layout provides the
application's storage-readiness, final-reader or external-publication edge.

[isa-instructions]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=149
[isa-addressing]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=150
[isa-padding]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=151
[isa-iteration]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=152
[isa-group0]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=153
[isa-group1]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=154
[isa-group2]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=155
[isa-bounds]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=156
[triton-create]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/TDMUtility.cpp#L503-L773
[triton-fill]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/TDMUtility.cpp#L777-L1030
[triton-update]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/TDMUtility.cpp#L317-L455
[triton-update-api]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/python/triton/experimental/gluon/language/amd/cdna5/tdm.py#L167-L244
[triton-copy-api]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/python/triton/experimental/gluon/language/amd/cdna5/tdm.py#L248-L375
[triton-clamp-caller]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/python/test/test_tdm_copy.py#L782-L811
[triton-padding]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/TDMUtility.cpp#L588-L656
[triton-store-padding]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/TDMUtility.cpp#L922-L945
[triton-store-verifier]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/Dialect/TritonAMDGPU/IR/Dialect.cpp#L1434-L1515
[triton-create-api]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/python/triton/experimental/gluon/language/amd/cdna5/tdm.py#L94-L148
[triton-index-packing]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/TDMUtility.cpp#L1137-L1190
[triton-gather-base]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/TDMUtility.cpp#L1033-L1133
[triton-gather-verifier]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/Dialect/TritonAMDGPU/IR/Dialect.cpp#L1518-L1614
[triton-gather-emit]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/TDMUtility.cpp#L1391-L1549
[triton-dense-emit]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/TDMUtility.cpp#L1288-L1383
[triton-partitions]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/TDMUtility.cpp#L80-L148
[triton-hint-verifier]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/Dialect/TritonAMDGPU/IR/Dialect.cpp#L1325-L1376
[triton-fused-api]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/python/triton/experimental/gluon/language/amd/cdna5/tdm.py#L298-L348
[triton-fused]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/TDMUtility.cpp#L1717-L1757
[triton-prefetch]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/TDMUtility.cpp#L1553-L1672
[triton-rank-caller]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/python/test/test_gluon_cdna5.py#L2244-L2341
[triton-gather-caller]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/python/test/test_gluon_cdna5.py#L4367-L4397
[triton-scatter-caller]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/python/test/test_gluon_cdna5.py#L3978-L4010
[triton-targets]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/Dialect/TritonAMDGPU/IR/TargetFeatures.cpp#L254-L270
[triton-family]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/Dialect/TritonAMDGPU/IR/TargetFeatures.cpp#L76-L88
[triton-wait-trait]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/include/Dialect/TritonAMDGPU/IR/TritonAMDGPUOps.td#L1111-L1144
[triton-completion]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/lib/Analysis/Membar.cpp#L165-L179
[triton-membar]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/lib/Analysis/Membar.cpp#L334-L389
[triton-pipeline]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/backend/compiler.py#L565-L576
[triton-local-barrier]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/MemoryOpToLLVM.cpp#L620-L667
[triton-native-wait]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/LoadStoreOpToLLVM.cpp#L2474-L2489
[llvm-multicast-features]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/lib/Target/AMDGPU/AMDGPU.td#L2490-L2546
[ck-group0]: https://github.com/ROCm/composable_kernel/blob/b51e8921db0e11a0a727032d7adcad94aabd8bdd/include/ck_tile/core/arch/amd_tdm_descriptor.hpp#L74-L90
[ck-padding]: https://github.com/ROCm/composable_kernel/blob/b51e8921db0e11a0a727032d7adcad94aabd8bdd/include/ck_tile/core/arch/amd_tdm_descriptor.hpp#L480-L486
[ck-iteration-config]: https://github.com/ROCm/composable_kernel/blob/b51e8921db0e11a0a727032d7adcad94aabd8bdd/include/ck_tile/core/arch/amd_tdm_descriptor.hpp#L27-L32
[ck-iteration]: https://github.com/ROCm/composable_kernel/blob/b51e8921db0e11a0a727032d7adcad94aabd8bdd/include/ck_tile/core/arch/amd_tdm_descriptor.hpp#L553-L561
[ck-index-array]: https://github.com/ROCm/composable_kernel/blob/b51e8921db0e11a0a727032d7adcad94aabd8bdd/include/ck_tile/core/arch/amd_tdm_descriptor.hpp#L289-L315
[ck-index-assignment]: https://github.com/ROCm/composable_kernel/blob/b51e8921db0e11a0a727032d7adcad94aabd8bdd/include/ck_tile/core/arch/amd_tdm_descriptor.hpp#L594-L621
[opus-group0]: https://github.com/ROCm/aiter/blob/42c7e4a817e62bdb2052664c0aa8e6d78b176755/csrc/include/opus/opus.hpp#L2626-L2629
[opus-padding]: https://github.com/ROCm/aiter/blob/42c7e4a817e62bdb2052664c0aa8e6d78b176755/csrc/include/opus/opus.hpp#L2441-L2469
[opus-padding-caller]: https://github.com/ROCm/aiter/blob/42c7e4a817e62bdb2052664c0aa8e6d78b176755/op_tests/opus/device/test_tdm_gfx1250.cu#L460-L498
[opus-index-packing]: https://github.com/ROCm/aiter/blob/42c7e4a817e62bdb2052664c0aa8e6d78b176755/csrc/include/opus/opus.hpp#L2811-L2822
[mlir-gather-mode]: https://github.com/llvm/llvm-project/blob/906e3ec0647aef01363973edf3cb0104a812589a/mlir/lib/Conversion/AMDGPUToROCDL/AMDGPUToROCDL.cpp#L3684-L3698
[mlir-gather-check]: https://github.com/llvm/llvm-project/blob/906e3ec0647aef01363973edf3cb0104a812589a/mlir/test/Conversion/AMDGPUToROCDL/gfx1250.mlir#L229-L267
